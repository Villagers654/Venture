#pragma once
#include "common/http/client.h"
#include "common/json.h"

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>

// Updating Venture in place from its latest GitHub release: the Windows
// installer reinstalls over this copy, an AppImage replaces itself, the
// headset APK installs over itself through PackageInstaller. Other
// packages (deb, rpm, tarball, a build) keep the release page; Flatpak updates
// through Flatpak.
namespace venture::launcher::update {
enum class Method { page, installer, appimage, apk };
// How this copy of Venture updates.
Method method();
struct Asset {
  std::string name, url;
  uint64_t size = 0;
};
// True when release `tag` (v1.2.3.4, or v1.2.3-alpha) is later than `current`.
// Missing parts count as 0, and a pre-release suffix sorts before the same
// numbers without one, so 0.3.0-alpha updates to 0.3.0.
bool newer_version(std::string_view tag, std::string_view current);
// The release's file for `method`; none for the page.
std::optional<Asset> pick(const Json &release, Method method);
// The release's SHA256SUMS file.
std::optional<Asset> sums(const Json &release);
// `name`'s lowercase hex digest in a SHA256SUMS text; empty when not listed.
std::string expected_sum(std::string_view sums, std::string_view name);
// Downloads `file` into `folder` and checks it against the release's
// SHA256SUMS; returns the checked file. Throws on any mismatch.
fs::path fetch(const Asset &file, const Asset &sums, const fs::path &folder, std::stop_token stop,
               const http::Progress &progress);
// Starts the downloaded update. The launcher quits right after, so the
// installer can replace its files. An APK installs over the running app,
// which Android restarts; it blocks until the user answers Android's
// confirmation, so it runs off the UI thread.
void apply(Method method, const fs::path &file);
}  // namespace venture::launcher::update
