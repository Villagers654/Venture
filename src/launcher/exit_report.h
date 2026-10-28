#pragma once
#include "common/json.h"
#include <optional>
#include <string_view>
#include <vector>
namespace venture::launcher {
inline Json exit_report(std::string_view reason, int code, int signal,
                        const std::vector<std::optional<int>> &codes) {
  Json children = Json::array();
  for (auto child : codes)
    children.push_back({{"exitCode", child ? Json(*child) : Json(nullptr)},
                        {"state", child ? "exited" : "running_at_launcher_teardown"}});
  return {{"reason", reason}, {"launcherExitCode", code},
          {"signal", signal}, {"children", children}};
}
} // namespace venture::launcher
