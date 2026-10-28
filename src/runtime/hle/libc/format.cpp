#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include "hle/libc/libc_a.h"
namespace venture::hle::libc {
Vec128 PrintfArgs::next_quad() {
  fatal("printf adapter does not support binary128 arguments; use PrintArgs");
}
namespace {
enum class Kind { none, integer, real, quad };
struct Value {
  std::uint64_t integer = 0;
  double real = 0;
  Vec128 quad{};
};
struct Spec {
  const char* start = nullptr;
  const char* end = nullptr;
  bool left = false, plus = false, blank = false, alternate = false, zero = false;
  int width = 0, precision = -1, arg = 0, width_arg = 0, precision_arg = 0;
  std::string length;
  char conversion = 0;
};
struct FormatError {
  int error;
};
int decimal(const char*& p) {
  int n = 0;
  while (*p >= '0' && *p <= '9') {
    if (n > (INT_MAX - (*p - '0')) / 10) throw FormatError{err::kEOVERFLOW};
    n = n * 10 + (*p++ - '0');
  }
  return n;
}
int position(const char*& p) {
  auto old = p;
  int n = decimal(p);
  if (n && *p == '$') {
    ++p;
    return n;
  }
  p = old;
  return 0;
}
Spec parse(const char*& p, int& next) {
  Spec s;
  s.start = p++;
  s.arg = position(p);
  for (bool done = false; !done;) switch (*p) {
      case '-':
        s.left = true;
        ++p;
        break;
      case '+':
        s.plus = true;
        ++p;
        break;
      case ' ':
        s.blank = true;
        ++p;
        break;
      case '#':
        s.alternate = true;
        ++p;
        break;
      case '0':
        s.zero = true;
        ++p;
        break;
      case '\'':
        ++p;
        break;
      default:
        done = true;
        break;
    }
  if (*p == '*') {
    ++p;
    s.width_arg = position(p);
    if (!s.width_arg) s.width_arg = next++;
  } else
    s.width = decimal(p);
  if (*p == '.') {
    ++p;
    s.precision = 0;
    if (*p == '*') {
      ++p;
      s.precision_arg = position(p);
      if (!s.precision_arg) s.precision_arg = next++;
    } else
      s.precision = decimal(p);
  }
  if (*p && std::strchr("hljztLq", *p)) {
    s.length += *p++;
    if ((s.length == "h" && *p == 'h') || (s.length == "l" && *p == 'l')) s.length += *p++;
  }
  s.conversion = *p;
  if (*p) ++p;
  if (s.conversion && std::strchr("diouxXfFeEgGaAcCsSpnDUO", s.conversion) && !s.arg) s.arg = next++;
  if (s.conversion == 'D' || s.conversion == 'U' || s.conversion == 'O') {
    s.length = "l";
    s.conversion = s.conversion == 'D' ? 'd' : s.conversion == 'U' ? 'u' : 'o';
  }
  if (s.conversion == 'S' || s.conversion == 'C') {
    s.length = "l";
    s.conversion = s.conversion == 'S' ? 's' : 'c';
  }
  s.end = p;
  return s;
}
std::string digits(std::uint64_t n, int base, bool upper) {
  char b[65];
  auto r = std::to_chars(b, b + 65, n, base);
  std::string s(b, r.ptr);
  if (upper)
    for (char& c : s)
      if (c >= 'a' && c <= 'f') c -= 32;
  return s;
}
void store_count(GuestAddr p, const std::string& length, std::uint64_t count) {
  if (length == "hh")
    *g2h<std::int8_t>(p) = static_cast<std::int8_t>(count);
  else if (length == "h")
    *g2h<std::int16_t>(p) = static_cast<std::int16_t>(count);
  else if (length.empty())
    *g2h<std::int32_t>(p) = static_cast<std::int32_t>(count);
  else
    *g2h<std::int64_t>(p) = static_cast<std::int64_t>(count);
}
// to_chars rounds to nearest. For directed guest FPCR modes, first obtain the
// exact binary64 value (at most 1074 decimal places), then round its digits.
std::string directed_digits(double value, char conv, int precision, bool upward) {
  std::array<char, 1200> buffer{};
  const bool hex = conv == 'a';
  auto r = hex ? std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::hex)
               : std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                               std::chars_format::scientific, 1074);
  if (r.ec != std::errc{}) throw FormatError{err::kEOVERFLOW};
  std::string digits(buffer.data(), r.ptr);
  auto split = digits.find(hex ? 'p' : 'e');
  int exponent = 0;
  auto begin = digits.data() + split + 1;
  if (*begin == '+') ++begin;
  std::from_chars(begin, digits.data() + digits.size(), exponent);
  digits.resize(split);
  std::erase(digits, '.');
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  std::int64_t keep = conv == 'f'   ? std::int64_t(exponent) + 1 + precision
                      : conv == 'g' ? precision
                                    : std::int64_t(precision) + 1;
  if (keep < static_cast<std::int64_t>(digits.size()) && value != 0) {
    if (keep <= 0) {
      digits = upward ? "1" : "0";
      exponent = upward ? static_cast<int>(exponent - keep + 1) : 0;
    } else {
      digits.resize(static_cast<std::size_t>(keep));
      if (upward) {
        const char last = hex ? 'f' : '9';
        auto i = digits.size();
        while (i && digits[i - 1] == last) digits[--i] = '0';
        if (i) {
          auto& c = digits[i - 1];
          c = hex && c == '9' ? 'a' : c + 1;
        } else {
          digits.insert(0, 1, '1');
          ++exponent;  // Decimal carry; normalized binary64 hex begins with 1.
        }
      }
    }
  }
  const bool scientific = hex || conv == 'e' || (conv == 'g' && (exponent < -4 || exponent >= precision));
  const std::int64_t places =
      conv == 'g' ? (scientific ? precision - 1 : std::int64_t(precision) - exponent - 1) : precision;
  std::string result;
  if (scientific) {
    result += digits[0];
    if (places) {
      result += '.';
      result += digits.substr(1, static_cast<std::size_t>(places));
      result.append(static_cast<std::size_t>(places) + 2 - result.size(), '0');
    }
  } else {
    auto point = std::int64_t(exponent) + 1;
    if (point <= 0) {
      result = "0.";
      result.append(static_cast<std::size_t>(-point), '0');
      result += digits;
    } else {
      result = digits.substr(0, static_cast<std::size_t>(point));
      if (static_cast<std::int64_t>(result.size()) < point)
        result.append(static_cast<std::size_t>(point) - result.size(), '0');
      result += '.';
      if (static_cast<std::size_t>(point) < digits.size())
        result += digits.substr(static_cast<std::size_t>(point));
    }
    auto dot = result.find('.');
    result.resize(dot + 1 + static_cast<std::size_t>(places), '0');
    if (!places) result.resize(dot);
  }
  if (conv == 'g' && result.find('.') != std::string::npos) {
    while (result.back() == '0') result.pop_back();
    if (result.back() == '.') result.pop_back();
  }
  if (scientific) {
    result += hex ? 'p' : 'e';
    result += exponent < 0 ? '-' : '+';
    auto e = std::to_string(std::abs(exponent));
    if (!hex && e.size() < 2) result += '0';
    result += e;
  }
  return result;
}
std::string floating(double v, Spec s, bool negative) {
  bool upper = s.conversion >= 'A' && s.conversion <= 'Z';
  char conv = s.conversion | 32;
  if (std::isnan(v)) return upper ? "NAN" : "nan";
  if (std::isinf(v)) return upper ? "INF" : "inf";
  auto style = conv == 'f'   ? std::chars_format::fixed
               : conv == 'e' ? std::chars_format::scientific
               : conv == 'a' ? std::chars_format::hex
                             : std::chars_format::general;
  int precision = s.precision < 0 ? 6 : s.precision;
  if (conv == 'g' && !precision) precision = 1;
  // bionic gdtoa/hdtoa.c normalizes subnormals for %a, unlike the usual
  // glibc/UCRT 0x0.000...p-1022 spelling. Scaling by 2^52 is exact.
  int hex_adjust = 0;
  if (conv == 'a' && v != 0 && v < std::numeric_limits<double>::min()) {
    v = std::ldexp(v, 52);
    hex_adjust = -52;
  }
  std::vector<char> buf(static_cast<std::size_t>(precision) + 768);
  auto r = conv == 'a' && s.precision < 0
               ? std::to_chars(buf.data(), buf.data() + buf.size(), v, style)
               : std::to_chars(buf.data(), buf.data() + buf.size(), v, style, precision);
  if (r.ec != std::errc{}) throw FormatError{err::kEOVERFLOW};
  std::string result(buf.data(), r.ptr);
  auto thread = GuestThread::current();
  auto mode = thread ? (thread->cpu().fpcr() >> 22) & 3 : 0;
  if (mode && !(conv == 'a' && s.precision < 0))
    result = directed_digits(v, conv, precision, (mode == 1 && !negative) || (mode == 2 && negative));
  if (hex_adjust) {
    auto p = result.find('p');
    int e = 0;
    auto begin = result.data() + p + 1;
    if (*begin == '+') ++begin;
    std::from_chars(begin, result.data() + result.size(), e);
    e += hex_adjust;
    result.resize(p + 1);
    if (e >= 0) result += '+';
    result += std::to_string(e);
  }
  auto exponent = result.find_first_of("ep");
  auto end = exponent == std::string::npos ? result.size() : exponent;
  if (s.alternate) {
    auto dot = result.find('.');
    if (dot == std::string::npos) {
      result.insert(end++, 1, '.');
      dot = end - 1;
    }
    if (conv == 'g') {
      int significant = 0;
      bool nonzero = false;
      for (std::size_t i = 0; i < end; ++i)
        if (result[i] != '.') {
          if (result[i] != '0') nonzero = true;
          if (nonzero) ++significant;
        }
      if (!significant) significant = 1;
      if (precision > significant) result.insert(end, static_cast<std::size_t>(precision - significant), '0');
    }
  }
  if (upper)
    for (char& c : result)
      if (c >= 'a' && c <= 'z') c -= 32;
  return result;
}
void append(std::string& out, std::string_view value) {
  if (value.size() > INT_MAX - out.size()) throw FormatError{err::kEOVERFLOW};
  out += value;
}
void pad(std::string& out, std::uint64_t n, char c) {
  if (n > INT_MAX - out.size()) throw FormatError{err::kEOVERFLOW};
  out.append(static_cast<std::size_t>(n), c);
}
std::vector<std::uint32_t> wide_chars(std::string_view text) {
  std::vector<std::uint32_t> out;
  while (!text.empty()) {
    utf8proc_int32_t cp;
    auto n = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(text.data()), text.size(), &cp);
    if (n < 1) throw FormatError{err::kEILSEQ};
    out.push_back(static_cast<std::uint32_t>(cp));
    text.remove_prefix(n);
  }
  return out;
}
struct Slot32 { Kind kind = Kind::none; PrintfConversion conversion{}; };
template<bool A32 = false>
std::string engine(const char* fmt, PrintfArgs& args, bool wide_output = false) {
  std::vector<Spec> specs;
  using Slot = std::conditional_t<A32, Slot32, Kind>;
  std::vector<Slot> kinds(1);
  int next = 1;
  auto use = [&](int pos, Kind kind, PrintfConversion conversion = {}) {
    if (!pos) return;
    if (pos > 4096) throw FormatError{err::kEINVAL};
    if (kinds.size() <= static_cast<unsigned>(pos)) kinds.resize(pos + 1);
    if constexpr (A32) {
      auto& slot = kinds[pos];
      if (slot.kind != Kind::none && (slot.kind != kind ||
          slot.conversion.width != conversion.width || slot.conversion.pointer != conversion.pointer))
        throw FormatError{err::kEINVAL};
      slot = {kind, conversion};
    } else {
      if (kinds[pos] != Kind::none && kinds[pos] != kind) throw FormatError{err::kEINVAL};
      kinds[pos] = kind;
    }
  };
  for (auto p = fmt; *p;) {
    if (*p != '%') {
      ++p;
      continue;
    }
    auto s = parse(p, next);
    if constexpr (A32) {
      // bionic ARM ILP32: long/size_t/ptrdiff_t are words; long double is
      // binary64. Normalize only numeric lengths, retaining %ls/%lc.
      if (s.conversion && std::strchr("diouxXn", s.conversion) &&
          (s.length == "l" || s.length == "z" || s.length == "t")) s.length.clear();
      if (s.conversion && std::strchr("fFeEgGaA", s.conversion) && s.length == "L") s.length.clear();
    }
    use(s.width_arg, Kind::integer, {4, true, false});
    use(s.precision_arg, Kind::integer, {4, true, false});
    if (s.arg)
      use(s.arg, s.conversion && std::strchr("fFeEgGaA", s.conversion)
                     ? s.length == "L" ? Kind::quad : Kind::real
                     : Kind::integer,
          {s.length == "ll" || s.length == "q" || s.length == "j" ? 8u : 4u,
           s.conversion == 'd' || s.conversion == 'i' || s.conversion == 'c',
           s.conversion == 's' || s.conversion == 'p' || s.conversion == 'n'});
    specs.push_back(s);
  }
  std::vector<Value> values(kinds.size());
  auto kind_at = [&](std::size_t i) { if constexpr (A32) return kinds[i].kind; else return kinds[i]; };
  for (std::size_t i = 1; i < kinds.size(); ++i) switch (kind_at(i)) {
      case Kind::integer:
        if constexpr (A32) {
          auto conversion = kinds[i].conversion;
          if (conversion.pointer) conversion.width = 4;
          values[i].integer = args.next_integer(conversion);
        } else values[i].integer = args.next_int();
        break;
      case Kind::real:
        values[i].real = args.next_double();
        break;
      case Kind::quad:
        values[i].quad = args.next_quad();
        break;
      default:
        throw FormatError{err::kEINVAL};
    }
  std::string out;
  const char* last = fmt;
  for (auto s : specs) {
    append(out, std::string_view(last, s.start - last));
    last = s.end;
    if (s.width_arg) {
      auto n = static_cast<std::int32_t>(values[s.width_arg].integer);
      if (n == INT_MIN) throw FormatError{err::kEOVERFLOW};
      if (n < 0) {
        s.left = true;
        n = -n;
      }
      s.width = n;
    }
    if (s.precision_arg) s.precision = static_cast<std::int32_t>(values[s.precision_arg].integer);
    auto v = values[s.arg];
    std::string body, prefix;
    bool numeric = false;
    char c = s.conversion;
    if (!c) break;
    if (c == 'n') {
      auto count = wide_output ? wide_chars(out).size() : out.size();
      if constexpr (A32) {
        auto bytes = s.length == "hh" ? 1u : s.length == "h" ? 2u : s.length.empty() ? 4u : 8u;
        a32::write_bytes(v.integer, &count, bytes);
      } else store_count(v.integer, s.length, count);
      continue;
    }
    if (std::strchr("diouxXp", c)) {
      numeric = true;
      std::uint64_t magnitude = v.integer;
      bool neg = false;
      if (c == 'd' || c == 'i') {
        auto signed_value =
            s.length == "hh"   ? static_cast<std::int64_t>(static_cast<std::int8_t>(magnitude))
            : s.length == "h"  ? static_cast<std::int64_t>(static_cast<std::int16_t>(magnitude))
            : s.length.empty() ? static_cast<std::int64_t>(static_cast<std::int32_t>(magnitude))
                               : static_cast<std::int64_t>(magnitude);
        neg = signed_value < 0;
        magnitude =
            neg ? 0 - static_cast<std::uint64_t>(signed_value) : static_cast<std::uint64_t>(signed_value);
        prefix = neg ? "-" : s.plus ? "+" : s.blank ? " " : "";
      } else if (c != 'p') {
        if (s.length == "hh")
          magnitude = static_cast<std::uint8_t>(magnitude);
        else if (s.length == "h")
          magnitude = static_cast<std::uint16_t>(magnitude);
        else if (s.length.empty())
          magnitude = static_cast<std::uint32_t>(magnitude);
      }
      int base = c == 'o' ? 8 : (c == 'x' || c == 'X' || c == 'p') ? 16 : 10;
      body = (!magnitude && s.precision == 0 && c != 'p') ? "" : digits(magnitude, base, c == 'X');
      if (s.precision > static_cast<int>(body.size()))
        body.insert(0, static_cast<std::size_t>(s.precision) - body.size(), '0');
      if (c == 'p' || (s.alternate && magnitude && (c == 'x' || c == 'X'))) prefix = c == 'X' ? "0X" : "0x";
      if (s.alternate && c == 'o' && (body.empty() || body.front() != '0')) body.insert(0, 1, '0');
      if (s.precision >= 0) s.zero = false;
    } else if (std::strchr("fFeEgGaA", c)) {
      // Intentional precision limit for %L: marshal the complete binary128 value,
      // then format its binary64 approximation. strtold itself remains binary128.
      auto d = s.length == "L" ? quad_to_double(v.quad) : v.real;
      prefix = std::signbit(d) ? "-" : s.plus ? "+" : s.blank ? " " : "";
      numeric = std::isfinite(d);
      body = floating(std::abs(d), s, std::signbit(d));
      if ((c == 'a' || c == 'A') && numeric) prefix += c == 'A' ? "0X" : "0x";
    } else if (c == 's' || c == 'm') {
      if (c == 's' && s.length == "l" && v.integer) {
        std::size_t count = 0;
        for (auto p = g2h<std::uint32_t>(v.integer); *p; ++p) {
          char bytes[4];
          int n = encode_utf8(bytes, *p);
          if (n < 0) throw FormatError{err::kEILSEQ};
          if (s.precision >= 0 &&
              (wide_output ? count + 1 : body.size() + n) > static_cast<unsigned>(s.precision))
            break;
          body.append(bytes, n);
          ++count;
        }
      } else {
        auto p = c == 'm' ? error_string(*error_location()) : v.integer ? g2h<char>(v.integer) : "(null)";
        std::size_t n = 0;
        if (wide_output) {
          std::size_t count = 0;
          while (p[n] && (s.precision < 0 || count < static_cast<unsigned>(s.precision))) {
            utf8proc_int32_t cp;
            auto bytes = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(p + n), -1, &cp);
            if (bytes < 1) throw FormatError{err::kEILSEQ};
            n += bytes;
            ++count;
          }
        } else
          while ((s.precision < 0 || n < static_cast<unsigned>(s.precision)) && p[n]) ++n;
        body.assign(p, n);
      }
    } else if (c == 'c') {
      if (s.length == "l") {
        char b[4];
        int n = encode_utf8(b, static_cast<std::uint32_t>(v.integer));
        if (n < 0) throw FormatError{err::kEILSEQ};
        body.assign(b, n);
      } else
        body.push_back(static_cast<char>(v.integer));
    } else
      body.push_back(c);
    auto size = prefix.size() + (wide_output ? wide_chars(body).size() : body.size());
    auto padding = static_cast<std::size_t>(s.width) > size ? s.width - size : 0;
    bool zero = s.zero && !s.left && numeric;
    if (!s.left && !zero) pad(out, padding, ' ');
    append(out, prefix);
    if (zero) pad(out, padding, '0');
    append(out, body);
    if (s.left) pad(out, padding, ' ');
  }
  append(out, last);
  return out;
}
template <class Cursor>
int format_into(char* d, std::uint64_t n, const char* f, Cursor cursor) {
  try {
    PrintArgs<Cursor> args(cursor);
    auto text = engine<is_a32_cursor<Cursor>>(f, args);
    if (n) {
      auto count = std::min<std::uint64_t>(n - 1, text.size());
      std::memcpy(d, text.data(), count);
      d[count] = 0;
    }
    return static_cast<int>(text.size());
  } catch (const FormatError& e) {
    set_error(e.error);
    if (n) d[0] = 0;
    return -1;
  } catch (const std::bad_alloc&) {
    set_error(err::kENOMEM);
    if (n) d[0] = 0;
    return -1;
  } catch (const std::length_error&) {
    set_error(err::kEOVERFLOW);
    if (n) d[0] = 0;
    return -1;
  }
}
int snprintf_impl(char* d, std::uint64_t n, const char* f, VarArgs a) { return format_into(d, n, f, a); }
int vsnprintf_impl(char* d, std::uint64_t n, const char* f, VaList a) { return format_into(d, n, f, a); }
int sprintf_impl(char* d, const char* f, VarArgs a) { return format_into(d, UINT64_MAX, f, a); }
int vsprintf_impl(char* d, const char* f, VaList a) { return format_into(d, UINT64_MAX, f, a); }
template <class Cursor>
int wide_format_into(std::uint32_t* out, std::uint64_t limit, const std::uint32_t* format, Cursor cursor) {
  try {
    std::string utf8;
    for (auto p = format; *p; ++p) {
      char bytes[4];
      int n = encode_utf8(bytes, *p);
      if (n < 0) throw FormatError{err::kEILSEQ};
      utf8.append(bytes, n);
    }
    PrintArgs<Cursor> args(cursor);
    auto text = wide_chars(engine<is_a32_cursor<Cursor>>(utf8.c_str(), args, true));
    if (limit) {
      auto n = std::min<std::uint64_t>(text.size(), limit - 1);
      std::copy_n(text.data(), n, out);
      out[n] = 0;
    }
    if (text.size() >= limit) {
      set_error(err::kEOVERFLOW);
      return -1;
    }
    return static_cast<int>(text.size());
  } catch (const FormatError& e) {
    set_error(e.error);
    return -1;
  } catch (const std::bad_alloc&) {
    set_error(err::kENOMEM);
    return -1;
  }
}
int swprintf_impl(std::uint32_t* d, std::uint64_t n, const std::uint32_t* f, VarArgs a) {
  return wide_format_into(d, n, f, a);
}
int vswprintf_impl(std::uint32_t* d, std::uint64_t n, const std::uint32_t* f, VaList a) {
  return wide_format_into(d, n, f, a);
}
template <class Cursor>
int allocate_format(char** out, const char* f, Cursor cursor) {
  *out = nullptr;
  try {
    PrintArgs<Cursor> args(cursor);
    auto text = engine<is_a32_cursor<Cursor>>(f, args);
    auto p = static_cast<char*>(allocate(text.size() + 1));
    if (!p) return -1;
    std::memcpy(p, text.c_str(), text.size() + 1);
    *out = p;
    return static_cast<int>(text.size());
  } catch (const FormatError& e) {
    set_error(e.error);
    return -1;
  } catch (const std::bad_alloc&) {
    set_error(err::kENOMEM);
    return -1;
  } catch (const std::length_error&) {
    set_error(err::kEOVERFLOW);
    return -1;
  }
}
int asprintf_impl(char** d, const char* f, VarArgs a) { return allocate_format(d, f, a); }
int vasprintf_impl(char** d, const char* f, VaList a) { return allocate_format(d, f, a); }
int snprintf_chk(char* d, std::uint64_t n, int, std::uint64_t cap, const char* f, VarArgs a) {
  if (n > cap) fatal("FORTIFY: snprintf size exceeds destination");
  return snprintf_impl(d, n, f, a);
}
int vsnprintf_chk(char* d, std::uint64_t n, int, std::uint64_t cap, const char* f, VaList a) {
  if (n > cap) fatal("FORTIFY: vsnprintf size exceeds destination");
  return vsnprintf_impl(d, n, f, a);
}
int sprintf_chk(char* d, int, std::uint64_t cap, const char* f, VarArgs a) {
  auto n = format_into(d, cap, f, a);
  if (n >= 0 && static_cast<std::uint64_t>(n) >= cap) fatal("FORTIFY: sprintf overflow");
  return n;
}
int vsprintf_chk(char* d, int, std::uint64_t cap, const char* f, VaList a) {
  auto n = format_into(d, cap, f, a);
  if (n >= 0 && static_cast<std::uint64_t>(n) >= cap) fatal("FORTIFY: vsprintf overflow");
  return n;
}
template<class C> int sprintf32(char* d, const char* f, C a) { return format_into(d, UINT32_MAX, f, a); }
template<class C> int snprintf_chk32(char* d, std::uint64_t n, int, std::uint64_t cap, const char* f, C a) {
  if (n > cap) fatal("FORTIFY: snprintf size exceeds destination");
  return format_into(d, n, f, a);
}
template<class C> int sprintf_chk32(char* d, int, std::uint64_t cap, const char* f, C a) {
  auto n = format_into(d, cap, f, a);
  if (n >= 0 && static_cast<std::uint64_t>(n) >= cap) fatal("FORTIFY: sprintf overflow");
  return n;
}
}  // namespace
std::string format_printf(const char* format, PrintfArgs& args) {
  try {
    return engine(format, args);
  } catch (const FormatError& e) {
    set_error(e.error);
    throw PrintfError(e.error);
  } catch (const std::bad_alloc&) {
    set_error(err::kENOMEM);
    throw PrintfError(err::kENOMEM);
  } catch (const std::length_error&) {
    set_error(err::kEOVERFLOW);
    throw PrintfError(err::kEOVERFLOW);
  }
}
std::string format_printf32(const char* format, PrintfArgs& args) {
  try { return engine<true>(format, args); }
  catch (const FormatError& e) { set_error(e.error); throw PrintfError(e.error); }
  catch (const std::bad_alloc&) { set_error(err::kENOMEM); throw PrintfError(err::kENOMEM); }
  catch (const std::length_error&) { set_error(err::kEOVERFLOW); throw PrintfError(err::kEOVERFLOW); }
}
void register_formatting(HleLibrary& lib) {
#define B(n, f, c) bind_cursors<&n##_impl, &f<a32::c>>(lib, #n)
  B(snprintf, format_into, VarArgs32); B(vsnprintf, format_into, VaList32);
  B(sprintf, sprintf32, VarArgs32); B(vsprintf, sprintf32, VaList32);
  B(asprintf, allocate_format, VarArgs32); B(vasprintf, allocate_format, VaList32);
  B(swprintf, wide_format_into, VarArgs32); B(vswprintf, wide_format_into, VaList32);
#undef B
  bind_cursors<&snprintf_chk, &snprintf_chk32<a32::VarArgs32>>(lib, "__snprintf_chk");
  bind_cursors<&vsnprintf_chk, &snprintf_chk32<a32::VaList32>>(lib, "__vsnprintf_chk");
  bind_cursors<&sprintf_chk, &sprintf_chk32<a32::VarArgs32>>(lib, "__sprintf_chk");
  bind_cursors<&vsprintf_chk, &sprintf_chk32<a32::VaList32>>(lib, "__vsprintf_chk");
  extern void register_scan(HleLibrary&);
  register_scan(lib);
}
}  // namespace venture::hle::libc
