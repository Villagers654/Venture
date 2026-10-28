#include "launcher/ui/logo.h"

#include <launcher/logo_svg.h>
#include "launcher/ui/app_icon.h"
#include <algorithm>
#include <string>

#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include <nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>

namespace venture::launcher::ui {
namespace {
SDL_Surface* rasterize(std::string source, int pixels) {  // nsvgParse edits its input
  if (pixels <= 0) return nullptr;
  NSVGimage* image = nsvgParse(source.data(), "px", 96);
  if (!image) return nullptr;
  SDL_Surface* surface = nullptr;
  if (image->width > 0 && image->height > 0)
    if (NSVGrasterizer* raster = nsvgCreateRasterizer()) {
      surface = SDL_CreateSurface(pixels, pixels, SDL_PIXELFORMAT_RGBA32);
      if (surface) {
        float scale = float(pixels) / std::max(image->width, image->height);
        nsvgRasterize(raster, image, 0, 0, scale, static_cast<unsigned char*>(surface->pixels), pixels, pixels,
                      surface->pitch);
      }
      nsvgDeleteRasterizer(raster);
    }
  nsvgDelete(image);
  return surface;
}
}  // namespace

SDL_Surface* logo_surface(int pixels) { return rasterize(kVentureLogoSvg, pixels); }
SDL_Surface* icon_surface(int pixels) { return rasterize(app_icon_svg(kVentureLogoSvg), pixels); }
SDL_Surface* steam_logo_surface(int pixels) { return rasterize(kSteamLogoSvg, pixels); }
SDL_Surface* discord_logo_surface(int pixels) { return rasterize(kDiscordLogoSvg, pixels); }
}  // namespace venture::launcher::ui
