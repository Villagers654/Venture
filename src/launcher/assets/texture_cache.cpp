#include "launcher/assets/texture_cache.h"
#include "common/log.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_MAX_DIMENSIONS 8192
#include <stb_image.h>
#include <webp/decode.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <vector>
namespace venture::launcher {
namespace {
// The prepared sizes: the widest is enough for a library tile on a 2x
// display; the full cover is only kept for the games last viewed.
constexpr int kTileWidth = 640, kSmallestWidth = 160, kFullWidth = 2560;
constexpr size_t kFullCovers = 2;
// Graph's hero banner is 3.33:1; the store's landscape cover is 16:9.
bool banner(int w, int h) { return h > 0 && w > h * 2.4; }

struct Image {
  std::shared_ptr<std::vector<uint8_t>> rgba;
  int w = 0, h = 0;
};

bool is_webp(const std::string &bytes) {
  return bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 &&
         bytes.compare(8, 4, "WEBP") == 0;
}
bool info(const std::string &bytes, int &w, int &h) {
  int c = 0;
  const auto *encoded = reinterpret_cast<const uint8_t *>(bytes.data());
  bool valid = is_webp(bytes)
                   ? WebPGetInfo(encoded, bytes.size(), &w, &h)
                   : stbi_info_from_memory(encoded, int(bytes.size()), &w, &h, &c);
  return valid && w > 0 && h > 0 && uint64_t(w) * h <= 32 * 1024 * 1024;
}

Image decode(const std::string &bytes) {
  int w = 0, h = 0, c = 0;
  if (!info(bytes, w, h))
    throw std::runtime_error("Artwork has an unsupported format or dimensions.");
  const auto *encoded = reinterpret_cast<const uint8_t *>(bytes.data());
  bool webp = is_webp(bytes);
  uint8_t *pixels = webp ? WebPDecodeRGBA(encoded, bytes.size(), &w, &h)
                         : stbi_load_from_memory(encoded, int(bytes.size()), &w, &h, &c, 4);
  if (!pixels)
    throw std::runtime_error("Could not decode the cover image.");
  Image out{std::make_shared<std::vector<uint8_t>>(pixels, pixels + size_t(w) * h * 4), w, h};
  if (webp)
    WebPFree(pixels);
  else
    stbi_image_free(pixels);
  return out;
}

// Area-average resampling in linear light: every source pixel contributes by
// the share of it an output pixel covers, so fine detail averages out instead
// of aliasing the way a bilinear shrink does.
struct Tap {
  int index;
  float weight;
};
std::vector<std::vector<Tap>> taps(int n, int dn) {
  std::vector<std::vector<Tap>> out(dn);
  double scale = double(n) / dn;
  for (int i = 0; i < dn; ++i) {
    double a = i * scale, b = (i + 1) * scale;
    for (int j = int(a); j < std::min(n, int(std::ceil(b))); ++j) {
      double cover = std::min(b, j + 1.0) - std::max(a, double(j));
      if (cover > 1e-6)
        out[i].push_back({j, float(cover / scale)});
    }
  }
  return out;
}
const std::array<float, 256> &to_linear() {
  static const auto table = [] {
    std::array<float, 256> t{};
    for (int i = 0; i < 256; ++i) {
      float c = i / 255.f;
      t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    return t;
  }();
  return table;
}
uint8_t to_srgb(float l) {
  static const auto table = [] {
    std::array<uint8_t, 4097> t{};
    for (int i = 0; i <= 4096; ++i) {
      float c = i / 4096.f;
      c = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f;
      t[i] = uint8_t(std::clamp(c * 255.f + 0.5f, 0.f, 255.f));
    }
    return t;
  }();
  return table[size_t(std::clamp(l, 0.f, 1.f) * 4096.f + 0.5f)];
}
Image resample(const Image &src, int dw, int dh) {
  if (dw == src.w && dh == src.h)
    return src;
  const auto &lin = to_linear();
  auto tx = taps(src.w, dw), ty = taps(src.h, dh);
  std::vector<float> rows(size_t(dw) * src.h * 4);
  const uint8_t *s = src.rgba->data();
  for (int y = 0; y < src.h; ++y)
    for (int x = 0; x < dw; ++x) {
      float acc[4] = {};
      for (auto [j, wgt] : tx[x]) {
        const uint8_t *p = s + (size_t(y) * src.w + j) * 4;
        acc[0] += lin[p[0]] * wgt;
        acc[1] += lin[p[1]] * wgt;
        acc[2] += lin[p[2]] * wgt;
        acc[3] += p[3] * wgt;
      }
      std::copy(acc, acc + 4, &rows[(size_t(y) * dw + x) * 4]);
    }
  Image out{std::make_shared<std::vector<uint8_t>>(size_t(dw) * dh * 4), dw, dh};
  uint8_t *d = out.rgba->data();
  for (int y = 0; y < dh; ++y)
    for (int x = 0; x < dw; ++x) {
      float acc[4] = {};
      for (auto [j, wgt] : ty[y]) {
        const float *p = &rows[(size_t(j) * dw + x) * 4];
        for (int k = 0; k < 4; ++k)
          acc[k] += p[k] * wgt;
      }
      uint8_t *q = d + (size_t(y) * dw + x) * 4;
      q[0] = to_srgb(acc[0]);
      q[1] = to_srgb(acc[1]);
      q[2] = to_srgb(acc[2]);
      q[3] = uint8_t(std::clamp(acc[3] + 0.5f, 0.f, 255.f));
    }
  return out;
}
Image fit_width(const Image &src, int width) {
  if (src.w <= width)
    return src;
  return resample(src, width, std::max(1, int(std::lround(double(src.h) * width / src.w))));
}

SDL_Texture *upload(SDL_Renderer *renderer, const Image &image) {
  SDL_Surface *surface = SDL_CreateSurfaceFrom(image.w, image.h, SDL_PIXELFORMAT_RGBA32,
                                               image.rgba->data(), image.w * 4);
  if (!surface)
    return nullptr;
  SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
  SDL_DestroySurface(surface);
  if (texture)
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
  return texture;
}

// Box-downsample to a tiny image and blur it; stretched back up with linear
// filtering this gives a cheap, smooth "frosted" backdrop.
std::vector<uint8_t> blurred_thumbnail(const uint8_t *rgba, int w, int h,
                                       int out_w, int out_h) {
  std::vector<float> acc(size_t(out_w) * out_h * 4, 0.f);
  std::vector<float> count(size_t(out_w) * out_h, 0.f);
  for (int y = 0; y < h; ++y) {
    int oy = std::min(out_h - 1, y * out_h / h);
    for (int x = 0; x < w; ++x) {
      int ox = std::min(out_w - 1, x * out_w / w);
      size_t o = size_t(oy) * out_w + ox;
      const uint8_t *p = rgba + (size_t(y) * w + x) * 4;
      for (int k = 0; k < 4; ++k)
        acc[o * 4 + k] += p[k];
      count[o] += 1;
    }
  }
  for (size_t i = 0; i < count.size(); ++i)
    for (int k = 0; k < 4; ++k)
      acc[i * 4 + k] /= std::max(1.f, count[i]);
  std::vector<float> tmp(acc.size());
  for (int pass = 0; pass < 3; ++pass) {
    for (int y = 0; y < out_h; ++y)
      for (int x = 0; x < out_w; ++x)
        for (int k = 0; k < 4; ++k) {
          float sum = 0;
          int n = 0;
          for (int d = -2; d <= 2; ++d) {
            int xx = std::clamp(x + d, 0, out_w - 1);
            sum += acc[(size_t(y) * out_w + xx) * 4 + k];
            ++n;
          }
          tmp[(size_t(y) * out_w + x) * 4 + k] = sum / n;
        }
    for (int y = 0; y < out_h; ++y)
      for (int x = 0; x < out_w; ++x)
        for (int k = 0; k < 4; ++k) {
          float sum = 0;
          int n = 0;
          for (int d = -2; d <= 2; ++d) {
            int yy = std::clamp(y + d, 0, out_h - 1);
            sum += tmp[(size_t(yy) * out_w + x) * 4 + k];
            ++n;
          }
          acc[(size_t(y) * out_w + x) * 4 + k] = sum / n;
        }
  }
  std::vector<uint8_t> out(acc.size());
  for (size_t i = 0; i < acc.size(); ++i)
    out[i] = uint8_t(std::clamp(acc[i], 0.f, 255.f));
  return out;
}
} // namespace
TextureCache::TextureCache(SDL_Renderer *r, fs::path data,
                           std::function<void()> wake)
    : renderer_(r), directory_(data / "cache/art"), worker_(std::move(wake)) {}
TextureCache::~TextureCache() {
  worker_.stop();
  for (auto &[id, cover] : covers_) {
    for (auto *t : cover.levels)
      SDL_DestroyTexture(t);
    if (cover.full)
      SDL_DestroyTexture(cover.full);
  }
  for (auto &[id, texture] : ambient_)
    SDL_DestroyTexture(texture);
}
SDL_Texture *TextureCache::ambient(std::string_view id) const {
  auto i = ambient_.find(std::string(id));
  return i == ambient_.end() ? nullptr : i->second;
}
SDL_Texture *TextureCache::get(std::string_view id, float width) {
  auto i = covers_.find(std::string(id));
  if (i == covers_.end() || i->second.levels.empty())
    return nullptr;
  Cover &c = i->second;
  float widest = 0;
  SDL_GetTextureSize(c.levels.front(), &widest, nullptr);
  if (width > widest * 1.05f && c.source_width > widest) {
    if (c.full) {
      c.used = ++frame_;
      return c.full;
    }
    if (!c.full_requested) {
      c.full_requested = true;
      load_full(i->first);
    }
    return c.levels.front();
  }
  for (auto it = c.levels.rbegin(); it != c.levels.rend(); ++it) {
    float w = 0;
    SDL_GetTextureSize(*it, &w, nullptr);
    if (w >= width)
      return *it;
  }
  return c.levels.front();
}
void TextureCache::load_full(const std::string &id) {
  auto file = directory_ / (id + ".image");
  worker_.submit([this, id, file](std::stop_token) -> Worker::Completion {
    try {
      auto image = fit_width(decode(paths::read(file, 20 * 1024 * 1024)), kFullWidth);
      return [this, id, image] {
        auto i = covers_.find(id);
        if (i == covers_.end())
          return;
        i->second.full = upload(renderer_, image);
        i->second.used = ++frame_;
        // Only the covers of the last few game pages stay at full size.
        std::vector<std::pair<int, Cover *>> full;
        for (auto &[key, c] : covers_)
          if (c.full)
            full.push_back({c.used, &c});
        std::ranges::sort(full, {}, &std::pair<int, Cover *>::first);
        for (size_t k = 0; k + kFullCovers < full.size(); ++k) {
          SDL_DestroyTexture(full[k].second->full);
          full[k].second->full = nullptr;
          full[k].second->full_requested = false;
        }
      };
    } catch (...) {
      log::warn("artwork", "Could not load the full cover for application {}.", id);
      return {};
    }
  });
}
void TextureCache::request(const meta::Game &game, bool allow_network) {
  // A Meta app id, or an import's package whose cover came from its APK.
  const bool store = digits(game.id, 5);
  if ((!store && !paths::safe_package(game.id)) || covers_.contains(game.id) ||
      !requested_.insert(game.id).second)
    return;
  allow_network = allow_network && store;
  worker_.submit([this, game, allow_network](std::stop_token stop) -> Worker::Completion {
    try {
      auto file = directory_ / (game.id + ".image");
      auto no_cover = directory_ / (game.id + ".nocover");
      std::string bytes;
      int w = 0, h = 0;
      if (fs::exists(file))
        bytes = paths::read(file, 20 * 1024 * 1024);
      // Earlier versions cached Graph's wide hero banner, which a 16:9 tile
      // crops. Fetch the landscape cover; the banner stays as Steam's hero.
      bool wide = !bytes.empty() && info(bytes, w, h) && banner(w, h);
      // A page without a cover is asked again after a week.
      std::error_code ec;
      auto checked = fs::last_write_time(no_cover, ec);
      bool recent_miss = !ec && fs::file_time_type::clock::now() - checked < std::chrono::hours(24 * 7);
      if ((bytes.empty() || wide) && allow_network && !recent_miss) {
        try {
          auto uri = meta::Store({}, stop).cover(game.id);
          std::string fresh = uri.empty() ? std::string() : http::get_image(uri, stop);
          int fw = 0, fh = 0;
          if (!fresh.empty() && info(fresh, fw, fh) && !banner(fw, fh)) {
            if (wide && !fs::exists(directory_ / (game.id + "_hero.image")))
              paths::atomic_write(directory_ / (game.id + "_hero.image"), bytes);
            paths::atomic_write(file, fresh);
            bytes = std::move(fresh);
          } else if (uri.empty()) {
            paths::atomic_write(no_cover, "");
          }
        } catch (...) {
          if (stop.stop_requested())
            return {};
          log::warn("artwork", "No store cover for application {}.", game.id);
        }
      }
      if (bytes.empty()) {
        if (!allow_network)
          return {};
        auto uri = game.image;
        if (uri.empty())
          uri = meta::Store({}, stop).artwork(game.id).image;
        if (uri.empty())
          return {};
        bytes = http::get_image(uri, stop);
        (void)decode(bytes); // validate before caching
        paths::atomic_write(file, bytes);
      }
      auto image = decode(bytes);
      std::vector<Image> levels{fit_width(image, kTileWidth)};
      while (levels.back().w / 2 >= kSmallestWidth)
        levels.push_back(resample(levels.back(), levels.back().w / 2,
                                  std::max(1, levels.back().h / 2)));
      constexpr int kAmbientW = 48, kAmbientH = 27;
      Image blur{std::make_shared<std::vector<uint8_t>>(blurred_thumbnail(
                     image.rgba->data(), image.w, image.h, kAmbientW, kAmbientH)),
                 kAmbientW, kAmbientH};
      return [this, id = game.id, levels, source = image.w, blur] {
        if (auto t = upload(renderer_, blur))
          ambient_[id] = t;
        Cover cover;
        cover.source_width = source;
        for (auto &level : levels)
          if (auto t = upload(renderer_, level))
            cover.levels.push_back(t);
        if (!cover.levels.empty())
          covers_[id] = std::move(cover);
      };
    } catch (const std::runtime_error &e) {
      log::warn("artwork", "Application {}: {}", game.id, e.what());
      return {};
    } catch (...) {
      log::warn("artwork", "Could not load artwork for application {}.", game.id);
      return {};
    }
  });
}
} // namespace venture::launcher
