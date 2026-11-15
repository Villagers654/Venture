// Installed host fonts: Windows' Fonts folders, fontconfig on POSIX.
#pragma once
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace venture::system_fonts {
// One face of a font file (a collection such as a .ttc holds several).
struct Face {
  std::filesystem::path file;
  int index = 0;
  bool operator==(const Face&) const = default;
};
// What to look for. Windows tries `windows_files` in its Fonts folder, then the
// user's; POSIX asks fontconfig for `fontconfig` (with `family_only`, only one of
// the pattern's families, never a substitute). A nonzero `sample` must be in the
// face's character map, so a match without the script's glyphs is no match.
struct Query {
  std::span<const std::string_view> windows_files;
  const char* fontconfig = nullptr;
  char32_t sample = 0;
  bool family_only = false;
};
std::optional<Face> find(const Query& query);
// True when the face's character map (cmap) maps `code_point`.
bool covers(const Face& face, char32_t code_point);

// Han, kana and hangul as one CJK font family of each kind covers them.
namespace scripts {
inline constexpr std::string_view simplified_files[] = {"msyh.ttc", "simsun.ttc", "simhei.ttf"};
inline constexpr Query simplified_chinese{simplified_files, "Noto Sans CJK SC,sans-serif:lang=zh-cn", char32_t{0x4E2D}};
inline constexpr std::string_view traditional_files[] = {"msjh.ttc", "mingliub.ttc", "mingliu.ttc"};
inline constexpr Query traditional_chinese{traditional_files, "Noto Sans CJK TC,sans-serif:lang=zh-tw", char32_t{0x7E41}};
inline constexpr std::string_view japanese_files[] = {"YuGothM.ttc", "YuGothR.ttc", "meiryo.ttc", "msgothic.ttc"};
inline constexpr Query japanese{japanese_files, "Noto Sans CJK JP,sans-serif:lang=ja", char32_t{0x3042}};
inline constexpr std::string_view korean_files[] = {"malgun.ttf", "gulim.ttc"};
inline constexpr Query korean{korean_files, "Noto Sans CJK KR,sans-serif:lang=ko", char32_t{0xAC00}};
inline constexpr std::string_view emoji_files[] = {"seguiemj.ttf"};
inline constexpr Query emoji{emoji_files, "Noto Color Emoji", U'\U0001F600', true};
inline constexpr std::string_view symbol_files[] = {"seguisym.ttf"};
inline constexpr Query symbols{symbol_files, "Noto Sans Symbols 2,Noto Sans Symbols,DejaVu Sans", char32_t{0x2605}, true};
}  // namespace scripts

#ifndef _WIN32
// fontconfig's file for `pattern` (such as "Inter,Noto Sans:weight=semibold"),
// or empty without fontconfig; with `family_only`, empty unless the match is one
// of the pattern's families rather than a substitute.
std::string fontconfig_file(const char* pattern, bool family_only = false);
#endif
}  // namespace venture::system_fonts
