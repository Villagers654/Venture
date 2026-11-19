#include "launcher/updater.h"

#include "common/digest.h"
#include "common/environment.h"
#include "common/paths.h"
#include "common/process.h"
#ifdef __ANDROID__
#include "launcher/headset_play.h"
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace venture::launcher::update {
Method method() {
#if defined(__ANDROID__)
  return Method::apk;
#elif defined(_WIN32)
  // The installer leaves its uninstaller beside venture.exe; a zip copy has none.
  std::error_code ec;
  if (fs::exists(paths::executable().parent_path() / "uninstall.exe", ec)) return Method::installer;
#else
  if (const auto *image = environment::getenv("APPIMAGE"); image && *image && !environment::getenv("FLATPAK_ID"))
    return Method::appimage;
#endif
  return Method::page;
}
namespace {
std::optional<Asset> find(const Json &release, auto &&matches) {
  const auto it = release.find("assets");
  if (it == release.end() || !it->is_array()) return {};
  for (const auto &a : *it) {
    if (!a.is_object()) continue;
    const auto name = a.value("name", std::string{});
    const auto url = a.value("browser_download_url", std::string{});
    if (!name.empty() && matches(name) && http::allowed_release(url))
      return Asset{name, url, a.value("size", uint64_t{0})};
  }
  return {};
}
std::string lower(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}
}  // namespace
std::optional<Asset> pick(const Json &release, Method method) {
  switch (method) {
    case Method::installer:
      return find(release, [](std::string_view n) { return n == "Venture-windows64-setup.exe"; });
    case Method::appimage:
      return find(release, [](std::string_view n) {
#if defined(__aarch64__)
        return n.starts_with("Venture-") && n.ends_with("-linux-aarch64.AppImage");
#else
        return n.starts_with("Venture-") && n.ends_with("-linux-x86_64.AppImage");
#endif
      });
    case Method::apk:
      return find(release, [](std::string_view n) {
        const auto name = lower(std::string(n));
        return name.starts_with("venture-") && name.ends_with("-android-arm64.apk");
      });
    case Method::page:
      break;
  }
  return {};
}
std::optional<Asset> sums(const Json &release) {
  return find(release, [](std::string_view n) { return n == "SHA256SUMS"; });
}
bool newer_version(std::string_view tag, std::string_view current) {
  struct Version {
    std::vector<int> parts;
    bool prerelease = false;
  };
  auto parse = [](std::string_view text) {
    Version out;
    if (text.starts_with('v')) text.remove_prefix(1);
    if (const auto dash = text.find('-'); dash != text.npos) {
      out.prerelease = dash + 1 < text.size();
      text = text.substr(0, dash);
    }
    for (std::size_t at = 0; at <= text.size();) {
      const auto end = std::min(text.find('.', at), text.size());
      const auto digits = text.substr(at, end - at);
      int value = 0;
      if (digits.empty()) return Version{};
      for (char c : digits) {
        if (c < '0' || c > '9') return Version{};
        value = value * 10 + (c - '0');
      }
      out.parts.push_back(value);
      at = end + 1;
    }
    return out;
  };
  auto a = parse(tag), b = parse(current);
  if (a.parts.empty() || b.parts.empty()) return false;
  a.parts.resize(std::max(a.parts.size(), b.parts.size()));
  b.parts.resize(a.parts.size());
  if (a.parts != b.parts) return a.parts > b.parts;
  return b.prerelease && !a.prerelease;
}

std::string expected_sum(std::string_view text, std::string_view name) {
  // "<64 hex>  <name>" (or " *<name>" for binary mode), one file per line.
  while (!text.empty()) {
    auto end = text.find('\n');
    auto line = text.substr(0, end);
    text = end == text.npos ? std::string_view{} : text.substr(end + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (line.size() < 66 || line[64] != ' ') continue;
    auto file = line.substr(65);
    if (file.starts_with(' ') || file.starts_with('*')) file.remove_prefix(1);
    const auto hex = line.substr(0, 64);
    if (file == name && std::ranges::all_of(hex, [](unsigned char c) { return std::isxdigit(c); }))
      return lower(std::string(hex));
  }
  return {};
}
fs::path fetch(const Asset &file, const Asset &sums, const fs::path &folder, std::stop_token stop,
               const http::Progress &progress) {
  const auto expected = expected_sum(http::get_release_text(sums.url, stop), file.name);
  if (expected.empty()) throw std::runtime_error("The release does not list a checksum for " + file.name + ".");
  fs::create_directories(folder);
  const auto path = folder / paths::from_utf8(file.name);
  http::download_release(file.url, path, file.size, stop, progress);
  std::ifstream in(path, std::ios::binary);
  if (lower(digest::sha256(in)) != expected) {
    in.close();
    std::error_code ec;
    fs::remove(path, ec);
    throw std::runtime_error("The downloaded update did not match its checksum. Try again.");
  }
  return path;
}
void apply(Method method, const fs::path &file) {
  if (method == Method::installer) {
    // The installer's own pages reinstall over this copy and can start Venture again.
    start_detached(file, {});
    return;
  }
  if (method == Method::apk) {
#ifdef __ANDROID__
    launcher::headset::update_self(file);
    return;
#endif
  }
  if (method != Method::appimage) throw std::runtime_error("This copy of Venture updates from the release page.");
  const auto *image = environment::getenv("APPIMAGE");
  if (!image || !*image) throw std::runtime_error("Could not find this AppImage.");
  const fs::path target = paths::from_utf8(image);
  // Same folder, then one rename: the running image stays mounted from the old file.
  auto staged = target.parent_path() / ("." + paths::utf8(target.filename()) + ".update");
  fs::copy_file(file, staged, fs::copy_options::overwrite_existing);
  fs::permissions(staged, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec);
  fs::rename(staged, target);
  std::error_code ec;
  fs::remove(file, ec);
  start_detached(target, {});
}
}  // namespace venture::launcher::update
