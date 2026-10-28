#pragma once
#include "common/meta/store.h"
#include "launcher/worker.h"
#include <SDL3/SDL.h>
#include <map>
#include <set>
#include <vector>
namespace venture::launcher {
class TextureCache {
public:
  TextureCache(SDL_Renderer *renderer, fs::path data,
               std::function<void()> wake);
  ~TextureCache();
  void request(const meta::Game &game, bool allow_network = true);
  // The cover to draw `width` pixels wide: the smallest prepared size at least
  // that wide, so linear filtering never shrinks it more than twofold. Views
  // wider than the prepared sizes load the full-resolution cover on demand.
  SDL_Texture *get(std::string_view id, float width = 0);
  // Heavily blurred, low-resolution version of the cover for ambient
  // backdrops (stretched with linear filtering it reads as a soft blur).
  SDL_Texture *ambient(std::string_view id) const;
  void drain() { worker_.drain(); }
  size_t size() const { return covers_.size(); }
  void retry() { requested_.clear(); }

private:
  struct Cover {
    std::vector<SDL_Texture *> levels; // widest first
    int source_width = 0;
    SDL_Texture *full = nullptr;
    bool full_requested = false;
    int used = 0; // frame of the last full-resolution draw
  };
  void load_full(const std::string &id);
  SDL_Renderer *renderer_;
  fs::path directory_;
  std::map<std::string, Cover> covers_;
  std::map<std::string, SDL_Texture *> ambient_;
  std::set<std::string> requested_;
  int frame_ = 0;
  Worker worker_;
};
} // namespace venture::launcher
