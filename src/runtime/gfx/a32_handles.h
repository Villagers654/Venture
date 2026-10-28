#pragma once
#include "core/a32_abi.h"
#include "core/token32.h"
#include "core/error.h"

namespace venture::gfx::a32 {
// Khronos EGL/eglplatform.h and GLES3/gl3.h: opaque pointers are ILP32
// words; GLuint texture/buffer names remain integers, not opaque handles.
enum class Handle { display, config, context, surface, image, egl_sync, gl_sync };
struct BadHandle { Handle kind; };
Handle handle_kind(std::string_view tag);
std::uint32_t encode(Handle, void*);
void* decode(Handle, std::uint32_t);
void retire(Handle, void*);
void clear_egl_handles();
void invalid_handle(hle::CallContext&, Handle);
void abi_failure(hle::CallContext&, int);

// Explicitly selected only for commands whose layouts have been adapted. The
// shared descriptor cursor still owns softfp, signed ILP32 and even-pair rules.
template<class R, class... A>
void invoke(R (*fn)(A...), hle::CallContext& ctx) {
  using namespace hle::a32;
  try {
    trace_enter(ctx);
    ArgCursor32 cursor(ctx.cpu());
    std::size_t index = 0;
    auto read = [&]<class T>() -> T {
      if constexpr (std::is_same_v<T, hle::CallContext&>) return ctx;
      else {
        auto type = ctx.info().a32->arguments[index++];
        auto bits = cursor.next(type);
        if constexpr (std::is_pointer_v<T>) {
          if (type.kind == Kind::Handle) {
            // EGLClientBuffer for GL_TEXTURE_2D is a GLuint cast to a pointer
            // (EGL_KHR_gl_texture_2D_image), not a host object.
            if (type.tag == "EGLClientBuffer" || type.tag.starts_with("EGLNative")) return reinterpret_cast<T>(bits);
            return reinterpret_cast<T>(decode(handle_kind(type.tag), bits));
          }
          return reinterpret_cast<T>(static_cast<std::uintptr_t>(bits));
        } else if constexpr (std::is_same_v<T, float>) return std::bit_cast<float>(std::uint32_t(bits));
        else if constexpr (std::is_same_v<T, double>) return std::bit_cast<double>(bits);
        else return static_cast<T>(bits);
      }
    };
    std::tuple<A...> args{read.template operator()<A>()...};
    if constexpr (std::is_void_v<R>) std::apply(fn, args);
    else {
      auto result = std::apply(fn, args);
      std::uint64_t bits;
      if constexpr (std::is_pointer_v<R>) {
        auto type = ctx.info().a32->result;
        bits = type.kind == Kind::Handle ? encode(handle_kind(type.tag), const_cast<void*>(static_cast<const void*>(result))) : h2g(result);
        if (bits > UINT32_MAX) throw AbiError{err::kEOVERFLOW};
      } else if constexpr (std::is_same_v<R, float>) bits = std::bit_cast<std::uint32_t>(result);
      else if constexpr (std::is_same_v<R, double>) bits = std::bit_cast<std::uint64_t>(result);
      else bits = static_cast<std::uint64_t>(result);
      ResultWriter32(ctx.cpu()).scalar(ctx.info().a32->result, bits);
    }
  } catch (BadHandle e) { invalid_handle(ctx, e.kind); }
  catch (const hle::a32::AbiError& e) { abi_failure(ctx, e.error); }
  hle::a32::trace_leave(ctx);
}
template<auto F> hle::Handler wrap() {
  return [](hle::CallContext& ctx) { invoke(F, ctx); };
}
} // namespace venture::gfx::a32
