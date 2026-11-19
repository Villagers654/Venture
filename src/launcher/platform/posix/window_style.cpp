#include "launcher/window_style.h"
#include "common/environment.h"
#include "launcher/desktop_id.h"

#include <SDL3/SDL.h>
#include <string>
#include <string_view>
#include <unistd.h>

namespace venture::launcher {
namespace {
bool on_path(std::string_view name) {
  const char* path = environment::getenv("PATH");
  std::string_view dirs = path ? path : "";
  while (!dirs.empty()) {
    const auto colon = dirs.find(':');
    const auto dir = dirs.substr(0, colon);
    dirs = colon == std::string_view::npos ? std::string_view{} : dirs.substr(colon + 1);
    if (!dir.empty() && access((std::string(dir) + "/" + std::string(name)).c_str(), X_OK) == 0) return true;
  }
  return false;
}
}  // namespace
void style_native_window(SDL_Window*, unsigned char, unsigned char, unsigned char) {}
void prefer_native_video_driver() {
  // Wayland only; SDL is built without X11, so there is no XWayland path.
  // Without the hint SDL 3.2 would skip Wayland on compositors lacking
  // wp_fifo_manager_v1 (a frame-pacing concern for games, not for this UI;
  // gamescope has none). A normal-priority hint yields to SDL_VIDEO_DRIVER
  // (tests use offscreen).
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
  // OpenGL through EGL from GLVND's libOpenGL, not libGL.so.1, which is GLX
  // (an X11 API) and loads libGLX and libX11.
  SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, "libOpenGL.so.0");
  // The Wayland app_id: the desktop file's name, so docks and task bars show
  // its icon (SDL would use the executable's name, "venture").
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING, kDesktopId);
  // Inside gamescope the portal's file chooser opens in another process,
  // which gamescope does not show (environment::inside_gamescope); zenity
  // runs as Venture's child, so its dialog does.
  if (environment::inside_gamescope() && on_path("zenity")) SDL_SetHint(SDL_HINT_FILE_DIALOG_DRIVER, "zenity");
}
bool client_side_decorations() { return true; }
const char* video_unavailable_message() {
  return "The Venture launcher needs a Wayland session, and no Wayland display was reachable "
         "(check WAYLAND_DISPLAY, or GAMESCOPE_WAYLAND_DISPLAY in SteamOS's game mode, and XDG_RUNTIME_DIR).";
}
}  // namespace venture::launcher
