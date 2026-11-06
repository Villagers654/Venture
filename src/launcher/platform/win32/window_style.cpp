#include "launcher/window_style.h"

#include "common/environment.h"

#include <string_view>

#include <SDL3/SDL.h>
#include <dwmapi.h>
#include <windows.h>

namespace venture::launcher {
void style_native_window(SDL_Window* window, unsigned char r, unsigned char g, unsigned char b) {
  auto hwnd = static_cast<HWND>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (!hwnd) return;
  // Attribute ids are stable Windows 11 values; older systems ignore them.
  constexpr DWORD kImmersiveDarkMode = 20, kBorderColor = 34, kCaptionColor = 35, kTextColor = 36;
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, kImmersiveDarkMode, &dark, sizeof(dark));
  COLORREF caption = RGB(r, g, b), text = RGB(200, 206, 216);
  DwmSetWindowAttribute(hwnd, kCaptionColor, &caption, sizeof(caption));
  DwmSetWindowAttribute(hwnd, kBorderColor, &caption, sizeof(caption));
  DwmSetWindowAttribute(hwnd, kTextColor, &text, sizeof(text));
}
void prefer_native_video_driver() {}  // SDL's default is the native one
// VENTURE_CLIENT_SIDE_DECORATIONS=1 frames it like Linux, for layout checks.
bool client_side_decorations() {
  static const bool forced = [] {
    const char* value = environment::getenv("VENTURE_CLIENT_SIDE_DECORATIONS");
    return value && std::string_view(value) == "1";
  }();
  return forced;
}
const char* video_unavailable_message() { return "Could not initialize SDL video."; }
}  // namespace venture::launcher
