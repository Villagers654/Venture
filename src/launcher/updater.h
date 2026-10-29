#pragma once
#include "common/http/client.h"
#include "common/json.h"

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>

// Updating Venture in place from its latest GitHub release: the Windows
// installer reinstalls over this copy, an AppImage replaces itself. Other
// packages (deb, rpm, tarball, a build) keep the release page; Flatpak updates
// through Flatpak.
namespace venture::launcher::update {
enum class Method { page, installer, appimage };
// How this copy of Venture updates.
Method method();
struct Asset {
  std::string name, url;
  uint64_t size = 0;
};
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
// installer can replace its files.
void apply(Method method, const fs::path &file);
}  // namespace venture::launcher::update
