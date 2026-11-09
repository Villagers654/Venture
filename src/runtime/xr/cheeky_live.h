#pragma once
// Live Cheeky settings from Venture's runtime, which hosts Cheeky's Vulkan
// layer in its own process (CheekyFoveatedDLSSHost.dll, CheekyHost_Command).
//
// Apply in game: the launcher writes Cheeky's command format ("1\n<id>\nset\n"
// then Name=value lines) to CheekyFoveatedDLSS\venture-live-command.txt beside
// the runtime; tick() applies a new request and answers in
// venture-live-status.txt ("<id> 1" applied, "<id> 0" refused).
//
// Toggle NR (VENTURE_NR_THUMBSTICK=1): holding both thumbstick clicks for two
// seconds switches Cheeky's NrEnabled; both clicks must release before the next.
#include <cstdint>

namespace venture::xr::cheeky_live {
// Once per frame (xrEndFrame); polls the command file twice a second.
void tick();
// The thumbstick clicks are wanted (VENTURE_NR_THUMBSTICK=1).
bool sticks_wanted();
// Both thumbstick click states at a sync (menu_chord.cpp), `active` false when
// they are unavailable.
void note_sticks(std::int64_t now_ns, bool left, bool right, bool active);
// Cheeky NR as last set: at launch (VENTURE_CHEEKY_NR=1), then by commands.
bool nr_enabled();
}  // namespace venture::xr::cheeky_live
