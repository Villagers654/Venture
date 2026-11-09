// The controller pictures: bundled ones embedded (images from
// tools/controller_art/build_controller_art.py), installed ones drawn and cached.
#include "launcher/assets/controller_art.h"
#include "common/json.h"
#include "launcher/assets/render_model.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <condition_variable>
#include <stb_image.h>
#include <stb_image_write.h>
#pragma clang diagnostic ignored "-Wc23-extensions"
namespace venture::launcher::art {
namespace {
constexpr unsigned char index_left[] = {
#embed "controllers/index-left.png"
};
constexpr unsigned char index_right[] = {
#embed "controllers/index-right.png"
};
constexpr unsigned char touch_left[] = {
#embed "controllers/touch-left.png"
};
constexpr unsigned char touch_right[] = {
#embed "controllers/touch-right.png"
};
constexpr unsigned char vive_left[] = {
#embed "controllers/vive-left.png"
};
constexpr unsigned char vive_right[] = {
#embed "controllers/vive-right.png"
};
constexpr unsigned char wmr_left[] = {
#embed "controllers/wmr-left.png"
};
constexpr unsigned char wmr_right[] = {
#embed "controllers/wmr-right.png"
};
constexpr unsigned char pico4_left[] = {
#embed "controllers/pico4-left.png"
};
constexpr unsigned char pico4_right[] = {
#embed "controllers/pico4-right.png"
};
constexpr unsigned char cosmos_left[] = {
#embed "controllers/cosmos-left.png"
};
constexpr unsigned char cosmos_right[] = {
#embed "controllers/cosmos-right.png"
};
constexpr unsigned char focus3_left[] = {
#embed "controllers/focus3-left.png"
};
constexpr unsigned char focus3_right[] = {
#embed "controllers/focus3-right.png"
};
constexpr unsigned char generic_left[] = {
#embed "controllers/generic-left.png"
};
constexpr unsigned char generic_right[] = {
#embed "controllers/generic-right.png"
};
// The models the pictures were drawn from, for turning them (the generic picture stays still).
constexpr unsigned char index_left_glb[] = {
#embed "controllers/index-left.glb"
};
constexpr unsigned char index_right_glb[] = {
#embed "controllers/index-right.glb"
};
constexpr unsigned char touch_left_glb[] = {
#embed "controllers/touch-left.glb"
};
constexpr unsigned char touch_right_glb[] = {
#embed "controllers/touch-right.glb"
};
constexpr unsigned char vive_glb[] = {
#embed "controllers/vive.glb"
};
constexpr unsigned char wmr_left_glb[] = {
#embed "controllers/wmr-left.glb"
};
constexpr unsigned char wmr_right_glb[] = {
#embed "controllers/wmr-right.glb"
};
constexpr unsigned char pico4_left_glb[] = {
#embed "controllers/pico4-left.glb"
};
constexpr unsigned char pico4_right_glb[] = {
#embed "controllers/pico4-right.glb"
};
constexpr unsigned char cosmos_left_glb[] = {
#embed "controllers/cosmos-left.glb"
};
constexpr unsigned char cosmos_right_glb[] = {
#embed "controllers/cosmos-right.glb"
};
constexpr unsigned char focus3_left_glb[] = {
#embed "controllers/focus3-left.glb"
};
constexpr unsigned char focus3_right_glb[] = {
#embed "controllers/focus3-right.glb"
};
struct RowSpot { const char* input = nullptr; float x = 0, y = 0; };
struct Row {
  const char* name;
  const char* hand;
  int width, height;
  float side, azimuth, elevation, roll;
  RowSpot spots[12];
};
constexpr Row kRows[] = {
#include "controllers/controller_art.inc"
};
constexpr std::span<const unsigned char> kPngs[] = {index_left, index_right, touch_left, touch_right, vive_left, vive_right, wmr_left, wmr_right, pico4_left, pico4_right, cosmos_left, cosmos_right, focus3_left, focus3_right, generic_left, generic_right};
constexpr std::span<const unsigned char> kGlbs[] = {index_left_glb, index_right_glb, touch_left_glb, touch_right_glb, vive_glb, vive_glb, wmr_left_glb, wmr_right_glb, pico4_left_glb, pico4_right_glb, cosmos_left_glb, cosmos_right_glb, focus3_left_glb, focus3_right_glb, {}, {}};
}  // namespace

struct SceneSource {
  std::function<std::array<ScenePtr, 2>()> load;
  std::once_flag once;
  std::array<ScenePtr, 2> scenes;
  ScenePtr get(bool right) {
    std::call_once(once, [&] { scenes = load(); });
    return scenes[right ? 1 : 0];
  }
};

const Image* controller(std::string_view name, std::string_view hand) {
  // The generated rows follow the embedded files' order: each controller, left then right.
  static_assert(std::size(kRows) == std::size(kPngs));
  static const auto images = [] {
    std::vector<Image> all(std::size(kRows));
    for (size_t i = 0; i < all.size(); ++i) {
      const auto& r = kRows[i];
      all[i] = Image{r.name, r.hand, r.width, r.height, {}, kPngs[i], {}};
      for (const auto& sp : r.spots)
        if (sp.input) all[i].spots.push_back({sp.input, sp.x, sp.y});
      if (!kGlbs[i].empty()) {
        all[i].source = std::make_shared<SceneSource>();
        all[i].source->load = [i, right = std::string_view(r.hand) == "right"] {
          const auto& row = kRows[i];
          std::vector<Spot2> spots;
          for (const auto& sp : row.spots)
            if (sp.input) spots.push_back({sp.input, sp.x, sp.y});
          std::array<ScenePtr, 2> out;
          out[right ? 1 : 0] =
              bundled_scene(kGlbs[i], row.side, row.azimuth, row.elevation, row.roll, row.height, spots);
          return out;
        };
      }
    }
    return all;
  }();
  for (const auto& image : images)
    if (image.name == name && image.hand == hand) return &image;
  return nullptr;
}

namespace {
// Bumped when the drawing changes, so cached pictures are drawn again.
constexpr int kDrawVersion = 4;
constexpr int kDrawHeight = 720;
using Pair = std::array<std::unique_ptr<Image>, 2>;

std::unique_ptr<Image> load_cached(const fs::path& png, const fs::path& spots) {
  std::ifstream in(spots, std::ios::binary), picture(png, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), {});
  const std::string bytes((std::istreambuf_iterator<char>(picture)), {});
  const Json j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object() || bytes.empty()) return nullptr;
  int w = 0, h = 0, n = 0;
  unsigned char* pixels =
      stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &w, &h, &n, 4);
  if (!pixels) return nullptr;
  auto image = std::make_unique<Image>();
  image->width = w, image->height = h;
  image->rgba.assign(pixels, pixels + size_t(w) * h * 4);
  stbi_image_free(pixels);
  for (const auto& [input, at] : j.items())
    if (at.is_array() && at.size() == 2) image->spots.push_back({input, at[0].get<float>(), at[1].get<float>()});
  return image;
}

void save(const Image& image, const fs::path& png, const fs::path& spots) {
  Json j = Json::object();
  for (const auto& sp : image.spots) j[sp.input] = {sp.x, sp.y};
  std::ofstream out(png, std::ios::binary);
  const auto write = [](void* stream, void* data, int size) {
    static_cast<std::ofstream*>(stream)->write(static_cast<const char*>(data), size);
  };
  if (out && stbi_write_png_to_func(write, &out, image.width, image.height, 4, image.rgba.data(), image.width * 4)) {
    out.close();
    if (out) std::ofstream(spots, std::ios::binary) << j.dump();
  }
}

// Both hands of `model`, drawn as a matched pair (or read from the cache).
// Inputs the installed model has no part for (a Touch thumbrest) come from the
// bundled model `art` of the same controller, when there is one.
Pair draw_installed(const std::string& model, const std::string& art, const fs::path& cache) {
  auto left = find_render_model(model + "_left"), right = find_render_model(model + "_right");
  if (!left || !right) left = right = find_render_model(model);
  if (!left) return {};
  std::error_code ec;
  std::string stamp;
  for (const auto& dir : {*left, *right}) {
    const std::string name = paths::utf8(dir.filename());
    stamp += std::format("{}|{}|", paths::utf8(dir),
                         fs::last_write_time(dir / (name + ".json"), ec).time_since_epoch().count());
  }
  const size_t key = std::hash<std::string>{}(std::format("{}{}|{}", stamp, kDrawVersion, art));
  Pair pair;
  std::array<fs::path, 2> png, spots;
  for (int hand = 0; hand < 2; ++hand) {
    const char* side = hand ? "right" : "left";
    png[hand] = cache / std::format("{}-{}-{:016x}.png", model, side, key);
    spots[hand] = cache / std::format("{}-{}-{:016x}.json", model, side, key);
    pair[hand] = load_cached(png[hand], spots[hand]);
  }
  // The models load again only when the player turns one (cached pictures skip them).
  auto source = std::make_shared<SceneSource>();
  source->load = [l = *left, r = *right, art] {
    auto scenes = installed_pair(l, r, View{}, kDrawHeight);
    if (!scenes) return std::array<ScenePtr, 2>{};
    for (int hand = 0; hand < 2; ++hand)
      if (const Image* bundled = art.empty() ? nullptr : controller(art, hand ? "right" : "left");
          bundled && bundled->source)
        if (const ScenePtr donor = bundled->source->get(hand == 1))
          (*scenes)[size_t(hand)] = with_points_from((*scenes)[size_t(hand)], *donor);
    return *scenes;
  };
  if (!pair[0] || !pair[1]) {
    const ScenePtr scenes[2] = {source->get(false), source->get(true)};
    if (!scenes[0] || !scenes[1]) return {};
    fs::create_directories(cache, ec);
    for (int hand = 0; hand < 2; ++hand) {
      Shot d = draw(*scenes[hand], Turn{}, 1, 2, true);
      pair[hand] = std::make_unique<Image>();
      pair[hand]->width = d.width, pair[hand]->height = d.height;
      pair[hand]->rgba = std::move(d.rgba);
      for (auto& a : d.anchors) pair[hand]->spots.push_back({a.input, a.x, a.y});
      save(*pair[hand], png[hand], spots[hand]);
    }
  }
  for (int hand = 0; hand < 2; ++hand) {
    pair[hand]->source = source;
    pair[hand]->name = model;
    pair[hand]->hand = hand ? "right" : "left";
    pair[hand]->installed = true;
  }
  return pair;
}

struct Installed {
  std::future<Pair> job;
  Pair images;
  bool done = false;
};
}  // namespace

const Image* installed_controller(std::string_view model, std::string_view art, std::string_view hand,
                                  const fs::path& cache, const std::function<void()>& wake, bool* pending) {
  static std::mutex lock;
  static std::map<std::string, Installed, std::less<>> drawings;
  if (pending) *pending = false;
  if (model.empty()) return nullptr;
  std::lock_guard guard(lock);
  auto& d = drawings[std::format("{}|{}", model, art)];
  if (!d.done && !d.job.valid())
    d.job = std::async(std::launch::async, [m = std::string(model), a = std::string(art), cache, wake] {
      auto pair = draw_installed(m, a, cache);
      if (wake) wake();
      return pair;
    });
  if (!d.done && d.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    d.images = d.job.get();
    d.done = true;
  }
  if (pending) *pending = !d.done;
  return d.images[hand == "right" ? 1 : 0].get();
}

namespace {
// The turntable: one background thread draws the newest request of each picture.
struct Turntable {
  struct Request {
    Turn turn;
    float scale = 1;
    int ss = 1;
    bool load_only = false;
  };
  struct Result {
    Turn turn;
    float scale = 0;
    int ss = 0;
    std::shared_ptr<const Shot> shot;
  };
  std::mutex lock;
  std::condition_variable wakeup;
  std::map<const Image*, Request> pending;
  std::map<const Image*, Result> done;
  std::function<void()> wake;
  bool started = false;

  void start() {
    if (std::exchange(started, true)) return;
    std::thread([this] {
      std::unique_lock guard(lock);
      for (;;) {
        wakeup.wait(guard, [&] { return !pending.empty(); });
        auto it = pending.begin();
        const Image* image = it->first;
        const Request job = it->second;
        pending.erase(it);
        auto notify = wake;
        guard.unlock();
        std::shared_ptr<const Shot> shot;
        if (ScenePtr scene = image->source->get(image->hand == "right"); scene && !job.load_only)
          shot = std::make_shared<const Shot>(draw(*scene, job.turn, job.scale, job.ss));
        guard.lock();
        if (shot) done[image] = {job.turn, job.scale, job.ss, std::move(shot)};
        if (notify && shot) {
          guard.unlock();
          notify();
          guard.lock();
        }
      }
    }).detach();
  }
};
Turntable& turntable() {
  static auto* t = new Turntable;  // its thread outlives static destruction
  return *t;
}
}  // namespace

ScenePtr scene_of(const Image& image) { return image.source ? image.source->get(image.hand == "right") : nullptr; }

void prepare_turn(const Image& image, const std::function<void()>& wake) {
  if (!image.source) return;
  auto& t = turntable();
  std::lock_guard guard(t.lock);
  static std::set<const Image*> asked;
  if (!asked.insert(&image).second) return;
  t.wake = wake;
  t.start();
  if (!t.pending.contains(&image)) t.pending[&image] = {Turn{}, 1, 1, true};
  t.wakeup.notify_one();
}

std::shared_ptr<const Shot> turned(const Image& image, const Turn& turn, float scale, bool sharp,
                                   const std::function<void()>& wake) {
  if (!image.source) return nullptr;
  auto& t = turntable();
  std::lock_guard guard(t.lock);
  t.wake = wake;
  t.start();
  const int ss = sharp ? 3 : 1;
  auto it = t.done.find(&image);
  const bool current = it != t.done.end() && it->second.turn == turn && it->second.ss >= ss &&
                       std::abs(it->second.scale - scale) <= 0.03f * scale;
  if (!current) {
    auto& job = t.pending[&image];
    job = {turn, scale, ss, false};
    t.wakeup.notify_one();
  }
  return it == t.done.end() ? nullptr : it->second.shot;
}

}  // namespace venture::launcher::art
