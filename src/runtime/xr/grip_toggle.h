#pragma once
#include "xr/api.h"
#include <array>
#include <cstdint>
namespace venture::xr {
// Grip toggle (common/controls.h, Choice::grip_toggle): Quest games treat the grip
// as hold-to-hold, which tires a hand on a controller whose grip only clicks (Vive
// wands). With the toggle on for the host controller of a hand, the game's grip
// actions (those it binds to a hand's /input/squeeze) report a latched state: one
// press holds, the next lets go. Off changes nothing.
//
// One latch per action and subaction path the game reads (per hand, or one for an
// action read without a hand). The raw grip presses past kPress and releases below
// kRelease; each new press flips the latch. A latch sees each xrSyncActions once,
// however often the game reads the state in between.
struct GripLatch {
  static constexpr float kPress = 0.5f, kRelease = 0.4f;
  bool held = false;       // what the game sees
  bool pressed = false;    // the raw grip
  bool changed = false;    // flipped on the sync last seen
  std::uint64_t sync = 0;  // the sync last seen
  XrTime time = 0;         // when it last flipped (0: never)
  bool update(std::uint64_t now_sync, float raw, bool active, XrTime now) {
    if (now_sync == sync) return held;
    sync = now_sync;
    changed = false;
    if (!active) {
      // No controller (or the game's action set is off): let go; the next press holds.
      if (held) held = false, changed = true, time = now;
      pressed = false;
      return held;
    }
    const bool was = pressed;
    pressed = was ? raw > kRelease : raw >= kPress;
    if (pressed && !was) held = !held, changed = true, time = now;
    return held;
  }
};
// After each successful host sync of `session` (`sync` counts them): the host
// controller per hand, re-read when `refreshed`.
void grip_toggle_sync(XrSession session, std::uint64_t sync, const std::array<XrPath, 2>& profiles, bool refreshed);
// A game's accepted suggestion for `profile` (it replaces that profile's earlier one).
void remember_grip_bindings(XrInstance instance, XrPath profile, const XrInteractionProfileSuggestedBinding* info);
// An action that is the grip on every profile (VrApi's own), on `hands` (bit 0 left, 1 right).
void register_grip_action(XrAction action, unsigned hands);
void forget_grip_action(XrAction action);
void reset_grip_session(XrSession session);
// The state the game reads, latched where the toggle applies.
void grip_action_state(XrSession session, const XrActionStateGetInfo* info, XrActionStateFloat* value);
void grip_action_state(XrSession session, const XrActionStateGetInfo* info, XrActionStateBoolean* value);
}  // namespace venture::xr
