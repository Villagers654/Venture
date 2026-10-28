// Engine SDK policy. Venture decides frame pacing features for the host it
// runs on (like any SteamVR or Monado application); titles learn them through
// their engine SDK's own entry points, never per-title code.
#pragma once

namespace venture {
class GuestThread;
}

namespace venture::engine_policy {

// Called on the engine thread when the guest creates its XR session.
// `space_warp`: XR_FB_space_warp is enabled for the guest instance, which
// Venture offers only when the host runtime provides it.
void on_xr_session(GuestThread& thread, bool space_warp);

// VrApi also hands cadence to the host's OpenXR wait. Recognized engine SDKs
// must not retain an independent mobile render cap on top of that wait.
void on_vrapi_session(GuestThread& thread);

// SVC #kSvcEntryHook: runs the hook installed at the instruction before PC and
// resumes the function. False when no hook is installed there.
bool entry_hook(GuestThread& thread);

}  // namespace venture::engine_policy
