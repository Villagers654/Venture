#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>
namespace venture::gfx {
struct AstcFormat {
  std::uint32_t block_width, block_height;
  bool srgb;
};
bool astc_format(std::uint32_t format, AstcFormat& out);
std::uint64_t astc_size(std::uint32_t format, std::uint32_t width,
                        std::uint32_t height, std::uint32_t layers);
// Packed 2D LDR blocks, including array slices. HDR/3D ASTC is not advertised.
// Thread-safe, bounded worker count. Throws on invalid sizes or decoder
// failure.
std::vector<std::uint8_t> decode_astc(
    std::uint32_t format, std::uint32_t width, std::uint32_t height,
    std::uint32_t layers, std::span<const std::uint8_t> blocks,
    const std::filesystem::path& cache_directory = {});
}  // namespace venture::gfx
