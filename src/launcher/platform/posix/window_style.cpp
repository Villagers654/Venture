#include "launcher/window_style.h"

#include <SDL3/SDL.h>

namespace venture::launcher {
void style_native_window(SDL_Window*, unsigned char, unsigned char, unsigned char) {}
void prefer_native_video_driver() {
  // Wayland only; SDL is built without X11, so there is no XWayland path.
  // Without the hint SDL 3.2 would skip Wayland on compositors lacking
  // wp_fifo_manager_v1 (a frame-pacing concern for games, not for this UI).
  // A normal-priority hint yields to SDL_VIDEO_DRIVER (tests use offscreen).
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
  // OpenGL through EGL from GLVND's libOpenGL, not libGL.so.1, which is GLX
  // (an X11 API) and loads libGLX and libX11.
  SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, "libOpenGL.so.0");
}
bool client_side_decorations() { return true; }
const char* video_unavailable_message() {
  return "The Venture launcher needs a Wayland session, and no Wayland display was reachable "
         "(check WAYLAND_DISPLAY and XDG_RUNTIME_DIR).";
}
}  // namespace venture::launcher
