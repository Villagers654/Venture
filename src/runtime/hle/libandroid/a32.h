#pragma once
#include "android/errors.h"
#include "android/handles32.h"
#include "android/framework_bridge.h"
#include "android/native_window.h"
#include "core/guest_storage.h"
#include "core/process.h"
#include "java/jni32.h"
#include "common/log.h"

namespace venture::hle::android_hle::abi32 {
// Wire kinds come from the NDK r30 clang descriptors (libmediandk: the pinned
// media/ndk declarations of tools/codegen/a32_media.py). Only implemented
// families opt in; an unknown layout still gets the ABI trap. AHardwareBuffer
// has no A32 adapters, so neither has the image accessor that returns one.
inline bool supported(std::string_view name) {
  if (name.starts_with("AMedia") || name.starts_with("AImage")) return name != "AImage_getHardwareBuffer";
  return name.starts_with("AAsset") || name.starts_with("AConfiguration_") || name.starts_with("ALooper_") ||
         name.starts_with("AInputQueue_") || name.starts_with("AInputEvent_") ||
         name.starts_with("AKeyEvent_") || name.starts_with("AMotionEvent_") ||
         name.starts_with("ANativeWindow_") || name.starts_with("AChoreographer") ||
         name.starts_with("ASensor") ||
         name == "ANativeActivity_setWindowFormat" || name == "ANativeActivity_setWindowFlags" ||
         name == "ANativeActivity_finish";
}
inline bool supported(a32::Type type) {
  if (type.kind == a32::Kind::Layout)
    return type.tag == "JNIEnv *" || type.tag == "ANativeActivity *" ||
           type.tag == "ANativeWindow_Buffer *" || type.tag == "ARect *" || type.tag == "AInputEvent **" ||
           type.tag == "ASensorEvent *" || type.tag == "AMediaCodecBufferInfo *" || type.tag == "AImageCropRect *" ||
           type.tag == "AImageReader_ImageListener *" || type.tag == "AImageReader_BufferRemovedListener *" ||
           type.tag == "AImageReader **" || type.tag == "AImage **" || type.tag == "ANativeWindow **";
  if (type.kind == a32::Kind::Handle) {
    auto tag = type.tag;
    if (tag.starts_with("const ")) tag.remove_prefix(6);
    return tag == "AAsset *" || tag == "AAssetDir *" || tag == "AAssetManager *" ||
           tag == "AConfiguration *" || tag == "ALooper *" || tag == "AInputQueue *" ||
           tag == "AInputEvent *" || tag == "ANativeWindow *" || tag == "AChoreographer *" ||
           tag == "AChoreographerFrameCallbackData *" || tag == "jobject" || tag == "ASensorManager *" ||
           tag == "ASensorEventQueue *" || tag == "ASensor *" || tag == "ASensorList *" ||
           tag == "AMediaFormat *" || tag == "AMediaExtractor *" || tag == "AMediaCodec *" ||
           tag == "AMediaDataSource *" || tag == "AMediaCrypto *" || tag == "AImageReader *" || tag == "AImage *";
  }
  if (type.kind == a32::Kind::Callback)
    return type.tag == "ALooper_callbackFunc" || type.tag.starts_with("AChoreographer_") ||
           type.tag.starts_with("AMediaDataSource");
  return !a32::needs_adapter(type);
}
// Out parameters that receive one opaque handle: written as that handle's token.
inline std::string_view out_handle(std::string_view tag) {
  if (tag == "AInputEvent **" || tag == "AImageReader **" || tag == "AImage **" || tag == "ANativeWindow **")
    return tag.substr(0, tag.size() - 3);
  return {};
}
// ILP32 records whose layout equals the host's: AMediaCodecBufferInfo (int32
// offset, size; int64 presentationTimeUs at 8; uint32 flags; 24 bytes on AAPCS32
// and AAPCS64) and AImageCropRect (four int32). The guest record is used in place.
inline std::size_t shared_layout(std::string_view tag) {
  return tag == "AMediaCodecBufferInfo *" ? 24 : tag == "AImageCropRect *" ? 16 : 0;
}
struct Frame {
  CallContext& ctx;
  java::jni32::PinScope jni_pins;
  a32::ArgCursor32 cursor;
  std::size_t index = 0;
  std::vector<std::shared_ptr<void>> pins;
  struct Output {
    GuestAddr guest;
    a32::Type type;
    alignas(8) std::uint64_t value = 0;
  };
  std::array<Output, 8> outputs{};
  unsigned output_count = 0;
  android::WindowBuffer window{};
  GuestAddr window_out = 0;
  // AImageReader_ImageListener / _BufferRemovedListener: {void* context; callback}.
  std::array<GuestAddr, 2> listener{};
  explicit Frame(CallContext& c) : ctx(c), cursor(c.cpu()) {}
  template <class T>
  T read() {
    if constexpr (std::is_same_v<T, CallContext&>)
      return ctx;
    else {
      auto type = ctx.info().a32->arguments[index++];
      auto bits = cursor.next(type);
      // ASensorList * is an out parameter (ASensorList is const ASensor *const *).
      const bool sensor_list = type.tag == "ASensorList *";
      if (type.kind == a32::Kind::Handle && type.tag != "AAssetManager *" && !sensor_list) {
        if (type.tag == "jobject")
          bits = h2g(java::jni32::object(static_cast<std::uint32_t>(bits)));
        else if (bits) {
          pins.push_back(android::handles32::resolve(static_cast<std::uint32_t>(bits), type.tag));
          bits = h2g(pins.back().get());
        }
      }
      if constexpr (std::is_pointer_v<T>) {
        if (bits && (a32::is_out(type.kind) || !out_handle(type.tag).empty() || sensor_list)) {
          a32::check_out(bits);
          auto& out = outputs.at(output_count++);
          out.guest = bits;
          out.type = type;
          return reinterpret_cast<T>(&out.value);
        }
        if (bits && type.tag == "ANativeWindow_Buffer *") {
          window_out = bits;
          return reinterpret_cast<T>(&window);
        }
        if (bits && (type.tag == "AImageReader_ImageListener *" || type.tag == "AImageReader_BufferRemovedListener *")) {
          std::array<std::uint32_t, 2> words{};
          a32::read_bytes(bits, words.data(), sizeof(words));
          listener = {words[0], words[1]};
          return reinterpret_cast<T>(listener.data());
        }
        if (auto size = shared_layout(type.tag); bits && size) {
          a32::check_out(bits);
          a32::check_out(bits + size - 4);
        }
        return reinterpret_cast<T>(static_cast<std::uintptr_t>(bits));
      } else if constexpr (std::is_same_v<T, float>)
        return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
      else if constexpr (std::is_same_v<T, double>)
        return std::bit_cast<double>(bits);
      else
        return static_cast<T>(bits);
    }
  }
  void finish(bool success) {
    // NDK native_window.h: ILP32 ANativeWindow_Buffer = 44 bytes; bits at 16.
    if (window_out && success) {
      std::array<std::uint32_t, 11> wire{static_cast<std::uint32_t>(window.width),
                                         static_cast<std::uint32_t>(window.height),
                                         static_cast<std::uint32_t>(window.stride),
                                         static_cast<std::uint32_t>(window.format), h2g32(g2h(window.bits))};
      a32::write_bytes(window_out, wire.data(), sizeof(wire));
    }
    if (!success) return;
    std::array<std::uint32_t, 8> words{};
    for (unsigned i = 0; i < output_count; ++i) {
      const auto& out = outputs[i];
      const auto handle = out_handle(out.type.tag);
      words[i] = !handle.empty() ? static_cast<std::uint32_t>(android::handles32::encode(out.value, handle))
                                 : a32::narrow_out(out.type, out.value);
      a32::check_out(out.guest);
    }
    for (unsigned i = 0; i < output_count; ++i) a32::write_bytes(outputs[i].guest, &words[i], 4);
  }
  template <class T>
  void result(T value) {
    auto type = ctx.info().a32->result;
    std::uint64_t bits;
    if constexpr (std::is_pointer_v<T>)
      bits = h2g(value);
    else if constexpr (std::is_same_v<T, float>)
      bits = std::bit_cast<std::uint32_t>(value);
    else if constexpr (std::is_same_v<T, double>)
      bits = std::bit_cast<std::uint64_t>(value);
    else
      bits = static_cast<std::uint64_t>(value);
    if (type.kind == a32::Kind::Handle && type.tag != "AAssetManager *") {
      if (type.tag == "jobject")
        bits = java::jni32::reference(g2h<_jobject>(bits));
      else
        bits = android::handles32::encode(bits, type.tag);
    } else if constexpr (std::is_same_v<T, const char*>) {
      if (value) bits = h2g(low_string(value));
    }
    a32::ResultWriter32(ctx.cpu()).scalar(type, bits);
  }
};
template <class R, class... A>
void invoke(R (*fn)(A...), CallContext& ctx) {
  try {
    a32::trace_enter(ctx);
    Frame frame(ctx);
    std::tuple<A...> args{frame.template read<A>()...};
    if constexpr (std::is_void_v<R>) {
      std::apply(fn, args);
      frame.finish(true);
    } else {
      auto result = std::apply(fn, args);
      bool success = true;
      if constexpr (std::is_same_v<R, bool>) success = result;  // NdkMediaFormat getters leave outputs alone
      else if constexpr (std::is_integral_v<R> && std::is_signed_v<R>) success = result >= 0;
      frame.finish(success);
      frame.result(result);
    }
    a32::trace_leave(ctx);
  } catch (const a32::AbiError& e) {
    a32::fail(ctx, e.error);
  } catch (const std::invalid_argument& e) {
    log::error("android-a32", "{}: {}", ctx.info().name, e.what());
    a32::fail(ctx, android::err::invalid);
  }
}
template <auto F>
void install(std::string_view library, std::string_view name) {
  if (process_config().isa() != GuestIsa::a32 || (library != "libandroid.so" && library != "libmediandk.so") ||
      !supported(name))
    return;
  auto* d = a32::descriptor(library, name);
  if (!d || !supported(d->result)) return;
  for (auto type : d->arguments)
    if (!supported(type)) return;
  a32::register_adapter(library, name, [](CallContext& c) { invoke(F, c); });
}
}  // namespace venture::hle::android_hle::abi32
