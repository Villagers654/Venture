#pragma once
#include "common/http/client.h"
#include "common/json.h"
#include "common/library/library.h"

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>

// Updating Venture in place from its newest GitHub release on the update
// channel (stable: full releases only; experimental: pre-releases too): the Windows
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
// The order of two versions or release tags (v1.2.3.4, 1.2.3-alpha.2): below,
// at or above 0 as `a` is earlier than, the same as or later than `b`; none
// when either does not parse. Missing numbers count as 0; a pre-release suffix
// sorts before the same numbers without one (0.3.0-alpha < 0.3.0), and
// suffixes compare as semantic versioning's do, by dot-separated parts, numbers
// numerically and below words (alpha.2 < alpha.10 < beta).
std::optional<int> compare_versions(std::string_view a, std::string_view b);
// True when release `tag` is later than `current`.
bool newer_version(std::string_view tag, std::string_view current);
// A version or tag with a pre-release suffix ("0.3.0-alpha.2").
bool prerelease(std::string_view version);

// Update channels. Stable offers full releases only (GitHub's latest
// release); experimental offers the newest release of any kind, so also a full
// release that overtakes the pre-releases (0.3.0 after 0.3.0-alpha.N).
enum class Channel { stable, experimental };
// The channel a build is on by itself: experimental for a pre-release.
Channel channel_of(std::string_view version);
// The channel this run switches to: the running build's own when `last_run`
// (the version that ran last) differs, because a new install, an update or
// another AppImage file is starting; none when the same version runs again,
// so a choice made in Settings holds until the version changes.
std::optional<Channel> channel_for_run(std::string_view last_run, std::string_view running);
// Applies channel_for_run to the settings (experimental_updates) and records
// `running` as the last version run; true when they changed and need saving.
bool adopt_build_channel(library::Settings &settings, std::string_view running);
// The newest release in a GitHub release list (an array, as /releases returns
// it): drafts never, pre-releases only on the experimental channel, the
// highest version tag otherwise (the list's order is not trusted). Null when
// none qualifies.
const Json *newest_release(const Json &releases, Channel channel);
// Whether release `tag` is offered to `current` on `channel`. Experimental: a
// later version. Stable: never a pre-release; a later release, or, when
// `current` is a pre-release, the latest stable release even when it is older,
// so switching a pre-release build to Stable goes back to stable
// (0.3.0-alpha.2 is offered 0.2.5.1). A release build is never offered an
// older one.
bool offered(std::string_view tag, std::string_view current, Channel channel);
// Whether Android refuses to install `tag`'s APK over `current`'s: its version
// code (tools/android/build_apk.py: major * 1,000,000 + minor * 1,000 + patch)
// is lower, which PackageInstaller rejects as a downgrade.
bool android_refuses(std::string_view tag, std::string_view current);

// The release's file for `method`; none for the page.
std::optional<Asset> pick(const Json &release, Method method);
// The same for a given processor ("x86_64" or "aarch64"), which only the
// AppImage depends on. Exposed for tests.
std::optional<Asset> pick(const Json &release, Method method, std::string_view arch);
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
