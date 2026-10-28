#include "common/log.h"
#include "common/steam_library.h"
#include <cmath>
#ifdef VENTURE_STEAM_ART
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
#include <webp/decode.h>
#endif
namespace venture::steam_library {
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
  for (auto role : {"portrait", "hero", "logo", "header", "icon", ""}) {
    if ((kind == "logo" || kind == "icon") && std::string_view(role) != kind)
      continue;
    for (auto separator : {"_", "."})
      for (auto ext : {"png", "jpg", "jpeg", "webp", "image"}) {
        if (!*role && std::string_view(separator) == "_")
          continue;
        auto name = std::string(id) +
                    (*role ? std::string(separator) + role : "") + "." + ext;
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
bool install_art(const fs::path &source, const fs::path &destination) {
#ifdef VENTURE_STEAM_ART
  try {
    auto bytes = paths::read(source, 20 * 1024 * 1024);
    auto encoded = reinterpret_cast<const uint8_t *>(bytes.data());
    bool webp = bytes.size() >= 12 && bytes.substr(0, 4) == "RIFF" &&
                bytes.substr(8, 4) == "WEBP";
    int w = 0, h = 0, c = 0;
    bool valid =
        webp ? WebPGetInfo(encoded, bytes.size(), &w, &h)
             : stbi_info_from_memory(encoded, int(bytes.size()), &w, &h, &c);
    if (!valid || w <= 0 || h <= 0 || w > 8192 || h > 8192 ||
        uint64_t(w) * h > 32 * 1024 * 1024)
      return false;
    auto pixels =
        webp ? WebPDecodeRGBA(encoded, bytes.size(), &w, &h)
             : stbi_load_from_memory(encoded, int(bytes.size()), &w, &h, &c, 4);
    if (!pixels)
      return false;
    std::unique_ptr<unsigned char, void (*)(unsigned char *)> data(
        pixels, webp ? +[](unsigned char *p) { WebPFree(p); }
                     : +[](unsigned char *p) { stbi_image_free(p); });
    std::string png;
    auto write = +[](void *context, void *data, int size) {
      static_cast<std::string *>(context)->append(static_cast<char *>(data),
                                                  size);
    };
    if (!stbi_write_png_to_func(write, &png, w, h, 4, pixels, w * 4))
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
} // namespace venture::steam_library
