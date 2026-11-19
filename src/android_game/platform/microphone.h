// SPDX-License-Identifier: GPL-3.0-or-later
// OVR_Microphone.h on the headset's own microphone (AAudio), for Venture's
// Platform loader in a game on an Android headset.
#pragma once
#include <span>

#include "hle/ovrplatform/hle.h"

namespace venture::ovrplatform {
std::span<const Export> native_microphone_exports();
}  // namespace venture::ovrplatform
