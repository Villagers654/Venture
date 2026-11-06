#pragma once
// Controller pictures for the Controls page, with the picture position of each
// input for the callouts: the user's own installed SteamVR render models first
// (drawn once and cached), else the bundled WebXR pictures (tools/controller_art).
// Both turn: the player drags a model to see it from any side.
#include "common/paths.h"
#include "launcher/assets/render_model.h"
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace venture::launcher::art {
struct Spot {
  std::string input;  // host input component: "a", "trigger", "thumbstick", "share_mndx"...
  float x = 0, y = 0; // picture pixels
};
// Loads a picture's model (both hands of an installed pair at once).
struct SceneSource;
struct Image {
  std::string name, hand;  // "index" / "left"
  int width = 0, height = 0;
  std::vector<Spot> spots;
  std::span<const unsigned char> png;  // bundled pictures
  std::vector<unsigned char> rgba;     // drawn pictures (straight alpha)
  bool installed = false;              // drawn from the user's installed model
  std::shared_ptr<SceneSource> source; // its model, when it can turn
};
// The bundled picture of `name` for `hand`, or nullptr.
const Image* controller(std::string_view name, std::string_view hand);
// The installed SteamVR render model `model` for `hand` ("<model>_<hand>", else
// "<model>"), drawn in the background and cached in `cache`. nullptr while it is
// drawn (`*pending` set; `wake` runs when it is ready) or when it is not installed.
const Image* installed_controller(std::string_view model, std::string_view hand, const fs::path& cache,
                                  const std::function<void()>& wake, bool* pending = nullptr);

// Turned views, drawn on one background thread; the newest request per picture wins.
inline bool turnable(const Image& image) { return image.source != nullptr; }
// The picture's model, loaded on first use (blocks); nullptr when it has none.
ScenePtr scene_of(const Image& image);
// Loads the picture's model in the background, so the first turn draws at once.
void prepare_turn(const Image& image, const std::function<void()>& wake);
// The newest finished view of `image` (perhaps of an earlier turn or coarser),
// asking for `turn` at `scale` screen pixels per picture pixel; `sharp` asks for
// the smooth drawing, else the quick one used while dragging. nullptr until the
// first view is ready; `wake` runs when a view is.
std::shared_ptr<const Shot> turned(const Image& image, const Turn& turn, float scale, bool sharp,
                                   const std::function<void()>& wake);
}  // namespace venture::launcher::art
