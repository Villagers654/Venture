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
std::string utf8(const fs::path &p) {
  auto s = p.u8string();
  return {s.begin(), s.end()};
}
fs::path from_utf8(std::string_view s) {
  return fs::path(std::u8string(s.begin(), s.end()));
}
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
fs::path game_dir(const fs::path &root, std::string_view package,
                  std::string_view version) {
  if (!safe_package(package) || !digits(version))
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
