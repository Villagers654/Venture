#include "xr/grip_toggle.h"
#include "common/log.h"
#include "core/wait.h"
#include "xr/bindings.h"
#include "xr/clock.h"
#include "xr/host_xr.h"
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
namespace venture::xr {
namespace {
std::mutex mutex;
// The game's grip actions by the profile it suggested them for: the hands each binds a grip on.
std::map<XrPath, std::map<XrAction, unsigned>> suggested;
std::map<XrAction, unsigned> registered;
std::map<XrAction, unsigned> grips;  // all of the above together
struct Session {
  std::uint64_t sync = 0;
  XrTime time = 0;
  std::array<XrPath, 2> hands{};
  unsigned enabled = 0;  // hands whose host controller has the toggle on
  // Each action and subaction path the game read, and whether it is a boolean.
  struct Entry {
    GripLatch latch;
    bool boolean = false;
  };
  std::map<std::pair<XrAction, XrPath>, Entry> latches;
};
std::map<XrSession, Session> sessions;
std::string path_string(XrInstance instance, XrPath path) {
  char value[XR_MAX_PATH_LENGTH]{};
  std::uint32_t size = 0;
  return path && XR_SUCCEEDED(::xrPathToString(instance, path, sizeof(value), &size, value)) ? value : "";
}
// The hand a grip binding is on, or -1: "/user/hand/left/input/squeeze[/value|/click]".
int grip_hand(std::string_view path) {
  for (int hand = 0; hand < 2; ++hand) {
    const std::string_view root = hand ? "/user/hand/right" : "/user/hand/left";
    if (!path.starts_with(root)) continue;
    const auto component = path.substr(root.size());
    if (component == "/input/squeeze" || component == "/input/squeeze/value" || component == "/input/squeeze/click")
      return hand;
  }
  return -1;
}
void rebuild() {
  grips = registered;
  for (const auto& [profile, actions] : suggested)
    for (const auto& [action, hands] : actions) grips[action] |= hands;
}
// The latch for one read, or null where the toggle does not apply.
GripLatch* latch(XrSession session, const XrActionStateGetInfo* info, bool boolean, bool active, float raw) {
  const auto s = sessions.find(session);
  if (s == sessions.end() || !s->second.enabled) return nullptr;
  const auto g = grips.find(info->action);
  if (g == grips.end()) return nullptr;
  auto& state = s->second;
  const unsigned read = info->subactionPath == XR_NULL_PATH        ? 3u
                        : info->subactionPath == state.hands[0] ? 1u
                        : info->subactionPath == state.hands[1] ? 2u
                                                                 : 0u;
  if (!(g->second & read & state.enabled)) return nullptr;
  auto& entry = state.latches[{info->action, info->subactionPath}];
  entry.boolean = boolean;
  entry.latch.update(state.sync, raw, active, state.time);
  return &entry.latch;
}
}  // namespace

void grip_toggle_sync(XrSession session, std::uint64_t sync, const std::array<XrPath, 2>& profiles, bool refreshed) {
  std::lock_guard lock(mutex);
  auto& state = sessions[session];
  state.sync = sync;
  if (refreshed) {
    const auto instance = host_xr().instance;
    for (int hand = 0; hand < 2; ++hand)
      if (!state.hands[hand]) ::xrStringToPath(instance, hand ? "/user/hand/right" : "/user/hand/left", &state.hands[hand]);
    unsigned enabled = 0;
    for (int hand = 0; hand < 2; ++hand) {
      const auto profile = path_string(instance, profiles[hand]);
      const auto resolved = profile.empty() ? std::nullopt : resolved_controls(profile);
      if (resolved && resolved->grip_toggle) enabled |= 1u << hand;
    }
    if (enabled != state.enabled) {
      if (enabled) log::info("xr", "Grip toggle on ({})", enabled == 3 ? "both hands" : enabled == 1 ? "left" : "right");
      // A hand that leaves the toggle lets go.
      std::erase_if(state.latches, [&](const auto& entry) {
        const auto path = entry.first.second;
        const unsigned hands = path == XR_NULL_PATH ? 3u : path == state.hands[0] ? 1u : path == state.hands[1] ? 2u : 0u;
        return (hands & enabled) == 0;
      });
      state.enabled = enabled;
    }
  }
  if (!state.enabled) return;
  if (XR_FAILED(monotonic_to_time(venture::monotonic_ns(), &state.time))) state.time = 0;
  // Every grip the game has read sees every sync, even one it skips reading: a
  // press and release between two reads still flips the latch.
  for (auto& [key, entry] : state.latches) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, key.first, key.second};
    if (entry.boolean) {
      XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
      if (XR_SUCCEEDED(::xrGetActionStateBoolean(session, &info, &value)))
        entry.latch.update(sync, value.currentState ? 1.f : 0.f, value.isActive, state.time);
    } else {
      XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
      if (XR_SUCCEEDED(::xrGetActionStateFloat(session, &info, &value)))
        entry.latch.update(sync, value.currentState, value.isActive, state.time);
    }
  }
}
void remember_grip_bindings(XrInstance instance, XrPath profile, const XrInteractionProfileSuggestedBinding* info) {
  std::map<XrAction, unsigned> actions;
  for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
    const auto& binding = info->suggestedBindings[i];
    if (const int hand = grip_hand(path_string(instance, binding.binding)); hand >= 0)
      actions[binding.action] |= 1u << hand;
  }
  std::lock_guard lock(mutex);
  if (actions.empty()) suggested.erase(profile);
  else suggested[profile] = std::move(actions);
  rebuild();
}
void register_grip_action(XrAction action, unsigned hands) {
  std::lock_guard lock(mutex);
  registered[action] = hands;
  rebuild();
}
void forget_grip_action(XrAction action) {
  std::lock_guard lock(mutex);
  registered.erase(action);
  for (auto& [profile, actions] : suggested) actions.erase(action);
  for (auto& [session, state] : sessions)
    std::erase_if(state.latches, [&](const auto& entry) { return entry.first.first == action; });
  rebuild();
}
void reset_grip_session(XrSession session) {
  std::lock_guard lock(mutex);
  sessions.erase(session);
}
void grip_action_state(XrSession session, const XrActionStateGetInfo* info, XrActionStateFloat* value) {
  std::lock_guard lock(mutex);
  const auto* l = latch(session, info, false, value->isActive, value->currentState);
  if (!l || !value->isActive) return;
  value->currentState = l->held ? 1.f : 0.f;
  value->changedSinceLastSync = l->changed;
  if (l->time) value->lastChangeTime = l->time;
}
void grip_action_state(XrSession session, const XrActionStateGetInfo* info, XrActionStateBoolean* value) {
  std::lock_guard lock(mutex);
  const auto* l = latch(session, info, true, value->isActive, value->currentState ? 1.f : 0.f);
  if (!l || !value->isActive) return;
  value->currentState = l->held;
  value->changedSinceLastSync = l->changed;
  if (l->time) value->lastChangeTime = l->time;
}
}  // namespace venture::xr
