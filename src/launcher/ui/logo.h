// The Venture logo (assets/venture.svg, embedded at build time) rasterized
// on demand for the window icon and the sidebar brand, the white Steam glyph
// (assets/steam.svg) beside the launcher's Steam choices and the white Discord
// glyph (assets/discord.svg) on the Discord button and the white heart
// (assets/heart.svg) on the donate links.
#pragma once
#include <SDL3/SDL.h>

namespace venture::launcher::ui {
// A `pixels` x `pixels` RGBA surface owned by the caller, or null.
SDL_Surface* logo_surface(int pixels);
// The application icon (ui/app_icon.h): the logo on its bordered tile, for the window icon.
SDL_Surface* icon_surface(int pixels);
SDL_Surface* steam_logo_surface(int pixels);
SDL_Surface* discord_logo_surface(int pixels);
// The filled heart, or with `outline` the line heart (assets/heart-outline.svg) beside the sidebar's line icons.
SDL_Surface* heart_surface(int pixels, bool outline = false);
}  // namespace venture::launcher::ui
