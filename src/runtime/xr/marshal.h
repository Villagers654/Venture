#pragma once
#include "core/hle.h"

namespace venture::xr {
// The registry uses uint64_t/int64_t, which are long on LP64 and long long on
// LLP64. Keep the shared marshaller's ban on ambiguous long signatures intact:
// adapt these fixed-width scalar slots to an explicitly 64-bit marshal type.
template <class T>
using MarshalScalar =
    std::conditional_t<std::is_integral_v<T> && sizeof(T) == 8,
                       std::conditional_t<std::is_signed_v<T>, long long, unsigned long long>, T>;

template <class R, class... A> void invoke_xr(R (*fn)(A...), hle::CallContext &ctx) {
  hle::ArgCursor cursor(ctx);
  std::tuple<A...> args{static_cast<A>(hle::detail::read_arg<MarshalScalar<A>>(cursor))...};
  if constexpr (std::is_void_v<R>)
    std::apply(fn, args);
  else
    hle::detail::write_result(ctx, static_cast<MarshalScalar<R>>(std::apply(fn, args)));
}
template <auto F> hle::Handler wrap_xr() {
  return hle::a32::bind<F>([](hle::CallContext &ctx) { invoke_xr(F, ctx); });
}
} // namespace venture::xr
