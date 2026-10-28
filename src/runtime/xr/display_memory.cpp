#include "xr/display_memory.h"
#include "common/log.h"
#include "common/paths.h"
#include "xr/display_timing.h"
#include <exception>
#include <mutex>
#include <string>

namespace venture::xr {
namespace {
std::mutex mutex;
std::string host_key;  // empty: no host runtime (tests, flat mode)
std::int64_t saved = 0;
fs::path file() { return paths::data_dir() / "host-displays.json"; }
} // namespace

void recall_display_period(std::string_view runtime, std::string_view system) {
  std::lock_guard lock(mutex);
  host_key = std::string(runtime) + " / " + std::string(system);
  try {
    if (!fs::exists(file())) return;
    const auto& entry = field(paths::read_json(file()), host_key);
    if (!entry.is_number_integer()) return;
    saved = entry.get<std::int64_t>();
    if (display_timing_state().remember(saved))
      log::info("xr", "Host {}: starting at the remembered {:.2f} Hz until the first frame", host_key, 1e9 / double(saved));
  } catch (const std::exception& e) {
    log::warn("xr", "Remembered host display rates unreadable: {}", e.what());
  }
}

void remember_display_period(std::int64_t period) {
  std::lock_guard lock(mutex);
  if (host_key.empty() || period == saved) return;
  try {
    Json all = Json::object();
    if (fs::exists(file())) {
      auto previous = paths::read_json(file());
      if (previous.is_object()) all = std::move(previous);
    }
    all[host_key] = period;
    paths::atomic_write(file(), all.dump(2) + "\n");
    saved = period;
  } catch (const std::exception& e) {
    log::warn("xr", "Cannot remember the host display rate: {}", e.what());
    saved = period;  // One warning per value, not one per frame.
  }
}
} // namespace venture::xr
