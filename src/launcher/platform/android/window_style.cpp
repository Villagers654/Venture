// SPDX-License-Identifier: GPL-3.0-or-later
// Android: the headset shows the launcher as a panel with its own frame, so
// the launcher draws no title bar; SDL's Android video driver is the only one.
#include "launcher/window_style.h"

#include <SDL3/SDL.h>

namespace venture::launcher {
void style_native_window(SDL_Window*, unsigned char, unsigned char, unsigned char) {}
void prefer_native_video_driver() {
  // Keep the panel's own orientation; the SDL default would lock it.
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown");
  // The back button and the controller's menu button stay the system's.
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
}
bool client_side_decorations() { return false; }
const char* video_unavailable_message() { return "The Venture launcher could not open its window."; }
}  // namespace venture::launcher
