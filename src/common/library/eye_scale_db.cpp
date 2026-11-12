#include "common/library/eye_scale_db.h"
#include "common/log.h"
#include "eye_scale_titles.h"
#include <algorithm>
#include <cmath>
#include <exception>

namespace venture::library::eye_scale {
namespace {
double area(Extent e) { return double(e.width) * double(e.height); }
bool valid(Extent e) { return e.width && e.height; }
// Equal within `tolerance` of the larger, per side.
bool near(Extent a, Extent b, double tolerance) {
  const auto side = [&](std::uint32_t x, std::uint32_t y) {
    return std::abs(double(x) - double(y)) <= tolerance * std::max(double(x), double(y));
  };
  return side(a.width, b.width) && side(a.height, b.height);
}
bool compensating(double applied) { return applied > 1 + 1e-4; }

Json extent_json(Extent e) { return Json::array({e.width, e.height}); }
Extent read_extent(const Json& j) {
  if (!j.is_array() || j.size() != 2 || !j[0].is_number_unsigned() || !j[1].is_number_unsigned()) return {};
  return {j[0].get<std::uint32_t>(), j[1].get<std::uint32_t>()};
}
} // namespace

std::string_view state_name(State s) {
  switch (s) {
  case State::native: return "native";
  case State::learned: return "learned";
  case State::confirmed: return "confirmed";
  case State::fixed: return "fixed";
  }
  return "native";
}
std::optional<State> parse_state(std::string_view s) {
  for (auto state : {State::native, State::learned, State::confirmed, State::fixed})
    if (s == state_name(state)) return state;
  return std::nullopt;
}

Record learn(const std::optional<Record>& previous, double applied, Extent told, Extent target, Extent rendered) {
  Record r;
  r.applied = applied;
  r.told = told;
  r.rendered = rendered;
  r.own = true;
  if (!valid(told) || !valid(rendered) || !valid(target)) {
    if (previous) r = *previous;
    return r;
  }
  const double relative = std::clamp(std::sqrt(area(rendered) / area(told)), kMinMultiplier, kMaxMultiplier);
  r.multiplier = relative;
  if (!compensating(applied)) {
    r.state = relative > 1 + kTolerance ? State::learned : State::native;
    return r;
  }
  // Compensated. A game whose eye did not move although the size it was told
  // did keeps a fixed size: compensation only shrinks its recommendation.
  if (previous && valid(previous->rendered) && std::abs(previous->applied - applied) > 1e-3 &&
      near(previous->rendered, rendered, 0.02) && !near(rendered, target, kTolerance)) {
    r.state = State::fixed;
    r.multiplier = previous->multiplier;
    return r;
  }
  r.state = near(rendered, target, kTolerance) ? State::confirmed : State::learned;
  return r;
}

double compensation_for(const Record& r) {
  if (r.state != State::learned && r.state != State::confirmed) return 1;
  return r.multiplier > 1 + kTolerance ? std::min(r.multiplier, kMaxMultiplier) : 1;
}

std::optional<Record> read_record(const Json& j, bool own) {
  if (!j.is_object()) return std::nullopt;
  const auto& m = field(j, "multiplier");
  if (!m.is_number() || !std::isfinite(m.get<double>()) || m.get<double>() < kMinMultiplier ||
      m.get<double>() > kMaxMultiplier)
    return std::nullopt;
  Record r;
  r.multiplier = m.get<double>();
  r.state = r.multiplier > 1 + kTolerance ? State::learned : State::native;
  if (const auto& s = field(j, "state"); s.is_string())
    if (const auto state = parse_state(s.get<std::string>())) r.state = *state;
  if (const auto& a = field(j, "applied"); a.is_number() && std::isfinite(a.get<double>()) && a.get<double>() >= 1)
    r.applied = a.get<double>();
  r.told = read_extent(field(j, "told"));
  r.rendered = read_extent(field(j, "rendered"));
  r.own = own;
  return r;
}

Json record_json(const Record& r) {
  return {{"multiplier", std::round(r.multiplier * 1000) / 1000},
          {"state", state_name(r.state)},
          {"applied", std::round(r.applied * 1000) / 1000},
          {"told", extent_json(r.told)},
          {"rendered", extent_json(r.rendered)}};
}

std::optional<Record> bundled(std::string_view package) {
  static const Json table = [] {
    try {
      return Json::parse(eye_scale_titles_json);
    } catch (const std::exception& e) {
      log::error("eye-scale", "Bundled title table unreadable: {}", e.what());
      return Json::object();
    }
  }();
  const auto& titles = field(table, "titles");
  if (!titles.is_object() || package.empty()) return std::nullopt;
  return read_record(field(titles, package), false);
}

fs::path user_file(const fs::path& data) { return data / "eye-scale.json"; }

std::optional<Record> user_record(const fs::path& data, std::string_view package) {
  if (package.empty()) return std::nullopt;
  try {
    if (!fs::exists(user_file(data))) return std::nullopt;
    return read_record(field(paths::read_json(user_file(data)), package), true);
  } catch (const std::exception& e) {
    log::warn("eye-scale", "Learned eye sizes unreadable: {}", e.what());
    return std::nullopt;
  }
}

std::optional<Record> record(const fs::path& data, std::string_view package) {
  if (auto own = user_record(data, package)) return own;
  return bundled(package);
}
} // namespace venture::library::eye_scale
