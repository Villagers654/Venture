#include "xr/display_memory.h"
#include "common/log.h"
#include "common/paths.h"
#include "xr/display_timing.h"
#include <cmath>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>

namespace venture::xr {
namespace {
std::mutex mutex;
std::string host_key;  // empty: no host runtime (tests, flat mode)
std::int64_t saved = 0;
std::optional<std::array<XrFovf, 2>> saved_fov;
fs::path file() { return paths::data_dir() / "host-displays.json"; }
fs::path fov_file() { return paths::data_dir() / "host-eye-fov.json"; }
fs::path latest_file() { return paths::data_dir() / "host-display.json"; }
Json latest;  // host-display.json as this process last wrote it
void write_latest() {
  try {
    paths::atomic_write(latest_file(), latest.dump(2) + "\n");
  } catch (const std::exception& e) {
    log::warn("xr", "Cannot record the host display: {}", e.what());
  }
}
// Each eye as [left, right, up, down] radians.
std::optional<std::array<XrFovf, 2>> read_fov(const Json &entry) {
  if (!entry.is_array() || entry.size() != 2) return std::nullopt;
  std::array<XrFovf, 2> fov{};
  for (int eye = 0; eye < 2; ++eye) {
    const auto &a = entry[eye];
    if (!a.is_array() || a.size() != 4) return std::nullopt;
    for (const auto &v : a)
      if (!v.is_number() || !std::isfinite(v.get<double>())) return std::nullopt;
    fov[eye] = {a[0].get<float>(), a[1].get<float>(), a[2].get<float>(), a[3].get<float>()};
    if (!(fov[eye].angleLeft < fov[eye].angleRight && fov[eye].angleDown < fov[eye].angleUp))
      return std::nullopt;
  }
  return fov;
}
bool same(const std::array<XrFovf, 2> &a, const std::array<XrFovf, 2> &b) {
  return std::memcmp(a.data(), b.data(), sizeof(a)) == 0;
}
} // namespace

void recall_display_period(std::string_view runtime, std::string_view system) {
  std::lock_guard lock(mutex);
  host_key = std::string(runtime) + " / " + std::string(system);
  saved_fov.reset();
  try {
    if (fs::exists(fov_file())) saved_fov = read_fov(field(paths::read_json(fov_file()), host_key));
  } catch (const std::exception& e) {
    log::warn("xr", "Remembered host eye fields of view unreadable: {}", e.what());
  }
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

void remember_host_views(std::string_view runtime, std::string_view system, std::uint32_t recommended_width,
                         std::uint32_t recommended_height, std::uint32_t max_width, std::uint32_t max_height) {
  std::lock_guard lock(mutex);
  latest = {{"runtime", std::string(runtime)},
            {"system", std::string(system)},
            {"recommended", {recommended_width, recommended_height}},
            {"max", {max_width, max_height}}};
  write_latest();
}

void remember_host_rates(float current, const std::vector<float> &rates) {
  std::lock_guard lock(mutex);
  if (!latest.is_object()) return;  // no host views recorded in this process
  Json listed = Json::array();
  for (auto r : rates)
    if (std::isfinite(r) && r > 0) listed.push_back(std::round(r * 100) / 100);
  const double now = std::isfinite(current) && current > 0 ? std::round(current * 100) / 100 : 0;
  if (field(latest, "rate") == Json(now) && field(latest, "rates") == listed) return;
  latest["rate"] = now;
  latest["rates"] = std::move(listed);
  write_latest();
}

std::optional<std::array<XrFovf, 2>> remembered_eye_fov() {
  std::lock_guard lock(mutex);
  return saved_fov;
}

void remember_eye_fov(const std::array<XrFovf, 2> &fov) {
  std::lock_guard lock(mutex);
  if (host_key.empty() || (saved_fov && same(*saved_fov, fov))) return;
  Json eyes = Json::array();
  for (const auto &f : fov) eyes.push_back({f.angleLeft, f.angleRight, f.angleUp, f.angleDown});
  if (!read_fov(eyes)) return;
  try {
    Json all = Json::object();
    if (fs::exists(fov_file())) {
      auto previous = paths::read_json(fov_file());
      if (previous.is_object()) all = std::move(previous);
    }
    all[host_key] = std::move(eyes);
    paths::atomic_write(fov_file(), all.dump(2) + "\n");
  } catch (const std::exception& e) {
    log::warn("xr", "Cannot remember the host eye fields of view: {}", e.what());
  }
  saved_fov = fov;  // One attempt per value.
}
} // namespace venture::xr
