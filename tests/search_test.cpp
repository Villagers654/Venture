// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/search.h"
#include <doctest/doctest.h>
using venture::search::matches;
using venture::search::words;
using V = std::vector<std::string>;

TEST_CASE("search words ignore case, accents and punctuation") {
  CHECK(words("  Pokémon: CAFÉ-Racer ") == V{"pokemon", "cafe", "racer"});
  CHECK(words("Lucky’s Tale") == words("lucky's tale"));
  CHECK(words("Lucky’s Tale") == V{"luckys", "tale"});
  CHECK(words("Lucky‘s Tale") == V{"luckys", "tale"});
  CHECK(words("").empty());
  // Decomposed accents (a base letter and a combining mark) fold too.
  CHECK(words("Pokemón") == V{"pokemon"});
}

TEST_CASE("search words fold letters without an accent mark") {
  CHECK(words("Ørsted Æble Łódź Œuvre Đakovo") ==
        V{"orsted", "aeble", "lodz", "oeuvre", "dakovo"});
  CHECK(words("Straße") == V{"strasse"});
}

TEST_CASE("search matches every word in any order") {
  CHECK(matches("echo lone", "Lone Echo"));
  CHECK(matches("vader episode", "Vader Immortal: Episode I"));
  CHECK(matches("immortal:", "Vader Immortal: Episode I"));
  CHECK(matches("pokemon cafe", "Pokémon Café"));
  CHECK(matches("storm", "Stormland"));
  CHECK(matches("luckys", "Lucky’s Tale"));
  CHECK(matches("", "Stormland"));
  CHECK_FALSE(matches("nothing like this", "Stormland"));
  CHECK_FALSE(matches("echo 2", "Lone Echo"));
}
