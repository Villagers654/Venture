#include <utf8proc.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "hle/libc/libc_a.h"
namespace venture::hle::libc {
namespace {
bool ws(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
void store_integer(void* out, const std::string& length, std::uint64_t value) {
  if (length == "hh")
    *static_cast<std::uint8_t*>(out) = static_cast<std::uint8_t>(value);
  else if (length == "h")
    *static_cast<std::uint16_t*>(out) = static_cast<std::uint16_t>(value);
  else if (length.empty())
    *static_cast<std::uint32_t*>(out) = static_cast<std::uint32_t>(value);
  else
    *static_cast<std::uint64_t*>(out) = value;
}
template <class Cursor>
int scan(const char* input, const char* format, Cursor args) {
  const char* start = input;
  int assigned = 0;
  std::vector<void*> pointers;
  auto destination = [&](unsigned pos) -> void* {
    if (pos > 4096) {
      set_error(err::kEINVAL);
      return nullptr;
    }
    while (pointers.size() < pos) pointers.push_back(args.template next<void*>());
    return pointers[pos - 1];
  };
  unsigned sequential = 1;
  auto fail = [&](bool eof) { return eof && !assigned ? -1 : assigned; };
  auto store = [&](void* out, std::string length, std::uint64_t bits) {
    if constexpr (is_a32_cursor<Cursor>) {
      auto bytes = length == "hh" ? 1u : length == "h" ? 2u :
          length == "ll" || length == "q" || length == "j" ? 8u : 4u;
      a32::write_bytes(h2g(out), &bits, bytes);
    } else store_integer(out, length, bits);
  };
  auto store_pointer = [&](void* out, void* pointer) {
    if constexpr (is_a32_cursor<Cursor>) {
      auto word = a32::narrow_out({a32::Kind::OutPtr}, h2g(pointer));
      a32::write_bytes(h2g(out), &word, 4);
    } else *static_cast<void**>(out) = pointer;
  };
  while (*format) {
    if (ws(*format)) {
      while (ws(*format)) ++format;
      while (ws(*input)) ++input;
      continue;
    }
    if (*format != '%') {
      if (*input != *format) return fail(!*input);
      ++format;
      ++input;
      continue;
    }
    ++format;
    if (*format == '%') {
      if (*input != '%') return fail(!*input);
      ++input;
      ++format;
      continue;
    }
    auto before = format;
    unsigned pos = 0;
    while (*format >= '0' && *format <= '9') {
      pos = std::min(100000u, pos * 10 + *format - '0');
      ++format;
    }
    if (*format == '$')
      ++format;
    else {
      format = before;
      pos = 0;
    }
    bool suppress = *format == '*';
    if (suppress) ++format;
    std::uint64_t width = 0;
    while (*format >= '0' && *format <= '9') {
      width = std::min<std::uint64_t>(INT_MAX, width * 10 + *format - '0');
      ++format;
    }
    bool alloc = *format == 'm';
    if (alloc) ++format;
    std::string length;
    if (*format && std::strchr("hljztLq", *format)) {
      length += *format++;
      if ((length == "h" && *format == 'h') || (length == "l" && *format == 'l')) length += *format++;
    }
    char conversion = *format++;
    if (!conversion) return assigned;
    std::array<bool, 256> accepted{};
    bool inverse = false;
    if (conversion == '[') {
      if (*format == '^') {
        inverse = true;
        ++format;
      }
      if (*format == ']') {
        accepted[']'] = true;
        ++format;
      }
      while (*format && *format != ']') {
        unsigned char a = *format++;
        accepted[a] = true;
        if (*format == '-' && format[1] && format[1] != ']') {
          ++format;
          unsigned char b = *format++;
          if (a <= b)
            for (unsigned c = a; c <= b; ++c) accepted[c] = true;
          else {
            accepted['-'] = true;
            accepted[b] = true;
          }
        }
      }
      if (*format != ']') {
        set_error(err::kEINVAL);
        return assigned;
      }
      ++format;
    }
    void* out = nullptr;
    if (!suppress) {
      out = destination(pos ? pos : sequential++);
      if (!out) return assigned;
    }
    if (conversion == 'n') {
      if (!suppress) store(out, length, input - start);
      continue;
    }
    if (conversion != 'c' && conversion != '[')
      while (ws(*input)) ++input;
    if (!*input) return fail(true);
    if (!width) width = conversion == 'c' ? 1 : UINT64_MAX;
    if (conversion == 'c' || conversion == 's' || conversion == '[') {
      std::vector<char> bytes;
      std::vector<std::uint32_t> wide;
      auto p = input;
      std::uint64_t count = 0;
      while (*p && count < width) {
        if (conversion == 's' && ws(*p)) break;
        if (conversion == '[' && (accepted[static_cast<unsigned char>(*p)] == inverse)) break;
        if (length == "l") {
          utf8proc_int32_t cp;
          auto n = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(p), -1, &cp);
          if (n <= 0) {
            set_error(err::kEILSEQ);
            return fail(true);
          }
          wide.push_back(static_cast<std::uint32_t>(cp));
          p += n;
        } else
          bytes.push_back(*p++);
        ++count;
      }
      // bionic accepts a short final %c field when at least one byte was read.
      if (!count) return fail(!*p);
      if (!suppress) {
        if (length == "l") {
          if (conversion != 'c') wide.push_back(0);
          auto d = out;
          if (alloc) {
            d = allocate(wide.size() * 4);
            if (!d) return fail(false);
            store_pointer(out, d);
          }
          std::memcpy(d, wide.data(), wide.size() * 4);
        } else {
          if (conversion != 'c') bytes.push_back(0);
          auto d = out;
          if (alloc) {
            d = allocate(bytes.size());
            if (!d) return fail(false);
            store_pointer(out, d);
          }
          std::memcpy(d, bytes.data(), bytes.size());
        }
        ++assigned;
      }
      input = p;
      continue;
    }
    std::string field;
    auto p = input;
    while (*p && !ws(*p) && field.size() < width) field.push_back(*p++);
    char* end = nullptr;
    if (std::strchr("diouxXp", conversion)) {
      int base = conversion == 'i'                                             ? 0
                 : conversion == 'o'                                           ? 8
                 : conversion == 'x' || conversion == 'X' || conversion == 'p' ? 16
                                                                               : 10;
      std::uint64_t value = conversion == 'd' || conversion == 'i'
                                ? static_cast<std::uint64_t>(parse_signed(field.c_str(), &end, base))
                                : parse_unsigned(field.c_str(), &end, base);
      if (end == field.c_str()) return fail(false);
      if (!suppress) {
        store(out, conversion == 'p' ? "l" : length, value);
        ++assigned;
      }
    } else if (std::strchr("fFeEgGaA", conversion)) {
      if (length == "L" && !is_a32_cursor<Cursor>) {
        auto value = parse_quad(field.c_str(), &end);
        if (end == field.c_str()) return fail(false);
        if (!suppress) std::memcpy(out, &value, 16);
      } else if (length == "l" || (is_a32_cursor<Cursor> && length == "L")) {
        auto value = parse_double(field.c_str(), &end);
        if (end == field.c_str()) return fail(false);
        if (!suppress) *static_cast<double*>(out) = value;
      } else {
        auto value = parse_float(field.c_str(), &end);
        if (end == field.c_str()) return fail(false);
        if (!suppress) *static_cast<float*>(out) = value;
      }
      if (!suppress) ++assigned;
    } else {
      set_error(err::kEINVAL);
      return assigned;
    }
    input += end - field.c_str();
  }
  return assigned;
}
int sscanf_impl(const char* input, const char* format, VarArgs args) { return scan(input, format, args); }
int vsscanf_impl(const char* input, const char* format, VaList args) { return scan(input, format, args); }
}  // namespace
void register_scan(HleLibrary& lib) {
  bind_cursors<&sscanf_impl, &scan<a32::VarArgs32>>(lib, "sscanf");
  bind_cursors<&vsscanf_impl, &scan<a32::VaList32>>(lib, "vsscanf");
}
}  // namespace venture::hle::libc
