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
  // Wayland, named: without the hint SDL 3.2 would pick X11 on compositors
  // lacking wp_fifo_manager_v1 (a frame-pacing concern for games, not for this
  // UI). Inside gamescope, its Xwayland: gamescope closes an app's windows
  // (SteamVR's close on the app's panel, the Steam Frame's) only by sending
  // WM_DELETE_WINDOW to its X11 windows, and logs that it cannot close Wayland
  // ones, so on Wayland the close would do nothing. Wayland there still when
  // this SDL has no X11. A normal-priority hint yields to SDL_VIDEO_DRIVER
  // (tests use offscreen).
  const bool xwayland = environment::gamescope_xwayland();
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, xwayland ? "x11,wayland" : "wayland");
  // OpenGL through EGL from GLVND's libOpenGL, not libGL.so.1, which is GLX
  // and loads libGLX: on X11 too (SDL would use GLX there).
  SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, "libOpenGL.so.0");
  if (xwayland) SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
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
