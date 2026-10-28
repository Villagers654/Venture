#pragma once
#include <SDL3/SDL.h>

namespace venture::launcher::chrome {
// The launcher's own window chrome where the platform does not frame the
// window (window_style.h: client_side_decorations). Window coordinates.
struct Frame {
  int width = 0, height = 0;
  bool maximized = false;
  float bar = 0;            // title strip that moves the window
  float edge = 0;           // border that resizes it
  SDL_FRect controls{};     // minimize / maximize / close
  bool over_item = false;   // the pointer was on an interactive UI item
};
// What a press at `point` does: resize from an edge, move from the title
// strip, or reach the UI.
SDL_HitTestResult hit_test(const Frame& frame, SDL_Point point);
}  // namespace venture::launcher::chrome
