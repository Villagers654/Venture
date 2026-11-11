// SPDX-License-Identifier: GPL-3.0-or-later
// Based on the RVR-Venture GLES flat-varying workaround.
#pragma once
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace venture::gfx {
namespace flat_varyings_detail {
inline bool blank(char c) { return c == ' ' || c == '\t'; }
inline bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
inline bool integer_type(std::string_view type) {
  if (type == "int" || type == "uint" || type == "bool") return true;
  return type.size() == 5 && (type[0] == 'i' || type[0] == 'u' || type[0] == 'b') &&
         type.substr(1, 3) == "vec" && type[4] >= '2' && type[4] <= '4';
}
// The range of `line` to replace with "flat " (empty, or covering a
// smooth/noperspective qualifier), or nothing when the line is not a global
// integer varying declaration with the given storage qualifier.
inline std::optional<std::pair<std::size_t, std::size_t>> qualifier_range(std::string_view line,
                                                                         std::string_view storage) {
  std::size_t p = 0;
  const auto skip = [&] { while (p < line.size() && blank(line[p])) ++p; };
  const auto word = [&] {
    const auto start = p;
    while (p < line.size() && word_char(line[p])) ++p;
    return line.substr(start, p - start);
  };
  skip();
  auto begin = p;
  auto token = word();
  if (token == "layout") {
    skip();
    if (p >= line.size() || line[p] != '(') return std::nullopt;
    const auto close = line.find(')', p);
    if (close == std::string_view::npos) return std::nullopt;
    p = close + 1;
    skip();
    begin = p;
    token = word();
  }
  if (token == "flat") return std::nullopt;
  if (token == "smooth" || token == "noperspective") {
    skip();
    token = word();
  }
  const auto end = p - token.size();
  if (token != storage || p >= line.size() || !blank(line[p])) return std::nullopt;
  skip();
  token = word();
  if (token == "lowp" || token == "mediump" || token == "highp") {
    if (p >= line.size() || !blank(line[p])) return std::nullopt;
    skip();
    token = word();
  }
  if (!integer_type(token) || p >= line.size() || !blank(line[p])) return std::nullopt;
  skip();
  if (p >= line.size() || std::isdigit(static_cast<unsigned char>(line[p])) || word().empty())
    return std::nullopt;
  skip();
  if (p < line.size() && line[p] == '[') {
    p = line.find(']', p);
    if (p == std::string_view::npos) return std::nullopt;
    ++p;
    skip();
  }
  if (p >= line.size() || line[p] != ';') return std::nullopt;
  return std::pair{begin, end};
}
}  // namespace flat_varyings_detail

// GLSL ES 3.00 sections 4.3.4 and 4.3.6: vertex outputs and fragment inputs of
// integer type must be qualified `flat`. Some shipped shaders omit it; mobile
// drivers accept them anyway, ANGLE rejects them. Add `flat` to whole-line
// global declarations of integer (and bool) varyings, and leave parameters,
// vertex inputs and fragment outputs untouched.
inline std::string flat_integer_varyings(std::string_view source, bool vertex) {
  const std::string_view storage = vertex ? "out" : "in";
  std::string result;
  std::size_t copied = 0;
  bool changed = false;
  for (std::size_t line = 0; line < source.size();) {
    auto end = source.find('\n', line);
    if (end == std::string_view::npos) end = source.size();
    if (const auto range = flat_varyings_detail::qualifier_range(source.substr(line, end - line), storage)) {
      result.append(source.substr(copied, line + range->first - copied));
      result += "flat ";
      copied = line + range->second;
      changed = true;
    }
    line = end + 1;
  }
  if (!changed) return std::string(source);
  result.append(source.substr(copied));
  return result;
}
}  // namespace venture::gfx
