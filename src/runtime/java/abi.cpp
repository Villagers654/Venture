#include "java/abi.h"
#include "java/jni32.h"

#include <bit>
#include <algorithm>
#include <stdexcept>

namespace venture::java {
namespace {
Type parse_type(std::string_view s, std::size_t& i, bool allow_void) {
  if (i >= s.size()) throw std::invalid_argument("truncated JNI descriptor");
  char c = s[i++];
  if (c == '[') {
    unsigned dimensions = 1;
    while (i < s.size() && s[i] == '[') {
      ++i;
      ++dimensions;
    }
    if (dimensions > 255) throw std::invalid_argument("too many JNI array dimensions");
    parse_type(s, i, false);
    return Type::Object;
  }
  if (c == 'L') {
    auto end = s.find(';', i);
    if (end == s.npos || end == i || s.substr(i, end - i).find_first_of(".[();") != s.npos)
      throw std::invalid_argument("invalid JNI class descriptor");
    i = end + 1;
    return Type::Object;
  }
  if (c == 'V' && allow_void) return Type::Void;
  if (std::string_view("ZBCSIJFD").find(c) != std::string_view::npos) return static_cast<Type>(c);
  throw std::invalid_argument("invalid JNI type");
}
template <class Cursor>
jvalue next_value(Type t, Cursor& cursor) {
  jvalue v{};
  switch (t) {
    case Type::Boolean:
      v.z = cursor.template next<jboolean>();
      break;
    case Type::Byte:
      v.b = cursor.template next<jbyte>();
      break;
    case Type::Char:
      v.c = cursor.template next<jchar>();
      break;
    case Type::Short:
      v.s = cursor.template next<jshort>();
      break;
    case Type::Int:
      v.i = cursor.template next<jint>();
      break;
    case Type::Long:
      v.j = cursor.template next<jlong>();
      break;
    case Type::Float:
      v.f = cursor.template next<jfloat>();
      break;
    case Type::Double:
      v.d = cursor.template next<jdouble>();
      break;
    case Type::Object:
      v.l = cursor.template next<jobject>();
      break;
    default:
      throw std::invalid_argument("void JNI parameter");
  }
  return v;
}
bool fp(Type t) { return t == Type::Float || t == Type::Double; }
std::uint64_t narrow(Type t, std::uint64_t v) {
  switch (t) {
    case Type::Boolean:
      return static_cast<std::uint8_t>(v);
    case Type::Byte:
      return static_cast<std::int8_t>(v);
    case Type::Char:
      return static_cast<std::uint16_t>(v);
    case Type::Short:
      return static_cast<std::int16_t>(v);
    case Type::Int:
      return static_cast<std::int32_t>(v);
    case Type::Float:
      return static_cast<std::uint32_t>(v);
    case Type::Void:
      return 0;
    default:
      return v;
  }
}
}  // namespace
Descriptor Descriptor::parse(std::string_view s) {
  Descriptor result{};
  if (s.empty() || s[0] != '(') throw std::invalid_argument("JNI method descriptor must start with (");
  std::size_t i = 1;
  unsigned slots = 0;
  while (i < s.size() && s[i] != ')') {
    Type t = parse_type(s, i, false);
    result.parameters.push_back(t);
    slots += (t == Type::Long || t == Type::Double) ? 2 : 1;
    if (slots > 255) throw std::invalid_argument("JNI descriptor exceeds JVM parameter slots");
  }
  if (i >= s.size() || s[i++] != ')') throw std::invalid_argument("unterminated JNI method descriptor");
  result.result = parse_type(s, i, true);
  if (i != s.size()) throw std::invalid_argument("trailing JNI descriptor bytes");
  return result;
}
std::vector<jvalue> read_arguments(const Descriptor& d, hle::ArgCursor& cursor, ArgumentForm form) {
  std::vector<jvalue> result(d.parameters.size());
  if (form == ArgumentForm::Array) {
    auto address = cursor.next_gpr();
    if (!result.empty()) {
      if (!address) throw std::invalid_argument("null JNI jvalue array");
      std::memcpy(result.data(), g2h(address), result.size() * sizeof(jvalue));
    }
  } else if (form == ArgumentForm::VaList) {
    hle::VaList list(cursor.next_gpr());
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = next_value(d.parameters[i], list);
  } else {
    hle::VarArgs list(cursor);
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = next_value(d.parameters[i], list);
  }
  return result;
}
std::vector<jvalue> read_arguments32(const Descriptor& d, hle::a32::ArgCursor32& cursor, ArgumentForm form) {
  std::vector<jvalue> result(d.parameters.size());
  if (form == ArgumentForm::Array) {
    auto address = cursor.word();
    if (!result.empty()) hle::a32::read_bytes(address, result.data(), result.size() * sizeof(jvalue));
  } else if (form == ArgumentForm::VaList) {
    hle::a32::VaList32 list(cursor.word());
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = next_value(d.parameters[i], list);
  } else {
    hle::a32::VarArgs32 list(cursor);
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = next_value(d.parameters[i], list);
  }
  for (std::size_t i = 0; i < result.size(); ++i)
    if (d.parameters[i] == Type::Object) {
      // Only the first four bytes of the union are jobject; upper bytes may
      // contain arbitrary padding in an NDK-compiled jvalue array.
      std::uint32_t token;
      std::memcpy(&token, &result[i], 4);
      result[i].l = jni32::object(token);
    }
  return result;
}
std::string mangle(std::u16string_view s) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (char16_t c : s) {
    if ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9'))
      out += static_cast<char>(c);
    else if (c == u'/')
      out += '_';
    else if (c == u'_')
      out += "_1";
    else if (c == u';')
      out += "_2";
    else if (c == u'[')
      out += "_3";
    else {
      out += "_0";
      for (int shift : {12, 8, 4, 0}) out += hex[(c >> shift) & 15];
    }
  }
  return out;
}
std::u16string unescape(std::string_view s) {
  std::u16string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '_') {
      if (static_cast<unsigned char>(s[i]) > 127) throw std::invalid_argument("non-ASCII JNI symbol");
      out += s[i];
      continue;
    }
    if (i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '3') {
      char c = s[++i];
      if (c == '1')
        out += u'_';
      else if (c == '2')
        out += u';';
      else if (c == '3')
        out += u'[';
      else {
        unsigned value = 0;
        for (int n = 0; n < 4; ++n) {
          if (++i >= s.size()) throw std::invalid_argument("truncated JNI Unicode escape");
          auto digit = std::string_view("0123456789abcdef").find(s[i]);
          if (digit == std::string_view::npos) throw std::invalid_argument("invalid JNI Unicode escape");
          value = value * 16 + static_cast<unsigned>(digit);
        }
        out += static_cast<char16_t>(value);
      }
    } else
      out += u'/';
  }
  return out;
}
NativeName demangle(std::string_view symbol) {
  if (!symbol.starts_with("Java_")) throw std::invalid_argument("not a JNI symbol");
  auto s = symbol.substr(5);
  std::size_t separator = s.npos, overload = s.npos;
  std::size_t previous_raw = s.npos;
  for (std::size_t i = 0; i < s.size(); ++i)
    if (s[i] == '_') {
      if (i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '3') {
        i += s[i + 1] == '0' ? 5 : 1;
        continue;
      }
      // Two adjacent *raw* separators delimit an overload. A separator followed
      // by _1 is instead a class/method whose first character is underscore.
      if (previous_raw != s.npos && previous_raw + 1 == i) {
        overload = previous_raw;
        break;
      }
      separator = previous_raw;
      previous_raw = i;
    }
  if (overload == s.npos) separator = previous_raw;
  if (separator == s.npos || !separator)
    throw std::invalid_argument("JNI symbol missing class/method separator");
  NativeName n{unescape(s.substr(0, separator)),
               unescape(s.substr(separator + 1, (overload == s.npos ? s.size() : overload) - separator - 1)),
               {}};
  if (n.method.empty()) throw std::invalid_argument("empty JNI method");
  if (overload != s.npos) {
    n.parameters = unescape(s.substr(overload + 2));
    Descriptor::parse("(" + to_modified_utf8(*n.parameters) + ")V");
  }
  return n;
}
std::string native_name(std::u16string_view owner, std::u16string_view method,
                        std::optional<std::u16string_view> parameters) {
  return "Java_" + mangle(owner) + "_" + mangle(method) + (parameters ? "__" + mangle(*parameters) : "");
}
std::u16string from_modified_utf8(std::string_view s) {
  std::u16string out;
  for (std::size_t i = 0; i < s.size();) {
    unsigned c = static_cast<unsigned char>(s[i++]), n = 0, value = c;
    if ((c & 0xe0) == 0xc0) {
      n = 1;
      value = c & 31;
    } else if ((c & 0xf0) == 0xe0) {
      n = 2;
      value = c & 15;
    } else if (c > 127)
      throw std::invalid_argument("invalid modified UTF-8");
    for (unsigned j = 0; j < n; ++j) {
      if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xc0) != 0x80)
        throw std::invalid_argument("truncated modified UTF-8");
      value = value * 64 + (static_cast<unsigned char>(s[i++]) & 63);
    }
    out += static_cast<char16_t>(value);
  }
  return out;
}
std::string to_modified_utf8(std::u16string_view s) {
  std::string out;
  for (unsigned c : s) {
    if (c && c < 128)
      out += static_cast<char>(c);
    else if (c < 2048) {
      out += static_cast<char>(0xc0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 63));
    } else {
      out += static_cast<char>(0xe0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 63));
      out += static_cast<char>(0x80 | (c & 63));
    }
  }
  return out;
}
NativeArguments native_arguments(const Descriptor& d, const HostArguments& host, HostAbi abi, GuestAddr env) {
  NativeArguments out;
  out.integers = {env, host.gp[1]};
  unsigned gp = 2, simd = 0, position = 2, stack = 0;
  for (Type t : d.parameters) {
    std::uint64_t value;
    if (abi == HostAbi::Win64) {
      value = position < 4 ? (fp(t) ? host.fp[position] : host.gp[position]) : host.stack[position - 4];
      ++position;
    } else if (fp(t))
      value = simd < 8 ? host.fp[simd++] : host.stack[stack++];
    else
      value = gp < 6 ? host.gp[gp++] : host.stack[stack++];
    value = narrow(t, value);
    if (fp(t) && out.doubles.size() < 8)
      out.doubles.push_back(std::bit_cast<double>(value));
    else if (!fp(t) && out.integers.size() < 8)
      out.integers.push_back(value);
    else
      out.stack.push_back(value);
  }
  return out;
}
std::uint64_t native_result(Type t, const GuestCallResult& r) {
  if (r.isa == GuestIsa::a32) return narrow(t, t == Type::Long || t == Type::Double ? r.u64() : r.x0);
  return narrow(t, fp(t) ? r.v0.lo : r.x0);
}
}  // namespace venture::java
