#include "common/paths.h"
#include "common/log.h"
#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>

namespace venture::paths {
fs::path migrate_data_dir(const fs::path &current, const fs::path &legacy) {
  std::error_code error;
  if (fs::exists(current, error) || error) return current;
  if (!fs::is_directory(legacy, error) || error) return current;
  // std::filesystem::rename is one same-volume rename. It never copies data.
  fs::rename(legacy, current, error);
  if (!error) return current;
  // Another process may have migrated first.
  std::error_code probe;
  if (fs::is_directory(current, probe) && !fs::exists(legacy, probe)) return current;
  static std::atomic_flag warned = ATOMIC_FLAG_INIT;
  if (!warned.test_and_set())
    log::warn("paths", "Data directory migration failed ({}); using the legacy directory and retrying next start", error.message());
  return legacy;
}
namespace {
// Lenient UTF-8 to UTF-16: well-formed sequences decode; CESU-8 surrogate
// encodings (ED A0..BF xx) yield their code unit; any other byte >= 0x80 maps
// to U+F000+byte (U+F080..U+F0FF), so distinct byte strings stay distinct.
template <class String> String decode_utf8(std::string_view s) {
  using Unit = typename String::value_type;
  String out;
  out.reserve(s.size());
  const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char b = byte(i);
    if (b < 0x80) { out += static_cast<Unit>(b); ++i; continue; }
    std::size_t n = 0;
    char32_t cp = 0;
    unsigned char low = 0x80, high = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) { n = 1; cp = b & 0x1F; }
    else if (b >= 0xE0 && b <= 0xEF) { n = 2; cp = b & 0x0F; if (b == 0xE0) low = 0xA0; }
    else if (b >= 0xF0 && b <= 0xF4) { n = 3; cp = b & 0x07; if (b == 0xF0) low = 0x90; if (b == 0xF4) high = 0x8F; }
    bool ok = n != 0 && i + n < s.size();  // the last continuation byte exists
    for (std::size_t k = 1; ok && k <= n; ++k) {
      const unsigned char c = byte(i + k);
      if (c < (k == 1 ? low : 0x80) || c > (k == 1 ? high : 0xBF)) ok = false;
      else cp = (cp << 6) | (c & 0x3F);
    }
    if (!ok) { out += static_cast<Unit>(0xF000 + b); ++i; continue; }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out += static_cast<Unit>(0xD800 + (cp >> 10));
      out += static_cast<Unit>(0xDC00 + (cp & 0x3FF));
    } else out += static_cast<Unit>(cp);
    i += n + 1;
  }
  return out;
}
#ifdef _WIN32
void append_utf8(std::string &out, char32_t cp) {
  if (cp < 0x80) { out += static_cast<char>(cp); return; }
  if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  }
  out += static_cast<char>(0x80 | (cp & 0x3F));
}
#endif
} // namespace
std::string utf8(const fs::path &p) {
#ifdef _WIN32
  const auto &w = p.native();
  std::string out;
  out.reserve(w.size());
  for (std::size_t i = 0; i < w.size(); ++i) {
    char32_t c = static_cast<char16_t>(w[i]);
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF)
      c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<char16_t>(w[++i]) - 0xDC00);
    else if (c >= 0xD800 && c <= 0xDFFF)
      c = 0xFFFD;
    append_utf8(out, c);
  }
  return out;
#else
  return p.native();
#endif
}
std::string generic_utf8(const fs::path &p) {
  auto s = utf8(p);
#ifdef _WIN32
  std::replace(s.begin(), s.end(), '\\', '/');
#endif
  return s;
}
fs::path from_utf8(std::string_view s) {
#ifdef _WIN32
  return fs::path(decode_utf8<std::wstring>(s));
#else
  return fs::path(std::string(s));
#endif
}
std::u16string utf16(std::string_view s) { return decode_utf8<std::u16string>(s); }
namespace {
constexpr bool host_forbidden(unsigned char c) {
#ifdef _WIN32
  return (c > 0 && c < 0x20) || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
#else
  (void)c;
  return false;
#endif
}
} // namespace
std::string host_name(std::string_view guest) {
  std::string out;
  out.reserve(guest.size());
  for (unsigned char c : guest) {
    if (!host_forbidden(c)) { out += char(c); continue; }
    // U+F000+c in UTF-8 (c < 0x80).
    out += char(0xEF); out += char(0x80 | (c >> 6)); out += char(0x80 | (c & 0x3F));
  }
  return out;
}
std::string guest_name(std::string_view host) {
  std::string out;
  out.reserve(host.size());
  for (std::size_t i = 0; i < host.size(); ++i) {
    auto b = [&](std::size_t k) { return static_cast<unsigned char>(host[k]); };
    if (i + 2 < host.size() && b(i) == 0xEF && (b(i + 1) == 0x80 || b(i + 1) == 0x81) && (b(i + 2) & 0xC0) == 0x80) {
      const unsigned char c = static_cast<unsigned char>(((b(i + 1) & 1) << 6) | (b(i + 2) & 0x3F));
      if (host_forbidden(c)) { out += char(c); i += 2; continue; }
    }
#ifdef _WIN32
    // U+F080..U+F0FF: a guest byte that was not UTF-8 (from_utf8).
    if (i + 2 < host.size() && b(i) == 0xEF && (b(i + 1) == 0x82 || b(i + 1) == 0x83) && (b(i + 2) & 0xC0) == 0x80) {
      out += char(0x80 | ((b(i + 1) & 1) << 6) | (b(i + 2) & 0x3F));
      i += 2;
      continue;
    }
#endif
    out += host[i];
  }
  return out;
}
bool safe_name(std::string_view s) {
  if (s.empty() || s.size() > 220 || s.back() == '.' || s.back() == ' ')
    return false;
  if (s.find_first_of("<>:\"/\\|?*") != s.npos)
    return false;
  for (unsigned char c : s)
    if (c < 32 || c == 127)
      return false;
  std::string base(s.substr(0, s.find('.')));
  std::transform(base.begin(), base.end(), base.begin(),
                 [](unsigned char c) { return char(std::toupper(c)); });
  if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL")
    return false;
  if (base.size() == 4 &&
      (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '1' &&
      base[3] <= '9')
    return false;
  return true;
}
bool safe_package(std::string_view s) {
  return safe_name(s) && s.find('.') != s.npos && s.front() != '.' &&
         s.find("..") == s.npos &&
         s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVW"
                             "XYZ0123456789._") == s.npos;
}
bool within(const fs::path &root, const fs::path &target) {
  auto r = fs::weakly_canonical(root);
  const auto absolute = fs::absolute(target);
  const auto status = fs::symlink_status(absolute);
  const bool leaf = fs::is_regular_file(status) || !fs::exists(status);
  // A regular file can be unlinked while canonical() resolves its handle.
  // Resolve its stable parent instead; symlinks still require full resolution.
  auto t = fs::weakly_canonical(leaf ? absolute.parent_path() : absolute);
  if (leaf) t /= absolute.filename();
  auto ri = r.begin(), ti = t.begin();
  for (; ri != r.end(); ++ri, ++ti) {
    if (ti == t.end())
      return false;
#ifdef _WIN32
    auto a = ri->wstring(), b = ti->wstring();
    std::transform(a.begin(), a.end(), a.begin(), ::towlower);
    std::transform(b.begin(), b.end(), b.begin(), ::towlower);
    if (a != b)
      return false;
#else
    if (*ri != *ti)
      return false;
#endif
  }
  return ti != t.end();
}
bool version_folder(std::string_view name) {
  const auto dash = name.find('-');
  return dash == name.npos ? digits(name) : digits(name.substr(0, dash)) && digits(name.substr(dash + 1));
}
bool safe_launch_target(std::string_view target) {
  const auto at = target.find('@');
  return at == target.npos ? safe_package(target)
                           : safe_package(target.substr(0, at)) && version_folder(target.substr(at + 1));
}
fs::path game_dir(const fs::path &root, std::string_view package,
                  std::string_view version) {
  if (!safe_package(package) || !version_folder(version))
    throw std::runtime_error("Invalid game installation path.");
  auto p = root / from_utf8(package) / from_utf8(version);
  if (!within(root, p))
    throw std::runtime_error("Game path escapes the games directory.");
  return p;
}
#ifndef _WIN32
std::string read(const fs::path &file, size_t limit) {
  std::ifstream in(file, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("Could not read local file.");
  auto size = in.tellg();
  if (size < 0 || uint64_t(size) > limit)
    throw std::runtime_error("Local file is too large.");
  std::string text(static_cast<size_t>(size), '\0');
  in.seekg(0);
  if (!in.read(text.data(), text.size()))
    throw std::runtime_error("Could not read local file.");
  return text;
}
#endif
Json read_json(const fs::path &file) { return parse_json(read(file)); }
std::string timestamp() {
  return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(
                                      std::chrono::system_clock::now()));
}
} // namespace venture::paths
