#pragma once
#include <charconv>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace venture {
using Json = nlohmann::json;
// Never let parser diagnostics (which can contain credentials) cross an API
// boundary.
inline Json parse_json(std::string_view text) {
  auto j = Json::parse(text, nullptr, false);
  if (j.is_discarded())
    throw std::runtime_error("Invalid JSON response or file.");
  return j;
}
inline const Json &field(const Json &j, std::string_view key) {
  static const Json empty;
  auto it = j.is_object() ? j.find(std::string(key)) : j.end();
  return it == j.end() ? empty : *it;
}
inline std::string str(const Json &j) {
  if (j.is_string())
    return j.get<std::string>();
  if (j.is_number_integer())
    return j.dump();
  return {};
}
inline std::string str(const Json &j, std::string_view key) {
  return str(field(j, key));
}
inline uint64_t number(const Json &j) {
  auto s = str(j);
  uint64_t n = 0;
  auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
  return e == std::errc{} && p == s.data() + s.size() ? n : 0;
}
inline uint64_t number(const Json &j, std::string_view key) {
  return number(field(j, key));
}
inline bool digits(std::string_view s, size_t minimum = 1,
                   size_t maximum = 30) {
  return s.size() >= minimum && s.size() <= maximum &&
         s.find_first_not_of("0123456789") == s.npos;
}
} // namespace venture
