// Interface translations from gettext catalogs (i18n/<locale>.po), compiled
// into the program at build time (cmake/I18n.cmake, tools/i18n_compile.py).
//
// English is the source language and is written directly in the code. Every
// string a translator should see is marked at its call site with one of the
// functions below, always with string literals, so tools/i18n_extract.py can
// find it and write the source catalog (i18n/<domain>.pot):
//
//   tr("Library")                         plain text
//   trc("verb", "Open")                   with a context (msgctxt) that tells
//                                         two identical English texts apart
//   trf("Uninstall {0}?", name)           a std::format pattern; translations
//                                         may reorder positional arguments
//   trn("{0} game", "{0} games", count)   plural forms; `count` picks the form
//                                         and is argument {0}, further
//                                         arguments are {1}, {2}...
//   trcf / trnc                           the two above with a context
//   mark("Automatic")                     marks text that is translated later
//                                         with translate() (tables of labels)
//
// A `// i18n: ...` comment on the lines just above a marked string becomes its
// translator comment. Text that is not marked (game names, paths, user input)
// is never looked up. With English active every function returns its English
// argument without a lookup.
//
// This file and i18n.cpp have no project dependencies; another project can
// take them as they are.
#pragma once
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace i18n {

// --- Language ----------------------------------------------------------------
// A compiled-in language: `code` is the catalog's file name ("fr", "pt_BR",
// "zh_Hans"), `name` its own name for itself ("Français").
struct Language {
  std::string_view code, name;
  float completion = 0;  // translated share of the source catalog, 0..1
};
// The compiled-in translations, English excluded.
std::span<const Language> languages();
// "auto" or empty follows the system's interface language (with fallbacks such
// as pt_BR -> pt -> English); otherwise a code from languages() or "en".
// Takes effect for the next string looked up.
void set_language(std::string_view setting);
// The active catalog's code; "en" when English is shown.
std::string_view language();
// The system's preferred interface languages, most preferred first, as
// locale names ("pt-BR", "zh_CN.UTF-8"...); "auto" chooses from these.
std::vector<std::string> system_languages();
// The catalog `preferred` resolves to among `available` codes, or "en":
// for each preference in turn, its exact locale, then language+region,
// language+script and the bare language, then any catalog of that language.
// An English preference ends the search with "en".
std::string resolve(std::span<const std::string> preferred, std::span<const std::string_view> available);

// --- Lookups (use the markers below at call sites) ---------------------------
namespace detail {
const char* lookup(std::string_view context, std::string_view msgid);
const char* lookup_plural(std::string_view context, std::string_view singular, std::string_view plural,
                          unsigned long long n);
std::string format(const char* pattern, const char* english, std::format_args args);
}  // namespace detail

// The markers take literals only (character arrays), so every translatable
// string is visible to the extractor. Results are NUL-terminated.
template <std::size_t N>
const char* tr(const char (&msgid)[N]) {
  return detail::lookup({}, {msgid, N - 1});
}
template <std::size_t C, std::size_t N>
const char* trc(const char (&context)[C], const char (&msgid)[N]) {
  return detail::lookup({context, C - 1}, {msgid, N - 1});
}
template <std::size_t N, class... Args>
std::string trf(const char (&pattern)[N], Args&&... args) {
  return detail::format(detail::lookup({}, {pattern, N - 1}), pattern, std::make_format_args(args...));
}
template <std::size_t C, std::size_t N, class... Args>
std::string trcf(const char (&context)[C], const char (&pattern)[N], Args&&... args) {
  return detail::format(detail::lookup({context, C - 1}, {pattern, N - 1}), pattern, std::make_format_args(args...));
}
template <std::size_t S, std::size_t P, class... Args>
std::string trn(const char (&singular)[S], const char (&plural)[P], unsigned long long n, Args&&... args) {
  const char* english = n == 1 ? singular : plural;
  return detail::format(detail::lookup_plural({}, {singular, S - 1}, {plural, P - 1}, n), english,
                        std::make_format_args(n, args...));
}
template <std::size_t C, std::size_t S, std::size_t P, class... Args>
std::string trnc(const char (&context)[C], const char (&singular)[S], const char (&plural)[P], unsigned long long n,
                 Args&&... args) {
  const char* english = n == 1 ? singular : plural;
  return detail::format(detail::lookup_plural({context, C - 1}, {singular, S - 1}, {plural, P - 1}, n), english,
                        std::make_format_args(n, args...));
}
// Marks text for extraction without translating it; translate() later.
template <std::size_t N>
constexpr const char* mark(const char (&msgid)[N]) {
  return msgid;
}
template <std::size_t C, std::size_t N>
constexpr const char* markc(const char (&)[C], const char (&msgid)[N]) {
  return msgid;
}
// The translation of text marked with mark() (or markc() with the same
// `context`); unmarked text comes back unchanged.
inline const char* translate(const char* msgid, std::string_view context = {}) {
  return detail::lookup(context, msgid);
}

// `using namespace i18n::markers;` brings the call-site names into scope.
namespace markers {
using i18n::mark;
using i18n::markc;
using i18n::tr;
using i18n::trc;
using i18n::trcf;
using i18n::trf;
using i18n::translate;
using i18n::trn;
using i18n::trnc;
}  // namespace markers

// --- Compiled catalogs (generated by tools/i18n_compile.py) ------------------
namespace detail {
struct Entry {
  std::string_view key;     // msgid, or context "\x04" msgid
  std::string_view plural;  // msgid_plural; empty for a singular entry
  std::string_view text;    // msgstr, or the plural forms joined by '\0'
  bool format;              // a std::format pattern: its arguments are checked
};
struct Catalog {
  Language language;
  unsigned long (*plural_form)(unsigned long long n);
  int forms;
  std::span<const Entry> entries;
};
std::span<const Catalog> catalogs();  // defined by the generated source
// Replaces the compiled catalogs (tests); an empty span restores them.
void set_catalogs(std::span<const Catalog> catalogs);
// The argument indices of a std::format pattern ("{}" counts up from 0), or
// false when the pattern is malformed or mixes automatic and manual indices.
bool placeholders(std::string_view pattern, std::vector<int>& out);
}  // namespace detail
}  // namespace i18n
