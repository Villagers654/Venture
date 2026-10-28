#include "gfx/texture_decode.h"

#include <astcenc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "common/log.h"
#include "common/digest.h"
#include "common/paths.h"
#include "common/process.h"

namespace venture::gfx {
namespace {
constexpr std::array<std::array<unsigned, 2>, 14> block_sizes{{{4, 4},
                                                               {5, 4},
                                                               {5, 5},
                                                               {6, 5},
                                                               {6, 6},
                                                               {8, 5},
                                                               {8, 6},
                                                               {8, 8},
                                                               {10, 5},
                                                               {10, 6},
                                                               {10, 8},
                                                               {10, 10},
                                                               {12, 10},
                                                               {12, 12}}};
std::mutex cache_lock;
std::atomic<std::uint64_t> serial{0};
void check_astc(astcenc_error e) {
  if (e != ASTCENC_SUCCESS)
    throw std::runtime_error(astcenc_get_error_string(e));
}
struct Decoder {
  astcenc_context* context = nullptr;
  std::mutex lock;
  Decoder(AstcFormat format, unsigned workers) {
    astcenc_config config{};
    check_astc(astcenc_config_init(
        format.srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR,
        format.block_width, format.block_height, 1, ASTCENC_PRE_FASTEST,
        ASTCENC_FLG_DECOMPRESS_ONLY, &config));
    check_astc(astcenc_context_alloc(&config, workers, &context));
  }
  ~Decoder() { astcenc_context_free(context); }
};
std::shared_ptr<Decoder> decoder(AstcFormat format, unsigned workers) {
  static std::mutex lock;
  static std::map<std::tuple<unsigned, unsigned, bool, unsigned>,
                  std::shared_ptr<Decoder>>
      contexts;
  std::lock_guard guard(lock);
  auto& result =
      contexts[{format.block_width, format.block_height, format.srgb, workers}];
  if (!result) result = std::make_shared<Decoder>(format, workers);
  return result;
}
}  // namespace
bool astc_format(std::uint32_t format, AstcFormat& out) {
  bool srgb = format >= 0x93D0 && format <= 0x93DD;
  const auto index = format - (srgb ? 0x93D0 : 0x93B0);
  if (index >= block_sizes.size()) return false;
  out = {block_sizes[index][0], block_sizes[index][1], srgb};
  return true;
}
std::uint64_t astc_size(std::uint32_t f, std::uint32_t w, std::uint32_t h,
                        std::uint32_t layers) {
  AstcFormat format{};
  if (!astc_format(f, format)) return 0;
  const auto x =
      (std::uint64_t(w) + format.block_width - 1) / format.block_width;
  const auto y =
      (std::uint64_t(h) + format.block_height - 1) / format.block_height;
  if (x && y > UINT64_MAX / x / 16 / std::max(layers, 1u)) return 0;
  return x * y * layers * 16;
}
std::vector<std::uint8_t> decode_astc(
    std::uint32_t f, std::uint32_t w, std::uint32_t h, std::uint32_t layers,
    std::span<const std::uint8_t> blocks,
    const std::filesystem::path& cache_directory) {
  const bool trace = log::level() <= log::Level::debug;
  const auto begin = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  AstcFormat format{};
  // Bounds also prevent corrupt cache files or malformed guest dimensions from
  // producing unbounded allocations before GL's own limit validation.
  if (!astc_format(f, format) || !w || !h || !layers || w > 32768 ||
      h > 32768 || layers > 2048 ||
      std::uint64_t(w) * h * layers > (1ull << 28) ||
      blocks.size() != astc_size(f, w, h, layers))
    throw std::invalid_argument("Invalid packed ASTC LDR image");
  std::vector<std::uint8_t> output(static_cast<std::size_t>(w) * h * layers *
                                   4);
  std::filesystem::path cached;
  if (!cache_directory.empty()) {
    auto key = std::format("astcenc-5.3.0-u8-v1-{}-{}-{}-{}-{}", f, w, h,
                           layers, digest::sha256(blocks));
    cached = paths::io_path(cache_directory / (key + ".rgba"));
    std::lock_guard lock(cache_lock);
    std::ifstream input(cached, std::ios::binary);
    if (input) {
      std::string hash(64, '\0');
      input.read(hash.data(), 64);
      input.read(reinterpret_cast<char*>(output.data()),
                 static_cast<std::streamsize>(output.size()));
      if (input && input.peek() == std::char_traits<char>::eof() &&
          hash == digest::sha256(output)) {
        if (trace) log::debug("gfx", "Texture cache hit: input={} output={} elapsed_us={}",
            blocks.size(), output.size(), std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - begin).count());
        return output;
      }
    }
  }
  const unsigned workers =
      w * h <= 1024
          ? 1
          : std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
  auto context = decoder(format, workers);
  std::lock_guard decode_lock(context->lock);
  const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B,
                                ASTCENC_SWZ_A};
  const auto stride = blocks.size() / layers;
  for (unsigned layer = 0; layer < layers; ++layer) {
    void* slice = output.data() + static_cast<std::size_t>(layer) * w * h * 4;
    astcenc_image image{w, h, 1, ASTCENC_TYPE_U8, &slice};
    std::vector<astcenc_error> results(workers);
    std::vector<std::jthread> threads;
    auto job = [&](unsigned index) {
      results[index] = astcenc_decompress_image(
          context->context, blocks.data() + layer * stride, stride, &image,
          &swizzle, index);
    };
    for (unsigned t = 1; t < workers; ++t) threads.emplace_back(job, t);
    job(0);
    threads.clear();
    // Reset even after a corrupt block: cached contexts must remain reusable.
    auto reset = astcenc_decompress_reset(context->context);
    for (auto e : results) check_astc(e);
    check_astc(reset);
  }
  if (!cached.empty()) {
    std::lock_guard lock(cache_lock);
    std::error_code ec;
    std::filesystem::create_directories(cached.parent_path(), ec);
    if (!ec) {
      // Process ID and serial distinguish concurrent publishers.
      // Atomic rename means other processes never see a partially written mip.
      auto temporary = cached;
      temporary += std::format(
          ".{}.{}.{}.tmp", current_process_id(),
          std::chrono::steady_clock::now().time_since_epoch().count(),
          serial++);
      try {
        {
          std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
          auto hash = digest::sha256(output);
          out.write(hash.data(), 64);
          out.write(reinterpret_cast<const char*>(output.data()),
                    static_cast<std::streamsize>(output.size()));
          out.close();
          if (!out) throw std::runtime_error("Incomplete texture cache write");
        }
        paths::replace_file(temporary, cached);
      } catch (const std::exception& error) {
        log::debug("gfx", "Texture cache write unavailable: {}", error.what());
        std::filesystem::remove(temporary, ec);
      }
    }
  }
  if (trace) log::debug("gfx", "Texture cache miss: input={} output={} elapsed_us={}",
      blocks.size(), output.size(), std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - begin).count());
  return output;
}
}  // namespace venture::gfx
