#pragma once
#include <span>
#include <string_view>
#include <tuple>
#include <cstdint>
#include <type_traits>

#include "core/hle.h"

namespace venture {
class HleLibrary;
}
namespace venture::gfx {
// Khronos pointer-sized integers are LP64 on Android even on LLP64 hosts.
// This uses the contract's ArgCursor, with fixed-width XML types. In particular
// int64_t is permitted on POSIX where its underlying type happens to be long.
template <class T>
T argument(hle::ArgCursor& cursor) {
  if constexpr (std::is_same_v<T, float>)
    return cursor.next_float();
  else if constexpr (std::is_same_v<T, double>)
    return cursor.next_double();
  else if constexpr (std::is_pointer_v<T>)
    return reinterpret_cast<T>(cursor.next_gpr());
  else {
    static_assert(std::is_integral_v<T> && sizeof(T) <= 8);
    return static_cast<T>(cursor.next_gpr());
  }
}
template <class R, class... A>
void invoke(R (*fn)(A...), hle::CallContext& ctx) {
  hle::ArgCursor cursor(ctx);
  auto read = [&]<class T>() -> T {
    if constexpr (std::is_same_v<T, hle::CallContext&>)
      return ctx;
    else
      return argument<T>(cursor);
  };
  std::tuple<A...> args{read.template operator()<A>()...};
  if constexpr (std::is_void_v<R>)
    std::apply(fn, args);
  else {
    auto value = std::apply(fn, args);
    if constexpr (std::is_pointer_v<R>)
      ctx.return_x(reinterpret_cast<std::uintptr_t>(value));
    else if constexpr (std::is_same_v<R, float>)
      ctx.return_s(value);
    else if constexpr (std::is_same_v<R, double>)
      ctx.return_d(value);
    else
      ctx.return_x(static_cast<std::uint64_t>(value));
  }
}
template <auto Fn>
hle::Handler wrap() {
  return hle::a32::bind<Fn>([](hle::CallContext& ctx) { invoke(Fn, ctx); });
}
// How a generated GL command interacts with ASTC fallback framebuffer checks,
// chosen per command by gl_thunks.py so other commands pay nothing per call.
enum class GlCheck : std::uint8_t { none, framebuffer_change, draw, read, blit, buffers };
bool validate_gl_call(GlCheck check);
template <class Signature, GlCheck Check = GlCheck::none>
hle::Handler dynamic_thunk() {
  if constexpr (Check == GlCheck::none)
    return hle::a32::bind_dynamic<Signature>([](hle::CallContext& ctx) {
      invoke(reinterpret_cast<Signature*>(ctx.user()), ctx);
    }, [](hle::CallContext& ctx) {
      hle::a32::invoke(reinterpret_cast<Signature*>(ctx.user()), ctx);
    });
  else
    return hle::a32::bind_dynamic<Signature>([](hle::CallContext& ctx) {
      if (validate_gl_call(Check))
        invoke(reinterpret_cast<Signature*>(ctx.user()), ctx);
    }, [](hle::CallContext& ctx) {
      if (validate_gl_call(Check))
        hle::a32::invoke(reinterpret_cast<Signature*>(ctx.user()), ctx);
    });
}
struct Command {
  const char* name;
  hle::Handler handler;
  GlCheck check = GlCheck::none;
};
struct Extension {
  const char* name;
  std::span<const char* const> commands;
};
std::span<const Command> gl_commands();
std::span<const Extension> gl_extensions();
// Cached guest ARM64 thunk, never the host procedure address. Unknown -> 0.
GuestAddr guest_proc_address(std::string_view name);
void register_graphics_libraries();
void register_egl(HleLibrary& library);
void register_gles(HleLibrary& library);
void add_graphics_function(HleLibrary& library, std::string_view name,
                           hle::Handler handler, void* host = nullptr, GlCheck check = GlCheck::none);
bool gl_extension_supported(std::string_view name);
hle::Handler special_gl_handler(std::string_view name);
bool blocked_gl_command(std::string_view name);
hle::Execution graphics_execution(std::string_view name, GuestIsa isa, GlCheck check = GlCheck::none);
}  // namespace venture::gfx
