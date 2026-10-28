#include "common/paths.h"
#include "common/log.h"
#include "common/environment.h"
#include <chrono>
#include <iostream>
#include <thread>

#include "async_wait.h"

int main(int argc, char** argv) {
  if (argc != 5) return 2;
  const auto root = venture::paths::from_utf8(argv[1]);
  const auto current = root / "current", legacy = root / "legacy";
  const auto ready = root / argv[2];
  venture::paths::atomic_write(ready, "ready");
  if (!venture::test::wait_until([&] { return venture::fs::exists(root / "go"); })) return 3;
  for (int i = 0; i < std::stoi(argv[3]); ++i) {
    if (venture::paths::migrate_data_dir(current, legacy) != current) return 4;
    if (venture::paths::read(current / "save") != "unchanged save") return 5;
  }
  const auto* value = venture::environment::getenv("VENTURE_MOCK_INPUT");
  if (std::string_view(argv[4]) == "unset") {
    if (value) return 6;
  } else if (!value || std::string_view(value) != argv[4]) return 6;
  venture::log::flush();
  return 0;
}
