#pragma once
// Controller models drawn by a small software rasterizer for the Controls page:
// the user's own installed SteamVR render models (read only: the model files are
// never copied; only the drawn picture is cached) and Venture's bundled WebXR
// glTF models. Both can be drawn turned to any angle, with the picture position
// of every input and whether the model hides it.
#include "common/paths.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace venture::launcher::art {

// Orthographic view, in degrees, in OpenVR controller space (+Y up, -Z forward):
// azimuth turns around +Y from behind the controller (0) toward its +X side,
// elevation looks down from above, roll turns the picture; the right hand mirrors
// it. Automatic stands the controller up and picks the view that shows its inputs best.
struct View {
  bool automatic = true;
  float azimuth = 0, elevation = 0, roll = 0;
};

// How far the player turned a model: a rotation in picture space (x right, y up,
// z toward the viewer), applied about the model's middle.
struct Turn {
  std::array<float, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};  // row major
  bool identity() const;
  // Turns by a pointer drag of `dx`, `dy` degrees: the near side follows the pointer.
  void drag(float dx, float dy);
  bool operator==(const Turn&) const = default;
};

struct Anchor {
  std::string input;   // host input component: "a", "trigger", "thumbstick", "share_mndx"...
  float x = 0, y = 0;  // picture pixels of the unturned picture
  bool hidden = false; // the model hides it from this side
  float z = 0;         // its depth toward the viewer, in model units
};

// One drawing. Its pixels cover x0..x1, y0..y1 in the unturned picture's pixels
// (a turned model can reach past that picture's edges).
struct Shot {
  int width = 0, height = 0;
  std::vector<unsigned char> rgba;  // straight alpha
  std::vector<float> depth;         // per pixel, the nearest surface toward the viewer; -inf where empty
  float slack = 0;                  // depth slack for surface points, in model units
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  std::vector<Anchor> anchors;
  Turn turn;  // what it shows
};

// Where the straight line from (fx, fy) to (tx, ty), in unturned picture pixels,
// passes behind the model as seen from `anchor`: the spans of the line's parameter
// (0 at from, 1 at to), in order, where the model's surface is nearer the viewer
// than the anchor. A shown anchor counts from the surface it sits on, so only
// geometry standing in front of that surface covers its line.
std::vector<std::pair<float, float>> covered_spans(const Shot& shot, const Anchor& anchor, float fx, float fy,
                                                   float tx, float ty);

// A model with its unturned view: what the pictures show before the player turns it.
struct Scene;
using ScenePtr = std::shared_ptr<const Scene>;

// The unturned picture's size.
std::array<int, 2> scene_size(const Scene& scene);

// Draws `scene` turned by `turn`, `scale` output pixels per unturned picture pixel
// at `ss` samples per pixel and axis. `whole` draws exactly the unturned picture's
// area (the stills) instead of the turned model's extent.
Shot draw(const Scene& scene, const Turn& turn, float scale, int ss, bool whole = false);

// Where render model `name` ("frame_controller_left") is installed: SteamVR's
// runtime and its drivers, openvrpaths' external drivers, and drivers installed
// as Steam apps in any Steam library.
std::optional<fs::path> find_render_model(std::string_view name);

// The left and right installed models (the same directory when one model serves
// both hands) `height` pixels tall as a matched pair: one camera chosen on the
// left and mirrored for the right, one scale, and mirrored framing.
std::optional<std::array<ScenePtr, 2>> installed_pair(const fs::path& left, const fs::path& right, View view,
                                                      int height);

// A bundled glTF binary model seen like tools/controller_art/render_controller.py
// sees it (`side` -1 for the left hand, +1 for the right; azimuth, elevation and
// roll in degrees), `height` pixels tall. `spots` are the bundled picture's input
// positions: inputs the model has no part for are found on its surface there.
struct Spot2 {
  std::string_view input;
  float x = 0, y = 0;
};
ScenePtr bundled_scene(std::span<const unsigned char> glb, float side, float azimuth, float elevation, float roll,
                       int height, std::span<const Spot2> spots);

}  // namespace venture::launcher::art
