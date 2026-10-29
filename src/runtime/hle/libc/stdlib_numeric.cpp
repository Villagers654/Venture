#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "hle/libc/libc_a.h"
namespace venture::hle::libc {
namespace {
bool space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}
template <bool Signed>
auto integer(const char* text, char** end, int base)
    -> std::conditional_t<Signed, std::int64_t, std::uint64_t> {
  if (end) *end = const_cast<char*>(text);
  if (base && (base < 2 || base > 36)) {
    set_error(err::kEINVAL);
    return 0;
  }
  // Keep a guest's invalid input in the guest fault path, rather than letting
  // the host parser dereference it and terminate the runtime with a host AV.
  if (!text) throw_memory_range_fault(0, false);
  auto p = text;
  while (space(*p)) ++p;
  bool neg = *p == '-';
  if (*p == '+' || *p == '-') ++p;
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit(p[2]) < 16) {
    p += 2;
    base = 16;
  }
  if (!base) base = *p == '0' ? 8 : 10;
  auto start = p;
  std::uint64_t value = 0, limit = Signed ? (neg ? 1ull << 63 : INT64_MAX) : UINT64_MAX;
  bool overflow = false;
  for (int d; (d = digit(*p)) < base; ++p) {
    if (value > (limit - static_cast<unsigned>(d)) / static_cast<unsigned>(base))
      overflow = true;
    else
      value = value * base + d;
  }
  if (p == start) return 0;
  if (end) *end = const_cast<char*>(p);
  if (overflow) {
    set_error(err::kERANGE);
    return static_cast<std::conditional_t<Signed, std::int64_t, std::uint64_t>>(Signed && neg ? 1ull << 63
                                                                                              : limit);
  }
  return static_cast<std::conditional_t<Signed, std::int64_t, std::uint64_t>>(neg ? 0 - value : value);
}
bool starts(const char* p, const char* literal) {
  while (*literal) {
    if ((*p | 32) != *literal++) return false;
    if (!*p++) return false;
  }
  return true;
}
struct Number {
  const char* begin;
  const char* end;
  const char* original;
  bool negative = false, hex = false;
  int special = 0;  // 1 infinity, 2 NaN
};
Number number(const char* text) {
  Number n{text, text, text};
  auto p = text;
  while (space(*p)) ++p;
  n.negative = *p == '-';
  if (*p == '+' || *p == '-') ++p;
  n.begin = p;
  if (starts(p, "inf")) {
    n.special = 1;
    n.end = p + (starts(p, "infinity") ? 8 : 3);
    return n;
  }
  if (starts(p, "nan")) {
    n.special = 2;
    n.end = p + 3;
    if (p[3] == '(') {
      auto q = p + 4;
      while ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || *q == '_')
        ++q;
      if (*q == ')') n.end = q + 1;
    }
    return n;
  }
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
      (digit(p[2]) < 16 || (p[2] == '.' && digit(p[3]) < 16))) {
    n.hex = true;
    p += 2;
    n.begin = p;
  }
  bool any = false;
  while (digit(*p) < (n.hex ? 16 : 10)) {
    ++p;
    any = true;
  }
  if (*p == '.') {
    ++p;
    while (digit(*p) < (n.hex ? 16 : 10)) {
      ++p;
      any = true;
    }
  }
  if (!any) {
    n.end = text;
    return n;
  }
  if (*p == (n.hex ? 'p' : 'e') || *p == (n.hex ? 'P' : 'E')) {
    auto q = p + 1;
    if (*q == '+' || *q == '-') ++q;
    if (digit(*q) < 10) {
      p = q + 1;
      while (digit(*p) < 10) ++p;
    }
  }
  n.end = p;
  return n;
}
// A small unsigned integer used only by strtold. Exact rational conversion avoids
// losing binary128 precision through the Windows x87 long double representation.
struct Big {
  std::vector<std::uint32_t> w;
  Big(std::uint32_t n = 0) {
    if (n) w.push_back(n);
  }
  void trim() {
    while (!w.empty() && !w.back()) w.pop_back();
  }
  int bits() const {
    return w.empty() ? 0 : static_cast<int>((w.size() - 1) * 32 + 32 - std::countl_zero(w.back()));
  }
  void mul(std::uint32_t m) {
    std::uint64_t carry = 0;
    for (auto& x : w) {
      auto v = std::uint64_t(x) * m + carry;
      x = static_cast<std::uint32_t>(v);
      carry = v >> 32;
    }
    if (carry) w.push_back(static_cast<std::uint32_t>(carry));
    trim();
  }
  void add(std::uint32_t a) {
    std::uint64_t c = a;
    for (auto& x : w) {
      c += x;
      x = static_cast<std::uint32_t>(c);
      c >>= 32;
      if (!c) return;
    }
    if (c) w.push_back(static_cast<std::uint32_t>(c));
  }
  Big shifted(int n) const {
    Big r;
    if (w.empty()) return r;
    r.w.resize(w.size() + n / 32 + 1);
    std::uint64_t c = 0;
    for (std::size_t i = 0; i < w.size(); ++i) {
      auto v = (std::uint64_t(w[i]) << (n % 32)) | c;
      r.w[i + n / 32] = static_cast<std::uint32_t>(v);
      c = v >> 32;
    }
    r.w[w.size() + n / 32] = static_cast<std::uint32_t>(c);
    r.trim();
    return r;
  }
  int cmp(const Big& b) const {
    if (w.size() != b.w.size()) return w.size() < b.w.size() ? -1 : 1;
    for (std::size_t i = w.size(); i--;)
      if (w[i] != b.w[i]) return w[i] < b.w[i] ? -1 : 1;
    return 0;
  }
  void sub(const Big& b) {
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < w.size(); ++i) {
      auto v = std::uint64_t(w[i]) - (i < b.w.size() ? b.w[i] : 0) - borrow;
      w[i] = static_cast<std::uint32_t>(v);
      borrow = v >> 63;
    }
    trim();
  }
};
int rounding_mode() {
  auto thread = GuestThread::current();
  return thread ? static_cast<int>((thread->cpu().fpcr() >> 22) & 3) : 0;
}
bool away_from_zero(int mode, bool negative) { return (mode == 1 && !negative) || (mode == 2 && negative); }
Vec128 quad_overflow(bool negative, int mode) {
  set_error(err::kERANGE);
  auto sign = negative ? 1ull << 63 : 0;
  return mode == 0 || away_from_zero(mode, negative) ? Vec128{0, sign | 0x7fff000000000000ull}
                                                     : Vec128{UINT64_MAX, sign | 0x7ffeffffffffffffull};
}
Vec128 quad_number(const Number& n) {
  const auto sign = n.negative ? 1ull << 63 : 0;
  const int mode = rounding_mode();
  if (n.special) return {0, sign | 0x7fff000000000000ull | (n.special == 2 ? 0x800000000000ull : 0)};
  if (n.end == n.original) return {};
  std::string digits;
  bool fraction = false;
  int fractional = 0;
  auto p = n.begin;
  while (p < n.end && *p != (n.hex ? 'p' : 'e') && *p != (n.hex ? 'P' : 'E')) {
    if (*p == '.')
      fraction = true;
    else {
      digits.push_back(*p);
      if (fraction) ++fractional;
    }
    ++p;
  }
  int exponent = 0;
  if (p < n.end) {
    ++p;
    bool neg = *p == '-';
    if (*p == '+' || *p == '-') ++p;
    while (p < n.end) {
      exponent = std::min(1000000, exponent * 10 + (*p++ - '0'));
    }
    if (neg) exponent = -exponent;
  }
  exponent -= fractional * (n.hex ? 4 : 1);
  auto first = digits.find_first_not_of('0');
  if (first == std::string::npos) return {0, sign};
  digits.erase(0, first);
  while (digits.size() > 1 && digits.back() == '0') {
    digits.pop_back();
    exponent += n.hex ? 4 : 1;
  }
  const auto magnitude = static_cast<std::int64_t>(digits.size()) * (n.hex ? 4 : 1) + exponent;
  if (magnitude > (n.hex ? 16400 : 5000)) return quad_overflow(n.negative, mode);
  if (magnitude < (n.hex ? -16600 : -5100)) {
    set_error(err::kERANGE);
    return {away_from_zero(mode, n.negative) ? 1ull : 0ull, sign};
  }
  Big a, b(1);
  for (char c : digits) {
    a.mul(n.hex ? 16 : 10);
    a.add(digit(c));
  }
  if (n.hex) {
    if (exponent > 0)
      a = a.shifted(exponent);
    else
      b = b.shifted(-exponent);
  } else if (exponent >= 0) {
    for (int i = 0; i < exponent; ++i) a.mul(10);
  } else {
    for (int i = 0; i < -exponent; ++i) b.mul(10);
  }
  int e = a.bits() - b.bits();
  if (e >= 0 ? a.cmp(b.shifted(e)) < 0 : a.shifted(-e).cmp(b) < 0) --e;
  if (e > 16383) return quad_overflow(n.negative, mode);
  int unit = std::max(e - 112, -16494);
  if (unit >= 0)
    b = b.shifted(unit);
  else
    a = a.shifted(-unit);
  Vec128 q{};
  for (int i = a.bits() - b.bits(); i >= 0; --i) {
    auto d = b.shifted(i);
    if (a.cmp(d) >= 0) {
      a.sub(d);
      if (i < 64)
        q.lo |= 1ull << i;
      else
        q.hi |= 1ull << (i - 64);
    }
  }
  auto halfway = a.shifted(1).cmp(b);
  bool increment = mode == 0 ? (halfway > 0 || (halfway == 0 && (q.lo & 1)))
                             : away_from_zero(mode, n.negative) && !a.w.empty();
  if (increment) {
    if (++q.lo == 0) ++q.hi;
  }
  if (q.hi & (1ull << 49)) {
    q.lo = (q.lo >> 1) | (q.hi << 63);
    q.hi >>= 1;
    ++e;
  }
  if (e > 16383) return quad_overflow(n.negative, mode);
  int biased = (q.hi & (1ull << 48)) ? std::max(e, -16382) + 16383 : 0;
  if (!biased && !a.w.empty()) set_error(err::kERANGE);
  q.hi = (q.hi & 0xffffffffffffull) | (std::uint64_t(biased) << 48) | sign;
  return q;
}
template <class T>
T floating(const char* text, char** end) {
  auto n = number(text);
  if (end) *end = const_cast<char*>(n.end);
  if (n.end == text) return 0;
  T value = 0;
  if (n.special)
    value = n.special == 1 ? std::numeric_limits<T>::infinity() : std::numeric_limits<T>::quiet_NaN();
  else {
    auto mode = rounding_mode();
    if (mode) {
      // Directed rounding is transitive through a wider format when both use
      // the same direction. Adjust the nearest binary32/64 candidate using
      // the exact ordered binary128 representation, rather than host errno/fenv.
      auto q = quad_number(n);
      T result = static_cast<T>(quad_to_double(q));
      bool overflow = std::isinf(result);
      auto r = double_to_quad(result);
      auto compare = [](Vec128 a, Vec128 b) {
        if (!(a.hi << 1) && !a.lo && !(b.hi << 1) && !b.lo) return 0;
        bool sa = a.hi >> 63, sb = b.hi >> 63;
        if (sa != sb) return sa ? -1 : 1;
        int order = a.hi == b.hi ? (a.lo == b.lo ? 0 : a.lo < b.lo ? -1 : 1) : a.hi < b.hi ? -1 : 1;
        return sa ? -order : order;
      };
      int order = compare(r, q);
      bool up = mode == 1 || (mode == 3 && n.negative);
      if ((up && order < 0) || (!up && order > 0))
        result = std::nextafter(result, static_cast<T>(up ? INFINITY : -INFINITY));
      if (overflow || (result != 0 && std::abs(result) < std::numeric_limits<T>::min()) ||
          (result == 0 && (q.lo || (q.hi & 0x7fffffffffffffffull))))
        set_error(err::kERANGE);
      return result;
    }
    auto result =
        std::from_chars(n.begin, n.end, value, n.hex ? std::chars_format::hex : std::chars_format::general);
    if (result.ec == std::errc::result_out_of_range) {
      // The exact quad parser distinguishes overflow from underflow including hex input.
      auto q = quad_number(n);
      auto d = quad_to_double(q);
      value = static_cast<T>(std::abs(d));
      set_error(err::kERANGE);
    }
    if (value != 0 && std::abs(value) < std::numeric_limits<T>::min()) set_error(err::kERANGE);
  }
  return n.negative ? -value : value;
}
int atoi_impl(const char* s) { return static_cast<int>(integer<true>(s, nullptr, 10)); }
std::int64_t atol_impl(const char* s) { return integer<true>(s, nullptr, 10); }
double atof_impl(const char* s) { return floating<double>(s, nullptr); }
template <class T>
T abs_impl(T x) {
  return x < 0 ? -x : x;
}
void div_impl(CallContext& c) {
  auto a = static_cast<std::int32_t>(c.x(0)), b = static_cast<std::int32_t>(c.x(1));
  if (!b) fatal("div: zero divisor");
  c.return_x(static_cast<std::uint32_t>(a / b) | (std::uint64_t(static_cast<std::uint32_t>(a % b)) << 32));
}
void ldiv_impl(CallContext& c) {
  auto a = static_cast<std::int64_t>(c.x(0)), b = static_cast<std::int64_t>(c.x(1));
  if (!b) fatal("ldiv: zero divisor");
  c.return_x(a / b);
  c.return_x1(a % b);
}
void strtold_impl(CallContext& c) { c.cpu().set_v(0, parse_quad(g2h<char>(c.x(0)), g2h<char*>(c.x(1)))); }
template <class F>
auto wide_number(const std::uint32_t* s, std::uint32_t** end, F f) {
  std::string ascii;
  for (auto p = s; *p && *p < 128; ++p) ascii.push_back(static_cast<char>(*p));
  char* tail = nullptr;
  auto r = f(ascii.c_str(), &tail);
  if (end) *end = const_cast<std::uint32_t*>(s + (tail - ascii.c_str()));
  return r;
}
template <bool S>
auto wide_integer(const std::uint32_t* s, std::uint32_t** end, int base) {
  return wide_number(s, end, [&](auto a, auto b) { return integer<S>(a, b, base); });
}
template <class T>
T wide_float(const std::uint32_t* s, std::uint32_t** end) {
  return wide_number(s, end, floating<T>);
}
void wcstold_impl(CallContext& c) {
  auto v = wide_number(g2h<std::uint32_t>(c.x(0)), g2h<std::uint32_t*>(c.x(1)), parse_quad);
  c.cpu().set_v(0, v);
}
}  // namespace
Vec128 parse_quad(const char* text, char** end) {
  auto n = number(text);
  if (end) *end = const_cast<char*>(n.end);
  return quad_number(n);
}
std::int64_t parse_signed(const char* text, char** end, int base) { return integer<true>(text, end, base); }
std::uint64_t parse_unsigned(const char* text, char** end, int base) {
  return integer<false>(text, end, base);
}
double parse_double(const char* text, char** end) { return floating<double>(text, end); }
float parse_float(const char* text, char** end) { return floating<float>(text, end); }
Vec128 double_to_quad(double d) {
  auto bits = std::bit_cast<std::uint64_t>(d);
  auto sign = bits & 0x8000000000000000ull;
  auto exponent = (bits >> 52) & 2047;
  auto mantissa = bits & 0xfffffffffffffull;
  if (exponent == 2047) return {mantissa << 60, sign | 0x7fff000000000000ull | (mantissa >> 4)};
  if (!exponent && !mantissa) return {0, sign};
  int e = static_cast<int>(exponent) - 1023;
  if (!exponent) {
    e = -1022;
    while (!(mantissa & (1ull << 52))) {
      mantissa <<= 1;
      --e;
    }
    mantissa &= 0xfffffffffffffull;
  }
  return {mantissa << 60, sign | (std::uint64_t(e + 16383) << 48) | (mantissa >> 4)};
}
double quad_to_double(Vec128 q) {
  bool sign = q.hi >> 63;
  int e = static_cast<int>((q.hi >> 48) & 32767);
  auto frac = q.hi & 0xffffffffffffull;
  if (e == 32767)
    return std::copysign(
        (frac || q.lo) ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity(),
        sign ? -1. : 1.);
  if (!e && !frac && !q.lo) return sign ? -0. : 0.;
  // Round binary128 directly to binary64, including subnormals (ties to even).
  int unbiased = (e ? e : 1) - 16383;
  std::uint64_t hi = frac | (e ? 1ull << 48 : 0);
  if (unbiased > 1023) return sign ? -INFINITY : INFINITY;
  int shift = 60 + std::max(0, -1022 - unbiased);
  std::uint64_t m = 0;
  bool half = false, sticky = false;
  if (shift < 64) {
    m = (hi << (64 - shift)) | (q.lo >> shift);
    half = (q.lo >> (shift - 1)) & 1;
    sticky = (q.lo & ((1ull << (shift - 1)) - 1)) != 0;
  } else if (shift < 128) {
    auto s = shift - 64;
    m = hi >> s;
    if (s) {
      half = (hi >> (s - 1)) & 1;
      sticky = q.lo || (hi & ((1ull << (s - 1)) - 1));
    } else {
      half = q.lo >> 63;
      sticky = (q.lo & INT64_MAX) != 0;
    }
  }
  if (half && (sticky || (m & 1))) ++m;
  double r = std::ldexp(static_cast<double>(m), std::max(unbiased, -1022) - 52);
  return sign ? -r : r;
}
void register_numeric(HleLibrary& lib) {
  for (auto n : {"strtol", "strtoll", "strtoimax", "strtol_l", "strtoll_l", "strtoimax_l"})
    bind<&integer<true>>(lib, n);
  for (auto n : {"strtoul", "strtoull", "strtoumax", "strtoul_l", "strtoull_l", "strtoumax_l"})
    bind<&integer<false>>(lib, n);
  for (auto n : {"strtod", "strtod_l"}) bind<&floating<double>>(lib, n);
  for (auto n : {"strtof", "strtof_l"}) bind<&floating<float>>(lib, n);
  bind(lib, "strtold", strtold_impl);
  bind(lib, "strtold_l", strtold_impl);
  for (auto n : {"wcstol", "wcstoll", "wcstoimax", "wcstol_l", "wcstoll_l"})
    bind<&wide_integer<true>>(lib, n);
  for (auto n : {"wcstoul", "wcstoull", "wcstoumax", "wcstoul_l", "wcstoull_l"})
    bind<&wide_integer<false>>(lib, n);
  bind<&wide_float<double>>(lib, "wcstod");
  bind<&wide_float<double>>(lib, "wcstod_l");
  bind<&wide_float<float>>(lib, "wcstof");
  bind<&wide_float<float>>(lib, "wcstof_l");
  bind(lib, "wcstold", wcstold_impl);
  bind(lib, "wcstold_l", wcstold_impl);
  bind<&atoi_impl>(lib, "atoi");
  bind<&atol_impl>(lib, "atol");
  bind<&atol_impl>(lib, "atoll");
  bind<&atof_impl>(lib, "atof");
  bind<&abs_impl<int>>(lib, "abs");
  bind<&abs_impl<std::int64_t>>(lib, "labs");
  bind<&abs_impl<std::int64_t>>(lib, "llabs");
  bind(lib, "div", div_impl);
  bind(lib, "ldiv", ldiv_impl);
  bind(lib, "lldiv", ldiv_impl);
  bind(lib, "imaxdiv", ldiv_impl);
  bind<&abs_impl<std::int64_t>>(lib, "imaxabs");
}
}  // namespace venture::hle::libc
