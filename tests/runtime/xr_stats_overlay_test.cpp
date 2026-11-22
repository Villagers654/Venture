// The performance overlay (xr/stats_overlay.h): its picture, the host layer
// limit, and over the mock runtime its quad on top of a VrApi game's frame
// only while it is on, uploaded without waiting and released with the session.
#include "../mock_openxr_runtime/control.h"
#include "hle/vrapi/state.h"
#include "hle/vrapi/vrapi.h"
#include "xr/session.h"
#include "xr/stats_overlay.h"
#include "xr_images_fixture.h"
#include <doctest/doctest.h>
#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <thread>
#include <vector>

namespace so = venture::xr::stats_overlay;
namespace vr = venture::hle::vrapi;
namespace vg = vr::guest;

namespace {
struct Shown {
  explicit Shown(bool shown) { so::set_enabled(shown); }
  ~Shown() { so::set_enabled(false); }
};
std::optional<XrCompositionLayerQuad> overlay_quad() {
  std::array<XrCompositionLayerQuad, 8> quads{};
  const auto count = std::min<std::uint32_t>(ventureMockXrLastQuads(quads.data(), 8), 8);
  for (std::uint32_t i = 0; i < count; ++i)
    if (std::abs(quads[i].size.width - 0.5f) < 1e-4f) return quads[i];
  return std::nullopt;
}
struct VrApiGame {
  venture::test::XrImageFixture images;
  vr::abi::Mobile *mobile{};
  vr::abi::Submit submit{0, 1, 0, 0, 0, 0, nullptr};
  std::uint64_t index = 0;
  VrApiGame() {
    venture::xr::install_xr_images(&images);
    vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
    REQUIRE(vg::vrapi_Initialize(&init) == 0);
    vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
    mobile = vg::vrapi_EnterVrMode(&mode);
    REQUIRE(mobile);
    vr::abi::Event event{};
    while (vg::vrapi_PollEvent(&event) == vr::abi::success) {}
  }
  ~VrApiGame() {
    if (vr::state().mobile) vg::vrapi_LeaveVrMode(mobile);
    vg::vrapi_Shutdown();
    venture::xr::install_xr_images(nullptr);
  }
  void frame() {
    submit.frame_index = ++index;
    submit.display_time = vg::vrapi_GetPredictedDisplayTime(mobile, std::int64_t(index));
    REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  }
  // Frames until the overlay's quad is in the host's frame (uploads are never waited for).
  bool frames_until_overlay() {
    for (int i = 0; i < 400; ++i) {
      frame();
      if (overlay_quad()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }
};
std::uint32_t at(const std::vector<std::uint32_t> &pixels, int x, int y) {
  return pixels[std::size_t(y) * so::panel_width + std::size_t(x)];
}
bool any_opaque(const std::vector<std::uint32_t> &pixels, int x0, int y0, int x1, int y1, std::uint32_t color) {
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x)
      if (at(pixels, x, y) == color) return true;
  return false;
}
} // namespace

TEST_CASE("The overlay's picture is a dark band with its labels and values; unknown values show a dash") {
  so::Snapshot s;
  s.headset_hz = 90;
  s.game_fps = 89.6;
  s.worst_ms = 40;  // over three periods: red
  s.eye_width = 2064;
  s.eye_height = 2208;
  s.cpu_percent = 30;
  s.gpu_percent = 95;
  const std::vector<float> headset(60, 90.f), game(60, 60.f);
  const auto pixels = so::paint(s, headset, game);
  REQUIRE(pixels.size() == std::size_t(so::panel_width) * so::panel_height);
  // Transparent outside the band, translucent dark inside it.
  CHECK(at(pixels, 5, 2) == 0);
  CHECK(at(pixels, 5, 77) == 0);
  CHECK((at(pixels, 130, 70) >> 24) >= 0xf0u);
  // Premultiplied: no channel exceeds alpha.
  bool premultiplied = true;
  for (const auto p : pixels)
    for (const int shift : {16, 8, 0}) premultiplied &= ((p >> shift) & 0xff) <= (p >> 24);
  CHECK(premultiplied);
  // Values in their colours: the game near the headset's rate (green), the long
  // frame (red), GPU load past 90% (red), CPU under 75% (green).
  constexpr std::uint32_t green = 0xff58ee8au, red = 0xffff5757u, violet = 0xffc07affu;
  CHECK(any_opaque(pixels, 152, 36, 262, 58, green));
  CHECK(any_opaque(pixels, 398, 36, 540, 58, red));
  CHECK(any_opaque(pixels, 828, 20, 960, 34, red));
  CHECK(any_opaque(pixels, 692, 20, 816, 34, green));
  // The game's rate in the graph.
  CHECK(any_opaque(pixels, 274, 18, 375, 63, violet));
  // RAM and VRAM unknown: a dash after the label, nothing more.
  const auto blank = so::paint(so::Snapshot{}, {}, {});
  constexpr std::uint32_t label = 0xffd8d8d8u;
  CHECK(any_opaque(blank, 692 + 4 * 12, 46, 692 + 5 * 12, 60, label));   // "RAM -"
  CHECK_FALSE(any_opaque(blank, 692 + 5 * 12, 46, 816, 60, label));
}

TEST_CASE("The overlay goes into a frame only while the host takes one more layer") {
  CHECK(so::fits(1, 0));  // no known limit
  CHECK(so::fits(15, 16));
  CHECK_FALSE(so::fits(16, 16));
  CHECK_FALSE(so::fits(2, 1));
}

TEST_CASE("The overlay is drawn over a VrApi game only while it is on, and comes back after a new session") {
  {
    Shown off(false);
    VrApiGame game;
    const auto before = ventureMockXrStats();
    for (int i = 0; i < 8; ++i) game.frame();
    CHECK_FALSE(overlay_quad());
    // Off creates no swapchain and uploads nothing.
    CHECK(ventureMockXrStats().live_swapchains == before.live_swapchains);
    CHECK(ventureMockXrStats().released == before.released);
  }
  Shown on(true);
  const auto baseline = ventureMockXrStats().live_swapchains;
  {
    VrApiGame game;
    REQUIRE(game.frames_until_overlay());
    const auto quad = *overlay_quad();
    CHECK(quad.layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
    CHECK(quad.subImage.imageRect.extent.width == std::int32_t(so::panel_width));
    CHECK(quad.subImage.imageRect.extent.height == std::int32_t(so::panel_height));
    CHECK(quad.size.height == doctest::Approx(0.5f * so::panel_height / so::panel_width));
    CHECK(quad.pose.position.z < 0);
    // A picture is uploaded at most twice a second: frames in between acquire nothing.
    const auto uploaded = ventureMockXrStats();
    for (int i = 0; i < 4; ++i) game.frame();
    CHECK(ventureMockXrStats().released - uploaded.released <= 1);
    CHECK(overlay_quad());
  }
  // The session took the overlay's swapchain with it; a new one draws it again.
  CHECK(ventureMockXrStats().live_swapchains == baseline);
  VrApiGame again;
  REQUIRE(again.frames_until_overlay());
}
