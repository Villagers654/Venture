#pragma once
// The fixed controller layer. A game's actions never reach the host runtime by
// name for the Quest controllers it binds: Venture gives the host one fixed
// action set ("venture_controls", one action per Touch input), bound on every
// host controller under the player's controls (xr/bindings.h), and works out
// each game action from the game's own Touch bindings. A runtime that keys
// custom bindings by action name (SteamVR) then keeps a player's binding for a
// game through every game update that renames or adds actions, the same in
// every game.
//
// What the fixed set cannot carry keeps a host action of the game's own name:
// bindings for a profile that is not a Quest controller (hand interaction, eye
// gaze) and a game's own bindings for the host's controller (an Index game
// that binds Index itself), combined with the fixed inputs as the host would.
#include "hle/openxr/state.h"
#include <vector>

namespace venture::hle::openxr::controls {
// The host actions a guest vibration action reaches for `info.subactionPath`:
// each fixed vibration input the game bound it to (per hand, or both hands at
// once), and its own host action where a profile Venture does not translate
// binds it. Empty: bound to nothing, a no-op.
std::vector<XrHapticActionInfo> haptic_targets(const XrHapticActionInfo& info);
// The game bound a vibration of this action on the hand (any hand for XR_NULL_PATH).
bool haptic_suggested(const XrHapticActionInfo& info);
// xrCreateActionSpace for a guest pose action.
XrResult create_action_space(XrSession session, const XrActionSpaceCreateInfo* info, XrSpace* out);
// The host space to locate for a guest space: an action space bound both to a
// fixed pose and to the game's own action follows whichever is live.
XrSpace located_space(XrSpace space);
// A guest action space is destroyed: its second host space goes too.
void destroy_space(XrSpace space);
// The host reported an interaction profile change.
void profiles_changed();
void reset_session(XrSession session);
// xrDestroyInstance: every host action set of the layer and the game's.
XrResult destroy_instance();
}  // namespace venture::hle::openxr::controls
