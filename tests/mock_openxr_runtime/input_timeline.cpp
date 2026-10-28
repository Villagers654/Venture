#include "input_timeline.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace mock {
namespace {
using Json = nlohmann::json;
std::array<XrPosef*, 7> pose_fields(MockXrScript& s) {
  return {&s.head, &s.left.aim, &s.left.grip, &s.right.aim, &s.right.grip,
          &s.left_hand, &s.right_hand};
}
XrPosef interpolate_pose(XrPosef a, XrPosef b, float t) {
  auto& q = a.orientation;
  auto r = b.orientation;
  float dot = q.x*r.x + q.y*r.y + q.z*r.z + q.w*r.w;
  if (dot < 0) { r = {-r.x,-r.y,-r.z,-r.w}; dot = -dot; }
  dot = std::clamp(dot, 0.0f, 1.0f);
  float u = 1-t, v = t;
  if (dot < 0.9995f) {
    const float angle = std::acos(dot), divisor = std::sin(angle);
    u = std::sin((1-t)*angle)/divisor;
    v = std::sin(t*angle)/divisor;
  }
  XrQuaternionf rotation{u*q.x+v*r.x,u*q.y+v*r.y,u*q.z+v*r.z,u*q.w+v*r.w};
  const float length = std::sqrt(rotation.x*rotation.x+rotation.y*rotation.y+
                                 rotation.z*rotation.z+rotation.w*rotation.w);
  rotation = {rotation.x/length,rotation.y/length,rotation.z/length,rotation.w/length};
  return {rotation, {a.position.x+t*(b.position.x-a.position.x),
                     a.position.y+t*(b.position.y-a.position.y),
                     a.position.z+t*(b.position.z-a.position.z)}};
}
float number(const Json& value, float low, float high) {
  const auto n = value.get<float>();
  if (!std::isfinite(n) || n < low || n > high)
    throw std::runtime_error("mock input value outside valid range");
  return n;
}
void pose(const Json& value, XrPosef& out) {
  if (!value.is_object()) throw std::runtime_error("mock input pose must be an object");
  for (const auto& [key, v] : value.items()) {
    if (key == "position" && v.is_array() && v.size() == 3) {
      out.position = {number(v[0], -1000, 1000), number(v[1], -1000, 1000), number(v[2], -1000, 1000)};
    } else if (key == "orientation" && v.is_array() && v.size() == 4) {
      XrQuaternionf q{number(v[0], -1, 1), number(v[1], -1, 1), number(v[2], -1, 1), number(v[3], -1, 1)};
      const auto length = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
      if (length < 0.001f) throw std::runtime_error("mock input zero quaternion");
      out.orientation = {q.x/length, q.y/length, q.z/length, q.w/length};
    } else throw std::runtime_error("invalid mock input pose member: " + key);
  }
}
void controller(const Json& value, MockXrController& out, bool left) {
  if (!value.is_object()) throw std::runtime_error("mock controller must be an object");
  for (const auto& [key, v] : value.items()) {
    if (key == "aim") pose(v, out.aim);
    else if (key == "grip") pose(v, out.grip);
    else if (key == "trigger") out.trigger = number(v, 0, 1);
    else if (key == "squeeze") out.squeeze = number(v, 0, 1);
    else if (key == "thumbstick" && v.is_array() && v.size() == 2)
      out.thumbstick = {number(v[0], -1, 1), number(v[1], -1, 1)};
    else if (key == (left ? "x" : "a")) out.primary = v.get<bool>();
    else if (key == (left ? "y" : "b")) out.secondary = v.get<bool>();
    else if (key == (left ? "x_touch" : "a_touch")) out.primary_touch = v.get<bool>();
    else if (key == (left ? "y_touch" : "b_touch")) out.secondary_touch = v.get<bool>();
    else if (key == "menu") out.menu = v.get<bool>();
    else if (key == "thumbstick_click") out.thumbstick_click = v.get<bool>();
    else if (key == "thumbstick_touch") out.thumbstick_touch = v.get<bool>();
    else if (key == "trigger_touch") out.trigger_touch = v.get<bool>();
    else if (key == "thumbrest_touch") out.thumbrest_touch = v.get<bool>();
    else if (key == "active") out.active = v.get<bool>();
    else throw std::runtime_error("unknown mock controller member: " + key);
  }
}
void hand_joints(const Json& value, std::optional<std::array<XrPosef, 26>>& out) {
  if (value.is_null()) {
    out.reset();
    return;
  }
  if (!value.is_array() || value.size() != 26)
    throw std::runtime_error("mock hand requires 26 wrist-local joint poses");
  std::array<XrPosef, 26> parsed{};
  for (std::size_t joint = 0; joint < parsed.size(); ++joint) {
    if (!value[joint].contains("position") || !value[joint].contains("orientation"))
      throw std::runtime_error("mock hand joint requires position and orientation");
    pose(value[joint], parsed[joint]);
  }
  out = parsed;
}
}
void InputTimeline::load(const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream) throw std::runtime_error("cannot open mock input timeline: " + path.string());
  const auto root = Json::parse(stream);
  InputTimeline parsed;
  parsed.profile_ = root.value("profile", parsed.profile_);
  if (parsed.profile_ != "/interaction_profiles/oculus/touch_controller" &&
      parsed.profile_ != "/interaction_profiles/valve/index_controller" &&
      parsed.profile_ != "/interaction_profiles/htc/vive_controller" &&
      parsed.profile_ != "/interaction_profiles/khr/simple_controller")
    throw std::runtime_error("unsupported mock controller profile");
  const auto& events = root.at("events");
  if (!events.is_array()) throw std::runtime_error("mock events must be an array");
  MockXrScript state;
  state.trigger = 0;
  state.hand_tracking_active = false;
  parsed.events_.push_back({0, state});
  auto fields = pose_fields(state);
  for (std::size_t i=0; i<fields.size(); ++i)
    parsed.poses_[i].push_back({0, *fields[i], false});
  double previous = 0;
  for (const auto& event : events) {
    const auto time = event.at("time").get<double>();
    if (!std::isfinite(time) || time < previous)
      throw std::runtime_error("mock input times must be finite, nonnegative and ordered");
    const auto interpolation = event.value("pose_interpolation", std::string("step"));
    std::optional<Gate> gate;
    if (const auto wait = event.find("wait"); wait != event.end()) {
      if (wait->is_boolean()) { if (wait->get<bool>()) gate = Gate{}; }
      else if (wait->is_object()) {
        gate = Gate{};
        for (const auto& [key, value] : wait->items()) {
          if (key == "projection") {
            const auto projection = value.get<std::string>();
            if (projection != "first" && projection != "last")
              throw std::runtime_error("mock wait projection must be first or last");
            gate->last_projection = projection == "last";
            continue;
          }
          if (key == "match") {
            // One signature (96 numbers) or a list of them, any of which counts.
            const bool list = value.is_array() && !value.empty() && value[0].is_array();
            if (!value.is_array() || (!list && value.size() != 96))
              throw std::runtime_error("mock wait match must hold 96 numbers or a list of 96-number signatures");
            for (const auto& signature : list ? value : Json::array({value})) {
              if (!signature.is_array() || signature.size() != 96) throw std::runtime_error("mock wait match must hold 96 numbers");
              for (const auto& v : signature) {
                const auto x = v.get<double>();
                if (!std::isfinite(x) || x < 0 || x > 1) throw std::runtime_error("mock wait match values must lie in [0, 1]");
                gate->match.push_back(float(x));
              }
            }
            continue;
          }
          const auto number = value.get<double>();
          if (!std::isfinite(number) || number < 0) throw std::runtime_error("mock wait values must be finite and nonnegative");
          if (key == "change") gate->change = number;
          else if (key == "still") gate->still = number;
          else if (key == "stable") gate->stable = number;
          else if (key == "timeout") gate->timeout = number;
          else if (key == "within") gate->within = number;
          else if (key == "skip") gate->skip = number;
          else throw std::runtime_error("unknown mock wait member: " + key);
        }
      } else throw std::runtime_error("mock wait must be true or an object");
    }
    if (interpolation != "step" && interpolation != "linear")
      throw std::runtime_error("mock pose_interpolation must be step or linear");
    for (const auto& [key, value] : event.items()) {
      if (key == "time" || key == "pose_interpolation" || key == "wait") continue;
      if (key == "head") pose(value, state.head);
      else if (key == "hand_tracking_active") state.hand_tracking_active = value.get<bool>();
      else if (key == "left_hand") pose(value, state.left_hand);
      else if (key == "right_hand") pose(value, state.right_hand);
      else if (key == "left_hand_joints") hand_joints(value, state.left_hand_joints);
      else if (key == "right_hand_joints") hand_joints(value, state.right_hand_joints);
      else if (key == "left") controller(value, state.left, true);
      else if (key == "right") controller(value, state.right, false);
      else throw std::runtime_error("unknown mock event member: " + key);
    }
    const std::array<bool, 7> explicit_pose{
      event.contains("head"),
      event.contains("left") && event["left"].contains("aim"),
      event.contains("left") && event["left"].contains("grip"),
      event.contains("right") && event["right"].contains("aim"),
      event.contains("right") && event["right"].contains("grip"),
      event.contains("left_hand"), event.contains("right_hand")};
    for (std::size_t i=0; i<fields.size(); ++i)
      if (explicit_pose[i]) parsed.poses_[i].push_back({time, *fields[i], interpolation=="linear"});
    parsed.events_.push_back({time, state, gate});
    previous = time;
  }
  if (const auto watches = root.find("watches"); watches != root.end()) {
    if (!watches->is_array()) throw std::runtime_error("mock watches must be an array");
    for (const auto& item : *watches) {
      Watch watch;
      for (const auto& [key, value] : item.items()) {
        if (key == "match") {
          if (!value.is_array() || value.empty()) throw std::runtime_error("mock watch match must list signatures");
          for (const auto& signature : value) {
            if (!signature.is_array() || signature.size() != 96) throw std::runtime_error("mock watch signatures must hold 96 numbers");
            SceneSignature parsed_signature{};
            for (std::size_t i = 0; i < 96; ++i) parsed_signature[i] = number(signature[i], 0, 1);
            watch.match.push_back(parsed_signature);
          }
        } else if (key == "press") {
          if (!value.is_array() || value.empty()) throw std::runtime_error("mock watch press must list events");
          double last = 0;
          for (const auto& event : value) {
            const auto time = event.at("time").get<double>();
            if (!std::isfinite(time) || time < last) throw std::runtime_error("mock watch press times must be finite, nonnegative and ordered");
            MockXrScript check;
            for (const auto& [member, update] : event.items()) {
              if (member == "time") continue;
              if (member == "left") controller(update, check.left, true);
              else if (member == "right") controller(update, check.right, false);
              else throw std::runtime_error("mock watch press events update only left/right: " + member);
            }
            watch.press.emplace_back(time, event.dump());
            last = time;
          }
        } else {
          const auto x = value.get<double>();
          if (!std::isfinite(x) || x < 0) throw std::runtime_error("mock watch values must be finite and nonnegative");
          if (key == "within") watch.within = x;
          else if (key == "stable") watch.stable = x;
          else if (key == "from") watch.from = x;
          else if (key == "until") watch.until = x;
          else if (key == "cooldown") watch.cooldown = x;
          else throw std::runtime_error("unknown mock watch member: " + key);
        }
      }
      if (watch.match.empty() || watch.press.empty()) throw std::runtime_error("mock watch needs match and press");
      parsed.watches_.push_back(std::move(watch));
    }
  }
  // An edit replays from the start: keep the route clock and the holds already passed.
  parsed.offset_ = offset_;
  parsed.opened_until_ = opened_until_;
  parsed.skipped_ = skipped_;
  *this = std::move(parsed);
}
bool InputTimeline::advance(double wall_seconds, MockXrScript& out) {
  bool changed = false;
  if (holding_) offset_ = wall_seconds - events_[next_].time;
  auto seconds = wall_seconds - offset_;
  while (next_ < events_.size() && events_[next_].time <= seconds) {
    if (events_[next_].gate && events_[next_].time > opened_until_) {
      holding_ = true;
      offset_ = wall_seconds - events_[next_].time;
      seconds = events_[next_].time;
      break;
    }
    if (events_[next_].time > skipped_.first && events_[next_].time <= skipped_.second) { ++next_; continue; }
    const auto period = out.display_period;
    out = events_[next_++].state;
    out.display_period = period;
    changed = true;
  }
  auto fields = pose_fields(out);
  for (std::size_t i=0; i<fields.size(); ++i) {
    const auto& track = poses_[i];
    if (track.empty()) continue;
    auto next = std::upper_bound(track.begin(), track.end(), seconds,
                                [](double t, const PoseKey& k) { return t < k.time; });
    if (next == track.begin()) continue;
    const auto& before = *std::prev(next);
    auto sampled = before.pose;
    if (next != track.end() && next->interpolate && next->time > before.time)
      sampled = interpolate_pose(before.pose, next->pose,
                                 float((seconds-before.time)/(next->time-before.time)));
    *fields[i] = sampled;
  }
  return changed;
}
bool WatchRunner::sampling(const std::vector<Watch>& watches, double route_seconds) const {
  if (active_ >= 0) return false;
  return std::any_of(watches.begin(), watches.end(), [&](const Watch& w) {
    return route_seconds >= w.from && route_seconds < w.until;
  });
}
void WatchRunner::restore(MockXrScript& script) const {
  if (active_ < 0) return;
  script.left = route_left_;
  script.right = route_right_;
}
int WatchRunner::update(const std::vector<Watch>& watches, double route_seconds, double seconds,
                        const SceneSignature* signature, const SceneSignature* alternate,
                        MockXrScript& script) {
  still_since_.resize(watches.size(), -1);
  ready_at_.resize(watches.size(), 0);
  int fired = -1;
  if (active_ >= int(watches.size())) active_ = -1;  // an edit removed it
  if (active_ < 0) {
    for (std::size_t i = 0; i < watches.size() && fired < 0; ++i) {
      const auto& w = watches[i];
      if (route_seconds < w.from || route_seconds >= w.until || seconds < ready_at_[i]) { still_since_[i] = -1; continue; }
      if (signature) {
        float distance = 1;
        for (const auto& match : w.match) {
          distance = std::min(distance, signature_distance(*signature, match));
          if (alternate) distance = std::min(distance, signature_distance(*alternate, match));
        }
        if (distance > w.within) still_since_[i] = -1;
        else if (still_since_[i] < 0) still_since_[i] = seconds;
      }
      if (still_since_[i] >= 0 && seconds - still_since_[i] >= w.stable) {
        fired = active_ = int(i);
        fired_ = seconds;
        step_ = 0;
        left_ = script.left;
        right_ = script.right;
        still_since_[i] = -1;
      }
    }
  }
  if (active_ < 0) return fired;
  const auto& w = watches[std::size_t(active_)];
  route_left_ = script.left;
  route_right_ = script.right;
  for (; step_ < w.press.size() && w.press[step_].first <= seconds - fired_; ++step_) {
    const auto event = Json::parse(w.press[step_].second);
    if (const auto left = event.find("left"); left != event.end()) controller(*left, left_, true);
    if (const auto right = event.find("right"); right != event.end()) controller(*right, right_, false);
  }
  if (seconds - fired_ >= w.press.back().first + 0.2) {
    ready_at_[std::size_t(active_)] = seconds + w.cooldown;
    active_ = -1;
    return fired;
  }
  script.left = left_;
  script.right = right_;
  return fired;
}
std::optional<TimeWindow> parse_time_window(std::string_view text) {
  if (text.empty()) return std::nullopt;
  const auto colon = text.find(':');
  if (colon == text.npos) throw std::invalid_argument("time window must be begin:end");
  const auto number = [](std::string_view part) {
    std::size_t used = 0;
    const std::string value(part);
    const double result = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(result)) throw std::invalid_argument("time window bound");
    return result;
  };
  TimeWindow window{number(text.substr(0, colon)), number(text.substr(colon + 1))};
  if (window.begin < 0 || window.end <= window.begin) throw std::invalid_argument("time window must have 0 <= begin < end");
  return window;
}
}
