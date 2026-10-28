#pragma once

namespace venture::platform {
// Calls changed(context) from an OS thread whenever the host adds, removes,
// enables or disables an audio endpoint, changes a default endpoint or an
// endpoint property. Registers once for the process; false when the host has
// no endpoint notifications (callers then query every time).
bool watch_audio_endpoints(void (*changed)(void*), void* context);
} // namespace venture::platform
