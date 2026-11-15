#include "launcher/ui/text_scripts.h"

#include <algorithm>
#include <cctype>

namespace venture::launcher::ui {
namespace {
void append_utf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out += char(c);
  } else if (c < 0x800) {
    out += char(0xC0 | (c >> 6));
    out += char(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += char(0xE0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  } else {
    out += char(0xF0 | (c >> 18));
    out += char(0x80 | ((c >> 12) & 0x3F));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  }
}
bool separator(char32_t c) { return c == ' ' || c == '_' || c == '.' || c == 0x3000 || c == 0xB7 || c == 0x30FB; }
// Letters and digits of any script; punctuation, symbols and emoji are not initials.
bool letter(char32_t c) {
  if (c < 0x80) return std::isalnum(int(c)) != 0;
  if (c < 0xC0 || c == 0xD7 || c == 0xF7 || c == 0xFFFD) return false;
  if ((c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFE30 && c <= 0xFE4F)) return false;
  if ((c >= 0xFF00 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xD800 && c <= 0xDFFF)) return false;
  return c < 0x1F000;
}
}  // namespace

std::vector<const system_fonts::Query*> script_font_queries(std::string_view language) {
  std::string code(language);
  std::ranges::transform(code, code.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  const std::string lang = code.substr(0, code.find_first_of("_-"));
  namespace s = system_fonts::scripts;
  std::vector<const system_fonts::Query*> out{&s::simplified_chinese, &s::traditional_chinese, &s::japanese,
                                              &s::korean};
  const system_fonts::Query* own = lang == "ja" ? &s::japanese
                                   : lang == "ko" ? &s::korean
                                   : lang == "zh" && (code.find("hant") != std::string::npos || code.ends_with("tw") ||
                                                      code.ends_with("hk") || code.ends_with("mo"))
                                       ? &s::traditional_chinese
                                       : &s::simplified_chinese;
  std::ranges::rotate(out.begin(), std::ranges::find(out, own), std::ranges::find(out, own) + 1);
  return out;
}
std::u32string decode_utf8(std::string_view text) {
  std::u32string out;
  for (std::size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    const int length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 0;
    char32_t c = length == 1 ? lead : length == 2 ? lead & 0x1F : length == 3 ? lead & 0x0F : lead & 0x07;
    bool valid = length > 0 && i + length <= text.size();
    for (int k = 1; valid && k < length; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      valid = (next & 0xC0) == 0x80;
      c = c << 6 | (next & 0x3F);
    }
    // Overlong forms, surrogates and values past U+10FFFF are malformed too.
    static constexpr char32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    valid = valid && c >= minimum[length] && c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF);
    out += valid ? c : char32_t{0xFFFD};
    i += valid ? length : 1;
  }
  return out;
}
std::string initials(std::string_view name) {
  std::string out;
  int count = 0;
  bool start = true;
  for (char32_t c : decode_utf8(name)) {
    if (separator(c)) {
      start = true;
    } else if (start && letter(c)) {
      append_utf8(out, c < 0x80 ? char32_t(std::toupper(int(c))) : c);
      start = false;
      if (++count == 2) break;
    }
  }
  return out;
}
}  // namespace venture::launcher::ui
