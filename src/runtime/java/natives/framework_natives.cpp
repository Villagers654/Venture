#include "java/natives/framework_natives.h"

#include <jni.h>

#include <atomic>
#include <array>
#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "android/native_window.h"
#include "android/package_assets.h"
#include "common/log.h"
#include "core/thread.h"
#include "core/shutdown_trace.h"
#include "core/thread_runtime.h"
#include "core/error.h"
#include "platform/thread_priority.h"
#include "platform/network.h"
#include "java/jvm.h"
#include "java/internal.h"
#include "java/natives/native_activity_bridge.h"
#include "java/natives/surface_bridge.h"

namespace venture::java {
namespace {
std::mutex bridge_mutex;
NativeActivityBridge activity_bridge;

void fail(JNIEnv* env, const char* type, const std::string& message) {
  venture::log::error("framework", "{}", message);
  if (env->ExceptionCheck()) return;
  auto cls = env->FindClass(type);
  if (cls) {
    env->ThrowNew(cls, message.c_str());
    env->DeleteLocalRef(cls);
  }
}

std::string utf8(JNIEnv* env, jstring value) {
  if (!value) throw std::invalid_argument("null framework string");
  // JNI GetStringUTFChars uses modified UTF-8 (CESU-8 for supplementary
  // characters). PackageAssets and the guest loader expect ordinary UTF-8.
  const auto length = env->GetStringLength(value);
  if (length == 0) return {};
  std::vector<jchar> chars(static_cast<std::size_t>(length));
  env->GetStringRegion(value, 0, length, chars.data());
  if (env->ExceptionCheck())
    throw std::runtime_error("Cannot read framework string");
  std::string result;
  for (std::size_t i = 0; i < chars.size(); ++i) {
    std::uint32_t c = chars[i];
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < chars.size() &&
        chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff) {
      c = 0x10000 + ((c - 0xd800) << 10) + (chars[++i] - 0xdc00);
    } else if (c >= 0xd800 && c <= 0xdfff) {
      c = 0xfffd;
    }
    if (c < 0x80)
      result.push_back(static_cast<char>(c));
    else if (c < 0x800) {
      result.push_back(static_cast<char>(0xc0 | (c >> 6)));
      result.push_back(static_cast<char>(0x80 | (c & 63)));
    } else {
      if (c >= 0x10000) {
        result.push_back(static_cast<char>(0xf0 | (c >> 18)));
        result.push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
      } else
        result.push_back(static_cast<char>(0xe0 | (c >> 12)));
      result.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
      result.push_back(static_cast<char>(0x80 | (c & 63)));
    }
  }
  return result;
}

jstring java_string(JNIEnv* env, const std::string& value) {
  auto cls = env->FindClass("java/lang/String");
  if (!cls) return nullptr;
  auto ctor = env->GetMethodID(cls, "<init>", "([BLjava/lang/String;)V");
  if (!ctor) {
    env->DeleteLocalRef(cls);
    return nullptr;
  }
  auto bytes = env->NewByteArray(static_cast<jsize>(value.size()));
  if (!bytes) {
    env->DeleteLocalRef(cls);
    return nullptr;
  }
  env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(value.size()),
                          reinterpret_cast<const jbyte*>(value.data()));
  auto encoding = env->NewStringUTF("UTF-8");
  auto result =
      encoding && !env->ExceptionCheck()
          ? static_cast<jstring>(env->NewObject(cls, ctor, bytes, encoding))
          : nullptr;
  env->DeleteLocalRef(encoding);
  env->DeleteLocalRef(bytes);
  env->DeleteLocalRef(cls);
  return result;
}

// No C++ exception may unwind into HotSpot.
template <class F>
auto guarded(JNIEnv* env, F&& fn) -> decltype(fn()) {
  using R = decltype(fn());
  try {
    return fn();
  } catch (const ThreadExit&) {
    // NativeActivity callbacks also enter guest code. Preserve the typed
    // termination signal, as the guest JNI and native Looper bridges do.
    throw_thread_death(env);
  } catch (const ProcessExit&) {
    throw_thread_death(env);
  } catch (const std::exception& e) {
    fail(env, "java/lang/IllegalStateException", e.what());
  } catch (...) {
    fail(env, "java/lang/IllegalStateException",
         "Unknown framework native failure");
  }
  if constexpr (!std::is_void_v<R>) return R{};
}

void JNICALL log_message(JNIEnv* env, jobject, jint priority, jstring tag,
                         jstring message) {
  guarded(env, [&] {
    const auto level = priority >= 6   ? log::Level::error
                       : priority == 5 ? log::Level::warn
                       : priority == 4 ? log::Level::info
                                       : log::Level::debug;
    const auto tag_text = utf8(env, tag), text = utf8(env, message);
    if (tag_text == "Lifecycle") shutdown_trace::sample_watch(text);
    log::write(level, tag_text, text);
  });
}

void JNICALL load_library(JNIEnv* env, jobject, jstring name, jobject loader) {
  guarded(env, [&] {
    GuestThread::adopt_current();
    auto global = loader ? env->NewGlobalRef(loader) : nullptr;
    if (loader && !global) return;
    std::string error;
    bool loaded = false;
    try {
      loaded = load_guest_library(utf8(env, name), global, &error);
    } catch (...) {
      if (global) env->DeleteGlobalRef(global);
      throw;
    }
    // load_guest_library owns any retained loader reference, never this call's.
    if (global) env->DeleteGlobalRef(global);
    if (!loaded) fail(env, "java/lang/UnsatisfiedLinkError", error);
  });
}

jlong JNICALL create_surface(JNIEnv* env, jobject, jint width, jint height,
                             jint format) {
  return guarded(env, [&]() -> jlong {
    if (width <= 0 || height <= 0)
      throw std::invalid_argument("Invalid surface dimensions");
    if (format != 1 && format != 4)
      throw std::invalid_argument("Unsupported surface pixel format");
    auto* window = android::create_native_window(width, height);
    if (!window) throw std::runtime_error("Cannot allocate native window");
    window->format = format;
    return static_cast<jlong>(h2g(window));
  });
}

void JNICALL release_surface(JNIEnv* env, jobject, jlong address) {
  guarded(env, [&] {
    auto* window =
        android::native_window_from_guest(static_cast<GuestAddr>(address));
    if (!window) throw std::invalid_argument("Invalid native window");
    android::release(window);
  });
}

jbyteArray JNICALL read_asset(JNIEnv* env, jobject, jstring path) {
  return guarded(env, [&]() -> jbyteArray {
    auto stream = android::PackageAssets::get().open(utf8(env, path));
    if (!stream) return nullptr;
    const auto size = stream->length();
    if (size > static_cast<std::uint64_t>(std::numeric_limits<jsize>::max()))
      throw std::length_error(
          "Asset exceeds Java array limit; use streaming API");
    auto result = env->NewByteArray(static_cast<jsize>(size));
    if (!result) return nullptr;
    std::vector<jbyte> buffer(64 * 1024);
    std::uint64_t offset = 0;
    while (offset < size) {
      auto count = stream->read(
          buffer.data(), std::min<std::uint64_t>(buffer.size(), size - offset));
      if (count <= 0 || static_cast<std::uint64_t>(count) > size - offset)
        throw std::runtime_error("Short package asset read");
      env->SetByteArrayRegion(result, static_cast<jsize>(offset),
                              static_cast<jsize>(count), buffer.data());
      if (env->ExceptionCheck()) return nullptr;
      offset += static_cast<std::uint64_t>(count);
    }
    return result;
  });
}

// The handle is private to NativeHost.PackageInputStream. Its State monitor
// serializes all operations, explicit close and Cleaner disposal.
android::AssetStream* asset_stream(jlong handle) {
  return reinterpret_cast<android::AssetStream*>(static_cast<std::uintptr_t>(handle));
}

jlong JNICALL open_asset_stream(JNIEnv* env, jclass, jstring path) {
  return guarded(env, [&]() -> jlong {
    auto stream = android::PackageAssets::get().open(utf8(env, path));
    if (!stream) {
      fail(env, "java/io/FileNotFoundException", "Package asset not found");
      return 0;
    }
    if (stream->length() > INT64_MAX) {
      fail(env, "java/io/IOException", "Asset exceeds stream position range");
      return 0;
    }
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(stream.release()));
  });
}

jlong JNICALL asset_stream_length(JNIEnv* env, jclass, jlong handle) {
  return guarded(env, [&]() -> jlong { return asset_stream(handle)->length(); });
}

jint JNICALL read_asset_stream(JNIEnv* env, jclass, jlong handle, jbyteArray data,
                              jint offset, jint count) {
  return guarded(env, [&]() -> jint {
    // A bounded native scratch buffer avoids pinning a Java array across I/O.
    std::array<jbyte, 16 * 1024> buffer;
    auto result = asset_stream(handle)->read(buffer.data(),
        std::min<std::uint64_t>(buffer.size(), static_cast<std::uint64_t>(count)));
    if (result < 0) {
      fail(env, "java/io/IOException", "Package asset read failed");
      return -1;
    }
    if (result == 0) return -1;
    env->SetByteArrayRegion(data, offset, static_cast<jsize>(result), buffer.data());
    return static_cast<jint>(result);
  });
}

jlong JNICALL seek_asset_stream(JNIEnv* env, jclass, jlong handle, jlong offset) {
  return guarded(env, [&]() -> jlong {
    auto result = asset_stream(handle)->seek(offset, SEEK_SET);
    if (result < 0) fail(env, "java/io/IOException", "Package asset seek failed");
    return result;
  });
}

void JNICALL close_asset_stream(JNIEnv* env, jclass, jlong handle) {
  guarded(env, [&] { delete asset_stream(handle); });
}

jobjectArray JNICALL list_assets(JNIEnv* env, jobject, jstring path) {
  return guarded(env, [&]() -> jobjectArray {
    const auto names = android::PackageAssets::get().list(utf8(env, path));
    auto cls = env->FindClass("java/lang/String");
    if (!cls) return nullptr;
    auto result =
        env->NewObjectArray(static_cast<jsize>(names.size()), cls, nullptr);
    env->DeleteLocalRef(cls);
    if (!result) return nullptr;
    for (std::size_t i = 0; i < names.size(); ++i) {
      auto value = java_string(env, names[i]);
      if (!value) return nullptr;
      env->SetObjectArrayElement(result, static_cast<jsize>(i), value);
      env->DeleteLocalRef(value);
      if (env->ExceptionCheck()) return nullptr;
    }
    return result;
  });
}

jobject JNICALL open_asset_fd(JNIEnv* env, jobject host, jstring name) {
  return guarded(env, [&]() -> jobject {
    auto asset = android::PackageAssets::get().open(utf8(env, name));
    std::string path;
    std::uint64_t offset = 0;
    // AOSP AssetManager.openFd/nativeOpenAssetFd only exposes stored entries;
    // the descriptor names the archive with an explicit offset and length.
    if (!asset || !asset->file_range(&path, &offset)) {
      fail(env, "java/io/FileNotFoundException", "Asset is absent or compressed");
      return nullptr;
    }
    if (offset > INT64_MAX || asset->length() > INT64_MAX) {
      fail(env, "java/io/IOException", "Asset file range is too large");
      return nullptr;
    }
    auto cls = env->GetObjectClass(host);
    if (!cls) return nullptr;
    auto open = env->GetStaticMethodID(cls, "openAssetRegion",
        "(Ljava/lang/String;JJ)Landroid/content/res/AssetFileDescriptor;");
    auto file = open ? java_string(env, path) : nullptr;
    jobject result = nullptr;
    if (file && !env->ExceptionCheck()) {
      result = env->CallStaticObjectMethod(cls, open, file, static_cast<jlong>(offset),
                                           static_cast<jlong>(asset->length()));
      env->ExceptionCheck();
    }
    if (file) env->DeleteLocalRef(file);
    env->DeleteLocalRef(cls);
    return result;
  });
}

jlong JNICALL create_activity(JNIEnv* env, jobject, jobject activity,
                              jstring library, jstring entry) {
  return guarded(env, [&]() -> jlong {
    NativeActivityBridge bridge;
    {
      std::lock_guard lock(bridge_mutex);
      bridge = activity_bridge;
    }
    if (!bridge.create) {
      fail(env, "java/lang/UnsupportedOperationException",
           "libandroid NativeActivity bridge is not installed");
      return 0;
    }
    GuestThread::adopt_current();
    std::string error;
    auto result = bridge.create(env, activity, utf8(env, library),
                                utf8(env, entry), &error);
    if (!result) fail(env, "java/lang/UnsatisfiedLinkError", error);
    return static_cast<jlong>(result);
  });
}

void JNICALL activity_event(JNIEnv* env, jobject, jlong activity, jint event,
                            jlong surface) {
  guarded(env, [&] {
    NativeActivityBridge bridge;
    {
      std::lock_guard lock(bridge_mutex);
      bridge = activity_bridge;
    }
    if (!bridge.event || event < 1 || event > 12) {
      fail(env, "java/lang/UnsupportedOperationException",
           "NativeActivity event bridge unavailable");
      return;
    }
    std::string error;
    if (!bridge.event(static_cast<GuestAddr>(activity),
                      static_cast<NativeActivityEvent>(event),
                      static_cast<GuestAddr>(surface), &error))
      fail(env, "java/lang/IllegalStateException", error);
  });
}

jint JNICALL current_tid(JNIEnv* env, jobject) {
  return guarded(env, [] {
    return static_cast<jint>(GuestThread::adopt_current().tid());
  });
}
void JNICALL set_priority(JNIEnv* env, jobject, jint tid, jint priority) {
  guarded(env, [&] {
    if (!tid) tid = GuestThread::adopt_current().tid();
    const auto result = platform::set_thread_priority(tid, priority);
    if (result) fail(env, result == -err::kEPERM ? "java/lang/SecurityException" : "java/lang/IllegalArgumentException",
                     "Cannot set guest thread priority: Linux errno " + std::to_string(-result));
  });
}

jint JNICALL copy_surface(JNIEnv* env, jclass, jlong address, jint left, jint top, jint right, jint bottom,
                           jint width, jint height, jintArray destination) {
  return guarded(env, [&]() -> jint {
    auto window = android::pin_native_window(static_cast<GuestAddr>(address));
    if (!window) return 4; // PixelCopy.ERROR_SOURCE_INVALID
    if (!destination || width <= 0 || height <= 0 || std::int64_t(width)*height != env->GetArrayLength(destination)) return 5;
    std::lock_guard lock(window->lock);
    if (!window->cpu_buffer) {
      // GPU window snapshots require a compositor readback producer. Report
      // an actual unsupported source, never a successful copy of empty pixels.
      log::warn("PixelCopy", "Surface has no CPU-readable posted buffer; GPU snapshot unavailable");
      return 4;
    }
    if (window->cpu_locked) return 2; // producer owns the buffer
    if (!window->cpu_posted) return 3; // no buffer has been posted at this geometry
    if (!right && !bottom) { right = window->width; bottom = window->height; }
    if (left < 0 || top < 0 || right > window->width || bottom > window->height || right <= left || bottom <= top) return 4;
    if (window->format != 1 && window->format != 2 && window->format != 4) return 4;
    std::vector<jint> pixels(static_cast<std::size_t>(width)*height);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
      const auto source = (top + std::int64_t(y)*(bottom-top)/height)*window->width + left + std::int64_t(x)*(right-left)/width;
      std::uint32_t color;
      if (window->format == 4) {
        const auto rgb = g2h<const std::uint16_t>(window->cpu_buffer)[source];
        color = 0xff000000u | (((rgb>>11)*255/31)<<16) | ((((rgb>>5)&63)*255/63)<<8) | ((rgb&31)*255/31);
      } else {
        const auto* rgba = g2h<const unsigned char>(window->cpu_buffer) + source*4;
        color = (window->format == 2 ? 0xff000000u : std::uint32_t(rgba[3])<<24) | (std::uint32_t(rgba[0])<<16) | (std::uint32_t(rgba[1])<<8) | rgba[2];
      }
      pixels[std::size_t(y)*width+x] = static_cast<jint>(color);
    }
    env->SetIntArrayRegion(destination, 0, static_cast<jsize>(pixels.size()), pixels.data());
    return env->ExceptionCheck() ? 1 : 0;
  });
}

void register_natives(JNIEnv* env) {
  auto cls = env->FindClass("com/venture/runtime/NativeHost");
  if (!cls) return;
  const JNINativeMethod methods[] = {
      {const_cast<char*>("hasInternetConnectivity"), const_cast<char*>("()Z"),
       reinterpret_cast<void*>(+[](JNIEnv*, jobject) -> jboolean {
         try {
           return platform::has_internet_connectivity() ? JNI_TRUE : JNI_FALSE;
         } catch (...) {
           static std::atomic<bool> reported{false};
           if (!reported.exchange(true))
             log::warn("framework", "Host Internet validation unavailable; omitting VALIDATED capability");
           return JNI_FALSE;
         }
       })},
      {const_cast<char*>("activeNetworkType"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(+[](JNIEnv* env, jobject) -> jint {
         try {
           const auto type = platform::active_network_type();
           static std::atomic<int> previous{-3};
           if (previous.exchange(type) != type)
             log::info("framework", "Host network connected={} transport={}", type != -2, type);
           return type;
         } catch (...) {
           fail(env, "java/lang/IllegalStateException", "Host network query failed");
           return -2;
         }
       })},
      {const_cast<char*>("guestTid"), const_cast<char*>("()I"),
       reinterpret_cast<void*>(current_tid)},
      {const_cast<char*>("setThreadPriority"), const_cast<char*>("(II)V"),
       reinterpret_cast<void*>(set_priority)},
      {const_cast<char*>("log"),
       const_cast<char*>("(ILjava/lang/String;Ljava/lang/String;)V"),
       reinterpret_cast<void*>(log_message)},
      {const_cast<char*>("loadLibrary"),
       const_cast<char*>("(Ljava/lang/String;Ljava/lang/ClassLoader;)V"),
       reinterpret_cast<void*>(load_library)},
      {const_cast<char*>("createSurface"), const_cast<char*>("(III)J"),
       reinterpret_cast<void*>(create_surface)},
      {const_cast<char*>("releaseSurface"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(release_surface)},
      {const_cast<char*>("copySurfacePixels"), const_cast<char*>("(JIIIIII[I)I"),
       reinterpret_cast<void*>(copy_surface)},
      {const_cast<char*>("readAsset"),
       const_cast<char*>("(Ljava/lang/String;)[B"),
       reinterpret_cast<void*>(read_asset)},
      {const_cast<char*>("openAssetStream"), const_cast<char*>("(Ljava/lang/String;)J"),
       reinterpret_cast<void*>(open_asset_stream)},
      {const_cast<char*>("assetStreamLength"), const_cast<char*>("(J)J"),
       reinterpret_cast<void*>(asset_stream_length)},
      {const_cast<char*>("readAssetStream"), const_cast<char*>("(J[BII)I"),
       reinterpret_cast<void*>(read_asset_stream)},
      {const_cast<char*>("seekAssetStream"), const_cast<char*>("(JJ)J"),
       reinterpret_cast<void*>(seek_asset_stream)},
      {const_cast<char*>("closeAssetStream"), const_cast<char*>("(J)V"),
       reinterpret_cast<void*>(close_asset_stream)},
      {const_cast<char*>("openAssetFd"),
       const_cast<char*>("(Ljava/lang/String;)Landroid/content/res/AssetFileDescriptor;"),
       reinterpret_cast<void*>(open_asset_fd)},
      {const_cast<char*>("listAssets"),
       const_cast<char*>("(Ljava/lang/String;)[Ljava/lang/String;"),
       reinterpret_cast<void*>(list_assets)},
      {const_cast<char*>("createNativeActivity"),
       const_cast<char*>("(Landroid/app/NativeActivity;Ljava/lang/String;Ljava/"
                         "lang/String;)J"),
       reinterpret_cast<void*>(create_activity)},
      {const_cast<char*>("nativeActivityEvent"), const_cast<char*>("(JIJ)V"),
       reinterpret_cast<void*>(activity_event)},
  };
  if (env->RegisterNatives(cls, methods,
                           static_cast<jint>(std::size(methods))) != JNI_OK)
    fail(env, "java/lang/UnsatisfiedLinkError",
         "Framework native registration failed");
  env->DeleteLocalRef(cls);
}
}  // namespace

void register_framework_natives() {
  static std::once_flag once;
  std::call_once(once, [] { add_framework_natives(register_natives); });
}

void set_native_activity_bridge(NativeActivityBridge bridge) {
  std::lock_guard lock(bridge_mutex);
  activity_bridge = bridge;
}

android::NativeWindow* acquire_surface_window(JNIEnv* env, void* object) {
  return guarded(env, [&]() -> android::NativeWindow* {
    auto surface = static_cast<jobject>(object);
    if (!surface) return nullptr;
    auto cls = env->FindClass("android/view/Surface");
    if (!cls) return nullptr;
    if (!env->IsInstanceOf(surface, cls)) {
      env->DeleteLocalRef(cls);
      return nullptr;
    }
    auto method = env->GetMethodID(cls, "ventureNativeWindow", "()J");
    env->DeleteLocalRef(cls);
    if (!method || env->MonitorEnter(surface) != JNI_OK) return nullptr;
    auto address = env->CallLongMethod(surface, method);
    android::NativeWindow* window = nullptr;
    if (!env->ExceptionCheck()) {
      window =
          android::native_window_from_guest(static_cast<GuestAddr>(address));
      if (window) android::acquire(window);
    }
    env->MonitorExit(surface);
    return window;
  });
}
}  // namespace venture::java
