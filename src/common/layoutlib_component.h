#pragma once
// The Android 2D framework (layoutlib, docs/design/layoutlib.md) as a component
// downloaded the first time a 2D app needs it, not part of every download: its
// fonts, ICU data, framework resources and natives are about 260 MB.
//
// A build tree stages it beside the runtime (bin/layoutlib/layoutlib.jar) and
// uses it there. A release instead carries bin/layoutlib.json, written by
// tools/package_release.py:
//   {"url": "https://github.com/.../Venture-layoutlib-windows64.zip",
//    "sha256": "<64 hex>", "size": <bytes>}
// and the archive (a layoutlib/ tree) is a file of the same GitHub release. It
// unpacks once into <data>/components/layoutlib-<first 16 hex of its SHA-256>,
// so a release with other 2D files never uses an older copy.
#include "common/paths.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace venture::layoutlib_component {
struct Manifest {
  std::string url, sha256;
  std::uint64_t size = 0;
};
// bin/layoutlib.json, when this copy gets layoutlib on demand.
std::optional<Manifest> manifest(const fs::path &bin);
// The installed component's folder for that manifest.
fs::path folder(const fs::path &data, const Manifest &m);
// layoutlib.jar: the build tree's, else the installed component's; empty when neither.
fs::path find(const fs::path &bin, const fs::path &data);
// Downloads, checks and unpacks the component when it is missing; returns
// layoutlib.jar, or empty when this copy has no layoutlib at all (no manifest:
// an arm64 build). Throws when the download or its check fails.
using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;
fs::path ensure(const fs::path &bin, const fs::path &data, std::stop_token stop = {},
                const Progress &progress = {});
// Unpacks a downloaded archive into `target` (through a sibling temporary folder
// and one rename); every member must stay inside it. Exposed for tests.
void unpack(const fs::path &archive, const fs::path &target, std::stop_token stop = {});
}  // namespace venture::layoutlib_component
