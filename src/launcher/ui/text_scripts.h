// Text the launcher shows in any language: Meta account names and store titles
// arrive in whatever script their owner wrote, whatever the UI language is.
#pragma once
#include "common/system_fonts.h"
#include <string>
#include <string_view>
#include <vector>

namespace venture::launcher::ui {
// The system fonts that give the UI fonts Han, kana and hangul, in the order
// they are merged: the UI language's own first (so shared Han characters take
// its forms), then the other CJK fonts, for names in any language.
std::vector<const system_fonts::Query*> script_font_queries(std::string_view language);
// Up to two initials of a UTF-8 name, one per word: "Ada Lovelace" -> "AL", and
// a Chinese name written without spaces gives its first character. Whole code
// points of letters and digits only; ASCII letters are capitalized.
std::string initials(std::string_view name);
// The code points of UTF-8 text; a malformed byte becomes U+FFFD.
std::u32string decode_utf8(std::string_view text);
}  // namespace venture::launcher::ui
