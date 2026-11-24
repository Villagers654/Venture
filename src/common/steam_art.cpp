#include "common/log.h"
#include "common/steam_library.h"
#include <cmath>
#include <string>
#ifdef VENTURE_STEAM_ART
#include "common/digest.h"
#include "common/json.h"
#include "steam_portraits.h"
#include <algorithm>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_MAX_DIMENSIONS 8192
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#include <stb_image_resize2.h>
#include <webp/decode.h>
#endif
namespace venture::steam_library {
#ifdef VENTURE_STEAM_ART
namespace {
constexpr int kPortraitW = 600, kPortraitH = 900;
struct Pixels {
  int w = 0, h = 0;
  std::vector<unsigned char> rgba;
};
// A cached image (PNG, JPEG, GIF or WebP) as RGBA; empty when unreadable or too large.
Pixels decode(const std::string &bytes) {
  auto encoded = reinterpret_cast<const uint8_t *>(bytes.data());
  const bool webp = bytes.size() >= 12 && bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP";
  int w = 0, h = 0, c = 0;
  const bool valid = webp ? WebPGetInfo(encoded, bytes.size(), &w, &h)
                          : stbi_info_from_memory(encoded, int(bytes.size()), &w, &h, &c);
  if (!valid || w <= 0 || h <= 0 || w > 8192 || h > 8192 || uint64_t(w) * h > 32 * 1024 * 1024)
    return {};
  std::unique_ptr<unsigned char, void (*)(unsigned char *)> pixels(
      webp ? WebPDecodeRGBA(encoded, bytes.size(), &w, &h)
           : stbi_load_from_memory(encoded, int(bytes.size()), &w, &h, &c, 4),
      webp ? +[](unsigned char *p) { WebPFree(p); } : +[](unsigned char *p) { stbi_image_free(p); });
  if (!pixels)
    return {};
  return {w, h, {pixels.get(), pixels.get() + std::size_t(w) * h * 4}};
}
// The `cw` x `ch` region of `src` at (x, y), resampled to `w` x `h`.
Pixels resize(const Pixels &src, int x, int y, int cw, int ch, int w, int h) {
  Pixels out{w, h, std::vector<unsigned char>(std::size_t(w) * h * 4)};
  stbir_resize_uint8_srgb(src.rgba.data() + (std::size_t(y) * src.w + x) * 4, cw, ch, src.w * 4, out.rgba.data(),
                          w, h, w * 4, STBIR_RGBA);
  return out;
}
// How a title's portrait shows its cover (third_party/venture-steam-art/titles.json): zoom 0
// the whole cover across the capsule's width, 1 the capsule's full height,
// above 1 closer still; the focus is the point of the cover (0 to 1 across
// its width and down its height) the capsule centres on, as far as the cover
// reaches.
struct Framing {
  double zoom = 0, focus_x = 0.5, focus_y = 0.5;
};
// The framing of package `package`'s cover `bytes` (a pinned version's
// <package>@<folder> takes its package's); nullopt for a title the table lacks,
// or a cover other than the one it was framed for (an updated one).
std::optional<Framing> known_framing(std::string_view package, std::string_view bytes) {
  static const Json table = [] {
    try {
      return Json::parse(steam_portraits_json);
    } catch (const std::exception &e) {
      log::error("steam", "Bundled portrait framings unreadable: {}", e.what());
      return Json::object();
    }
  }();
  const auto &entry = field(field(table, "titles"), package.substr(0, package.find('@')));
  if (!entry.is_object()) return std::nullopt;
  if (str(entry, "cover_sha256") != digest::sha256({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()}))
    return std::nullopt;
  Framing f;
  if (const auto &zoom = field(entry, "zoom"); zoom.is_number()) f.zoom = std::clamp(zoom.get<double>(), 0.0, 2.0);
  if (const auto &x = field(entry, "focus_x"); x.is_number()) f.focus_x = std::clamp(x.get<double>(), 0.0, 1.0);
  if (const auto &y = field(entry, "focus_y"); y.is_number()) f.focus_y = std::clamp(y.get<double>(), 0.0, 1.0);
  return f;
}
// Box blurs of radius `r` along rows and columns, three of each: close to a
// Gaussian blur.
void blur(Pixels &p, int r) {
  std::vector<unsigned char> line;
  for (int pass = 0; pass < 6; ++pass) {
    const bool rows = pass % 2 == 0;
    const int n = rows ? p.w : p.h, lines = rows ? p.h : p.w;
    const std::size_t step = rows ? 4 : std::size_t(p.w) * 4;
    line.resize(std::size_t(n) * 4);
    for (int l = 0; l < lines; ++l) {
      auto *base = p.rgba.data() + (rows ? std::size_t(l) * p.w * 4 : std::size_t(l) * 4);
      for (int i = 0; i < n; ++i) std::copy_n(base + i * step, 4, line.data() + std::size_t(i) * 4);
      for (int i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
          int sum = 0;
          for (int k = -r; k <= r; ++k) sum += line[std::size_t(std::clamp(i + k, 0, n - 1)) * 4 + c];
          base[i * step + c] = static_cast<unsigned char>(sum / (2 * r + 1));
        }
    }
  }
}
// Steam's 600x900 library capsule from a landscape cover (Meta's store has no
// portrait art): the cover framed as `f`, over a blurred, darkened fill made
// of its middle where it does not reach. Opaque, whatever the cover's alpha.
Pixels portrait(const Pixels &cover, Framing f) {
  constexpr int W = kPortraitW, H = kPortraitH;
  // The fill, blurred at a quarter of the capsule's size, then scaled up.
  const int fill_w = std::max(1, std::min(cover.w, cover.h * W / H));
  auto small = resize(cover, (cover.w - fill_w) / 2, 0, fill_w, cover.h, W / 4, H / 4);
  blur(small, 6);
  auto out = resize(small, 0, 0, small.w, small.h, W, H);
  for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
    for (std::size_t c = 0; c < 3; ++c) out.rgba[i + c] = static_cast<unsigned char>(out.rgba[i + c] * 55 / 100);
    out.rgba[i + 3] = 255;
  }
  // The cover `scaled_h` tall in the capsule: as much of it as fits.
  const double fit_h = double(W) * cover.h / cover.w;
  const double scaled_h = fit_h + f.zoom * (H - fit_h);
  const int fg_h = std::clamp(int(scaled_h), 1, H);  // rounded down: zoom 0 shows all of it
  const int shown_h = scaled_h > H ? std::clamp(int(std::lround(cover.h * H / scaled_h)), 1, cover.h) : cover.h;
  const int shown_w = std::clamp(int(std::lround(W * cover.h / scaled_h)), 1, cover.w);
  const int x = std::clamp(int(std::lround(f.focus_x * cover.w - shown_w / 2.0)), 0, cover.w - shown_w);
  const int y = std::clamp(int(std::lround(f.focus_y * cover.h - shown_h / 2.0)), 0, cover.h - shown_h);
  const auto fg = resize(cover, x, y, shown_w, shown_h, W, fg_h);
  // Over the fill: a transparent cover shows it, never a hole.
  auto *to = out.rgba.data() + std::ptrdiff_t((H - fg_h) / 2) * W * 4;
  for (std::size_t i = 0; i < fg.rgba.size(); i += 4) {
    const int a = fg.rgba[i + 3];
    for (std::size_t c = 0; c < 3; ++c)
      to[i + c] = static_cast<unsigned char>((fg.rgba[i + c] * a + to[i + c] * (255 - a) + 127) / 255);
  }
  return out;
}
// Wider than a portrait capsule by more than rounding.
bool landscape(const Pixels &p) { return double(p.w) / p.h > 2.0 / 3 * 1.15; }
// The PNG Steam gets for package `package`'s image `bytes` as `kind` (a
// portrait framed as `framing`, else as the table says); empty when it cannot
// be read.
std::string art_png(const std::string &bytes, std::string_view kind, std::string_view package,
                    std::optional<Framing> framing = std::nullopt) {
  auto pixels = decode(bytes);
  if (pixels.rgba.empty())
    return {};
  if (kind == "portrait" && landscape(pixels))
    pixels = portrait(pixels, framing ? *framing : known_framing(package, bytes).value_or(Framing{}));
  std::string png;
  auto write = +[](void *context, void *data, int size) {
    static_cast<std::string *>(context)->append(static_cast<char *>(data), size);
  };
  if (!stbi_write_png_to_func(write, &png, pixels.w, pixels.h, 4, pixels.rgba.data(), pixels.w * 4))
    return {};
  return png;
}
std::string read_art(const fs::path &source) { return paths::read(source, 20 * 1024 * 1024); }
// A PNG's width and height from its header; zero when `bytes` is no PNG.
std::pair<std::uint32_t, std::uint32_t> png_size(std::string_view bytes) {
  if (bytes.size() < 24 || bytes.substr(0, 8) != "\x89PNG\r\n\x1a\n") return {};
  auto be32 = [&](std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) v = v << 8 | std::uint8_t(bytes[at + i]);
    return v;
  };
  return {be32(16), be32(20)};
}
} // namespace
#endif
fs::path best_art(const fs::path &cache, std::string_view id,
                  std::string_view kind) {
  if (!paths::safe_name(id))
    return {};
  fs::path best;
#ifdef VENTURE_STEAM_ART
  const double ratio = kind == "portrait" ? 2.0 / 3
                       : kind == "hero"   ? 3.24
                       : kind == "header" ? 2.14
                                          : 1.0;
  double score = 1e9;
  for (std::string_view role : {"portrait", "hero", "logo", "header", "icon", ""}) {
    if ((kind == "logo" || kind == "icon") && role != kind)
      continue;
    // A portrait is made from a cover (art_png), never from a small icon or a logo.
    if (kind == "portrait" && (role == "icon" || role == "logo"))
      continue;
    for (auto separator : {"_", "."})
      for (auto ext : {"png", "jpg", "jpeg", "webp", "image"}) {
        if (role.empty() && std::string_view(separator) == "_")
          continue;
        auto name = std::string(id) +
                    (!role.empty() ? std::string(separator) + std::string(role) : "") + "." + ext;
        auto source = cache / paths::from_utf8(name);
        try {
          if (!fs::is_regular_file(source))
            continue;
          auto bytes = paths::read(source, 20 * 1024 * 1024);
          auto data = reinterpret_cast<const uint8_t *>(bytes.data());
          bool webp = bytes.size() >= 12 && bytes.substr(0, 4) == "RIFF" &&
                      bytes.substr(8, 4) == "WEBP";
          int w = 0, h = 0, c = 0;
          bool valid =
              webp ? WebPGetInfo(data, bytes.size(), &w, &h)
                   : stbi_info_from_memory(data, int(bytes.size()), &w, &h, &c);
          if (!valid || w <= 0 || h <= 0 || w > 8192 || h > 8192 ||
              uint64_t(w) * h > 32 * 1024 * 1024)
            continue;
          double fit = std::abs(std::log((double(w) / h) / ratio));
          if (kind == role)
            fit -= 10; // Explicit role beats aspect-ratio fallback.
          if (fit < score) {
            score = fit;
            best = source;
          }
        } catch (...) {
        } // A bad cache file is not a reason to lose shortcuts.
      }
  }
#endif
  return best;
}
bool install_art(const fs::path &source, const fs::path &destination, std::string_view kind,
                 std::string_view package) {
#ifdef VENTURE_STEAM_ART
  try {
    const auto png = art_png(read_art(source), kind, package);
    if (png.empty())
      return false;
    // Avoid rewriting unchanged cached artwork on every startup.
    if (!fs::exists(destination) ||
        paths::read(destination, 64 * 1024 * 1024) != png)
      paths::atomic_write(destination, png);
    return true;
  } catch (...) {
    log::warn("steam", "Could not convert cached Steam artwork; skipping it.");
  }
#endif
  return false;
}
bool replaced_art(const fs::path &existing, const fs::path &cache, std::string_view id, std::string_view package,
                  std::string_view kind) {
#ifdef VENTURE_STEAM_ART
  // Only a PNG of ours can be one; anything else Steam has there stays.
  if (kind != "portrait" || existing.extension() != ".png")
    return false;
  // Portraits already judged to stay this run, as they were then: the checks
  // below compose images, too costly to repeat at every sync.
  static std::mutex mutex;
  static std::map<fs::path, std::pair<std::uintmax_t, fs::file_time_type>> kept;
  try {
    const std::pair state{fs::file_size(existing), fs::last_write_time(existing)};
    {
      std::lock_guard lock(mutex);
      if (auto it = kept.find(existing); it != kept.end() && it->second == state) return false;
    }
    const auto bytes = paths::read(existing, 64 * 1024 * 1024);
    const auto cover = best_art(cache, id, "portrait");
    const auto cover_bytes = cover.empty() ? std::string() : read_art(cover);
    const bool ours = [&] {
      if (png_size(bytes) == std::pair<std::uint32_t, std::uint32_t>{kPortraitW, kPortraitH})
        // A whole-cover portrait from before the table framed the title.
        return !cover_bytes.empty() && known_framing(package, cover_bytes) &&
               bytes == art_png(cover_bytes, "portrait", package, Framing{});
      // Copied by an earlier version: the icon, or the cover where it had no icon.
      if (const auto icon = best_art(cache, id, "icon"); !icon.empty() && bytes == art_png(read_art(icon), "icon", package))
        return true;
      return !cover_bytes.empty() && bytes == art_png(cover_bytes, "cover", package);
    }();
    if (!ours) {
      std::lock_guard lock(mutex);
      kept[existing] = state;
    }
    return ours;
  } catch (...) {
  }
#endif
  return false;
}
} // namespace venture::steam_library
