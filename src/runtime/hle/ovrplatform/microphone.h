#pragma once
#include <span>

#include "hle/ovrplatform/hle.h"

namespace venture::ovrplatform {
// OVR_Voip_LowLevel.h / OVR_Microphone.h on the default input. Built into
// venture_audio; the runtime passes them to register_library.
std::span<const Export> microphone_exports();
// Before the audio backend shuts down.
void close_microphones();
}  // namespace venture::ovrplatform
