#include "launcher/ui/chrome.h"

namespace venture::launcher::chrome {
SDL_HitTestResult hit_test(const Frame& f, SDL_Point point) {
  const auto x = float(point.x), y = float(point.y);
  if (!f.maximized) {
    const bool left = x < f.edge, right = x >= float(f.width) - f.edge;
    const bool top = y < f.edge, bottom = y >= float(f.height) - f.edge;
    if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (top) return SDL_HITTEST_RESIZE_TOP;
    if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
    if (left) return SDL_HITTEST_RESIZE_LEFT;
    if (right) return SDL_HITTEST_RESIZE_RIGHT;
  }
  const auto& c = f.controls;
  const bool on_controls = x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h;
  if (y < f.bar && !on_controls && !f.over_item) return SDL_HITTEST_DRAGGABLE;
  return SDL_HITTEST_NORMAL;
}
}  // namespace venture::launcher::chrome
