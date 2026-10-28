// Build tool: rasterizes the application icon (ui/app_icon.h around assets/venture.svg) into the multi-size icon that venture.exe embeds,
// which Windows shows on the taskbar, in the Start menu and on shortcuts.
// usage: venture-make-icon <venture.svg> <venture.ico>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../ui/app_icon.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include <nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: venture-make-icon <venture.svg> <venture.ico>\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  std::string svg = venture::launcher::ui::app_icon_svg(
      std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
  NSVGimage* image = nsvgParse(svg.data(), "px", 96);  // nsvgParse edits its input
  NSVGrasterizer* raster = nsvgCreateRasterizer();
  if (!image || !raster || image->width <= 0 || image->height <= 0) {
    std::fprintf(stderr, "venture-make-icon: cannot read %s\n", argv[1]);
    return 1;
  }
  const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
  std::vector<std::vector<unsigned char>> pngs;
  for (int px : sizes) {
    std::vector<unsigned char> rgba(std::size_t(px) * px * 4);
    nsvgRasterize(raster, image, 0, 0, float(px) / std::max(image->width, image->height), rgba.data(), px, px, px * 4);
    int length = 0;
    unsigned char* png = stbi_write_png_to_mem(rgba.data(), px * 4, px, px, 4, &length);
    if (!png) return 1;
    pngs.emplace_back(png, png + length);
    std::free(png);
  }
  nsvgDeleteRasterizer(raster);
  nsvgDelete(image);

  // ICONDIR, one ICONDIRENTRY per size, then the images; icons may hold PNG data directly.
  std::vector<unsigned char> ico;
  auto u8 = [&](unsigned v) { ico.push_back(static_cast<unsigned char>(v)); };
  auto u16 = [&](unsigned v) { u8(v & 0xff); u8(v >> 8 & 0xff); };
  auto u32 = [&](std::uint32_t v) { u16(v & 0xffff); u16(v >> 16); };
  const auto count = unsigned(std::size(sizes));
  u16(0), u16(1), u16(count);
  std::uint32_t offset = 6 + 16 * count;
  for (unsigned i = 0; i < count; ++i) {
    u8(sizes[i] >= 256 ? 0 : sizes[i]), u8(sizes[i] >= 256 ? 0 : sizes[i]);  // 0 means 256
    u8(0), u8(0), u16(1), u16(32), u32(std::uint32_t(pngs[i].size())), u32(offset);
    offset += std::uint32_t(pngs[i].size());
  }
  for (auto& png : pngs) ico.insert(ico.end(), png.begin(), png.end());
  std::ofstream out(argv[2], std::ios::binary);
  out.write(reinterpret_cast<const char*>(ico.data()), std::streamsize(ico.size()));
  return out ? 0 : 1;
}
