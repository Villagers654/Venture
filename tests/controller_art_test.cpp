// The Controls page's turnable controller models: the bundled glTF models drawn
// unturned land on their pictures (same size, inputs where the pictures show
// them), turning hides what the model covers, and drags compose as rotations.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include "launcher/assets/controller_art.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

using namespace venture::launcher::art;
namespace fs = venture::fs;

namespace {
constexpr const char* kTurnable[] = {"index", "touch", "vive", "wmr", "pico4", "cosmos", "focus3"};

const Anchor* find(const Shot& shot, std::string_view input) {
  for (const auto& a : shot.anchors)
    if (a.input == input) return &a;
  return nullptr;
}
}  // namespace

TEST_CASE("bundled models drawn unturned match their pictures") {
  for (const char* name : kTurnable)
    for (const char* hand : {"left", "right"}) {
      const std::string which = std::string(name) + "-" + hand;
      CAPTURE(which);
      const Image* image = controller(name, hand);
      REQUIRE(image);
      REQUIRE(turnable(*image));
      const ScenePtr scene = scene_of(*image);
      REQUIRE(scene);
      const auto [w, h] = scene_size(*scene);
      CHECK(std::abs(w - image->width) <= 1);
      CHECK(h == image->height);
      const Shot shot = draw(*scene, Turn{}, 0.5f, 1, true);
      if (const char* dir = std::getenv("VENTURE_ART_DUMP")) {
        const Shot big = draw(*scene, Turn{}, 1, 2, true);
        stbi_write_png((std::string(dir) + "/" + which + ".png").c_str(), big.width, big.height, 4, big.rgba.data(),
                       big.width * 4);
      }
      CHECK(shot.width == (image->width + 1) / 2);
      CHECK(shot.anchors.size() == image->spots.size());
      int hidden = 0;
      for (const auto& spot : image->spots) {
        CAPTURE(spot.input);
        const Anchor* a = find(shot, spot.input);
        REQUIRE(a);
        // render_controller.py measures node boxes a little differently.
        CHECK(std::hypot(a->x - spot.x, a->y - spot.y) < 7.f);
        hidden += a->hidden;
      }
      // Most inputs show from the picture's side (the Cosmos ring covers its face
      // buttons; a Vive wand's far grip button is behind it).
      CHECK(hidden * 2 <= int(image->spots.size()));
      // Some pixels are the controller.
      size_t covered = 0;
      for (size_t i = 3; i < shot.rgba.size(); i += 4) covered += shot.rgba[i] > 0;
      CHECK(covered > shot.rgba.size() / 4 / 8);
    }
  CHECK_FALSE(turnable(*controller("generic", "left")));  // the neutral picture stays still
}

TEST_CASE("turning a model around hides the inputs on its far side") {
  const Image* image = controller("touch", "left");
  const ScenePtr scene = scene_of(*image);
  REQUIRE(scene);
  Turn around;
  around.drag(180, 0);
  const Shot back = draw(*scene, around, 0.5f, 1);
  REQUIRE(find(back, "x"));
  CHECK(find(back, "x")->hidden);
  CHECK(find(back, "y")->hidden);
  CHECK(find(back, "thumbstick")->hidden);
  // Mirrored across the pivot: the face buttons swap sides of the picture.
  const Shot front = draw(*scene, Turn{}, 0.5f, 1);
  const auto [w, h] = scene_size(*scene);
  (void)h;
  const float pivot_guess = (front.x0 + front.x1) * 0.5f;
  CHECK((find(front, "x")->x - pivot_guess) * (find(back, "x")->x - pivot_guess) < 0);
  CHECK(w > 0);
}

TEST_CASE("drags compose into rotations") {
  Turn t;
  CHECK(t.identity());
  t.drag(30, 0);
  CHECK_FALSE(t.identity());
  t.drag(-30, 0);
  for (int i = 0; i < 9; ++i) CHECK(t.m[size_t(i)] == doctest::Approx(Turn{}.m[size_t(i)]).epsilon(1e-5));
  // A quarter turn right brings the near side (+z) to the right (+x).
  Turn q;
  q.drag(90, 0);
  CHECK(q.m[2] == doctest::Approx(1).epsilon(1e-5));  // x of the old z axis
  // Down brings the near side down.
  Turn d;
  d.drag(0, 90);
  CHECK(d.m[5] == doctest::Approx(-1).epsilon(1e-5));  // y of the old z axis
  // Many small drags stay a rotation.
  Turn many;
  for (int i = 0; i < 1000; ++i) many.drag(1.3f, -0.7f);
  float det = many.m[0] * (many.m[4] * many.m[8] - many.m[5] * many.m[7]) -
              many.m[1] * (many.m[3] * many.m[8] - many.m[5] * many.m[6]) +
              many.m[2] * (many.m[3] * many.m[7] - many.m[4] * many.m[6]);
  CHECK(det == doctest::Approx(1).epsilon(1e-4));
}

TEST_CASE("turned drawings are quick enough to follow a drag") {
  // Reported, not asserted: CI machines vary. The launcher draws the quick
  // version while dragging and the smooth one on release.
  for (const char* name : {"index", "cosmos", "vive"}) {
    const ScenePtr scene = scene_of(*controller(name, "left"));
    REQUIRE(scene);
    Turn t;
    t.drag(40, 25);
    auto time = [&](int ss) {
      const auto start = std::chrono::steady_clock::now();
      const Shot shot = draw(*scene, t, 0.6f, ss);
      CHECK(shot.width > 0);
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    const double quick = time(1), sharp = time(3);
    MESSAGE(std::string(name) << ": quick " << quick << " ms, sharp " << sharp << " ms");
  }
}

namespace {
#ifdef _WIN32
void set_env(const char* name, const std::string& value) { _putenv_s(name, value.c_str()); }
#else
void set_env(const char* name, const std::string& value) { setenv(name, value.c_str(), 1); }
#endif
// A box with a button on its +Z face, as an OBJ.
std::string box_obj(float x0, float y0, float z0, float x1, float y1, float z1) {
  std::string out;
  for (int i = 0; i < 8; ++i)
    out += std::format("v {} {} {}\n", i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0);
  // Faces by corner index (1-based): -x, +x, -y, +y, -z, +z.
  for (const char* f : {"1 3 7 5", "2 6 8 4", "1 5 6 2", "3 4 8 7", "1 2 4 3", "5 7 8 6"}) out += std::string("f ") + f + "\n";
  return out;
}
}  // namespace

TEST_CASE("installed render models are found through openvrpaths and turn") {
  const fs::path root = fs::temp_directory_path() / "venture-render-model-test";
  std::error_code ec;
  fs::remove_all(root, ec);
  const fs::path models = root / "runtime/resources/rendermodels/venture_test_controller";
  fs::create_directories(models);
  std::ofstream(models / "venture_test_controller.json")
      << R"({"thumbnail":"","components":{"body":{"filename":"body.obj"},"a":{"filename":"a.obj","motion":{"pressed_path":"/input/a/click"}}}})";
  std::ofstream(models / "body.obj") << box_obj(-0.02f, -0.08f, -0.02f, 0.02f, 0.08f, 0.02f);
  std::ofstream(models / "a.obj") << box_obj(-0.006f, 0.04f, 0.02f, 0.006f, 0.052f, 0.026f);
  const fs::path config = root / "config/openvr";
  fs::create_directories(config);
  std::string runtime = venture::paths::utf8(root / "runtime");
  std::ranges::replace(runtime, char(92), '/');
  std::ofstream(config / "openvrpaths.vrpath") << std::format(R"({{"runtime":["{}"],"version":1}})", runtime);
#ifdef _WIN32
  set_env("LOCALAPPDATA", venture::paths::utf8(root / "config"));
#else
  set_env("XDG_CONFIG_HOME", venture::paths::utf8(root / "config"));
#endif
  const auto dir = find_render_model("venture_test_controller");
  REQUIRE(dir);
  const auto pair = installed_pair(*dir, *dir, View{.automatic = false, .azimuth = 0, .elevation = 0, .roll = 0}, 200);
  REQUIRE(pair);
  const Shot front = draw(*(*pair)[0], Turn{}, 1, 1, true);
  REQUIRE(find(front, "a"));
  CHECK_FALSE(find(front, "a")->hidden);
  Turn around;
  around.drag(180, 0);
  const Shot back = draw(*(*pair)[0], around, 1, 1);
  CHECK(find(back, "a")->hidden);
  // Callouts: a shown button's line runs clear across the face it sits on; seen
  // from behind, the box covers the line from the button out to its silhouette.
  const Anchor& fa = *find(front, "a");
  CHECK(covered_spans(front, fa, fa.x, fa.y, front.x1 + 20, fa.y).empty());
  const Anchor& ba = *find(back, "a");
  const auto spans = covered_spans(back, ba, ba.x, ba.y, back.x1 + 20, ba.y);
  REQUIRE(spans.size() == 1);
  CHECK(spans[0].first == 0.f);
  CHECK(spans[0].second > 0.f);
  CHECK(spans[0].second < 1.f);
  // It ends at the box's edge, where the drawing stops covering.
  const float edge_x = ba.x + (back.x1 + 20 - ba.x) * spans[0].second;
  const float scale = float(back.width) / (back.x1 - back.x0);
  const int px = int((edge_x - back.x0) * scale), py = int((ba.y - back.y0) * scale);
  CHECK(back.rgba[(size_t(py) * back.width + size_t(std::min(px + 2, back.width - 1))) * 4 + 3] == 0);
  CHECK(back.rgba[(size_t(py) * back.width + size_t(std::max(px - 3, 0))) * 4 + 3] > 0);
  fs::remove_all(root, ec);
}

TEST_CASE("render models a Steam app's driver ships are found in a library SteamVR's registry leads to") {
  // The PlayStation VR2 App layout: <library>/steamapps/common/<app>/<plug-in>/
  // driver.vrdrivermanifest beside resources/rendermodels/<model>_left|_right, each
  // an OBJ + MTL + PNG set. The library is listed only in libraryfolders.vdf of the
  // Steam root that holds SteamVR, and the launcher runs data-dir isolated, so
  // Steam's own registration is not consulted.
  const fs::path root = fs::temp_directory_path() / "venture-render-model-app-test";
  std::error_code ec;
  fs::remove_all(root, ec);
  const fs::path steam = root / "steam", library = root / "library two";
  fs::create_directories(steam / "steamapps/common/SteamVR");
  std::string library_text = venture::paths::utf8(library), escaped;
  for (char c : library_text) escaped += c == char(92) ? std::string(2, char(92)) : std::string(1, c);
  std::ofstream(steam / "steamapps/libraryfolders.vdf")
      << std::format("\"libraryfolders\"\n{{\n\t\"1\"\n\t{{\n\t\t\"path\"\t\t\"{}\"\n\t}}\n}}\n", escaped);
  const fs::path plugin = library / "steamapps/common/Test VR App/SteamVR_Plug-In";
  fs::create_directories(plugin);
  std::ofstream(plugin / "driver.vrdrivermanifest") << R"({"name":"venture_test","directory":""})";
  for (const char* hand : {"left", "right"}) {
    const std::string name = std::string("venture_test_sense_") + hand;
    const fs::path dir = plugin / "resources/rendermodels" / name;
    fs::create_directories(dir);
    std::ofstream(dir / (name + ".json")) << std::format(
        R"({{"thumbnail":"{0}_thumbnail.png","components":{{"body":{{"filename":"{0}_body.obj","component_local":{{"origin":[0,0,0],"rotate_xyz":[0,0,0]}}}},)"
        R"("button_a":{{"filename":"{0}_square.obj","motion":{{"pressed_path":"/input/square/click"}}}},)"
        R"("sensor":{{"visibility":{{"default":false}}}}}}}})",
        name);
    const unsigned char texel[4 * 4 * 3] = {200, 40, 40, 200, 40, 40, 200, 40, 40, 200, 40, 40, 200, 40, 40, 200, 40, 40,
                                            200, 40, 40, 200, 40, 40, 200, 40, 40, 200, 40, 40, 200, 40, 40, 200, 40, 40,
                                            200, 40, 40, 200, 40, 40, 200, 40, 40, 200, 40, 40};
    stbi_write_png(venture::paths::utf8(dir / (name + ".png")).c_str(), 4, 4, 3, texel, 12);
    for (const char* part : {"body", "square"}) {
      std::ofstream(dir / std::format("{}_{}.mtl", name, part))
          << std::format("newmtl mat\nKd 1 1 1\nmap_Kd {}.png\n", name);
      std::string obj = std::format("mtllib {}_{}.mtl\nusemtl mat\nvt 0.5 0.5\n", name, part);
      // Every corner textured from the one texture coordinate ("f 1/1 3/1 ...").
      std::istringstream box(std::string(part) == "body" ? box_obj(-0.02f, -0.08f, -0.02f, 0.02f, 0.08f, 0.02f)
                                                         : box_obj(-0.006f, 0.04f, 0.02f, 0.006f, 0.052f, 0.026f));
      for (std::string line; std::getline(box, line);) {
        if (line.starts_with("f ")) {
          std::istringstream corners(line.substr(2));
          line = "f";
          for (std::string c; corners >> c;) line += " " + c + "/1";
        }
        obj += line + "\n";
      }
      std::ofstream(dir / std::format("{}_{}.obj", name, part)) << obj;
    }
  }
  const fs::path config = root / "config/openvr";
  fs::create_directories(config);
  std::string runtime = venture::paths::utf8(steam / "steamapps/common/SteamVR");
  std::ranges::replace(runtime, char(92), '/');
  std::ofstream(config / "openvrpaths.vrpath")
      << std::format(R"({{"runtime":["{}"],"external_drivers":null,"version":1}})", runtime);
#ifdef _WIN32
  set_env("LOCALAPPDATA", venture::paths::utf8(root / "config"));
#else
  set_env("XDG_CONFIG_HOME", venture::paths::utf8(root / "config"));
#endif
  set_env("VENTURE_DATA_DIR", venture::paths::utf8(root / "data"));
  const auto left = find_render_model("venture_test_sense_left"), right = find_render_model("venture_test_sense_right");
  set_env("VENTURE_DATA_DIR", "");
  REQUIRE(left);
  REQUIRE(right);
  CHECK(venture::paths::utf8(left->filename()) == "venture_test_sense_left");
  const auto pair = installed_pair(*left, *right, View{}, 200);
  REQUIRE(pair);
  for (const auto& scene : *pair) {
    const Shot shot = draw(*scene, Turn{}, 1, 1, true);
    REQUIRE(find(shot, "square_mndx"));
    size_t covered = 0, textured = 0;
    for (size_t i = 0; i + 3 < shot.rgba.size(); i += 4)
      if (shot.rgba[i + 3] > 0) {
        ++covered;
        textured += shot.rgba[i] > shot.rgba[i + 1] + 40;  // the red texture, not the untextured grey
      }
    CHECK(covered > 500);
    CHECK(textured * 2 > covered);
  }
  fs::remove_all(root, ec);
}
