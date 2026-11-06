// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/search.h"

#include <iterator>

namespace venture::search {
namespace {
// U+00C0 to U+024F: each letter's plain lowercase spelling (case folded, accents
// removed, and ø æ œ ł đ, which have no decomposition, spelled out), or nullptr
// for a code point that is not such a letter. Generated from Python's unicodedata.
constexpr const char* kLatin[] = {
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e",
    "i", "i", "i", "i", nullptr, "n", "o", "o", "o", "o", "o", nullptr,
    "o", "u", "u", "u", "u", "y", nullptr, "ss", "a", "a", "a", "a",
    "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    nullptr, "n", "o", "o", "o", "o", "o", nullptr, "o", "u", "u", "u",
    "u", "y", nullptr, "y", "a", "a", "a", "a", "a", "a", "c", "c",
    "c", "c", "c", "c", "c", "c", "d", "d", "d", "d", "e", "e",
    "e", "e", "e", "e", "e", "e", "e", "e", "g", "g", "g", "g",
    "g", "g", "g", "g", "h", "h", nullptr, nullptr, "i", "i", "i", "i",
    "i", "i", "i", "i", "i", nullptr, "ij", "ij", "j", "j", "k", "k",
    nullptr, "l", "l", "l", "l", "l", "l", nullptr, nullptr, "l", "l", "n",
    "n", "n", "n", "n", "n", nullptr, nullptr, nullptr, "o", "o", "o", "o",
    "o", "o", "oe", "oe", "r", "r", "r", "r", "r", "r", "s", "s",
    "s", "s", "s", "s", "s", "s", "t", "t", "t", "t", nullptr, nullptr,
    "u", "u", "u", "u", "u", "u", "u", "u", "u", "u", "u", "u",
    "w", "w", "y", "y", "y", "z", "z", "z", "z", "z", "z", "s",
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "o", "o", nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "u",
    "u", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "dz", "dz", "dz", "lj",
    "lj", "lj", "nj", "nj", "nj", "a", "a", "i", "i", "o", "o", "u",
    "u", "u", "u", "u", "u", "u", "u", "u", "u", nullptr, "a", "a",
    "a", "a", nullptr, nullptr, nullptr, nullptr, "g", "g", "k", "k", "o", "o",
    "o", "o", nullptr, nullptr, "j", "dz", "dz", "dz", "g", "g", nullptr, nullptr,
    "n", "n", "a", "a", nullptr, nullptr, nullptr, nullptr, "a", "a", "a", "a",
    "e", "e", "e", "e", "i", "i", "i", "i", "o", "o", "o", "o",
    "r", "r", "r", "r", "u", "u", "u", "u", "s", "s", "t", "t",
    nullptr, nullptr, "h", "h", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "a", "a",
    "e", "e", "o", "o", "o", "o", "o", "o", "o", "o", "y", "y",
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr,
};
static_assert(std::size(kLatin) == 0x250 - 0xC0);

// Decodes one UTF-8 sequence at s[i]; an invalid byte reads as itself.
char32_t next(std::string_view s, std::size_t& i) {
  auto b = static_cast<unsigned char>(s[i++]);
  int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
  if (b < 0x80 || b >= 0xF8 || i + extra > s.size()) return b;
  char32_t cp = b & (0x3F >> extra);
  for (int k = 0; k < extra; ++k) {
    auto c = static_cast<unsigned char>(s[i]);
    if ((c & 0xC0) != 0x80) return b;
    cp = (cp << 6) | (c & 0x3F);
    ++i;
  }
  return cp;
}
bool apostrophe(char32_t cp) { return cp == U'\'' || cp == 0x2018 || cp == 0x2019 || cp == 0x02BC; }
bool combining(char32_t cp) { return cp >= 0x300 && cp <= 0x36F; }
// Punctuation, symbols and spaces separate words.
bool separator(char32_t cp) {
  if (cp < 0x80) return !((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'));
  return cp < 0xC0 || cp == 0xD7 || cp == 0xF7 || (cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x3000 && cp <= 0x303F) ||
         (cp >= 0xFE10 && cp <= 0xFE6F) || (cp >= 0xFF01 && cp <= 0xFF0F);
}
} // namespace

std::vector<std::string> words(std::string_view text) {
  std::vector<std::string> out;
  std::string word;
  auto flush = [&] {
    if (!word.empty()) out.push_back(std::move(word));
    word.clear();
  };
  for (std::size_t i = 0; i < text.size();) {
    std::size_t start = i;
    char32_t cp = next(text, i);
    // An apostrophe belongs to its word: "Lucky's" is found as "luckys" too.
    if (apostrophe(cp) || combining(cp)) continue;
    if (separator(cp)) { flush(); continue; }
    if (cp >= 'A' && cp <= 'Z') word += char(cp - 'A' + 'a');
    else if (cp >= 0xC0 && cp < 0x250 && kLatin[cp - 0xC0]) word += kLatin[cp - 0xC0];
    else word.append(text.substr(start, i - start));
  }
  flush();
  return out;
}

bool matches(std::string_view query, std::string_view title) {
  std::string joined;
  for (auto& w : words(title)) joined += " " + w;
  for (auto& w : words(query))
    if (joined.find(w) == std::string::npos) return false;
  return true;
}
} // namespace venture::search
