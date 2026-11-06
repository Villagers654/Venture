#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
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
// Tight RGBA8 slices to BC7 mode 6 blocks, slice by slice; the CPU twin of the
// Vulkan HLE's bc7_encode.comp.
std::vector<std::uint8_t> encode_bc7(std::uint32_t width, std::uint32_t height,
                                     std::uint32_t layers,
                                     std::span<const std::uint8_t> rgba);
// decode_astc stored as BC7 blocks (a quarter of the RGBA8 size), cached by the
// ASTC data like decode_astc.
std::vector<std::uint8_t> transcode_astc_bc7(
    std::uint32_t format, std::uint32_t width, std::uint32_t height,
    std::uint32_t layers, std::span<const std::uint8_t> blocks,
    const std::filesystem::path& cache_directory = {});
// Waits until every texture stored so far is in its cache directory's pack file.
void flush_texture_caches();
// transcode_astc_bc7 (or decode_astc) of a copied ASTC image on a background
// thread, so the caller need not wait for it.
struct TextureJob;
std::shared_ptr<TextureJob> start_astc_job(
    std::uint32_t format, std::uint32_t width, std::uint32_t height,
    std::uint32_t layers, std::vector<std::uint8_t> blocks,
    std::filesystem::path cache_directory, bool bc7);
bool astc_job_done(const TextureJob& job) noexcept;
// A job not started yet never runs.
void cancel_astc_job(TextureJob& job) noexcept;
// The output, waiting for it (or producing it here when no thread has started
// the job). Empty after a decoder failure, which is logged.
const std::vector<std::uint8_t>& finish_astc_job(TextureJob& job);
std::size_t astc_job_bytes(const TextureJob& job) noexcept;
}  // namespace venture::gfx
