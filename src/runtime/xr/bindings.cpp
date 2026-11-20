#include "xr/bindings.h"
#include "common/environment.h"
#include "common/json.h"
#include "common/log.h"
#include "common/paths.h"
#include "xr/host_xr.h"
#include "xr/menu_chord.h"
#include "xr/openvr_devices.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
namespace venture::xr {
namespace {
struct Controls {
  std::uint64_t generation = ~0ull;
  std::map<std::string, controls::Resolved, std::less<>> resolved;
  // By host profile, the controller whose player controls apply there: the host
  // profile, or the controller the host's evidence names (evident_profile).
  // Kept once known, so every binding of an instance follows one controller.
  std::map<std::string, std::string, std::less<>> evident;
};
std::mutex controls_mutex;
Controls& current() {
  static Controls controls;
  const auto generation = host_procedure_generation();
  if (controls.generation != generation) {
    controls.generation = generation;
    controls.resolved.clear();
    controls.evident.clear();
    const auto* text = venture::environment::getenv("VENTURE_CONTROLS");
    const auto config = controls::decode(text ? text : "");
    for (const auto& p : controls::kProfiles) {
      const auto it = config.find(p.path);
      controls.resolved.emplace(std::string(p.path), controls::resolve(p, it == config.end() ? nullptr : &it->second));
    }
    if (!config.empty()) log::info("xr", "Player controls for {} controller(s)", config.size());
  }
  return controls;
}
std::string path_string(XrInstance instance, XrPath path) {
  char value[XR_MAX_PATH_LENGTH]{};
  std::uint32_t size = 0;
  return path && XR_SUCCEEDED(::xrPathToString(instance, path, sizeof(value), &size, value)) ? value : "";
}
std::mutex models_mutex;
std::optional<std::array<std::string, 2>> models_override;
// The render models the host runtime gives the controllers (openvr_devices.h),
// read at most every two seconds: a query may reach the runtime's server.
std::array<std::string, 2> controller_models() {
  static std::array<std::string, 2> models;
  static std::chrono::steady_clock::time_point next{};
  std::lock_guard lock(models_mutex);
  if (models_override) return *models_override;
  const auto now = std::chrono::steady_clock::now();
  if (now >= next) {
    models = openvr_controller_render_models();
    next = now + std::chrono::seconds(2);
  }
  return models;
}
// khr/simple_controller has no remap: select and the poses.
std::optional<std::string> simple_binding(std::string_view path) {
  bool left = path.starts_with("/user/hand/left/");
  if (!left && !path.starts_with("/user/hand/right/")) return {};
  const auto component = path.substr(left ? 15 : 16);
  const std::string root(path.substr(0, left ? 15 : 16));
  if (component == "/input/trigger" || component == "/input/trigger/value" || component == "/input/trigger/click")
    return root + "/input/select/click";
  if (component == "/input/grip/pose" || component == "/input/aim/pose" || component == "/output/haptic")
    return std::string(path);
  return {};
}
}  // namespace

bool is_touch_profile(std::string_view p) {
  return p == "/interaction_profiles/oculus/touch_controller" ||
         p == "/interaction_profiles/facebook/touch_controller_pro" ||
         p == "/interaction_profiles/meta/touch_controller_plus";
}
bool host_dpad() {
  const auto& extensions = host_xr().enabled_extensions;
  return extensions.contains("XR_EXT_dpad_binding") && extensions.contains("XR_KHR_binding_modification");
}
std::optional<controls::Resolved> resolved_controls(std::string_view profile) {
  std::lock_guard lock(controls_mutex);
  auto& c = current();
  std::string_view key = profile;
  if (const auto evident = c.evident.find(profile); evident != c.evident.end()) {
    key = evident->second;
  } else if (controls::find_profile(profile)) {
    const auto models = controller_models();
    const auto& model = models[0].empty() ? models[1] : models[0];
    if (!model.empty()) {
      key = c.evident.emplace(std::string(profile), std::string(controls::evident_profile(profile, model))).first->second;
      if (key != profile) log::info("xr", "Player controls of {} apply on {} (render model {})", key, profile, model);
    }
  }
  const auto it = c.resolved.find(key);
  if (it == c.resolved.end()) return std::nullopt;
  return it->second;
}
std::vector<std::string> touch_bindings(std::string_view path, std::string_view profile) {
  if (profile == "/interaction_profiles/khr/simple_controller") {
    auto simple = simple_binding(path);
    if (!simple) return {};
    return {std::move(*simple)};
  }
  const auto* p = controls::find_profile(profile);
  const auto r = resolved_controls(profile);
  if (!p || !r) return {};
  return controls::host_bindings(path, *p, *r, host_dpad());
}
std::vector<std::string> pad_bindings(std::string_view profile, int hand, bool press) {
  std::vector<std::string> out;
  if (host_dpad()) return out;
  const auto* p = controls::find_profile(profile);
  const auto r = resolved_controls(profile);
  if (!p || !r) return out;
  for (const auto& pad : controls::pads(*p, *r, hand))
    out.push_back(std::string(controls::hand_root(hand)) + (press ? pad.press : pad.axes));
  return out;
}
std::array<bool, controls::kQuestCount> split_pad(std::string_view profile, int hand, XrVector2f at, bool pressed) {
  std::array<bool, controls::kQuestCount> out{};
  if (!pressed || host_dpad()) return out;
  const auto r = resolved_controls(profile);
  if (!r) return out;
  for (int q = 0; q < controls::kQuestCount; ++q)
    for (const auto& host : r->rows[std::size_t(hand)][std::size_t(q)])
      if (host.find("/dpad_", 7) != std::string::npos && controls::half_pressed(host, at.x, at.y)) out[std::size_t(q)] = true;
  return out;
}
namespace {
// `offset` placed before `pose` in an action space: the controller moved and turned, then `pose`.
XrPosef place(const controls::Offset& offset, const XrPosef& pose) {
  if (offset.zero()) return pose;
  const auto q = controls::orientation(offset);
  const XrQuaternionf a{q[0], q[1], q[2], q[3]}, b = pose.orientation;
  // v + 2w(u x v) + 2u x (u x v), rotating the pose's position into the moved controller.
  const XrVector3f u{a.x, a.y, a.z}, v = pose.position;
  const XrVector3f t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
  const XrVector3f moved{v.x + a.w * t.x + (u.y * t.z - u.z * t.y), v.y + a.w * t.y + (u.z * t.x - u.x * t.z),
                         v.z + a.w * t.z + (u.x * t.y - u.y * t.x)};
  XrPosef out;
  out.orientation = {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
  out.position = {offset.position[0] / 1000.f + moved.x, offset.position[1] / 1000.f + moved.y,
                  offset.position[2] / 1000.f + moved.z};
  return out;
}
}  // namespace
XrPosef offset_pose(std::string_view profile, int hand, const XrPosef& pose, bool grip) {
  const auto r = resolved_controls(profile);
  if (!r || hand < 0 || hand > 1) return pose;
  const auto placed = place(r->offsets[std::size_t(hand)], pose);
  return grip ? place(r->quest_grip[std::size_t(hand)], placed) : placed;
}
std::vector<std::string> touch_targets() {
  std::vector<std::string> targets{"/interaction_profiles/oculus/touch_controller"};
  const auto& extensions = host_xr().enabled_extensions;
  for (const auto& p : controls::kProfiles)
    if ((p.extension.empty() || extensions.contains(std::string(p.extension))) && p.path != targets.front())
      targets.emplace_back(p.path);
  // The simple controller last: a runtime picks it only when nothing else fits.
  targets.emplace_back("/interaction_profiles/khr/simple_controller");
  return targets;
}

XrResult suggest_translated(XrInstance instance, XrPath profile, std::vector<XrActionSuggestedBinding> bindings) {
  const auto name = path_string(instance, profile);
  const auto* p = controls::find_profile(name);
  XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  info.interactionProfile = profile;
  XrResult result = XR_ERROR_PATH_UNSUPPORTED;
  for (int step = 0; step < 3; ++step) {
    if (step) {
      // A host that does not know one of these paths refuses the whole profile.
      std::erase_if(bindings, [&](const XrActionSuggestedBinding& b) {
        const auto path = path_string(instance, b.binding);
        const auto slash = path.find("/input/");
        if (slash == std::string::npos) return false;
        const std::string_view component = std::string_view(path).substr(slash);
        if (component.ends_with("/pose")) return false;
        if (step == 1)
          return component.ends_with("/touch") && !component.ends_with("/thumbrest/touch") &&
                 !(p && controls::find_input(*p, component));
        if (!p) return false;
        std::string_view base = component;
        if (base.ends_with("/x") || base.ends_with("/y")) base.remove_suffix(2);
        return !controls::find_input(*p, base);
      });
    }
    info.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
    info.suggestedBindings = bindings.data();
    result = suggest_with_menu_chord(instance, &info);
    if (result != XR_ERROR_PATH_UNSUPPORTED) {
      log::info("xr", "Bindings for {}: {} suggested, result {}", name, bindings.size(), int(result));
      break;
    }
    log::warn("xr", "Host refused bindings for {} ({}); retrying with fewer inputs", name, step + 1);
  }
  return result;
}

void override_controller_models(std::optional<std::array<std::string, 2>> models) {
  std::lock_guard lock(models_mutex);
  models_override = std::move(models);
}

void report_controllers(XrInstance instance, XrPath left, XrPath right) {
  static std::mutex mutex;
  static std::string last;
  const std::array<std::string, 2> host{path_string(instance, left), path_string(instance, right)};
  // A runtime without a controller's own profile reports a general one; the
  // runtime's render model for the device names the controller itself.
  const auto models = host[0].empty() && host[1].empty() ? std::array<std::string, 2>{} : controller_models();
  const std::array<std::string, 2> shown{std::string(controls::evident_profile(host[0], models[0])),
                                         std::string(controls::evident_profile(host[1], models[1]))};
  {
    static std::string logged;
    auto now = host[0] + " | " + host[1];
    if (shown != host)
      now += " (reported as " + shown[0] + " | " + shown[1] + " from render models " + models[0] + " | " + models[1] + ")";
    std::lock_guard lock(mutex);
    if (std::exchange(logged, now) != now) log::info("xr", "Controllers: {}", now);
  }
  const auto* file = venture::environment::getenv("VENTURE_CONTROLLER_REPORT");
  if (!file || !*file) return;
  // Venture's scripted mock runtime has no controller to report.
  if (const auto* mode = venture::environment::getenv("VENTURE_XR_RUNTIME"); mode && std::string_view(mode) == "mock") return;
  const Json report{{"left", shown[0]},
                    {"right", shown[1]},
#ifdef _WIN32
                    {"pid", _getpid()},
#else
                    {"pid", getpid()},
#endif
                    {"time", std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch()).count()}};
  const auto key = report["left"].get<std::string>() + "|" + report["right"].get<std::string>();
  std::lock_guard lock(mutex);
  if (key == last || key == "|") return;
  last = key;
  const auto target = paths::from_utf8(file);
  auto temporary = target;
  temporary += ".tmp";
  std::error_code ec;
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << report.dump();
    if (!out) return;
  }
  std::filesystem::rename(temporary, target, ec);
  if (ec) log::debug("xr", "Controller report not written: {}", ec.message());
}
} // namespace venture::xr
