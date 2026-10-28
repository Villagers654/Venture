#pragma once
#include <windows.h>
#include <mmdeviceapi.h>

namespace venture::platform {
// Registers the endpoint client through register_client instead of the system
// MMDeviceEnumerator (test seam). The client lives for the process once
// registered and is released when register_client returns false.
using RegisterEndpointClient = bool (*)(IMMNotificationClient* client);
bool watch_audio_endpoints(void (*changed)(void*), void* context, RegisterEndpointClient register_client);
} // namespace venture::platform
