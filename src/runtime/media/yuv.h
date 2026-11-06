#pragma once
#include <cstddef>
#include <cstdint>

namespace venture::media {
// Bytes of a planar YUV 4:2:0 image (full Y plane, then quarter-size U and V).
inline std::size_t yuv420_size(std::size_t width, std::size_t height) {
  return width * height + 2 * ((width + 1) / 2) * ((height + 1) / 2);
}
// RGBA (top-left origin) to planar YUV 4:2:0, BT.601 limited range as video
// decoders output: Y rows of `width`, U then V rows of (width + 1) / 2, each
// chroma sample the average of its 2x2 block.
inline void rgba_to_yuv420(const std::uint8_t* rgba, std::size_t width, std::size_t height, std::uint8_t* out) {
  const std::size_t cw = (width + 1) / 2, ch = (height + 1) / 2;
  auto* u = out + width * height;
  auto* v = u + cw * ch;
  auto at = [&](std::size_t x, std::size_t y) { return rgba + (y * width + x) * 4; };
  for (std::size_t y = 0; y < height; ++y)
    for (std::size_t x = 0; x < width; ++x) {
      const auto* c = at(x, y);
      out[y * width + x] = std::uint8_t((66 * c[0] + 129 * c[1] + 25 * c[2] + 128) / 256 + 16);
    }
  for (std::size_t y = 0; y < ch; ++y)
    for (std::size_t x = 0; x < cw; ++x) {
      int r = 0, g = 0, b = 0, n = 0;
      for (std::size_t dy = 0; dy < 2 && 2 * y + dy < height; ++dy)
        for (std::size_t dx = 0; dx < 2 && 2 * x + dx < width; ++dx, ++n) {
          const auto* c = at(2 * x + dx, 2 * y + dy);
          r += c[0];
          g += c[1];
          b += c[2];
        }
      r /= n;
      g /= n;
      b /= n;
      u[y * cw + x] = std::uint8_t((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
      v[y * cw + x] = std::uint8_t((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
    }
}
// NV12 (luma rows of `pitch`, then interleaved CbCr rows of `pitch` starting
// at row `rows`) to planar YUV 4:2:0 laid out as rgba_to_yuv420 writes it.
inline void nv12_to_yuv420(const std::uint8_t* nv12, std::size_t pitch, std::size_t rows, std::size_t width,
                           std::size_t height, std::uint8_t* out) {
  const std::size_t cw = (width + 1) / 2, ch = (height + 1) / 2;
  auto* u = out + width * height;
  auto* v = u + cw * ch;
  for (std::size_t y = 0; y < height; ++y)
    for (std::size_t x = 0; x < width; ++x) out[y * width + x] = nv12[y * pitch + x];
  const auto* chroma = nv12 + pitch * rows;
  for (std::size_t y = 0; y < ch; ++y)
    for (std::size_t x = 0; x < cw; ++x) {
      u[y * cw + x] = chroma[y * pitch + 2 * x];
      v[y * cw + x] = chroma[y * pitch + 2 * x + 1];
    }
}
} // namespace venture::media
