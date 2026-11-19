// What the VrApi core asks of the host it runs in. The core (vrapi.cpp,
// input.cpp, haptics.cpp, properties.cpp, layers.cpp, submit.h) is one code
// base on OpenXR for every host: the PC runtime, where the game is guest code
// and its graphics reach the runtime through Venture's renderer
// (runtime_host.cpp, swapchains.cpp), and libvrapi.so in games Venture
// installs on Android headsets, where the game renders natively on the
// device's runtime (src/android_game/vrapi). Each host links its own
// definitions; nothing here is chosen at run time.
#pragma once
#include <cstdint>
#include <string_view>

#include "hle/vrapi/abi.h"
#include "xr/api.h"

namespace venture::hle::vrapi::host {
// Whether a game may initialize VrApi for Vulkan (GLES is always served).
bool vulkan_available();
// The swapchain images exist: texture arrays and sRGB layer sources.
bool images_available();
// VRAPI_SYS_PROP_DEVICE_TYPE of the identity the game sees.
std::int32_t device_type();
// The game declares hand tracking required (its manifest).
bool hand_tracking_required();
// A Vulkan extension the runtime requires that the game may enable.
bool vulkan_extension_usable(std::string_view name);
// Creates the session (xr::session(), owner vrapi) for the graphics the game
// initialized VrApi with (state().init), bound to its EnterVrMode parameters.
XrResult create_session(const abi::Mode &);
// The session lasts from the game's first swapchain until Shutdown, because
// the swapchain images are the runtime's own (Android): Leave/EnterVrMode keep
// it with its spaces and input. False: each EnterVrMode makes a session (PC).
bool keeps_session();
// After EnterVrMode succeeded, with no VrApi lock held.
void vr_mode_entered();
// Blocks the calling thread until the session's frame epoch is no longer
// `epoch` or the monotonic `deadline` (ns; -1: none) passes. Called with no
// lock held. False when this thread cannot wait for frames at all.
bool wait_frame_change(std::uint32_t epoch, std::int64_t deadline);
}  // namespace venture::hle::vrapi::host
