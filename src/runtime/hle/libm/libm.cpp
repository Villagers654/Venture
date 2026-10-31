#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cfenv>
#include <climits>
#include <cmath>
#include <limits>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

#include "hle/libc/libc_a.h"
#include "hle/libc/abi32.h"
#pragma STDC FENV_ACCESS ON
namespace venture::hle::libc {
namespace {
// bionic bits/fenv_arm.h: FE_* are FPSR bits; rounding modes are FPCR[23:22].
constexpr unsigned all_exceptions = 0x9f;
unsigned guest_exceptions(int host) {
  return ((host & FE_INVALID) ? 1 : 0) | ((host & FE_DIVBYZERO) ? 2 : 0) | ((host & FE_OVERFLOW) ? 4 : 0) |
         ((host & FE_UNDERFLOW) ? 8 : 0) | ((host & FE_INEXACT) ? 16 : 0);
}
// Host float and double math runs in SSE registers, except nexttoward's
// long double operand. For the SSE
// functions MXCSR is the whole host environment, so the scope only sets its
// rounding mode and reads its flags, without saving the x87 environment.
#if defined(__x86_64__) || defined(_M_X64)
constexpr bool sse_math = true;
// MXCSR flags: invalid 0x01, denormal 0x02, divide-by-zero 0x04, overflow 0x08,
// underflow 0x10, inexact 0x20. Denormal input has no FE_* bit.
unsigned guest_mxcsr_exceptions(unsigned csr) {
  return (csr & 0x01) | ((csr >> 1) & 0x1e);
}
#else
constexpr bool sse_math = false;
#endif
template <bool X87 = false>
struct MathScope {
  static constexpr bool full = X87 || !sse_math;
  CallContext& c;
  // Inside the A64 JIT an InJitGuestFp handler runs in the guest MXCSR: the
  // math already rounds in the guest mode and raises guest FPSR flags, so only
  // errno needs translating.
  const bool guest = !full && guest_fp_environment;
  std::fenv_t saved{};
  unsigned saved_csr = 0;
  int* host_errno;
  int saved_errno;
  explicit MathScope(CallContext& ctx) : c(ctx), host_errno(&errno), saved_errno(*host_errno) {
    if (!guest) enter();
    *host_errno = 0;
  }
  void enter() {
    const unsigned mode = (c.cpu().fpcr() >> 22) & 3;
    if constexpr (full) {
      std::feholdexcept(&saved);
      int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
      std::fesetround(modes[mode]);
    } else {
#if defined(__x86_64__) || defined(_M_X64)
      // MXCSR: flags [5:0], masks [12:7], rounding [14:13] = nearest, -inf, +inf, zero.
      constexpr unsigned rounding[] = {0u << 13, 2u << 13, 1u << 13, 3u << 13};
      saved_csr = _mm_getcsr();
      _mm_setcsr((saved_csr & ~0x603fu) | 0x1f80u | rounding[mode]);
#endif
    }
  }
  ~MathScope() {
    if (!guest) leave();
    if (*host_errno == EDOM)
      set_error(err::kEDOM);
    else if (*host_errno == ERANGE)
      set_error(err::kERANGE);
    *host_errno = saved_errno;
  }
  void leave() {
    unsigned raised = 0;
    if constexpr (full)
      raised = guest_exceptions(std::fetestexcept(FE_ALL_EXCEPT));
#if defined(__x86_64__) || defined(_M_X64)
    else
      raised = guest_mxcsr_exceptions(_mm_getcsr());
#endif
    c.cpu().set_fpsr(c.cpu().fpsr() | raised);
    if constexpr (full)
      std::fesetenv(&saved);
#if defined(__x86_64__) || defined(_M_X64)
    else
      _mm_setcsr(saved_csr);
#endif
  }
};
// FPSR.IOC for a handler that computes its result without host FP operations.
void raise_invalid(CallContext& c) {
#if defined(__x86_64__) || defined(_M_X64)
  if (guest_fp_environment) { _mm_setcsr(_mm_getcsr() | 1); return; }
#endif
  c.cpu().set_fpsr(c.cpu().fpsr() | 1);
}
// Quad math currently converts through binary64 (53-bit precision and range).
// frexpl and the nexttoward direction comparison below preserve all 128 bits.
#define UNARY(name)                        \
  template <class T>                       \
  T name##_impl(CallContext& c, T x) {     \
    MathScope scope(c);                    \
    return std::name(x);                   \
  }                                        \
  void name##_quad(CallContext& c) {       \
    auto d = quad_to_double(c.cpu().v(0)); \
    auto r = name##_impl(c, d);            \
    c.cpu().set_v(0, double_to_quad(r));   \
  }
UNARY(acos);
UNARY(acosh);
UNARY(asin);
UNARY(asinh);
UNARY(atan);
UNARY(atanh);
UNARY(cos);
UNARY(cosh);
UNARY(sin);
UNARY(sinh);
UNARY(tan);
UNARY(tanh);
UNARY(exp);
UNARY(exp2);
UNARY(expm1);
UNARY(log);
UNARY(log10);
UNARY(log2);
UNARY(log1p);
UNARY(logb);
UNARY(sqrt);
UNARY(cbrt);
UNARY(ceil);
UNARY(floor);
UNARY(trunc);
UNARY(round);
UNARY(rint);
UNARY(nearbyint);
UNARY(fabs);
UNARY(tgamma);
UNARY(erf);
UNARY(erfc);
#undef UNARY
template <>
float floor_impl<float>(CallContext& c, float x) {
  // Integer rounding needs only the binary32 fraction bits. Keep the host
  // environment untouched; floor is independent of the guest rounding mode.
  auto bits = std::bit_cast<std::uint32_t>(x);
  const unsigned exponent = (bits >> 23) & 255;
  if (exponent == 255) {
    if ((bits & 0x007fffff) && !(bits & 0x00400000)) {
      raise_invalid(c); // quiet a signalling NaN, preserving its payload
      bits |= 0x00400000;
    }
    return std::bit_cast<float>(bits);
  }
  if (exponent >= 150) return x; // every finite binary32 value here is integral
  if (exponent < 127) {
    if (!(bits & 0x7fffffff)) return x; // preserve signed zero
    return std::bit_cast<float>((bits & 0x80000000) ? 0xbf800000u : 0u);
  }
  const unsigned fractional_bits = 150 - exponent;
  const auto mask = (1u << fractional_bits) - 1;
  if (!(bits & mask)) return x;
  if (bits & 0x80000000) bits += 1u << fractional_bits;
  return std::bit_cast<float>(bits & ~mask);
}
#define BINARY(name)                                                         \
  template <class T>                                                         \
  T name##_impl(CallContext& c, T x, T y) {                                  \
    MathScope scope(c);                                                      \
    return std::name(x, y);                                                  \
  }                                                                          \
  void name##_quad(CallContext& c) {                                         \
    auto x = quad_to_double(c.cpu().v(0)), y = quad_to_double(c.cpu().v(1)); \
    auto r = name##_impl(c, x, y);                                           \
    c.cpu().set_v(0, double_to_quad(r));                                     \
  }
BINARY(atan2);
BINARY(pow);
BINARY(fmod);
BINARY(remainder);
BINARY(hypot);
BINARY(fmin);
BINARY(fmax);
BINARY(fdim);
BINARY(copysign);
BINARY(nextafter);
#undef BINARY
template <class T>
T fma_impl(CallContext& c, T x, T y, T z) {
  MathScope scope(c);
  return std::fma(x, y, z);
}
void fma_quad(CallContext& c) {
  auto r =
      fma_impl(c, quad_to_double(c.cpu().v(0)), quad_to_double(c.cpu().v(1)), quad_to_double(c.cpu().v(2)));
  c.cpu().set_v(0, double_to_quad(r));
}
template <class T>
T ldexp_impl(CallContext& c, T x, int e) {
  MathScope<> scope(c);
  // Adapted from musl scalbn/scalbnf. Copyright (c) 2005-2020 Rich Felker,
  // et al.; MIT license, see LICENSES/MIT.txt.
  using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
  constexpr int precision = std::numeric_limits<T>::digits;
  constexpr int max_exponent = std::numeric_limits<T>::max_exponent - 1;
  constexpr int min_exponent = std::numeric_limits<T>::min_exponent - 1;
  const auto power = [](int exponent) {
    return std::bit_cast<T>(Bits(exponent + max_exponent) << (precision - 1));
  };
  if (e > max_exponent) {
    x *= power(max_exponent);
    e -= max_exponent;
    if (e > max_exponent) {
      x *= power(max_exponent);
      e = std::min(e - max_exponent, max_exponent);
    }
  } else if (e < min_exponent) {
    // Keep intermediate products normal until the final rounding to a
    // subnormal. Otherwise two rounded products can double-round the result.
    x *= power(min_exponent + precision);
    e -= min_exponent + precision;
    if (e < min_exponent) {
      x *= power(min_exponent + precision);
      e = std::max(e - (min_exponent + precision), min_exponent);
    }
  }
  x *= power(e);
  // Inspect this call's flags, not its result: directed overflow may return
  // the largest finite value, and underflow may return a nonzero subnormal.
#if defined(__x86_64__) || defined(_M_X64)
  const unsigned raised = guest_mxcsr_exceptions(_mm_getcsr());
#else
  const unsigned raised = guest_exceptions(std::fetestexcept(FE_ALL_EXCEPT));
#endif
  if (raised & (4 | 8)) *scope.host_errno = ERANGE;
  return x;
}
template <class T>
T scalbln_impl(CallContext& c, T x, std::int64_t e) {
  return ldexp_impl(c, x, static_cast<int>(std::clamp<std::int64_t>(e, INT_MIN, INT_MAX)));
}
void ldexp_quad(CallContext& c) {
  auto r = ldexp_impl(c, quad_to_double(c.cpu().v(0)), static_cast<int>(c.x(0)));
  c.cpu().set_v(0, double_to_quad(r));
}
template <class T>
T frexp_impl(CallContext& c, T x, int* e) {
  MathScope scope(c);
  return std::frexp(x, e);
}
void frexp_quad(CallContext& c) {
  // frexpl is exact even when the original value cannot be represented as double.
  auto q = c.cpu().v(0);
  int e = static_cast<int>((q.hi >> 48) & 32767);
  auto fraction = q.hi & 0xffffffffffffull;
  if (e == 32767 || (!e && !fraction && !q.lo))
    *g2h<int>(c.x(0)) = 0;
  else {
    int adjust = 0;
    if (!e) {
      e = 1;
      while (!(fraction & (1ull << 48))) {
        fraction = (fraction << 1) | (q.lo >> 63);
        q.lo <<= 1;
        --adjust;
      }
    }
    *g2h<int>(c.x(0)) = e - 16382 + adjust;
    q.hi = (q.hi & 0x8000000000000000ull) | (std::uint64_t(16382) << 48) | (fraction & 0xffffffffffffull);
  }
  c.cpu().set_v(0, q);
}
template <class T>
T modf_impl(CallContext& c, T x, T* whole) {
  MathScope scope(c);
  return std::modf(x, whole);
}
void modf_quad(CallContext& c) {
  double whole;
  auto r = modf_impl(c, quad_to_double(c.cpu().v(0)), &whole);
  *g2h<Vec128>(c.x(0)) = double_to_quad(whole);
  c.cpu().set_v(0, double_to_quad(r));
}
template <class T>
T remquo_impl(CallContext& c, T x, T y, int* q) {
  MathScope scope(c);
  return std::remquo(x, y, q);
}
template <class T>
void sincos_impl(CallContext& c, T x, T* s, T* co) {
  MathScope scope(c);
  *s = std::sin(x);
  *co = std::cos(x);
}
template <class T>
T lgamma_r_impl(CallContext& c, T x, int* sign) {
  MathScope scope(c);
  *sign = (x == 0 && std::signbit(x)) ||
                  (x < 0 && std::sin(std::remainder(x, T(2)) * T(3.14159265358979323846)) < 0)
              ? -1
              : 1;
  return std::lgamma(x);
}
// signgam is the traditional process-global result; lgamma_r avoids it.
int signgam_value = 0;
GuestStorage signgam32;
template <class T, bool Low = false>
T lgamma_impl(CallContext& c, T x) {
  int sign;
  auto result = lgamma_r_impl(c, x, &sign);
  std::atomic_ref(signgam_value).store(sign, std::memory_order_relaxed);
  if constexpr (Low) std::atomic_ref(*g2h<int>(signgam32.address())).store(sign, std::memory_order_relaxed);
  return result;
}
void lgamma_quad(CallContext& c) {
  c.cpu().set_v(0, double_to_quad(lgamma_impl(c, quad_to_double(c.cpu().v(0)))));
}
template <class T>
std::int64_t lrint_impl(CallContext& c, T x) {
  MathScope scope(c);
  return static_cast<std::int64_t>(std::llrint(x));
}
template <class T>
std::int64_t lround_impl(CallContext& c, T x) {
  MathScope scope(c);
  return static_cast<std::int64_t>(std::llround(x));
}
template <class T>
int ilogb_impl(CallContext& c, T x) {
  MathScope scope(c);
  return std::ilogb(x);
}
template <class T>
int isnan_impl(T x) {
  return std::isnan(x);
}
template <class T>
int isinf_impl(T x) {
  return std::isinf(x);
}
template <class T>
int finite_impl(T x) {
  return std::isfinite(x);
}
template <class T>
T nan_impl(const char* tag) {
  if constexpr (std::is_same_v<T, float>)
    return std::nanf(tag);
  else
    return std::nan(tag);
}
// nexttoward's second operand is binary128. Compare the full representation so
// directions differing below binary64 precision still advance by one float ULP.
int compare_quad(Vec128 a, Vec128 b) {
  if ((a.hi << 1) == 0 && !a.lo && (b.hi << 1) == 0 && !b.lo) return 0;
  bool sa = a.hi >> 63, sb = b.hi >> 63;
  if (sa != sb) return sa ? -1 : 1;
  int r = a.hi == b.hi ? (a.lo == b.lo ? 0 : a.lo < b.lo ? -1 : 1) : a.hi < b.hi ? -1 : 1;
  return sa ? -r : r;
}
template <class T>
void nexttoward_impl(CallContext& c) {
  T x;
  if constexpr (std::is_same_v<T, float>)
    x = c.s(0);
  else
    x = c.d(0);
  auto q = c.cpu().v(1);
  auto y = quad_to_double(q);
  T r;
  if (std::isnan(x) || std::isnan(y))
    r = static_cast<T>(x + y);
  else {
    auto order = compare_quad(double_to_quad(x), q);
    r = order ? nextafter_impl(c, x, static_cast<T>(order < 0 ? INFINITY : -INFINITY)) : static_cast<T>(y);
  }
  if constexpr (std::is_same_v<T, float>)
    c.return_s(r);
  else
    c.return_d(r);
}
struct Fenv {
  std::uint32_t control, status;
};
const Fenv default_env{};
int fegetround_impl(CallContext& c) { return (c.cpu().fpcr() >> 22) & 3; }
int fesetround_impl(CallContext& c, int n) {
  if (n < 0 || n > 3) return -1;
  c.cpu().set_fpcr((c.cpu().fpcr() & ~(3u << 22)) | (static_cast<unsigned>(n) << 22));
  return 0;
}
int feclearexcept_impl(CallContext& c, int n) {
  c.cpu().set_fpsr(c.cpu().fpsr() & ~(n & all_exceptions));
  return 0;
}
int fetestexcept_impl(CallContext& c, int n) { return c.cpu().fpsr() & n & all_exceptions; }
int feraiseexcept_impl(CallContext& c, int n) {
  c.cpu().set_fpsr(c.cpu().fpsr() | (n & all_exceptions));
  return 0;
}
int fegetexceptflag_impl(CallContext& c, std::uint32_t* f, int n) {
  *f = c.cpu().fpsr() & n & all_exceptions;
  return 0;
}
int fesetexceptflag_impl(CallContext& c, const std::uint32_t* f, int n) {
  auto mask = n & all_exceptions;
  c.cpu().set_fpsr((c.cpu().fpsr() & ~mask) | (*f & mask));
  return 0;
}
int fegetenv_impl(CallContext& c, Fenv* env) {
  *env = {c.cpu().fpcr(), c.cpu().fpsr()};
  return 0;
}
int fesetenv_impl(CallContext& c, const Fenv* env) {
  if (h2g(env) == UINT64_MAX) env = &default_env;
  c.cpu().set_fpcr(env->control);
  c.cpu().set_fpsr(env->status);
  return 0;
}
int feholdexcept_impl(CallContext& c, Fenv* env) {
  fegetenv_impl(c, env);
  c.cpu().set_fpsr(c.cpu().fpsr() & ~all_exceptions);
  c.cpu().set_fpcr(c.cpu().fpcr() & ~(all_exceptions << 8));
  return 0;
}
int feupdateenv_impl(CallContext& c, const Fenv* env) {
  auto flags = c.cpu().fpsr() & all_exceptions;
  fesetenv_impl(c, env);
  return feraiseexcept_impl(c, flags);
}
int fegetexcept_impl(CallContext& c) { return (c.cpu().fpcr() >> 8) & all_exceptions; }
int fedisableexcept_impl(CallContext& c, int flags) {
  auto old = fegetexcept_impl(c);
  c.cpu().set_fpcr(c.cpu().fpcr() & ~((flags & all_exceptions) << 8));
  return old;
}
int feenableexcept_impl(CallContext& c, int flags) {
  auto old = fegetexcept_impl(c);
  c.cpu().set_fpcr(c.cpu().fpcr() | ((flags & all_exceptions) << 8));
  return old;
}
// bionic bits/fenv_arm.h: A32 fenv_t is the single FPSCR word.
int fegetenv32(CallContext& c, std::uint32_t* env) { *env = c.cpu().fpcr(); return 0; }
int fesetenv32(CallContext& c, const std::uint32_t* env) { c.cpu().set_fpcr(h2g(env) == UINT32_MAX ? 0 : *env); return 0; }
int feholdexcept32(CallContext& c, std::uint32_t* env) {
  *env = c.cpu().fpcr(); c.cpu().set_fpcr(*env & ~(all_exceptions | (all_exceptions << 8))); return 0;
}
int feupdateenv32(CallContext& c, const std::uint32_t* env) {
  auto flags = c.cpu().fpcr() & all_exceptions; fesetenv32(c, env); c.cpu().set_fpcr(c.cpu().fpcr() | flags); return 0;
}
template<class T> T nexttoward32(CallContext& c, T x, double y) { MathScope<true> scope(c); return std::nexttoward(x, static_cast<long double>(y)); }
}  // namespace
void register_libm(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
    abi32::adapt<&fegetenv32>("fegetenv", "libm.so"); abi32::adapt<&fesetenv32>("fesetenv", "libm.so");
    abi32::adapt<&feholdexcept32>("feholdexcept", "libm.so"); abi32::adapt<&feupdateenv32>("feupdateenv", "libm.so");
    abi32::adapt<&nexttoward32<double>>("nexttoward", "libm.so"); abi32::adapt<&nexttoward32<float>>("nexttowardf", "libm.so");
  }
  // Pure math keeps the guest MXCSR inside the A64 JIT. ldexp inspects its own
  // MXCSR flags, so it keeps the cleared host environment of InJitFp.
#define MX(n, e)                    \
  bind<&n##_impl<double>>(lib, #n, e); \
  bind<&n##_impl<float>>(lib, #n "f", e)
#define QX(n, e) \
  MX(n, e);      \
  if (process_config().isa() == GuestIsa::a32) bind<&n##_impl<double>>(lib, #n "l", e); \
  else bind(lib, #n "l", n##_quad, e)
#define M(n) MX(n, Execution::InJitGuestFp)
#define Q(n) QX(n, Execution::InJitGuestFp)
  Q(acos);
  Q(acosh);
  Q(asin);
  Q(asinh);
  Q(atan);
  Q(atanh);
  Q(cos);
  Q(cosh);
  Q(sin);
  Q(sinh);
  Q(tan);
  Q(tanh);
  Q(exp);
  Q(exp2);
  Q(expm1);
  Q(log);
  Q(log10);
  Q(log2);
  Q(log1p);
  Q(logb);
  Q(sqrt);
  Q(cbrt);
  Q(ceil);
  Q(floor);
  Q(trunc);
  Q(round);
  Q(rint);
  Q(nearbyint);
  Q(fabs);
  Q(tgamma);
  Q(lgamma);
  Q(erf);
  Q(erfc);
  Q(atan2);
  Q(pow);
  Q(fmod);
  Q(remainder);
  Q(hypot);
  Q(fmin);
  Q(fmax);
  Q(fdim);
  Q(copysign);
  Q(nextafter);
  Q(fma);
  QX(ldexp, Execution::InJitFp);
  Q(frexp);
  Q(modf);
  M(remquo);
  M(sincos);
  M(lgamma_r);
  M(lrint);
  M(lround);
  M(ilogb);
  M(isnan);
  M(isinf);
  M(finite);
  M(nan);
  MX(scalbln, Execution::InJitFp);
  bind<&lrint_impl<double>>(lib, "llrint");
  bind<&lrint_impl<float>>(lib, "llrintf");
  bind<&lround_impl<double>>(lib, "llround");
  bind<&lround_impl<float>>(lib, "llroundf");
  bind<&ldexp_impl<double>>(lib, "scalbn");
  bind<&ldexp_impl<float>>(lib, "scalbnf");
  if (process_config().isa() == GuestIsa::a32) bind<&ldexp_impl<double>>(lib, "scalbnl");
  else bind(lib, "scalbnl", ldexp_quad);
  extern void register_guest_math(HleLibrary&);
  register_guest_math(lib);
  bind(lib, "nexttoward", nexttoward_impl<double>);
  bind(lib, "nexttowardf", nexttoward_impl<float>);
  bind<&isnan_impl<float>>(lib, "__isnanf");
  bind<&isnan_impl<double>>(lib, "__isnan");
  bind<&finite_impl<float>>(lib, "__isfinitef");
  bind<&finite_impl<double>>(lib, "__isfinite");
#undef Q
#undef M
#undef QX
#undef MX
#define F(n) bind<&n##_impl>(lib, #n)
  F(fegetround);
  F(fesetround);
  F(feclearexcept);
  F(fetestexcept);
  F(feraiseexcept);
  F(fegetexceptflag);
  F(fesetexceptflag);
  F(fegetenv);
  F(fesetenv);
  F(feholdexcept);
  F(feupdateenv);
  F(fegetexcept);
  F(fedisableexcept);
  F(feenableexcept);
#undef F
  if (process_config().isa() == GuestIsa::a32) {
    bind<&lgamma_impl<double, true>>(lib, "lgamma");
    bind<&lgamma_impl<float, true>>(lib, "lgammaf");
    // A32 long double is double; a32-abi selects the matching descriptor.
    bind<&lgamma_impl<double, true>>(lib, "lgammal");
    const std::uint32_t default32 = 0;
    lib.add_data("__fe_dfl_env", low_data(std::as_bytes(std::span(&default32, 1))));
    lib.add_data("signgam", signgam32.resize(sizeof(int)));
  } else {
    lib.add_data("__fe_dfl_env", h2g(&default_env));
    lib.add_data("signgam", h2g(&signgam_value));
  }
}
}  // namespace venture::hle::libc
