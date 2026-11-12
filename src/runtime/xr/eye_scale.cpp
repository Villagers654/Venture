#include "xr/eye_scale.h"
#include "common/environment.h"
#include "common/json.h"
#include "common/log.h"
#include "common/paths.h"
#include <exception>
#include <format>
#include <mutex>

namespace venture::xr::eye_scale {
namespace db = library::eye_scale;
namespace {
double area(VkExtent2D e) { return double(e.width) * double(e.height); }
bool valid(VkExtent2D e) { return e.width && e.height; }

std::mutex mutex;
struct Process {
  bool begun = false;
  std::string package;
  double applied = 1;
  std::optional<Record> start;  // the record the process began with (user's, else bundled)
  VkExtent2D largest{};
  std::optional<Record> written;
};
Process& process() {
  static Process p;
  return p;
}
} // namespace

Mode parse_mode(std::string_view value) { return value == "off" ? Mode::off : Mode::automatic; }

void begin(std::string_view package) {
  auto& p = process();
  std::lock_guard lock(mutex);
  p = {};
  p.begun = true;
  p.package = package;
  if (package.empty()) return;
  p.start = db::record(paths::data_dir(), package);
  const char* mode = environment::getenv("VENTURE_EYE_SCALE");
  const bool automatic = parse_mode(mode ? mode : "") == Mode::automatic;
  if (p.start && automatic) p.applied = compensation_for(*p.start);
  if (!p.start)
    log::info("eye-scale", "{}: no eye size record yet; telling the target size and learning", package);
  else
    log::info("eye-scale", "{}: {} multiplier {:.3f} ({}); {}", package, state_name(p.start->state),
              p.start->multiplier, p.start->own ? "learned here" : "Venture database",
              p.applied > 1 ? std::format("telling 1/{:.3f} of the target size", p.applied)
                            : std::string(automatic ? "telling the target size" : "compensation off"));
}

double compensation() {
  auto& p = process();
  std::lock_guard lock(mutex);
  return p.applied;
}

void observe(VkExtent2D told, VkExtent2D target, VkExtent2D rendered) {
  auto& p = process();
  std::lock_guard lock(mutex);
  if (!p.begun || p.package.empty() || !valid(told) || !valid(target) || !valid(rendered)) return;
  if (area(rendered) <= area(p.largest)) return;
  p.largest = rendered;
  const auto record = learn(p.start, p.applied, told, target, rendered);
  log::info("eye-scale", "{}: eye {}x{} told {}x{} target {}x{}: multiplier {:.3f}, {}", p.package, rendered.width,
            rendered.height, told.width, told.height, target.width, target.height, record.multiplier,
            state_name(record.state));
  if (p.written && db::record_json(*p.written) == db::record_json(record)) return;
  const auto file = db::user_file(paths::data_dir());
  try {
    Json all = Json::object();
    if (fs::exists(file))
      if (auto previous = paths::read_json(file); previous.is_object()) all = std::move(previous);
    all[p.package] = db::record_json(record);
    paths::atomic_write(file, all.dump(2) + "\n");
    p.written = record;
  } catch (const std::exception& e) {
    log::warn("eye-scale", "Cannot record the eye size: {}", e.what());
  }
}

void reset_for_tests() {
  auto& p = process();
  std::lock_guard lock(mutex);
  p = {};
}
} // namespace venture::xr::eye_scale
