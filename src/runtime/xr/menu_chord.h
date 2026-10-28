#pragma once
#include "xr/api.h"
#include <cstdint>
#include <string_view>
#include <vector>
namespace venture::xr {
// Holding both stick clicks for 0.5 s presses the game's menu and keeps it pressed until a click
// releases, so menu holds (height calibration, hold-to-skip) work. On by default for a hand
// whose actual native menu binding is absent after a focused sync. VENTURE_MENU_CHORD=off disables it and
// VENTURE_MENU_CHORD=stick-clicks enables it on every profile. Ordinary game clicks are preserved.
struct MenuChord {
  static constexpr std::int64_t threshold = 500'000'000;
  std::int64_t since = -1;
  bool release_needed = false;
  bool update(std::int64_t now, bool left, bool right, bool active) {
    if (!active || (since >= 0 && now < since)) { since = -1; release_needed = true; return false; }
    if (!left || !right) {
      // Releasing one click ends the press; both must release before the chord rearms.
      if (since >= 0 && now - since >= threshold) release_needed = true;
      since = -1;
      if (!left && !right) release_needed = false;
      return false;
    }
    if (release_needed) return false;
    if (since < 0) since = now;
    return now - since >= threshold;
  }
};
bool menu_chord_enabled();
// VENTURE_MENU_CHORD=stick-clicks: the chord on every profile, whether or not it has a menu.
bool menu_chord_forced();
// The host reported an interaction profile change; re-read the current profiles at next sync.
void note_menu_profiles_changed(XrSession);
void remember_menu_bindings(XrInstance, XrPath profile, const XrInteractionProfileSuggestedBinding*);
XrResult suggest_with_menu_chord(XrInstance, const XrInteractionProfileSuggestedBinding*);
XrResult attach_with_menu_chord(XrSession, const XrSessionActionSetsAttachInfo*);
XrResult sync_with_menu_chord(XrSession, const XrActionsSyncInfo*);
void register_menu_action(XrActionSet, XrAction);
void forget_menu_object(std::uintptr_t);
void reset_menu_session(XrSession);
void menu_action_state(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean*);
bool menu_chord_pulse(XrSession);
std::vector<XrPath> menu_bound_sources(XrSession, XrAction);
} // namespace venture::xr
