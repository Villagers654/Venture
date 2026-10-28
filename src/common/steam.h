#pragma once
#include "common/paths.h"
#include <cstdint>
#include <vector>

namespace venture::steam {
struct File { std::string name, kind; std::uint64_t size{}; };
struct Plan {
  std::uint32_t appid{}, depot{};
  std::string manifest, name, package, version;
  std::int64_t version_code{};
  std::vector<File> files;
  std::string compatibility_key() const;
};
// Steam plans are independent of Meta entitlements. Ownership is checked live
// by the runtime; an installation's presence never establishes ownership.
Plan parse_plan(const Json&);
struct Install { Plan plan; fs::path directory; bool complete{}; };
std::vector<Install> scan(const fs::path& games);
} // namespace venture::steam
