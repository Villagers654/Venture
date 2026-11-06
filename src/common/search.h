// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>
#include <vector>

// Library search: case, accents, punctuation and word order don't matter.
namespace venture::search {
// The comparable words of UTF-8 text: "Pokémon: CAFÉ-Racer" is pokemon, cafe, racer.
std::vector<std::string> words(std::string_view text);
// Every word of query occurs in title, in any order ("echo lone" finds Lone Echo).
bool matches(std::string_view query, std::string_view title);
} // namespace venture::search
