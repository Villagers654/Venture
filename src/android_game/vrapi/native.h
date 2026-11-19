// SPDX-License-Identifier: GPL-3.0-or-later
// libvrapi.so for games Venture installs on Android headsets
// (docs/design/native-headsets.md, 5.4): the shared VrApi core
// (hle/vrapi) on the device's OpenXR runtime, with the game's own GLES
// context and the runtime's swapchain images. What its files share.
#pragma once
#include <jni.h>

#include <openxr/openxr.h>

#include <cstdint>

#include "hle/vrapi/abi.h"

namespace venture::vrapi_native {
// The game's Java VM and activity, from the first VrApi call that carries them
// (vrapi_Initialize, the property queries). The loader and the instance need them.
void remember_java(const hle::vrapi::abi::Java *);
JavaVM *java_vm();
jobject activity();
// The kernel's futex wait on a word of this process: 0, or -errno.
int futex_wait(const std::uint32_t *word, std::uint32_t expected, std::int64_t deadline_ns);
// The session swapchains are created in: made on the calling thread's current
// EGL context the first time (a game may create swapchains before EnterVrMode).
// The VrApi state lock is held.
XrResult ensure_session();
}  // namespace venture::vrapi_native
