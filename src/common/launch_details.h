#pragma once
#include "common/json.h"
#include <cstdint>
#include <string>

namespace venture {
// OVR_LaunchType.h: Normal=1, Invite=2, Coordinated=3, Deeplink=4.
struct LaunchDetails {
  std::int32_t type = 1;
  std::string source = "library", deeplink, destination, lobby, match;
  std::uint64_t room = 0;
};
Json launch_details_json(const LaunchDetails&);
LaunchDetails read_launch_details(const Json&); // bounded; never logs payloads
} // namespace venture
