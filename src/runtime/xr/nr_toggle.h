#pragma once
// Toggle NR (VENTURE_NR_THUMBSTICK=1): holding the controller shortcut (VENTURE_NR_SHORTCUT: both thumbstick
// clicks, or the left or right one) for VENTURE_NR_HOLD_MS switches the NR of the game's mode
// (VENTURE_NR_TARGET: "vdxr" for VDXR DLSS-NR, "venture" for Venture NR) through its per-user key; every click
// releases before the next. VRS stays on.
#include <cstdint>

namespace venture::xr::nr_toggle {
// The thumbstick clicks are wanted (VENTURE_NR_THUMBSTICK=1).
bool sticks_wanted();
// Both thumbstick click states at a sync (menu_chord.cpp), `active` false when they are unavailable.
void note_sticks(std::int64_t now_ns, bool left, bool right, bool active);
// NR as last set: on at launch (VENTURE_OVERLAY_NR), then as its key says (the overlay's NR).
bool nr_enabled();
}  // namespace venture::xr::nr_toggle
