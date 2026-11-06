#include "i18n.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#endif

namespace i18n {
namespace {
struct Table {
  const detail::Catalog* catalog = nullptr;
  std::unordered_map<std::string_view, const detail::Entry*> entries;
};
std::span<const detail::Catalog> g_override;
std::mutex g_mutex;
// Tables are built once per catalog and kept, so a pointer handed out stays
// valid when the language changes; English is a null table.
std::vector<std::unique_ptr<Table>> g_tables;
std::atomic<const Table*> g_active{nullptr};
std::vector<Language> g_languages;
bool g_languages_ready = false;

std::span<const detail::Catalog> current_catalogs() {
  return g_override.empty() ? detail::catalogs() : g_override;
}

std::string lower(std::string_view s) {
  std::string out(s);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return out;
}

// Same argument indices (each set sorted, duplicates removed).
bool same_placeholders(std::string_view source, std::string_view translation, bool allow_missing_count) {
  std::vector<int> want, got;
  if (!detail::placeholders(source, want) || !detail::placeholders(translation, got)) return false;
  std::ranges::sort(want);
  want.erase(std::unique(want.begin(), want.end()), want.end());
  std::ranges::sort(got);
  got.erase(std::unique(got.begin(), got.end()), got.end());
  // A plural form may leave out the count ({0}), as in "one game".
  if (allow_missing_count && !want.empty() && want.front() == 0 && (got.empty() || got.front() != 0))
    want.erase(want.begin());
  return want == got;
}

// A translation is used only when every form keeps the source's arguments.
bool valid(const detail::Catalog& catalog, const detail::Entry& entry) {
  std::string_view msgid = entry.key.substr(entry.key.find('\x04') + 1);
  if (entry.plural.empty())
    return !entry.text.empty() && (!entry.format || same_placeholders(msgid, entry.text, false));
  // The plural source carries every argument (the singular may spell out "one").
  std::string_view source = entry.plural;
  int forms = 0;
  for (size_t start = 0; start <= entry.text.size(); ++forms) {
    size_t end = entry.text.find('\0', start);
    if (end == std::string_view::npos) end = entry.text.size();
    std::string_view form = entry.text.substr(start, end - start);
    if (form.empty() || !same_placeholders(source, form, true)) return false;
    start = end + 1;
  }
  return forms == catalog.forms;
}

const Table* table_for(const detail::Catalog& catalog) {
  for (const auto& table : g_tables)
    if (table->catalog == &catalog) return table.get();
  auto table = std::make_unique<Table>();
  table->catalog = &catalog;
  table->entries.reserve(catalog.entries.size());
  for (const auto& entry : catalog.entries)
    if (valid(catalog, entry)) table->entries.emplace(entry.key, &entry);
  g_tables.push_back(std::move(table));
  return g_tables.back().get();
}

const detail::Entry* find(std::string_view context, std::string_view msgid) {
  const Table* table = g_active.load(std::memory_order_acquire);
  if (!table) return nullptr;
  if (context.empty()) {
    auto it = table->entries.find(msgid);
    return it == table->entries.end() ? nullptr : it->second;
  }
  std::string key;
  key.reserve(context.size() + 1 + msgid.size());
  key.append(context).push_back('\x04');
  key.append(msgid);
  auto it = table->entries.find(key);
  return it == table->entries.end() ? nullptr : it->second;
}

struct Locale {
  std::string language, script, region;
};
// "pt-BR", "pt_BR.UTF-8", "sr@latin", "zh-Hans-CN" -> parts, lowercase.
Locale parse_locale(std::string_view name) {
  Locale out;
  std::string modifier;
  if (auto at = name.find('@'); at != std::string_view::npos) {
    modifier = lower(name.substr(at + 1));
    name = name.substr(0, at);
  }
  name = name.substr(0, name.find('.'));
  std::vector<std::string> parts;
  for (size_t start = 0; start <= name.size();) {
    size_t end = name.find_first_of("-_", start);
    if (end == std::string_view::npos) end = name.size();
    parts.push_back(lower(name.substr(start, end - start)));
    start = end + 1;
  }
  if (parts.empty() || parts[0].empty()) return out;
  out.language = parts[0];
  for (size_t i = 1; i < parts.size(); ++i) {
    if (parts[i].size() == 4 && out.script.empty())
      out.script = parts[i];
    else if ((parts[i].size() == 2 || parts[i].size() == 3) && out.region.empty())
      out.region = parts[i];
  }
  if (modifier == "latin") out.script = "latn";
  if (modifier == "cyrillic") out.script = "cyrl";
  // Chinese is chosen by script; the region implies it when it is missing.
  if (out.language == "zh" && out.script.empty())
    out.script = out.region == "tw" || out.region == "hk" || out.region == "mo" ? "hant" : "hans";
  return out;
}
}  // namespace

std::span<const Language> languages() {
  std::lock_guard lock(g_mutex);
  if (!g_languages_ready) {
    for (const auto& catalog : current_catalogs()) g_languages.push_back(catalog.language);
    g_languages_ready = true;
  }
  return g_languages;
}

std::string_view language() {
  const Table* table = g_active.load(std::memory_order_acquire);
  return table ? table->catalog->language.code : "en";
}

std::vector<std::string> system_languages() {
  std::vector<std::string> out;
#ifdef _WIN32
  // The display languages in the user's order, not the regional format.
  ULONG count = 0, size = 0;
  if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size) {
    std::wstring buffer(size, L'\0');
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size))
      for (const wchar_t* p = buffer.c_str(); *p; p += std::wcslen(p) + 1) {
        std::string name;
        for (const wchar_t* c = p; *c; ++c) name += char(*c < 0x80 ? *c : '?');
        out.push_back(name);
      }
  }
#else
  // gettext's precedence: LANGUAGE (a list) unless the locale is C, then
  // LC_ALL, LC_MESSAGES, LANG.
  std::string locale;
  for (const char* variable : {"LC_ALL", "LC_MESSAGES", "LANG"})
    if (const char* value = std::getenv(variable); value && *value) {
      locale = value;
      break;
    }
  if (const char* list = std::getenv("LANGUAGE"); list && *list && locale != "C" && locale != "POSIX") {
    std::string_view rest = list;
    while (!rest.empty()) {
      size_t colon = rest.find(':');
      if (colon) out.emplace_back(rest.substr(0, colon));
      rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
    }
  }
  if (!locale.empty() && locale != "C" && locale != "POSIX" && !locale.starts_with("C.")) out.push_back(locale);
#endif
  return out;
}

std::string resolve(std::span<const std::string> preferred, std::span<const std::string_view> available) {
  auto has = [&](const std::string& code) -> std::string {
    for (std::string_view a : available)
      if (lower(a) == code) return std::string(a);
    return {};
  };
  for (const auto& name : preferred) {
    Locale l = parse_locale(name);
    if (l.language.empty()) continue;
    std::vector<std::string> candidates;
    if (!l.script.empty() && !l.region.empty()) candidates.push_back(l.language + "_" + l.script + "_" + l.region);
    if (!l.region.empty()) candidates.push_back(l.language + "_" + l.region);
    if (!l.script.empty()) candidates.push_back(l.language + "_" + l.script);
    // A bare language is fine unless it would cross a script (zh -> zh_Hant).
    if (l.language != "zh") candidates.push_back(l.language);
    for (const auto& candidate : candidates)
      if (auto found = has(candidate); !found.empty()) return found;
    // English is the source: a regional English catalog only when asked for.
    if (l.language == "en") return "en";
    // Any catalog of the language, except across Chinese scripts.
    for (std::string_view a : available) {
      Locale c = parse_locale(a);
      if (c.language == l.language && (l.language != "zh" || c.script == l.script)) return std::string(a);
    }
  }
  return "en";
}

void set_language(std::string_view setting) {
  std::lock_guard lock(g_mutex);
  auto catalogs = current_catalogs();
  std::vector<std::string_view> codes;
  for (const auto& catalog : catalogs) codes.push_back(catalog.language.code);
  std::string code;
  if (setting.empty() || setting == "auto") {
    code = resolve(system_languages(), codes);
  } else {
    // An exact code, else resolved like a system preference (a removed
    // catalog's setting falls back to its language, then English).
    std::string wanted(setting);
    code = resolve(std::span(&wanted, 1), codes);
  }
  const Table* table = nullptr;
  for (const auto& catalog : catalogs)
    if (catalog.language.code == code) table = table_for(catalog);
  g_active.store(table, std::memory_order_release);
}

namespace detail {
const char* lookup(std::string_view context, std::string_view msgid) {
  const Entry* entry = find(context, msgid);
  return entry ? entry->text.data() : msgid.data();
}

const char* lookup_plural(std::string_view context, std::string_view singular, std::string_view plural,
                          unsigned long long n) {
  const char* english = n == 1 ? singular.data() : plural.data();
  const Entry* entry = find(context, singular);
  if (!entry || entry->plural.empty()) return english;
  const Table* table = g_active.load(std::memory_order_acquire);
  unsigned long form = table->catalog->plural_form(n);
  if (form >= unsigned(table->catalog->forms)) form = 0;
  const char* text = entry->text.data();
  const char* end = text + entry->text.size();
  for (unsigned long i = 0; i < form && text < end; ++i) text += std::strlen(text) + 1;
  return text < end ? text : english;
}

std::string format(const char* pattern, const char* english, std::format_args args) {
  try {
    return std::vformat(pattern, args);
  } catch (const std::format_error&) {
    if (pattern == english) throw;
    return std::vformat(english, args);  // a translation with a bad format spec
  }
}

void set_catalogs(std::span<const Catalog> catalogs) {
  std::lock_guard lock(g_mutex);
  g_active.store(nullptr, std::memory_order_release);
  g_tables.clear();
  g_override = catalogs;
  g_languages.clear();
  g_languages_ready = false;
}

bool placeholders(std::string_view pattern, std::vector<int>& out) {
  out.clear();
  int next = 0;
  bool automatic = false, manual = false;
  for (size_t i = 0; i < pattern.size(); ++i) {
    char c = pattern[i];
    if (c == '}') {
      if (i + 1 < pattern.size() && pattern[i + 1] == '}') {
        ++i;
        continue;
      }
      return false;
    }
    if (c != '{') continue;
    if (i + 1 < pattern.size() && pattern[i + 1] == '{') {
      ++i;
      continue;
    }
    size_t close = pattern.find('}', i);
    if (close == std::string_view::npos) return false;
    std::string_view field = pattern.substr(i + 1, close - i - 1);
    // A nested replacement field in the spec ({0:{1}}) is not supported.
    if (field.find('{') != std::string_view::npos) return false;
    std::string_view index = field.substr(0, field.find(':'));
    if (index.empty()) {
      automatic = true;
      out.push_back(next++);
    } else {
      int value = 0;
      for (char d : index) {
        if (d < '0' || d > '9' || value > 99) return false;
        value = value * 10 + (d - '0');
      }
      manual = true;
      out.push_back(value);
    }
    i = close;
  }
  return !(automatic && manual);
}
}  // namespace detail
}  // namespace i18n
