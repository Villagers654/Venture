// Fixed-width audio signatures on LP64 hosts. core/hle.h rejects the C++
// type 'long', which is also int64_t/uint64_t on POSIX. Keep this local to
// audio until the core integrates a width-aware guard; no core contract is
// changed.
#pragma once
#include <system_error>

#include "common/log.h"
#include "core/hle.h"

namespace venture::audio {
namespace detail {
template <class T>
T read_arg(hle::ArgCursor& cursor) {
  if constexpr (std::is_same_v<T, std::int64_t> ||
                std::is_same_v<T, std::uint64_t>)
    return static_cast<T>(cursor.next_gpr());
  else
    return hle::detail::read_arg<T>(cursor);
}
template <class R, class... A>
void invoke(R (*fn)(A...), hle::CallContext& ctx) {
  hle::ArgCursor cursor(ctx);
  auto read = [&]<class T>() -> T {
    if constexpr (std::is_same_v<T, hle::CallContext&>)
      return ctx;
    else
      return read_arg<T>(cursor);
  };
  std::tuple<A...> args{read.template operator()<A>()...};
  if constexpr (std::is_void_v<R>)
    std::apply(fn, args);
  else if constexpr (std::is_same_v<R, std::uint64_t> ||
                     std::is_same_v<R, std::int64_t>)
    ctx.return_x(static_cast<std::uint64_t>(std::apply(fn, args)));
  else
    hle::detail::write_result(ctx, std::apply(fn, args));
}
}  // namespace detail
template <auto F>
hle::Handler wrap_abi() {
  const hle::Handler original = [](hle::CallContext& ctx) {
    try {
      detail::invoke(F, ctx);
    } catch (const std::bad_alloc&) {
      log::error("audio", "{}: allocation failed", ctx.info().name);
      ctx.return_x(ctx.info().library == "libaaudio.so"
                       ? std::uint64_t(std::int64_t(-887))
                       : 3);
    } catch (const std::system_error& e) {
      log::error("audio", "{}: host operation failed: {}", ctx.info().name,
                 e.what());
      ctx.return_x(ctx.info().library == "libaaudio.so"
                       ? std::uint64_t(std::int64_t(-889))
                       : 4);
    }
  };
  return hle::a32::bind_handler(original, [](hle::CallContext& ctx) {
    try { hle::a32::invoke(F, ctx); }
    catch (const std::bad_alloc&) {
      log::error("audio", "{}: allocation failed", ctx.info().name);
      ctx.cpu().set_x(0, ctx.info().library == "libaaudio.so" ? std::uint32_t(-887) : 3);
    } catch (const std::system_error& e) {
      log::error("audio", "{}: host operation failed: {}", ctx.info().name, e.what());
      ctx.cpu().set_x(0, ctx.info().library == "libaaudio.so" ? std::uint32_t(-889) : 4);
    }
  }, [](const hle::a32::Descriptor& d) { return hle::a32::check_signature(F, d); });
}
}  // namespace venture::audio
