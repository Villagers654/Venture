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
// Wire kinds come from the NDK r30 clang descriptors. Only this packet's
// implemented families opt in; an unknown layout still gets the ABI trap.
inline bool supported(std::string_view name) {
  return name.starts_with("AAsset") || name.starts_with("AConfiguration_") || name.starts_with("ALooper_") ||
         name.starts_with("AInputQueue_") || name.starts_with("AInputEvent_") ||
         name.starts_with("AKeyEvent_") || name.starts_with("AMotionEvent_") ||
         name.starts_with("ANativeWindow_") || name.starts_with("AChoreographer") ||
         name == "ANativeActivity_setWindowFormat" || name == "ANativeActivity_setWindowFlags";
}
inline bool supported(a32::Type type) {
  if (type.kind == a32::Kind::Layout)
    return type.tag == "JNIEnv *" || type.tag == "ANativeActivity *" ||
           type.tag == "ANativeWindow_Buffer *" || type.tag == "ARect *" || type.tag == "AInputEvent **";
  if (type.kind == a32::Kind::Handle) {
    auto tag = type.tag;
    if (tag.starts_with("const ")) tag.remove_prefix(6);
    return tag == "AAsset *" || tag == "AAssetDir *" || tag == "AAssetManager *" ||
           tag == "AConfiguration *" || tag == "ALooper *" || tag == "AInputQueue *" ||
           tag == "AInputEvent *" || tag == "ANativeWindow *" || tag == "AChoreographer *" ||
           tag == "AChoreographerFrameCallbackData *" || tag == "jobject";
  }
  if (type.kind == a32::Kind::Callback)
    return type.tag == "ALooper_callbackFunc" || type.tag.starts_with("AChoreographer_");
  return !a32::needs_adapter(type);
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
  explicit Frame(CallContext& c) : ctx(c), cursor(c.cpu()) {}
  template <class T>
  T read() {
    if constexpr (std::is_same_v<T, CallContext&>)
      return ctx;
    else {
      auto type = ctx.info().a32->arguments[index++];
      auto bits = cursor.next(type);
      if (type.kind == a32::Kind::Handle && type.tag != "AAssetManager *") {
        if (type.tag == "jobject")
          bits = h2g(java::jni32::object(static_cast<std::uint32_t>(bits)));
        else if (bits) {
          pins.push_back(android::handles32::resolve(static_cast<std::uint32_t>(bits), type.tag));
          bits = h2g(pins.back().get());
        }
      }
      if constexpr (std::is_pointer_v<T>) {
        if (bits && (a32::is_out(type.kind) || type.tag == "AInputEvent **")) {
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
      words[i] = out.type.tag == "AInputEvent **"
                     ? static_cast<std::uint32_t>(android::handles32::encode(out.value, "AInputEvent"))
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
      if constexpr (std::is_integral_v<R> && std::is_signed_v<R>) success = result >= 0;
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
  if (process_config().isa() != GuestIsa::a32 || library != "libandroid.so" || !supported(name)) return;
  auto* d = a32::descriptor(library, name);
  if (!d || !supported(d->result)) return;
  for (auto type : d->arguments)
    if (!supported(type)) return;
  a32::register_adapter(library, name, [](CallContext& c) { invoke(F, c); });
}
}  // namespace venture::hle::android_hle::abi32
