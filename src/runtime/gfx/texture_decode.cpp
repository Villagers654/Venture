#include "gfx/texture_decode.h"

#include <astcenc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <latch>
#include <unordered_map>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "common/log.h"
#include "common/digest.h"
#include "common/file_ops.h"
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
// Decoded and transcoded textures of one cache directory live in one append-only
// pack file, written by a background thread. One file per mip cost a create,
// write, rename and close each, through every file system filter: The Walking
// Dead's level load spent 8 of its 10.5 s render-thread freeze there, against
// 0.7 s of decoding. Records are [key size, data size, data check, key check],
// the key, then the data. One process per directory writes (a file lock);
// others only read. Cache failures only cost time.
constexpr char kPackMagic[4] = {'V', 'T', 'X', '1'};
constexpr std::uint64_t kPackLimit = 4ull << 30;
constexpr std::uint64_t kPendingLimit = 512ull << 20;
struct PackHeader {
  std::uint32_t key_size, data_size, data_check, key_check;
};
static_assert(sizeof(PackHeader) == 16);
std::uint32_t pack_check(const std::uint8_t* p, std::size_t size) {
  // Four independent multiply lanes keep a large mip's check off the critical path.
  std::uint64_t lane[4] = {0x9e3779b97f4a7c15ull ^ size, 0xc2b2ae3d27d4eb4full,
                           0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
  std::size_t i = 0;
  for (; i + 32 <= size; i += 32)
    for (int k = 0; k < 4; ++k) {
      std::uint64_t w;
      std::memcpy(&w, p + i + 8 * k, 8);
      lane[k] = (lane[k] ^ w) * 0xff51afd7ed558ccdull;
      lane[k] ^= lane[k] >> 32;
    }
  std::uint64_t h = lane[0] ^ (lane[1] * 0x100000001b3ull) ^
                    (lane[2] * 0xff51afd7ed558ccdull) ^ (lane[3] * 0xc4ceb9fe1a85ec53ull);
  for (; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  h ^= h >> 29;
  return std::uint32_t(h);
}
std::FILE* open_pack(const std::filesystem::path& file, const char* mode) {
#ifdef _WIN32
  const std::wstring wide(mode, mode + std::strlen(mode));
  return _wfopen(paths::io_path(file).c_str(), wide.c_str());
#else
  return std::fopen(paths::io_path(file).c_str(), mode);
#endif
}
bool seek(std::FILE* f, std::uint64_t offset) {
#ifdef _WIN32
  return _fseeki64(f, static_cast<long long>(offset), SEEK_SET) == 0;
#else
  return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
// Reads at an offset without stdio's buffer. glibc's fseek keeps a buffer that covers
// the target, so a buffered reader returned bytes rewritten in place after it filled
// (a damaged record then passed its check from the stale copy and was never replaced).
bool read_at(std::FILE* f, std::uint64_t offset, void* out, std::size_t size) {
  auto* p = static_cast<char*>(out);
  while (size) {
#ifdef _WIN32
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD n = 0;
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 30));
    if (!ReadFile(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f))), p, chunk, &n, &at) || !n) return false;
#else
    const auto n = ::pread(fileno(f), p, size, static_cast<off_t>(offset));
    if (n <= 0) return false;
#endif
    p += n;
    offset += static_cast<std::uint64_t>(n);
    size -= static_cast<std::size_t>(n);
  }
  return true;
}
class TexturePack {
 public:
  explicit TexturePack(const std::filesystem::path& directory) : file_(directory / "textures.vtx") {
    std::error_code error;
    std::filesystem::create_directories(paths::io_path(directory), error);
    if (error) return;
    try {
      lock_ = std::make_unique<FileLock>(directory / "textures.lock", false);
    } catch (...) {
      // Another process owns this directory's pack: read only.
    }
    std::uint64_t good = 0;
    auto size = std::filesystem::file_size(paths::io_path(file_), error);
    if (error) size = 0;
    reader_ = size >= 4 ? open_pack(file_, "rb") : nullptr;
    if (reader_) {
      char magic[4]{};
      if (std::fread(magic, 1, 4, reader_) == 4 && !std::memcmp(magic, kPackMagic, 4)) {
        good = 4;
        std::string key;
        PackHeader h{};
        while (good + sizeof(h) <= size && seek(reader_, good) && std::fread(&h, sizeof(h), 1, reader_) == 1) {
          const auto next = good + sizeof(h) + h.key_size + h.data_size;
          if (h.key_size > 4096 || next > size) break;  // a torn tail
          key.resize(h.key_size);
          if (std::fread(key.data(), 1, key.size(), reader_) != key.size() ||
              pack_check(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()) != h.key_check)
            break;
          index_.insert_or_assign(key, good);
          good = next;
        }
      }
    }
    if (lock_) {
      if (reader_ && good != size) {
        // A torn tail from a killed process, or another format: cut it before appending.
        std::fclose(reader_);
        reader_ = nullptr;
        if (good) std::filesystem::resize_file(paths::io_path(file_), good, error);
        if (error) good = 0;
      }
      if (!good) {
        index_.clear();
        if (std::FILE* f = open_pack(file_, "wb")) {
          good = std::fwrite(kPackMagic, 1, 4, f) == 4 ? 4 : 0;
          std::fclose(f);
        }
      }
      writer_ = good ? open_pack(file_, "ab") : nullptr;
      if (!reader_ && good) reader_ = open_pack(file_, "rb");
    }
    end_ = good;
    if (writer_) std::thread([this, directory] { run(directory); }).detach();
  }
  // Copies a stored record of exactly out.size() bytes.
  bool find(const std::string& key, std::span<std::uint8_t> out) {
    std::lock_guard held(mutex_);
    if (const auto it = pending_.find(key); it != pending_.end()) {
      if (it->second.size() != out.size()) return false;
      std::memcpy(out.data(), it->second.data(), out.size());
      return true;
    }
    const auto it = index_.find(key);
    PackHeader h{};
    // Positional reads: the pack may have changed since the index was read (find
    // only trusts the checks), and stdio could serve a stale buffered copy.
    if (it == index_.end() || !reader_ || !read_at(reader_, it->second, &h, sizeof(h)) || h.key_size != key.size() ||
        h.data_size != out.size() || !read_at(reader_, it->second + sizeof(h) + h.key_size, out.data(), out.size()))
      return false;
    return pack_check(out.data(), out.size()) == h.data_check;
  }
  void store(const std::string& key, std::span<const std::uint8_t> data) {
    if (key.size() > 4096 || data.size() > UINT32_MAX) return;
    try {
      std::lock_guard held(mutex_);
      if (!writer_ || end_ + pending_bytes_ + data.size() > kPackLimit || pending_bytes_ + data.size() > kPendingLimit) return;
      if (!pending_.emplace(key, std::vector<std::uint8_t>(data.begin(), data.end())).second) return;
      pending_bytes_ += data.size();
      changed_.notify_one();
    } catch (...) {}
  }
  void flush() {
    std::unique_lock held(mutex_);
    changed_.notify_one();
    flushed_.wait(held, [&] { return (pending_.empty() && !writing_) || !writer_; });
  }

 private:
  void run(std::filesystem::path directory) {
    // One-file-per-mip entries of earlier builds.
    std::error_code error;
    for (std::filesystem::directory_iterator it(paths::io_path(directory), error), last; !error && it != last; it.increment(error)) {
      const auto extension = it->path().extension();
      if (extension == ".rgba" || extension == ".bc7" || extension == ".tmp") {
        std::error_code ignored;
        std::filesystem::remove(it->path(), ignored);
      }
    }
    std::unique_lock held(mutex_);
    for (;;) {
      changed_.wait(held, [&] { return !pending_.empty(); });
      while (!pending_.empty()) {
        auto node = pending_.extract(pending_.begin());
        writing_ = true;
        held.unlock();
        const auto& key = node.key();
        const auto& data = node.mapped();
        const PackHeader h{std::uint32_t(key.size()), std::uint32_t(data.size()), pack_check(data.data(), data.size()),
                           pack_check(reinterpret_cast<const std::uint8_t*>(key.data()), key.size())};
        const bool ok = std::fwrite(&h, sizeof(h), 1, writer_) == 1 && std::fwrite(key.data(), 1, key.size(), writer_) == key.size() &&
                        std::fwrite(data.data(), 1, data.size(), writer_) == data.size() && std::fflush(writer_) == 0;
        held.lock();
        writing_ = false;
        pending_bytes_ -= data.size();
        if (!ok) {
          // A short write leaves a torn record, which the next open truncates.
          std::fclose(writer_);
          writer_ = nullptr;
          pending_.clear();
          pending_bytes_ = 0;
          flushed_.notify_all();
          return;
        }
        index_.insert_or_assign(key, end_);
        end_ += sizeof(h) + key.size() + data.size();
      }
      flushed_.notify_all();
    }
  }
  std::filesystem::path file_;
  std::unique_ptr<FileLock> lock_;
  std::mutex mutex_;
  std::condition_variable changed_, flushed_;
  std::FILE* reader_ = nullptr;
  std::FILE* writer_ = nullptr;
  bool writing_ = false;
  std::unordered_map<std::string, std::uint64_t> index_;
  std::map<std::string, std::vector<std::uint8_t>> pending_;
  std::uint64_t pending_bytes_ = 0, end_ = 0;
};
std::mutex packs_lock;
std::map<std::filesystem::path, std::unique_ptr<TexturePack>>& packs() {
  // Never destroyed: writer threads outlive static destruction.
  static auto* value = new std::map<std::filesystem::path, std::unique_ptr<TexturePack>>;
  return *value;
}
TexturePack* pack(const std::filesystem::path& directory) {
  std::lock_guard held(packs_lock);
  auto& p = packs()[directory];
  if (!p) p = std::make_unique<TexturePack>(directory);
  return p.get();
}
// produce() fills the output on a miss.
std::vector<std::uint8_t> cached(
    const std::filesystem::path& cache_directory, const std::string& key,
    std::size_t size, std::size_t input_size,
    const std::function<void(std::vector<std::uint8_t>&)>& produce) {
  const bool trace = log::level() <= log::Level::debug;
  const auto begin = trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  std::vector<std::uint8_t> output(size);
  auto* store = cache_directory.empty() ? nullptr : pack(cache_directory);
  if (store && store->find(key, output)) {
    if (trace) log::debug("gfx", "Texture cache hit: input={} output={} elapsed_us={}",
        input_size, output.size(), std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin).count());
    return output;
  }
  produce(output);
  if (store) store->store(key, output);
  if (trace) log::debug("gfx", "Texture cache miss: input={} output={} elapsed_us={}",
      input_size, output.size(), std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - begin).count());
  return output;
}
// Decoder and encoder workers: a fixed pool instead of new threads for every
// mip (thread creation alone took 0.5-1 s of a level load). The caller runs
// index 0; a queued index that starts late finds no work left.
void parallel(unsigned workers, const std::function<void(unsigned)>& job) {
  struct Pool {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::function<void()>> queue;
    explicit Pool(unsigned threads) {
      for (unsigned i = 0; i < threads; ++i)
        std::thread([this] {
          for (;;) {
            std::unique_lock held(mutex);
            ready.wait(held, [&] { return !queue.empty(); });
            auto task = std::move(queue.front());
            queue.pop_front();
            held.unlock();
            task();
          }
        }).detach();
    }
  };
  if (workers <= 1) {
    job(0);
    return;
  }
  // Never destroyed: its threads outlive static destruction.
  static auto* pool = new Pool(std::clamp(std::thread::hardware_concurrency() / 2, 2u, 8u) - 1);
  std::latch done(workers - 1);
  {
    std::lock_guard held(pool->mutex);
    for (unsigned t = 1; t < workers; ++t) pool->queue.emplace_back([&job, &done, t] { job(t); done.count_down(); });
  }
  pool->ready.notify_all();
  job(0);
  done.wait();
}
}  // namespace
void flush_texture_caches() {
  std::vector<TexturePack*> all;
  {
    std::lock_guard held(packs_lock);
    for (auto& [directory, p] : packs()) all.push_back(p.get());
  }
  for (auto* p : all) p->flush();
}
namespace {
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
    parallel(workers, [&](unsigned index) {
      results[index] = astcenc_decompress_image(
          context->context, blocks.data() + layer * stride, stride, &image,
          &swizzle, index);
    });
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
// nearbyint in the current rounding mode, without the library call (v in [-1, 128]).
inline int round_even(float v) {
#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__)
  return _mm_cvtss_si32(_mm_set_ss(v));
#else
  return int(std::nearbyint(v));
#endif
}
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
        c[i] = unsigned(std::clamp(round_even((v[i] - float(b)) * 0.5f), 0, 127));
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
    // The 16 colors the indices select, once per block rather than per texel and candidate.
    Vec palette[16];
    for (unsigned k = 0; k < 16; ++k)
      for (int c = 0; c < 4; ++c)
        palette[k][c] = float((e0[c] * (64 - weights[k]) + e1[c] * weights[k] + 32) >> 6);
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
          Vec d;
          for (int c = 0; c < 4; ++c) d[c] = palette[k][c] - texels[i][c];
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
  parallel(workers, [&](unsigned) { job(); });
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
struct TextureJob {
  std::uint32_t format, width, height, layers;
  std::vector<std::uint8_t> input, output;
  std::filesystem::path directory;
  bool bc7;
  // queued -> running -> done; a cancelled queued job never runs.
  enum : int { queued, running, done, cancelled };
  std::atomic<int> state{queued};
  std::mutex lock;
  std::condition_variable finished;
};
namespace {
void run_job(TextureJob& job) {
  try {
    job.output = job.bc7 ? transcode_astc_bc7(job.format, job.width, job.height, job.layers, job.input, job.directory)
                         : decode_astc(job.format, job.width, job.height, job.layers, job.input, job.directory);
  } catch (const std::exception& e) {
    log::error("gles", "ASTC decode failed: {}", e.what());
    job.output.clear();
  }
  job.input = {};
  {
    std::lock_guard held(job.lock);
    job.state.store(TextureJob::done, std::memory_order_release);
  }
  job.finished.notify_all();
}
bool claim(TextureJob& job) {
  int expected = TextureJob::queued;
  return job.state.compare_exchange_strong(expected, TextureJob::running, std::memory_order_acq_rel);
}
// Two threads take jobs in order; each job uses the decoder pool as an upload
// call would, so a large image is not limited to one core.
struct JobQueue {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::shared_ptr<TextureJob>> jobs;
  JobQueue() {
    for (int i = 0; i < 2; ++i)
      std::thread([this] {
        for (;;) {
          std::unique_lock held(mutex);
          ready.wait(held, [&] { return !jobs.empty(); });
          auto job = std::move(jobs.front());
          jobs.pop_front();
          held.unlock();
          if (claim(*job)) run_job(*job);
        }
      }).detach();
  }
};
}  // namespace
std::shared_ptr<TextureJob> start_astc_job(std::uint32_t format, std::uint32_t width, std::uint32_t height,
                                           std::uint32_t layers, std::vector<std::uint8_t> blocks,
                                           std::filesystem::path cache_directory, bool bc7) {
  auto job = std::make_shared<TextureJob>();
  job->format = format;
  job->width = width;
  job->height = height;
  job->layers = layers;
  job->input = std::move(blocks);
  job->directory = std::move(cache_directory);
  job->bc7 = bc7;
  // Never destroyed: its threads outlive static destruction.
  static auto* queue = new JobQueue;
  {
    std::lock_guard held(queue->mutex);
    queue->jobs.push_back(job);
  }
  queue->ready.notify_one();
  return job;
}
bool astc_job_done(const TextureJob& job) noexcept {
  return job.state.load(std::memory_order_acquire) == TextureJob::done;
}
void cancel_astc_job(TextureJob& job) noexcept {
  int expected = TextureJob::queued;
  if (job.state.compare_exchange_strong(expected, TextureJob::cancelled, std::memory_order_acq_rel)) job.input = {};
}
const std::vector<std::uint8_t>& finish_astc_job(TextureJob& job) {
  if (claim(job)) {
    run_job(job);
  } else {
    std::unique_lock held(job.lock);
    job.finished.wait(held, [&] { return job.state.load(std::memory_order_acquire) != TextureJob::running; });
  }
  return job.output;
}
std::size_t astc_job_bytes(const TextureJob& job) noexcept {
  // Input until decoded, then the output: an estimate for memory limits.
  const std::size_t texels = std::size_t(job.width) * job.height * job.layers;
  return job.bc7 ? texels : texels * 4;
}
}  // namespace venture::gfx
