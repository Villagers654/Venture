// Native window chrome tweaks (dark title bar blended with the app background)
// and backend choice.
#pragma once
struct SDL_Window;
namespace venture::launcher {
void style_native_window(SDL_Window* window, unsigned char r, unsigned char g, unsigned char b);
// Before SDL_Init: the platform's native video backend (Wayland on Linux),
// unless the user chose one with SDL_VIDEO_DRIVER.
void prefer_native_video_driver();
// What to tell the user when SDL video cannot start.
const char* video_unavailable_message();
// Whether the launcher frames its own window (a borderless window with its
// own title strip and buttons): on Wayland, where decorations would otherwise
// need libdecor and its GTK/cairo plugins. Windows frames the window natively.
bool client_side_decorations();
}
