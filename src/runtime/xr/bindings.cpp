#include "xr/bindings.h"
#include "common/environment.h"
#include "common/json.h"
#include "common/log.h"
#include "common/paths.h"
#include "xr/host_xr.h"
#include "xr/menu_chord.h"
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
};
std::mutex controls_mutex;
Controls& current() {
  static Controls controls;
  const auto generation = host_procedure_generation();
  if (controls.generation != generation) {
    controls.generation = generation;
    controls.resolved.clear();
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
// khr/simple_controller has no remap: select and the poses.
std::optional<std::string> simple_binding(std::string_view path) {
  bool left = path.starts_with("/user/hand/left/");
  if (!left && !path.starts_with("/user/hand/right/")) return {};
  const auto component = path.substr(left ? 15 : 16);
  const std::string root(path.substr(0, left ? 15 : 16));
  if (component == "/input/trigger/value" || component == "/input/trigger/click") return root + "/input/select/click";
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
  const auto it = c.resolved.find(profile);
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
        if (step == 1) return component.ends_with("/touch") && !component.ends_with("/thumbrest/touch");
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

void report_controllers(XrInstance instance, XrPath left, XrPath right) {
  static std::mutex mutex;
  static std::string last;
  {
    static std::string logged;
    const auto now = path_string(instance, left) + " | " + path_string(instance, right);
    std::lock_guard lock(mutex);
    if (std::exchange(logged, now) != now) log::info("xr", "Controllers: {}", now);
  }
  const auto* file = venture::environment::getenv("VENTURE_CONTROLLER_REPORT");
  if (!file || !*file) return;
  const Json report{{"left", path_string(instance, left)},
                    {"right", path_string(instance, right)},
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
