#include "xr/haptic_output.h"
#include "xr/host_xr.h"
#include "core/av_telemetry.h"
#include <cmath>
#include <algorithm>
#include <bit>
#include <map>
#include <set>
#include <mutex>
#include <vector>
namespace venture::xr {
namespace {
std::mutex cache_mutex;
std::map<std::pair<XrSession, XrAction>, std::vector<XrPath>> cache;
std::map<std::pair<XrSession, XrPath>, XrPath> profiles;
std::map<XrAction, std::set<std::string>> suggested;  // action -> hands Venture bound to */output/haptic
// Path strings are fixed for an instance, and PCM titles query every frame; xrPathToString can be a
// host round trip. Only successful conversions are cached.
std::mutex path_mutex;
std::map<XrPath, std::string> path_names;
XrInstance path_instance{};
std::uint64_t path_generation{};
std::string path_name(XrPath path) {
  if (!path) return {};
  std::lock_guard lock(path_mutex);
  const auto generation = host_procedure_generation();
  if (path_instance != host_xr().instance || path_generation != generation) {
    path_names.clear();
    path_instance = host_xr().instance;
    path_generation = generation;
  }
  if (auto found = path_names.find(path); found != path_names.end()) return found->second;
  char text[XR_MAX_PATH_LENGTH]{};
  uint32_t count = 0;
  if (XR_FAILED(::xrPathToString(host_xr().instance, path, sizeof(text), &count, text))) return {};
  return path_names.emplace(path, text).first->second;
}
}
bool haptic_bound(XrSession session, const XrHapticActionInfo &info) {
  std::lock_guard lock(cache_mutex);
  auto key = std::pair{session, info.action};
  auto it = cache.find(key);
  if (it == cache.end()) {
    XrBoundSourcesForActionEnumerateInfo query{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
    query.action = info.action;
    uint32_t count = 0;
    if (XR_FAILED(::xrEnumerateBoundSourcesForAction(session, &query, 0, &count, nullptr))) return false;
    std::vector<XrPath> sources(count);
    if (count && XR_FAILED(::xrEnumerateBoundSourcesForAction(session, &query, count, &count, sources.data()))) return false;
    it = cache.emplace(key, std::move(sources)).first;
  }
  auto hand = path_name(info.subactionPath);
  if (info.subactionPath && hand.empty()) return false;
  for (auto source : it->second) {
    auto name = path_name(source);
    if (name.ends_with("/output/haptic") && (hand.empty() || name.starts_with(hand + "/"))) return true;
  }
  if (!it->second.empty()) return false;
  // SteamVR lists no bound sources for vibration outputs at all (an empty, successful answer even with Touch
  // bindings active), which would leave PCM titles with a 0 Hz sample rate and no haptics. With no list to go by,
  // a hand counts as bound when Venture suggested a haptic binding for this action on it and the host reports an
  // active interaction profile for that hand (it only does so once the attached bindings apply there).
  auto wanted = suggested.find(info.action);
  if (wanted == suggested.end()) return false;
  auto active = [&](XrPath hand_path) {
    if (!wanted->second.contains(path_name(hand_path))) return false;
    auto key = std::pair{session, hand_path};
    if (profiles.contains(key)) return true;
    // Only an active profile is cached: before focus SteamVR reports none, and Batman asked then, cached 0 Hz
    // and never vibrated. An unbound hand is asked again on the next query.
    XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (XR_FAILED(::xrGetCurrentInteractionProfile(session, hand_path, &profile)) ||
        profile.interactionProfile == XR_NULL_PATH)
      return false;
    profiles.emplace(key, profile.interactionProfile);
    return true;
  };
  if (info.subactionPath) return active(info.subactionPath);
  for (auto name : {"/user/hand/left", "/user/hand/right"}) {
    XrPath path;
    if (XR_SUCCEEDED(::xrStringToPath(host_xr().instance, name, &path)) && active(path)) return true;
  }
  return false;
}
void note_haptic_suggestions(XrInstance instance, const XrInteractionProfileSuggestedBinding &info) {
  std::lock_guard lock(cache_mutex);
  for (std::uint32_t i = 0; i < info.countSuggestedBindings; ++i) {
    char text[XR_MAX_PATH_LENGTH]{};
    uint32_t count = 0;
    if (XR_FAILED(::xrPathToString(instance, info.suggestedBindings[i].binding, sizeof(text), &count, text))) continue;
    std::string_view path(text);
    if (!path.ends_with("/output/haptic")) continue;
    suggested[info.suggestedBindings[i].action].emplace(path.substr(0, path.size() - std::string_view("/output/haptic").size()));
  }
}
bool haptic_suggested(const XrHapticActionInfo &info) {
  std::lock_guard lock(cache_mutex);
  auto wanted = suggested.find(info.action);
  if (wanted == suggested.end()) return false;
  return info.subactionPath ? wanted->second.contains(path_name(info.subactionPath)) : !wanted->second.empty();
}
void clear_haptic_bindings(XrSession session) {
  std::lock_guard lock(cache_mutex);
  std::erase_if(cache, [session](auto &entry) { return entry.first.first == session; });
  std::erase_if(profiles, [session](auto &entry) { return entry.first.first == session; });
}
void haptic_event(std::string_view api, XrSession session, const XrHapticActionInfo &info,
                  float amplitude, XrDuration duration, float frequency,
                  std::string_view call, XrResult result, nlohmann::json fields) {
  if (!av::enabled()) return;
  bool safe = !fields.is_object() || fields.value("host_query_safe", true);
  auto hand = safe ? path_name(info.subactionPath) : std::string{};
  nlohmann::json event{{"kind", "haptic"}, {"api", api},
    {"hand", hand.empty() ? (info.subactionPath ? "invalid" : "all") : hand}, {"amplitude", amplitude},
    {"duration_ns", duration}, {"frequency", frequency}, {"host_call", call},
    {"result", int(result)}, {"bound", safe && haptic_bound(session, info)},
    {"action_info_type", int(info.type)}, {"action_info_has_next", info.next != nullptr},
    {"action", reinterpret_cast<std::uintptr_t>(info.action)}};
  if (safe) {
    auto per_hand = info;
    for (auto name : {"left", "right"}) {
      auto root = std::string("/user/hand/") + name;
      if (XR_SUCCEEDED(::xrStringToPath(host_xr().instance, root.c_str(), &per_hand.subactionPath))) {
        if (!info.subactionPath) event["bound_hands"][name] = haptic_bound(session, per_hand);
        std::lock_guard lock(cache_mutex);
        auto key = std::pair{session, per_hand.subactionPath};
        auto it = profiles.find(key);
        if (it == profiles.end()) {
          XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
          auto r = ::xrGetCurrentInteractionProfile(session, per_hand.subactionPath, &profile);
          if (XR_FAILED(r)) event["profile_query_result"][name] = int(r);
          else if (profile.interactionProfile) it = profiles.emplace(key, profile.interactionProfile).first;
          else event["active_profiles"][name] = "";  // host reports no active profile yet; not cached
        }
        if (!event["active_profiles"].contains(name))
          event["active_profiles"][name] = it == profiles.end() ? "unknown" : path_name(it->second);
      }
    }
  }
  // Retain the scalar field consumed by the AV verdict tool. The per-hand
  // map also describes wildcard requests on hosts with different profiles.
  if (event.contains("active_profiles")) {
    if (hand == "/user/hand/left") event["profile"] = event["active_profiles"].value("left", "unknown");
    else if (hand == "/user/hand/right") event["profile"] = event["active_profiles"].value("right", "unknown");
    else if (event["active_profiles"].value("left", "unknown") == event["active_profiles"].value("right", "unknown"))
      event["profile"] = event["active_profiles"].value("left", "unknown");
  }
  if (fields.is_object()) event.update(fields);
  if (result != XR_SUCCESS) {
    auto error = event;
    error["kind"] = "haptic_error";
    av::emit(std::move(error));
  }
  av::emit(std::move(event));
}
XrResult apply_haptic(std::string_view api, XrSession session, const XrHapticActionInfo &info,
                      const XrHapticVibration &pulse, std::string_view stage) {
  // Some guest SDKs submit finite overdrive values. Preserve their intent at
  // the actuator's maximum strength while keeping native requests in range.
  // Negative and nonfinite amplitudes retain normal validation errors.
  auto translated = pulse;
  if (std::isfinite(translated.amplitude)) translated.amplitude = std::min(translated.amplitude, 1.f);
  auto result = ::xrApplyHapticFeedback(session, &info, reinterpret_cast<const XrHapticBaseHeader *>(&translated));
  if (av::enabled()) {
    nlohmann::json fields{{"payload_type", int(pulse.type)}, {"has_next", pulse.next != nullptr}, {"stage", stage}};
    fields.update({{"input_amplitude", pulse.amplitude},
                   {"amplitude_clamped", std::isfinite(pulse.amplitude) && pulse.amplitude > 1}});
    if (result != XR_SUCCESS) fields.update({{"amplitude_bits", std::bit_cast<uint32_t>(translated.amplitude)},
                                            {"input_amplitude_bits", std::bit_cast<uint32_t>(pulse.amplitude)},
                                            {"frequency_bits", std::bit_cast<uint32_t>(pulse.frequency)}});
    haptic_event(api, session, info, translated.amplitude, translated.duration, translated.frequency, "xrApplyHapticFeedback", result, std::move(fields));
  }
  return result;
}
XrResult stop_haptic(std::string_view api, XrSession session, const XrHapticActionInfo &info,
                    std::string_view stage, nlohmann::json fields) {
  auto result = ::xrStopHapticFeedback(session, &info);
  if (av::enabled()) {
    if (!fields.is_object()) fields = nlohmann::json::object();
    fields["stage"] = stage;
    haptic_event(api, session, info, 0, 0, 0, "xrStopHapticFeedback", result, std::move(fields));
  }
  return result;
}
}
