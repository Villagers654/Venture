// Names in any script: Meta's UTF-8 reaches the launcher's text unchanged, and
// the UI fonts cover Chinese whatever the UI language.
#include "common/library/library.h"
#include "common/meta/store.h"
#include "common/paths.h"
#include "common/system_fonts.h"
#include "launcher/ui/text_scripts.h"
#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <vector>

using namespace venture;
namespace {
// "Zhang Wei" and "Battle Talent" in Chinese, as UTF-8.
constexpr std::string_view zhang_wei = "\xE5\xBC\xA0\xE4\xBC\x9F";
constexpr std::string_view battle_talent = "\xE6\x88\x98\xE6\x96\x97\xE5\xA4\xA9\xE8\xB5\x8B";

// A one-face OpenType file whose cmap (format 4) maps exactly `code_point`.
std::string font_with(char32_t code_point, std::uint32_t table_base = 0) {
  std::string out;
  const auto u16 = [&](unsigned v) { out += char(v >> 8); out += char(v & 0xFF); };
  const auto u32 = [&](std::uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); };
  u32(0x00010000); u16(1); u16(16); u16(0); u16(0);              // offset table, one table
  u32(0x636D6170); u32(0); u32(table_base + 28); u32(44);        // 'cmap' record
  u16(0); u16(1); u16(3); u16(1); u32(12);                       // cmap header: (3,1) at 12
  u16(4); u16(32); u16(0); u16(4); u16(4); u16(1); u16(0);       // format 4, two segments
  u16(code_point); u16(0xFFFF); u16(0);                          // end codes, pad
  u16(code_point); u16(0xFFFF);                                  // start codes
  u16((1 - code_point) & 0xFFFF); u16(1);                        // deltas: glyph 1
  u16(0); u16(0);                                                // range offsets
  return out;
}
fs::path scratch(std::string_view name) {
  auto dir = paths::executable().parent_path() / "test-artifacts" /
             ("launcher-text-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(dir);
  return dir / name;
}
}  // namespace

TEST_CASE("font coverage reads a face's character map, also inside a collection") {
  const auto file = scratch("han.ttf");
  paths::atomic_write(file, font_with(0x4E2D));
  CHECK(system_fonts::covers({file, 0}, 0x4E2D));
  CHECK_FALSE(system_fonts::covers({file, 0}, U'A'));
  CHECK_FALSE(system_fonts::covers({file, 1}, 0x4E2D));
  // 'ttcf' with two faces; table offsets are from the start of the collection.
  std::string ttc = "ttcf";
  ttc += std::string("\x00\x01\x00\x00\x00\x00\x00\x02\x00\x00\x00\x14\x00\x00\x00\x30", 16);
  ttc += font_with(0x4E2D, 48).substr(0, 28) + font_with(U'A', 92).substr(0, 28);
  ttc += font_with(0x4E2D).substr(28) + font_with(U'A').substr(28);
  const auto collection = scratch("pair.ttc");
  paths::atomic_write(collection, ttc);
  CHECK(system_fonts::covers({collection, 0}, 0x4E2D));
  CHECK_FALSE(system_fonts::covers({collection, 0}, U'A'));
  CHECK(system_fonts::covers({collection, 1}, U'A'));
  CHECK_FALSE(system_fonts::covers({collection, 2}, U'A'));
  CHECK_FALSE(system_fonts::covers({scratch("missing.ttf"), 0}, U'A'));
  fs::remove_all(file.parent_path());
  fs::remove_all(collection.parent_path());
}

TEST_CASE("a Chinese account name and title stay UTF-8 from Meta's reply to the library cache") {
  // Meta's JSON may escape the characters or send them raw; both are UTF-8 here.
  auto reply = parse_json(
      R"({"data":{"viewer":{"user":{"id":"777777","alias":"\u5f20\u4f1f","active_entitlements":{"nodes":[)"
      R"({"item":{"id":"123456","display_name":")" + std::string(battle_talent) +
      R"(","platform":"ANDROID_6DOF"}}],"page_info":{"has_next_page":false}}}}}})");
  auto snapshot = meta::parse_library(reply);
  CHECK(snapshot.alias == zhang_wei);
  REQUIRE(snapshot.games.size() == 1);
  CHECK(snapshot.games[0].name == battle_talent);
  const auto cache = scratch("library.json");
  library::save(cache, snapshot.games);
  CHECK(paths::read(cache).find(battle_talent) != std::string::npos);  // raw UTF-8, not ANSI or escapes
  auto loaded = library::load(cache);
  REQUIRE(loaded.size() == 1);
  CHECK(loaded[0].name == battle_talent);
  fs::remove_all(cache.parent_path());
}

TEST_CASE("initials take whole UTF-8 characters") {
  using launcher::ui::initials;
  CHECK(initials("ada lovelace") == "AL");
  CHECK(initials("Fixture_user.name") == "FU");
  CHECK(initials(zhang_wei) == "\xE5\xBC\xA0");
  CHECK(initials(std::string(zhang_wei.substr(0, 3)) + " " + std::string(zhang_wei.substr(3))) == zhang_wei);
  CHECK(initials("\xC3\xA9mile zola") == "\xC3\xA9Z");
  CHECK(initials("(!) \xE2\x98\x85 x") == "X");  // punctuation and symbols are no initials
  CHECK(initials("\xE5\xBC") == "");              // a cut character is never half drawn
  CHECK(launcher::ui::decode_utf8("a\xE5\xBC\xA0\xC0\xAF") == U"a\u5F20\uFFFD\uFFFD");
}

TEST_CASE("the UI fonts cover a Chinese name whatever the UI language") {
  for (auto language : {"en", "fr", "pt_BR"}) {
    auto queries = launcher::ui::script_font_queries(language);
    REQUIRE(queries.size() == 4);
    CHECK(queries.front() == &system_fonts::scripts::simplified_chinese);
  }
  CHECK(launcher::ui::script_font_queries("ja").front() == &system_fonts::scripts::japanese);
  CHECK(launcher::ui::script_font_queries("ko-KR").front() == &system_fonts::scripts::korean);
  CHECK(launcher::ui::script_font_queries("zh-Hant").front() == &system_fonts::scripts::traditional_chinese);
  CHECK(launcher::ui::script_font_queries("zh_TW").front() == &system_fonts::scripts::traditional_chinese);
  CHECK(launcher::ui::script_font_queries("zh-Hans-CN").front() == &system_fonts::scripts::simplified_chinese);
  // On this host: the fonts merged for an English UI draw every character.
  std::vector<system_fonts::Face> faces;
  for (const auto* query : launcher::ui::script_font_queries("en"))
    if (auto face = system_fonts::find(*query)) faces.push_back(*face);
  if (faces.empty()) {
    MESSAGE("No CJK font installed on this host; coverage not checked");
    return;
  }
  for (auto text : {zhang_wei, battle_talent})
    for (char32_t c : launcher::ui::decode_utf8(text)) {
      CAPTURE(std::uint32_t(c));
      CHECK(std::ranges::any_of(faces, [&](const auto& face) { return system_fonts::covers(face, c); }));
    }
}
