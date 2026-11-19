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
std::optional<Asset> pick(const Json &release, Method method, std::string_view arch) {
  switch (method) {
    case Method::installer:
      return find(release, [](std::string_view n) { return n == "Venture-windows64-setup.exe"; });
    case Method::appimage: {
      // Packages carry the plain numbers; a pre-release's name may carry its suffix too.
      const std::string tail = "-linux-" + std::string(arch) + ".AppImage";
      return find(release, [&](std::string_view n) { return n.starts_with("Venture-") && n.ends_with(tail); });
    }
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
std::optional<Asset> pick(const Json &release, Method method) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return pick(release, method, "aarch64");
#else
  return pick(release, method, "x86_64");
#endif
}
std::optional<Asset> sums(const Json &release) {
  return find(release, [](std::string_view n) { return n == "SHA256SUMS"; });
}
namespace {
struct Version {
  std::vector<int> parts;
  std::vector<std::string_view> suffix;  // dot-separated; empty for a release
};
bool all_digits(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}
std::optional<Version> parse_version(std::string_view text) {
  Version out;
  if (text.starts_with('v')) text.remove_prefix(1);
  std::string_view suffix;
  if (const auto dash = text.find('-'); dash != text.npos) {
    suffix = text.substr(dash + 1);
    text = text.substr(0, dash);
    if (suffix.empty()) return {};
  }
  for (std::size_t at = 0; at <= text.size();) {
    const auto end = std::min(text.find('.', at), text.size());
    const auto digits = text.substr(at, end - at);
    if (!all_digits(digits) || digits.size() > 9) return {};
    int value = 0;
    for (char c : digits) value = value * 10 + (c - '0');
    out.parts.push_back(value);
    at = end + 1;
  }
  for (std::size_t at = 0; !suffix.empty() && at <= suffix.size();) {
    const auto end = std::min(suffix.find('.', at), suffix.size());
    const auto part = suffix.substr(at, end - at);
    if (part.empty()) return {};
    out.suffix.push_back(part);
    at = end + 1;
  }
  return out;
}
int compare_part(std::string_view a, std::string_view b) {
  const bool an = all_digits(a), bn = all_digits(b);
  if (an != bn) return an ? -1 : 1;  // numbers sort before words
  if (an) {
    while (a.size() > 1 && a.front() == '0') a.remove_prefix(1);
    while (b.size() > 1 && b.front() == '0') b.remove_prefix(1);
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  }
  const int c = a.compare(b);
  return c < 0 ? -1 : c > 0 ? 1 : 0;
}
}  // namespace
std::optional<int> compare_versions(std::string_view a_text, std::string_view b_text) {
  auto a = parse_version(a_text), b = parse_version(b_text);
  if (!a || !b) return {};
  a->parts.resize(std::max(a->parts.size(), b->parts.size()));
  b->parts.resize(a->parts.size());
  if (a->parts != b->parts) return a->parts < b->parts ? -1 : 1;
  if (a->suffix.empty() || b->suffix.empty())
    return a->suffix.empty() == b->suffix.empty() ? 0 : a->suffix.empty() ? 1 : -1;
  for (std::size_t i = 0; i < std::min(a->suffix.size(), b->suffix.size()); ++i)
    if (const int c = compare_part(a->suffix[i], b->suffix[i])) return c;
  return a->suffix.size() == b->suffix.size() ? 0 : a->suffix.size() < b->suffix.size() ? -1 : 1;
}
bool newer_version(std::string_view tag, std::string_view current) {
  const auto order = compare_versions(tag, current);
  return order && *order > 0;
}
bool prerelease(std::string_view version) {
  const auto parsed = parse_version(version);
  return parsed && !parsed->suffix.empty();
}
Channel channel_of(std::string_view version) {
  return prerelease(version) ? Channel::experimental : Channel::stable;
}
std::optional<Channel> channel_for_run(std::string_view last_run, std::string_view running) {
  if (last_run == running) return {};
  return channel_of(running);
}
bool adopt_build_channel(library::Settings &settings, std::string_view running) {
  const auto channel = channel_for_run(settings.last_run_version, running);
  if (!channel) return false;
  settings.experimental_updates = *channel == Channel::experimental;
  settings.last_run_version = std::string(running);
  return true;
}
const Json *newest_release(const Json &releases, Channel channel) {
  if (!releases.is_array()) return nullptr;
  const Json *best = nullptr;
  std::string best_tag;
  for (const auto &release : releases) {
    if (!release.is_object() || release.value("draft", false)) continue;
    const auto tag = release.value("tag_name", std::string{});
    if (!parse_version(tag)) continue;
    // GitHub's flag and the tag's suffix both mark a pre-release.
    if (channel == Channel::stable && (release.value("prerelease", false) || prerelease(tag))) continue;
    if (!best || newer_version(tag, best_tag)) {
      best = &release;
      best_tag = tag;
    }
  }
  return best;
}
bool offered(std::string_view tag, std::string_view current, Channel channel) {
  if (channel == Channel::experimental) return newer_version(tag, current);
  const auto order = compare_versions(tag, current);
  if (!order || prerelease(tag)) return false;
  return *order > 0 || (*order != 0 && prerelease(current));
}
bool android_refuses(std::string_view tag, std::string_view current) {
  auto code = [](std::string_view version) -> std::optional<int64_t> {
    const auto parsed = parse_version(version);
    if (!parsed) return {};
    auto parts = parsed->parts;
    parts.resize(std::max<std::size_t>(parts.size(), 3));
    return int64_t(parts[0]) * 1'000'000 + int64_t(parts[1]) * 1'000 + parts[2];
  };
  const auto a = code(tag), b = code(current);
  return a && b && *a < *b;
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
