#pragma once
// Preparation of a game's first launch (venture-runtime --prepare): the one-time
// work a first launch does before it can run, done right after the install while
// the player is still in the launcher. The runtime records it in the game's cache
// directory (<data>/cache/<package>/<version>/prepared.json); the launcher reads
// that to know which installs still need it.
#include "common/json.h"
#include "common/paths.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace venture::first_launch {
inline constexpr const char *kStampName = "prepared.json";

// The runtime build that prepared it: its translated code is only valid for that
// build (the block cache's own key starts with the same size and time).
inline std::string runtime_identity(const fs::path &runtime) {
  std::error_code error;
  const auto size = fs::file_size(runtime, error);
  if (error) return {};
  const auto time = fs::last_write_time(runtime, error).time_since_epoch().count();
  if (error) return {};
  return std::to_string(size) + "-" + std::to_string(static_cast<long long>(time));
}

enum class State { none, started, complete };

// What the stamp in `cache` says about preparation by `runtime`.
inline State stamp_state(const fs::path &cache, const fs::path &runtime) {
  try {
    std::error_code error;
    const auto file = cache / kStampName;
    if (!fs::is_regular_file(file, error)) return State::none;
    const auto stamp = paths::read_json(file);
    if (str(stamp, "runtime") != runtime_identity(runtime)) return State::none;
    return field(stamp, "complete") == true ? State::complete : State::started;
  } catch (...) {
    return State::none;
  }
}
} // namespace venture::first_launch
