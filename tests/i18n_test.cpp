#include "launcher/i18n/i18n.h"
#include <doctest/doctest.h>

using namespace std::string_view_literals;
using namespace i18n::markers;

namespace {
// Russian-style plural rules exercise a three-form catalog.
unsigned long plural_test(unsigned long long n) {
  return n % 10 == 1 && n % 100 != 11 ? 0 : n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 10 || n % 100 >= 20) ? 1 : 2;
}
unsigned long plural_two(unsigned long long n) { return n > 1; }
constexpr i18n::detail::Entry kTest[] = {
    {"Library"sv, {}, "Bibliothek"sv, false},
    {"verb\x04Open"sv, {}, "Öffnen (verb)"sv, false},
    {"Open"sv, {}, "Offen"sv, false},
    {"Copy {0} to {1}"sv, {}, "Nach {1}: {0} kopieren"sv, true},
    {"{0} file"sv, "{0} files"sv, "{0} Datei A\0{0} Dateien B\0{0} Dateien C"sv, true},
    {"{0} game in {1}"sv, "{0} games in {1}"sv, "ein Spiel in {1}\0{0} Spiele in {1}\0{0} Spiele in {1}"sv, true},
    // Rejected at load: an argument dropped, one invented, a malformed pattern,
    // too few plural forms, and an empty translation.
    {"Remove {0}?"sv, {}, "Entfernen?"sv, true},
    {"Saved"sv, {}, "Gespeichert {0}"sv, false},  // not a format string: braces are just text
    {"Move {0}"sv, {}, "Verschieben {2}"sv, true},
    {"Size {0}"sv, {}, "Größe {0"sv, true},
    {"{0} item"sv, "{0} items"sv, "{0} Ding\0{0} Dinge"sv, true},
    {"Empty"sv, {}, ""sv, false},
};
constexpr i18n::detail::Entry kBrazil[] = {{"Library"sv, {}, "Biblioteca (BR)"sv, false}};
constexpr i18n::detail::Entry kPortugal[] = {{"Library"sv, {}, "Biblioteca (PT)"sv, false}};
constexpr i18n::detail::Catalog kCatalogs[] = {
    {{"de"sv, "Deutsch"sv, 1}, plural_test, 3, kTest},
    {{"pt_BR"sv, "Português (Brasil)"sv, 1}, plural_two, 2, kBrazil},
    {{"pt"sv, "Português"sv, 1}, plural_two, 2, kPortugal},
};
struct UseTestCatalogs {
  UseTestCatalogs() { i18n::detail::set_catalogs(kCatalogs); }
  ~UseTestCatalogs() {
    i18n::detail::set_catalogs({});
    i18n::set_language("en");
  }
};
std::string resolve(std::initializer_list<std::string> preferred, std::initializer_list<std::string_view> available) {
  std::vector<std::string> p(preferred);
  std::vector<std::string_view> a(available);
  return i18n::resolve(p, a);
}
}  // namespace

TEST_CASE("i18n: English returns the source text") {
  UseTestCatalogs catalogs;
  i18n::set_language("en");
  CHECK(i18n::language() == "en");
  const char* source = "Library";
  CHECK(std::string_view(tr("Library")) == "Library");
  CHECK(trf("Copy {0} to {1}", "a", "b") == "Copy a to b");
  CHECK(trn("{0} file", "{0} files", 1) == "1 file");
  CHECK(trn("{0} file", "{0} files", 2) == "2 files");
  CHECK(std::string_view(i18n::translate(source)) == "Library");
}

TEST_CASE("i18n: lookups, contexts, reordering and plural forms") {
  UseTestCatalogs catalogs;
  i18n::set_language("de");
  CHECK(i18n::language() == "de");
  CHECK(std::string_view(tr("Library")) == "Bibliothek");
  CHECK(std::string_view(tr("Lone Echo")) == "Lone Echo");  // unknown text comes back as is
  CHECK(std::string_view(tr("Open")) == "Offen");
  CHECK(std::string_view(trc("verb", "Open")) == "Öffnen (verb)");
  CHECK(std::string_view(trc("noun", "Open")) == "Open");
  CHECK(trf("Copy {0} to {1}", "a.txt", "D:") == "Nach D:: a.txt kopieren");
  CHECK(trn("{0} file", "{0} files", 1) == "1 Datei A");
  CHECK(trn("{0} file", "{0} files", 3) == "3 Dateien B");
  CHECK(trn("{0} file", "{0} files", 5) == "5 Dateien C");
  CHECK(trn("{0} file", "{0} files", 21) == "21 Datei A");
  CHECK(trn("{0} game in {1}", "{0} games in {1}", 1, "Steam") == "ein Spiel in Steam");
  CHECK(trn("{0} game in {1}", "{0} games in {1}", 2, "Steam") == "2 Spiele in Steam");
  const char* marked = mark("Library");
  CHECK(std::string_view(i18n::translate(marked)) == "Bibliothek");
  CHECK(std::string_view(i18n::translate(markc("verb", "Open"), "verb")) == "Öffnen (verb)");
}

TEST_CASE("i18n: translations that do not keep the arguments fall back to English") {
  UseTestCatalogs catalogs;
  i18n::set_language("de");
  CHECK(trf("Remove {0}?", "x") == "Remove x?");
  CHECK(std::string_view(tr("Saved")) == "Gespeichert {0}");
  CHECK(trf("Move {0}", "x") == "Move x");
  CHECK(trf("Size {0}", 3) == "Size 3");
  CHECK(trn("{0} item", "{0} items", 2) == "2 items");
  CHECK(std::string_view(tr("Empty")) == "Empty");
}

TEST_CASE("i18n: language settings and the fallback chain") {
  UseTestCatalogs catalogs;
  CHECK(i18n::languages().size() == 3);
  i18n::set_language("pt_BR");
  CHECK(std::string_view(tr("Library")) == "Biblioteca (BR)");
  i18n::set_language("pt-PT");
  CHECK(std::string_view(tr("Library")) == "Biblioteca (PT)");
  i18n::set_language("xx");
  CHECK(i18n::language() == "en");
  i18n::set_language("auto");  // whatever the system prefers, it resolves to a catalog or English
  CHECK((i18n::language() == "en" || i18n::language() == "de" || i18n::language().starts_with("pt")));

  CHECK(resolve({"pt-BR"}, {"pt", "pt_BR"}) == "pt_BR");
  CHECK(resolve({"pt_BR.UTF-8"}, {"pt"}) == "pt");
  CHECK(resolve({"pt"}, {"pt_BR"}) == "pt_BR");  // any catalog of the language before English
  CHECK(resolve({"de-AT", "fr"}, {"de", "fr"}) == "de");
  CHECK(resolve({"en-GB", "fr"}, {"fr"}) == "en");  // English preferred over a later choice
  CHECK(resolve({"en-GB"}, {"en_GB", "fr"}) == "en_GB");
  CHECK(resolve({"ja-JP", "fr-CA"}, {"fr"}) == "fr");
  CHECK(resolve({"zh-CN"}, {"zh_Hans", "zh_Hant"}) == "zh_Hans");
  CHECK(resolve({"zh-TW"}, {"zh_Hans", "zh_Hant"}) == "zh_Hant");
  CHECK(resolve({"zh-Hant-HK"}, {"zh_Hans", "zh_Hant"}) == "zh_Hant");
  CHECK(resolve({"zh_HK"}, {"zh_Hans"}) == "en");  // never across Chinese scripts
  CHECK(resolve({"sr@latin"}, {"sr", "sr_Latn"}) == "sr_Latn");
  CHECK(resolve({"C"}, {"fr"}) == "en");
  CHECK(resolve({}, {"fr"}) == "en");
}

TEST_CASE("i18n: format pattern arguments") {
  std::vector<int> out;
  CHECK(i18n::detail::placeholders("{0} of {1}", out));
  CHECK(out == std::vector<int>{0, 1});
  CHECK(i18n::detail::placeholders("{} and {:>4}", out));
  CHECK(out == std::vector<int>{0, 1});
  CHECK(i18n::detail::placeholders("{{literal}} {1:%H}", out));
  CHECK(out == std::vector<int>{1});
  CHECK_FALSE(i18n::detail::placeholders("{} {1}", out));
  CHECK_FALSE(i18n::detail::placeholders("{0", out));
  CHECK_FALSE(i18n::detail::placeholders("0}", out));
  CHECK_FALSE(i18n::detail::placeholders("{name}", out));
}

TEST_CASE("i18n: the compiled-in catalogs load") {
  // Every compiled-in language's table builds; entries that fail the argument
  // check are dropped there instead of reaching std::format.
  for (const auto& language : i18n::languages()) {
    i18n::set_language(language.code);
    CHECK(i18n::language() == language.code);
    CHECK(!language.name.empty());
    CHECK(language.completion > 0);
  }
  i18n::set_language("en");
}
