#include <doctest/doctest.h>

#include <cfenv>
#include <cerrno>
#include <climits>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

#include "hle/libc/libc_a.h"
#include "core/signals.h"
#include "core/signal_frame.h"
#include "libc_a_test_support.h"
using namespace venture;
using namespace venture::hle::libc;
#if defined(__x86_64__) || defined(_M_X64)
#endif
TEST_CASE("libc A null integer input delivers a recoverable guest SIGSEGV") {
  test::LibcFixture f;
  unsigned calls = 0;
  const auto handler = hle::make_thunk("test", "numeric segv", [](hle::CallContext& ctx) {
    ++*static_cast<unsigned*>(ctx.user());
    const auto* info = g2h<signals::SigInfo>(ctx.x(1));
    auto* uc = g2h<signals::UContext>(ctx.x(2));
    CHECK(ctx.x(0) == signals::kSigSegv);
    CHECK(info->signo == signals::kSigSegv);
    CHECK(uc->mcontext.fault_address == 0);
    // A signal handler can skip the faulting libc call, just as on Android.
    uc->mcontext.pc = uc->mcontext.regs[30];
    uc->mcontext.regs[0] = 42;
  }, &calls);
  signals::Action action{handler, 0, 4}, previous;
  REQUIRE(signals::set_action(signals::kSigSegv, &action, &previous) == 0);
  struct Restore {
    signals::Action old;
    ~Restore() { signals::set_action(signals::kSigSegv, &old, nullptr); }
  } restore{previous};
  auto* libc = Linker::get().open("libc.so", kRtldNow, nullptr);
  for (auto name : {"atoi", "atol", "atoll", "strtol", "strtoll", "strtoimax",
                    "strtoul", "strtoull", "strtoumax", "strtol_l", "strtoul_l"}) {
    CAPTURE(name);
    const auto address = libc->find_symbol(name);
    REQUIRE(address != 0);
    CHECK(f.thread.call(address, {0, 0, 10, 0}) == 42);
  }
  CHECK(calls == 11);
}
TEST_CASE("libc A LP64 numeric parsing, end pointers and Linux errno") {
  test::LibcFixture f;
  char* end = nullptr;
  const char* s = "  -9223372036854775808tail";
  CHECK(f.invoke("strtol", {h2g(s), h2g(&end), 10}) == (1ull << 63));
  CHECK(std::string(end) == "tail");
  CHECK(*error_location() == 0);
  CHECK(f.invoke("strtoul", {h2g("18446744073709551615!"), h2g(&end), 10}) == UINT64_MAX);
  CHECK(*end == '!');
  CHECK(f.invoke("strtoll", {h2g("9223372036854775808"), h2g(&end), 10}) == INT64_MAX);
  CHECK(*error_location() == hle::err::kERANGE);
  set_error(0);
  CHECK(f.invoke("strtoull", {h2g("-1"), h2g(&end), 10}) == UINT64_MAX);
  CHECK(*error_location() == 0);
  s = "0x";
  CHECK(f.invoke("strtol", {h2g(s), h2g(&end), 0}) == 0);
  CHECK(end == s + 1);
  s = " +!";
  CHECK(f.invoke("strtol", {h2g(s), h2g(&end), 0}) == 0);
  CHECK(end == s);
  CHECK(f.invoke("strtol", {h2g("1"), 0, 37}) == 0);
  CHECK(*error_location() == hle::err::kEINVAL);
  f.invoke("strtod", {h2g(" +0x1.8p+3rest"), h2g(&end)});
  CHECK(f.real() == 12);
  CHECK(std::string(end) == "rest");
  f.invoke("strtod", {h2g("-0.0"), 0});
  CHECK(std::signbit(f.real()));
  f.invoke("strtod", {h2g("1e9999"), 0});
  CHECK(std::isinf(f.real()));
  CHECK(*error_location() == hle::err::kERANGE);
  f.invoke("strtod", {h2g("1e-9999"), 0});
  CHECK(f.real() == 0);
  f.invoke("strtod", {h2g("NaN(payload)end"), h2g(&end)});
  CHECK(std::isnan(f.real()));
  CHECK(std::string(end) == "end");
  f.cpu.set_fpcr(1u << 22);
  f.invoke("strtod", {h2g("1.00000000000000000001"), 0});
  CHECK(f.real() == std::nextafter(1., 2.));
  f.cpu.set_fpcr(2u << 22);
  f.invoke("strtod", {h2g("-1.00000000000000000001"), 0});
  CHECK(f.real() == std::nextafter(-1., -2.));
  f.cpu.set_fpcr(0);
  f.invoke("div", {17, 5});
  CHECK(f.cpu.x(0) == ((2ull << 32) | 3));
  f.invoke("ldiv", {static_cast<std::uint64_t>(-17), 5});
  CHECK(f.cpu.x(0) == static_cast<std::uint64_t>(-3));
  CHECK(f.cpu.x(1) == static_cast<std::uint64_t>(-2));
}
TEST_CASE("libc A strtold binary128 exact rounding and Q0 return") {
  test::LibcFixture f;
  char* end = nullptr;
  f.invoke("strtold", {h2g("1.0000000000000000000000000000000002!"), h2g(&end)});
  CHECK(f.cpu.v(0).hi == 0x3fff000000000000ull);
  CHECK(f.cpu.v(0).lo == 1);
  CHECK(*end == '!');
  auto q = parse_quad("0.1", nullptr);
  CHECK(q.hi == 0x3ffb999999999999ull);
  CHECK(q.lo == 0x999999999999999aull);
  q = parse_quad("0x1.00000000000000000000000000008p0", nullptr);
  CHECK(q.hi == 0x3fff000000000000ull);
  CHECK(q.lo == 0);
  q = parse_quad("0x1.00000000000000000000000000018p0", nullptr);
  CHECK(q.lo == 2);
  q = parse_quad("0x1p-16494", nullptr);
  CHECK(q.hi == 0);
  CHECK(q.lo == 1);
  q = parse_quad("0x1p16383", nullptr);
  CHECK(q.hi == 0x7ffe000000000000ull);
  CHECK(q.lo == 0);
  q = parse_quad("-inf", nullptr);
  CHECK(q.hi == 0xffff000000000000ull);
  for (double d : {0., -0., 1., -1., 0.1, 1e300, 1e-300, std::numeric_limits<double>::denorm_min()})
    CHECK(std::bit_cast<std::uint64_t>(quad_to_double(double_to_quad(d))) == std::bit_cast<std::uint64_t>(d));
  std::uint32_t text[] = {'1', '2', '.', '5', 'x', 0};
  std::uint32_t* tail;
  f.invoke("wcstod", {h2g(text), h2g(&tail)});
  CHECK(f.real() == 12.5);
  CHECK(tail == text + 4);
}
TEST_CASE("libc A allocation alignment, overflow and realloc preservation") {
  test::LibcFixture f;
  auto p = f.invoke("malloc", {103});
  REQUIRE(p != 0);
  CHECK(p % 16 == 0);
  CHECK(f.invoke("malloc_usable_size", {p}) >= 103);
  std::memset(g2h(p), 0x5a, 103);
  auto bigger = f.invoke("realloc", {p, 1024 * 1024});
  REQUIRE(bigger != 0);
  CHECK(bigger % 16 == 0);
  CHECK(g2h<unsigned char>(bigger)[102] == 0x5a);
  f.invoke("free", {bigger});
  CHECK(f.invoke("calloc", {UINT64_MAX, 2}) == 0);
  CHECK(*error_location() == hle::err::kENOMEM);
  void* aligned = reinterpret_cast<void*>(0x1234);
  CHECK(f.invoke("posix_memalign", {h2g(&aligned), 24, 100}) == hle::err::kEINVAL);
  CHECK(h2g(aligned) == 0x1234);
  CHECK(f.invoke("posix_memalign", {h2g(&aligned), 4096, 100}) == 0);
  CHECK(h2g(aligned) % 4096 == 0);
  f.invoke("free", {h2g(aligned)});
  auto empty = f.invoke("calloc", {32, 8});
  REQUIRE(empty != 0);
  for (int i = 0; i < 256; ++i) CHECK(g2h<unsigned char>(empty)[i] == 0);
  f.invoke("free", {empty});
}
TEST_CASE("libc A math operates on guest FPCR and FPSR, including quad frexp") {
  test::LibcFixture f;
  f.invoke("fesetround", {1}, "libm.so");
  CHECK((f.cpu.fpcr() >> 22) == 1);
  f.real_arg(0, 1.25);
  f.invoke("rint", {}, "libm.so");
  CHECK(f.real() == 2);
  CHECK((f.cpu.fpsr() & 16) == 16);
  f.invoke("feclearexcept", {0x9f}, "libm.so");
  CHECK(f.cpu.fpsr() == 0);
  f.invoke("fesetround", {2}, "libm.so");
  f.real_arg(0, 1.75);
  f.invoke("nearbyint", {}, "libm.so");
  CHECK(f.real() == 1);
  CHECK(f.cpu.fpsr() == 0);
  f.invoke("fesetround", {0}, "libm.so");
  f.real_arg(0, -1);
  f.invoke("sqrt", {}, "libm.so");
  CHECK(std::isnan(f.real()));
  CHECK((f.cpu.fpsr() & 1) == 1);
  // SSE functions, including the ldexp family, use the guest rounding mode,
  // report guest flags and restore the host environment.
  std::fenv_t host_before, host_after;
  std::fegetenv(&host_before);
  f.invoke("feclearexcept", {0x9f}, "libm.so");
  f.invoke("fesetround", {1}, "libm.so");
  f.real_arg(0, 2);
  f.invoke("sqrt", {}, "libm.so");
  const double up = f.real();
  CHECK(f.cpu.fpsr() == 16);
  f.invoke("fesetround", {2}, "libm.so");
  f.real_arg(0, 2);
  f.invoke("sqrt", {}, "libm.so");
  CHECK(std::nextafter(f.real(), 2.0) == up);
  f.invoke("feclearexcept", {0x9f}, "libm.so");
  f.invoke("fesetround", {3}, "libm.so");
  f.real_arg(0, DBL_MAX);
  f.invoke("ldexp", {1}, "libm.so");
  CHECK(f.real() == DBL_MAX);
  CHECK(f.cpu.fpsr() == (4 | 16));
  f.invoke("feclearexcept", {0x9f}, "libm.so");
  f.invoke("fesetround", {0}, "libm.so");
  f.real_arg(0, DBL_MAX);
  f.invoke("ldexp", {1}, "libm.so");
  CHECK(std::isinf(f.real()));
  f.real_arg(0, 0.5);
  f.invoke("floor", {}, "libm.so");
  CHECK(f.real() == 0);
  CHECK(f.cpu.fpsr() == (4 | 16));
  f.invoke("feclearexcept", {0x9f}, "libm.so");
  std::fegetenv(&host_after);
  CHECK(std::memcmp(&host_before, &host_after, sizeof host_before) == 0);
  int sign = 0;
  f.real_arg(0, -0.0);
  f.invoke("lgamma_r", {h2g(&sign)}, "libm.so");
  CHECK(sign == -1);
  CHECK(std::isinf(f.real()));
  f.real_arg(0, -0.5);
  f.invoke("lgamma", {}, "libm.so");
  CHECK(*g2h<int>(f.data("signgam", "libm.so")) == -1);
  int exponent = 0;
  f.cpu.set_v(0, parse_quad("0x1p16383", nullptr));
  f.invoke("frexpl", {h2g(&exponent)}, "libm.so");
  CHECK(exponent == 16384);
  CHECK(f.cpu.v(0).hi == 0x3ffe000000000000ull);
  f.cpu.set_v(0, {std::bit_cast<std::uint32_t>(1.0f), 0});
  f.cpu.set_v(1, parse_quad("1.0000000000000000000000000000000002", nullptr));
  f.invoke("nexttowardf", {}, "libm.so");
  CHECK(std::bit_cast<float>(static_cast<std::uint32_t>(f.cpu.v(0).lo)) == std::nextafter(1.f, 2.f));
}
namespace {
template<class T> void check_scaling() {
  using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
  test::LibcFixture f;
  constexpr int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
  const T inputs[] = {T(0), -T(0), T(1), T(-1), T(1.5), T(-1.5),
      std::numeric_limits<T>::min(), std::numeric_limits<T>::max(),
      -std::numeric_limits<T>::max(), std::numeric_limits<T>::denorm_min(),
      T(3) * std::numeric_limits<T>::denorm_min(),
      std::nextafter(std::numeric_limits<T>::min(), T(0)),
      std::nextafter(T(2) * std::numeric_limits<T>::min(), T(0)),
      std::numeric_limits<T>::infinity(), -std::numeric_limits<T>::infinity(),
      std::numeric_limits<T>::quiet_NaN()};
  const int exponents[] = {INT_MIN, -2200, -1075, -1074, -1023, -1022, -150,
      -149, -127, -126, -54, -24, -1, 0, 1, 24, 53, 127, 128, 1023, 1024, 2200, INT_MAX};
  for (unsigned mode = 0; mode < 4; ++mode) {
    f.cpu.set_fpcr(mode << 22);
    for (T x : inputs) for (int e : exponents) {
      std::fenv_t saved;
      std::feholdexcept(&saved);
      std::fesetround(modes[mode]);
      // Keep the previous CRT implementation as an independent oracle.
      T (*volatile reference)(T, int) = static_cast<T(*)(T,int)>(&std::ldexp);
      const T expected = reference(x, e);
      const int flags = std::fetestexcept(FE_ALL_EXCEPT);
      std::fesetenv(&saved);
      const unsigned guest_flags = ((flags & FE_INVALID) ? 1 : 0) |
          ((flags & FE_OVERFLOW) ? 4 : 0) | ((flags & FE_UNDERFLOW) ? 8 : 0) |
          ((flags & FE_INEXACT) ? 16 : 0);
      for (const char* name : sizeof(T) == 4 ? std::initializer_list<const char*>{"ldexpf", "scalbnf", "scalblnf"}
                                          : std::initializer_list<const char*>{"ldexp", "scalbn", "scalbln"}) {
        CAPTURE(mode);
        CAPTURE(x);
        CAPTURE(e);
        CAPTURE(name);
        set_error(77);
        f.cpu.set_fpsr(2); // existing flags must survive
        f.cpu.set_v(0, {std::bit_cast<Bits>(x), 0});
        errno = E2BIG;
        f.invoke(name, {static_cast<std::uint64_t>(static_cast<std::int64_t>(e))}, "libm.so");
        CHECK(errno == E2BIG);
        CHECK(static_cast<Bits>(f.cpu.v(0).lo) == std::bit_cast<Bits>(expected));
        CHECK(f.cpu.fpsr() == (guest_flags | 2));
        // A64 ldexp and scalbn are guest routines (bionic: FP exceptions only,
        // never errno); scalbln keeps the host handler, which sets ERANGE.
        const bool host = std::string_view(name).starts_with("scalbln");
        CHECK(*error_location() == ((host && (guest_flags & (4 | 8))) ? hle::err::kERANGE : 77));
      }
    }
  }
}
}
TEST_CASE("libc A ldexp family matches the CRT in every rounding mode, with guest flags") {
  check_scaling<double>();
  check_scaling<float>();
}
