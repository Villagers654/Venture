#include "gfx/texture_decode.h"

#include <astcenc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
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
namespace {
void check_astc_image(std::uint32_t f, std::uint32_t w, std::uint32_t h,
                      std::uint32_t layers,
                      std::span<const std::uint8_t> blocks) {
  AstcFormat format{};
  // Bounds also prevent corrupt cache files or malformed guest dimensions from
  // producing unbounded allocations before GL's own limit validation.
  if (!astc_format(f, format) || !w || !h || !layers || w > 32768 ||
      h > 32768 || layers > 2048 ||
      std::uint64_t(w) * h * layers > (1ull << 28) ||
      blocks.size() != astc_size(f, w, h, layers))
    throw std::invalid_argument("Invalid packed ASTC LDR image");
}
// A content-addressed cache file: the output's hash, then `size` bytes.
// produce() fills the output on a miss; cache failures only cost time.
std::vector<std::uint8_t> cached(
    const std::filesystem::path& cache_directory, const std::string& key,
    std::size_t size, std::size_t input_size,
    const std::function<void(std::vector<std::uint8_t>&)>& produce) {
  const bool trace = log::level() <= log::Level::debug;
  const auto begin = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  std::vector<std::uint8_t> output(size);
  std::filesystem::path cached;
  if (!cache_directory.empty()) {
    cached = paths::io_path(cache_directory / key);
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
            input_size, output.size(), std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - begin).count());
        return output;
      }
    }
  }
  produce(output);
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
      input_size, output.size(), std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - begin).count());
  return output;
}
void decode_into(std::uint32_t f, std::uint32_t w, std::uint32_t h,
                 std::uint32_t layers, std::span<const std::uint8_t> blocks,
                 std::vector<std::uint8_t>& output) {
  AstcFormat format{};
  astc_format(f, format);
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
}
// BC7 mode 6 (one subset, RGBA 7-bit endpoints with a p-bit each, 4-bit
// indices) as in the Vulkan HLE's shaders/bc7_encode.comp: principal-axis
// endpoints, nearest indices, one least-squares refit. Alpha weighs more than
// a color channel, and texels that are fully transparent (0) or opaque (255)
// decode exactly, since mode 6 shares each index between color and alpha.
namespace bc7 {
constexpr unsigned weights[16] = {0,  4,  9,  13, 17, 21, 26, 30,
                                  34, 38, 43, 47, 51, 55, 60, 64};
constexpr float weight[4] = {1, 1, 1, 16}, root_weight[4] = {1, 1, 1, 4};
using Vec = std::array<float, 4>;
using Endpoint = std::array<unsigned, 4>;
float weighted(const Vec& a, const Vec& b) {
  float sum = 0;
  for (int c = 0; c < 4; ++c) sum += a[c] * weight[c] * b[c];
  return sum;
}
struct Block {
  Vec texels[16];
  unsigned indices[16]{};
  Endpoint low{}, high{};  // 7-bit endpoints
  unsigned low_p = 0, high_p = 0;
  float min_alpha = 0, max_alpha = 0;
  // Nearest endpoint; alpha 0 needs p-bit 0 and alpha 255 p-bit 1.
  static void quantize(Vec v, int exact_alpha, Endpoint& q, unsigned& pbit) {
    for (auto& c : v) c = std::clamp(c, 0.0f, 255.0f);
    if (exact_alpha >= 0) v[3] = float(exact_alpha);
    float best = 1e30f;
    for (unsigned b = 0; b < 2; ++b) {
      if (exact_alpha >= 0 && b != unsigned(exact_alpha) / 255) continue;
      Endpoint c;
      Vec d;
      for (int i = 0; i < 4; ++i) {
        c[i] = unsigned(std::clamp(std::nearbyint((v[i] - float(b)) * 0.5f), 0.0f, 127.0f));
        d[i] = float(c[i] * 2 + b) - v[i];
      }
      if (float e = weighted(d, d); e < best) {
        best = e;
        q = c;
        pbit = b;
      }
    }
  }
  // The lower-alpha end takes the block's alpha 0, the higher its alpha 255.
  void set_endpoints(Vec a, Vec b) {
    if (a[3] > b[3]) std::swap(a, b);
    int exact_a = min_alpha == 0 ? 0 : max_alpha == 255 && a[3] >= 254.5f ? 255 : -1;
    int exact_b = max_alpha == 255 ? 255 : min_alpha == 0 && b[3] <= 0.5f ? 0 : -1;
    quantize(a, exact_a, low, low_p);
    quantize(b, exact_b, high, high_p);
  }
  // Nearest index per texel: the projection's rounding and its neighbours,
  // since the weights are close to, but not exactly, k * 64 / 15.
  void choose_indices() {
    unsigned e0[4], e1[4];
    Vec base, axis;
    for (int c = 0; c < 4; ++c) {
      e0[c] = low[c] * 2 + low_p;
      e1[c] = high[c] * 2 + high_p;
      base[c] = float(e0[c]);
      axis[c] = float(e1[c]) - base[c];
    }
    const float length2 = weighted(axis, axis);
    for (unsigned i = 0; i < 16; ++i) {
      Vec offset;
      for (int c = 0; c < 4; ++c) offset[c] = texels[i][c] - base[c];
      const float t = length2 > 0 ? weighted(offset, axis) / length2 : 0;
      const int guess = std::clamp(int(t * 15 + 0.5f), 0, 15);
      // A transparent or opaque texel takes only an index that keeps its alpha.
      const bool exact = (texels[i][3] == 0 || texels[i][3] == 255) && e0[3] != e1[3];
      int first = exact ? 0 : std::max(guess - 1, 0), last = exact ? 15 : std::min(guess + 1, 15);
      unsigned best = 0;
      float best_error = 1e30f;
      for (int pass = exact ? 0 : 1; pass < 2; ++pass) {
        for (int k = first; k <= last; ++k) {
          const unsigned w = weights[k];
          Vec d;
          for (int c = 0; c < 4; ++c)
            d[c] = float((e0[c] * (64 - w) + e1[c] * w + 32) >> 6) - texels[i][c];
          if (pass == 0 && d[3] != 0) continue;
          if (float e = weighted(d, d); e < best_error) {
            best_error = e;
            best = unsigned(k);
          }
        }
        if (best_error < 1e30f) break;
        first = std::max(guess - 1, 0);
        last = std::min(guess + 1, 15);
      }
      indices[i] = best;
    }
  }
};
void encode_block(const std::uint8_t* rgba, unsigned width, unsigned height,
                  unsigned bx, unsigned by, std::uint8_t* out) {
  Block b;
  // Texels past the edge repeat the edge, so partial blocks fit only real
  // colors. The principal axis is fitted in the weighted space.
  Vec mean{}, lo{1e30f, 1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f, -1e30f};
  for (unsigned i = 0; i < 16; ++i) {
    const unsigned x = std::min(bx * 4 + (i & 3), width - 1),
                   y = std::min(by * 4 + (i >> 2), height - 1);
    const auto* p = rgba + (std::size_t(y) * width + x) * 4;
    for (int c = 0; c < 4; ++c) {
      b.texels[i][c] = p[c];
      const float w = p[c] * root_weight[c];
      mean[c] += w;
      lo[c] = std::min(lo[c], w);
      hi[c] = std::max(hi[c], w);
    }
  }
  for (auto& c : mean) c *= 1.0f / 16;
  b.min_alpha = lo[3] / root_weight[3];
  b.max_alpha = hi[3] / root_weight[3];
  float covariance[4][4]{};
  for (unsigned i = 0; i < 16; ++i) {
    Vec d;
    for (int c = 0; c < 4; ++c) d[c] = b.texels[i][c] * root_weight[c] - mean[c];
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) covariance[r][c] += d[r] * d[c];
  }
  // Principal axis by power iteration, from the bounding-box diagonal.
  Vec axis;
  for (int c = 0; c < 4; ++c) axis[c] = hi[c] - lo[c];
  for (int i = 0; i < 6; ++i) {
    Vec next{};
    float scale = 0;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) next[r] += covariance[r][c] * axis[c];
      scale = std::max(scale, std::abs(next[r]));
    }
    if (scale == 0) break;
    for (int c = 0; c < 4; ++c) axis[c] = next[c] / scale;
  }
  float tmin = 0, tmax = 0;
  const float length2 = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] + axis[3] * axis[3];
  if (length2 > 0) {
    for (auto& c : axis) c /= std::sqrt(length2);
    tmin = 1e30f;
    tmax = -1e30f;
    for (unsigned i = 0; i < 16; ++i) {
      float t = 0;
      for (int c = 0; c < 4; ++c) t += (b.texels[i][c] * root_weight[c] - mean[c]) * axis[c];
      tmin = std::min(tmin, t);
      tmax = std::max(tmax, t);
    }
  }
  Vec e0, e1;
  for (int c = 0; c < 4; ++c) {
    e0[c] = (mean[c] + axis[c] * tmin) / root_weight[c];
    e1[c] = (mean[c] + axis[c] * tmax) / root_weight[c];
  }
  b.set_endpoints(e0, e1);
  b.choose_indices();
  // Least-squares endpoints for those indices.
  float aa = 0, ab = 0, bb = 0;
  Vec ac{}, bc{};
  for (unsigned i = 0; i < 16; ++i) {
    const float wb = float(weights[b.indices[i]]) / 64, wa = 1 - wb;
    aa += wa * wa;
    ab += wa * wb;
    bb += wb * wb;
    for (int c = 0; c < 4; ++c) {
      ac[c] += wa * b.texels[i][c];
      bc[c] += wb * b.texels[i][c];
    }
  }
  if (const float det = aa * bb - ab * ab; std::abs(det) > 1e-4f) {
    for (int c = 0; c < 4; ++c) {
      e0[c] = (bb * ac[c] - ab * bc[c]) / det;
      e1[c] = (aa * bc[c] - ab * ac[c]) / det;
    }
    b.set_endpoints(e0, e1);
    b.choose_indices();
  }
  // The first index's top bit is implicit zero: swap the endpoints otherwise.
  const bool swapped = b.indices[0] >= 8;
  const auto& q0 = swapped ? b.high : b.low;
  const auto& q1 = swapped ? b.low : b.high;
  std::uint64_t words[2]{};
  unsigned position = 0;
  auto put = [&](std::uint64_t value, unsigned count) {
    const unsigned w = position >> 6, o = position & 63;
    words[w] |= value << o;
    if (o + count > 64) words[w + 1] |= value >> (64 - o);
    position += count;
  };
  put(64, 7);
  for (int c = 0; c < 4; ++c) {
    put(q0[c], 7);
    put(q1[c], 7);
  }
  put(swapped ? b.high_p : b.low_p, 1);
  put(swapped ? b.low_p : b.high_p, 1);
  for (unsigned i = 0; i < 16; ++i)
    put(swapped ? 15 - b.indices[i] : b.indices[i], i == 0 ? 3 : 4);
  for (int i = 0; i < 16; ++i)
    out[i] = std::uint8_t(words[i >> 3] >> ((i & 7) * 8));
}
}  // namespace bc7
std::size_t bc7_size(std::uint32_t w, std::uint32_t h, std::uint32_t layers) {
  return std::size_t((w + 3) / 4) * ((h + 3) / 4) * layers * 16;
}
}  // namespace
std::vector<std::uint8_t> decode_astc(
    std::uint32_t f, std::uint32_t w, std::uint32_t h, std::uint32_t layers,
    std::span<const std::uint8_t> blocks,
    const std::filesystem::path& cache_directory) {
  check_astc_image(f, w, h, layers, blocks);
  return cached(cache_directory,
                std::format("astcenc-5.3.0-u8-v1-{}-{}-{}-{}-{}.rgba", f, w, h,
                            layers, digest::sha256(blocks)),
                static_cast<std::size_t>(w) * h * layers * 4, blocks.size(),
                [&](auto& output) { decode_into(f, w, h, layers, blocks, output); });
}
std::vector<std::uint8_t> encode_bc7(std::uint32_t w, std::uint32_t h,
                                     std::uint32_t layers,
                                     std::span<const std::uint8_t> rgba) {
  if (!w || !h || !layers ||
      rgba.size() != static_cast<std::size_t>(w) * h * layers * 4)
    throw std::invalid_argument("Invalid RGBA8 image");
  std::vector<std::uint8_t> output(bc7_size(w, h, layers));
  const unsigned columns = (w + 3) / 4, rows = (h + 3) / 4,
                 total = rows * layers;
  const unsigned workers =
      std::uint64_t(columns) * total <= 256
          ? 1
          : std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
  std::atomic<unsigned> next{0};
  // Each worker takes the next row of blocks, across all slices.
  auto job = [&] {
    for (unsigned row; (row = next++) < total;) {
      const auto* slice = rgba.data() + std::size_t(row / rows) * w * h * 4;
      auto* out = output.data() + std::size_t(row) * columns * 16;
      for (unsigned x = 0; x < columns; ++x)
        bc7::encode_block(slice, w, h, x, row % rows, out + x * 16);
    }
  };
  std::vector<std::jthread> threads;
  for (unsigned t = 1; t < workers; ++t) threads.emplace_back(job);
  job();
  threads.clear();
  return output;
}
std::vector<std::uint8_t> transcode_astc_bc7(
    std::uint32_t f, std::uint32_t w, std::uint32_t h, std::uint32_t layers,
    std::span<const std::uint8_t> blocks,
    const std::filesystem::path& cache_directory) {
  check_astc_image(f, w, h, layers, blocks);
  return cached(cache_directory,
                std::format("astcenc-5.3.0-bc7m6-v1-{}-{}-{}-{}-{}.bc7", f, w,
                            h, layers, digest::sha256(blocks)),
                bc7_size(w, h, layers), blocks.size(), [&](auto& output) {
                  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * layers * 4);
                  decode_into(f, w, h, layers, blocks, rgba);
                  output = encode_bc7(w, h, layers, rgba);
                });
}
}  // namespace venture::gfx
