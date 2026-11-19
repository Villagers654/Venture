// SPDX-License-Identifier: GPL-3.0-or-later
// Android's own fonts in /system/fonts, found from the same fontconfig-style
// patterns the desktop launcher asks for. Android has no fontconfig; the
// families Venture asks for map to the files every Android build ships
// (Roboto, Noto Sans CJK, Noto Color Emoji, Noto Sans Symbols), each checked
// for existence and, with a sample, for the glyph.
#include "common/system_fonts.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace venture::system_fonts {
namespace {
const std::filesystem::path kFonts = "/system/fonts";

std::string lower(std::string s) {
  std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}
std::string trim(std::string s) {
  auto first = s.find_first_not_of(' ');
  auto last = s.find_last_not_of(' ');
  return first == s.npos ? std::string{} : s.substr(first, last - first + 1);
}
struct Pattern {
  std::vector<std::string> families;
  std::string weight, slant, lang;
};
Pattern parse(const char* text) {
  Pattern out;
  std::string pattern = text ? text : "";
  auto colon = pattern.find(':');
  std::string families = pattern.substr(0, colon);
  for (std::size_t start = 0; start <= families.size();) {
    auto comma = families.find(',', start);
    if (comma == families.npos) comma = families.size();
    if (auto family = lower(trim(families.substr(start, comma - start))); !family.empty())
      out.families.push_back(family);
    start = comma + 1;
  }
  for (std::size_t start = colon; start != pattern.npos && start < pattern.size();) {
    auto next = pattern.find(':', start + 1);
    auto item = lower(pattern.substr(start + 1, next == pattern.npos ? pattern.npos : next - start - 1));
    if (item.starts_with("weight=")) out.weight = item.substr(7);
    if (item.starts_with("slant=")) out.slant = item.substr(6);
    if (item.starts_with("style=")) {
      if (item.find("italic") != item.npos) out.slant = "italic";
      if (item.find("semibold") != item.npos || item.find("medium") != item.npos) out.weight = "semibold";
    }
    if (item.starts_with("lang=")) out.lang = item.substr(5);
    start = next;
  }
  return out;
}
std::optional<Face> first_existing(std::initializer_list<const char*> files, int index = 0) {
  std::error_code ec;
  for (const char* file : files)
    if (std::filesystem::is_regular_file(kFonts / file, ec)) return Face{kFonts / file, index};
  return std::nullopt;
}
// Noto Sans CJK's collection order in Android's fonts.xml: ja, ko, zh-Hans, zh-Hant.
int cjk_index(const std::string& family, const std::string& lang) {
  if (family.ends_with(" kr") || lang.starts_with("ko")) return 1;
  if (family.ends_with(" sc") || lang == "zh-cn") return 2;
  if (family.ends_with(" tc") || lang == "zh-tw") return 3;
  return 0;
}
std::optional<Face> family_face(const std::string& family, const Pattern& p) {
  if (family == "inter" || family == "noto sans" || family == "sans-serif" || family == "roboto") {
    if (p.slant == "italic") return first_existing({"Roboto-Italic.ttf", "Roboto-Regular.ttf"});
    if (p.weight == "semibold" || p.weight == "medium" || p.weight == "bold")
      return first_existing({"Roboto-Medium.ttf", "Roboto-Regular.ttf"});
    return first_existing({"Roboto-Regular.ttf"});
  }
  if (family.starts_with("noto sans cjk"))
    return first_existing({"NotoSansCJK-Regular.ttc", "NotoSansCJKjp-Regular.otf"}, cjk_index(family, p.lang));
  if (family == "noto color emoji") return first_existing({"NotoColorEmoji.ttf", "NotoColorEmojiLegacy.ttf"});
  if (family == "noto sans symbols 2") return first_existing({"NotoSansSymbols-Regular-Subsetted2.ttf"});
  if (family == "noto sans symbols") return first_existing({"NotoSansSymbols-Regular-Subsetted.ttf"});
  return std::nullopt;
}
std::optional<Face> pattern_face(const char* text, bool /*family_only: only listed families are tried*/) {
  const auto p = parse(text);
  for (const auto& family : p.families)
    if (auto face = family_face(family, p)) return face;
  return std::nullopt;
}
}  // namespace

std::string fontconfig_file(const char* pattern, bool family_only) {
  auto face = pattern_face(pattern, family_only);
  return face ? face->file.string() : std::string{};
}
std::optional<Face> find(const Query& query) {
  auto face = pattern_face(query.fontconfig, query.family_only);
  if (face && query.sample && !covers(*face, query.sample)) return std::nullopt;
  return face;
}
}  // namespace venture::system_fonts
