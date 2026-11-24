// A 2D app's desktop window (xr/panel_window.h): the PC keyboard's keys and
// modifiers as Android's KeyEvent codes and meta state; and its headset view's
// geometry: where the panel opens and where a controller's laser meets it.
#include "xr/panel_window.h"
#include "xr/math.h"
#include <doctest/doctest.h>
#include <cmath>

namespace panel = venture::xr::panel;

TEST_CASE("2D panel window maps PC keys to Android keycodes") {
  CHECK(panel::android_keycode('a') == 29);           // KEYCODE_A
  CHECK(panel::android_keycode('z') == 54);           // KEYCODE_Z
  CHECK(panel::android_keycode('0') == 7);            // KEYCODE_0
  CHECK(panel::android_keycode('\r') == 66);          // Enter
  CHECK(panel::android_keycode('\b') == 67);          // Backspace is KEYCODE_DEL
  CHECK(panel::android_keycode(0x7f) == 112);         // Delete is KEYCODE_FORWARD_DEL
  CHECK(panel::android_keycode(0x1b) == 4);           // Escape is Back
  CHECK(panel::android_keycode(0x40000052u) == 19);   // Up
  CHECK(panel::android_keycode(0x4000004fu) == 22);   // Right
  CHECK(panel::android_keycode(0x4000003au) == 131);  // F1
  CHECK(panel::android_keycode(0x40000081u) == 0);    // a key Android has no code for (Volume Down)
}

TEST_CASE("2D panel window maps PC modifiers to Android meta state") {
  CHECK(panel::android_meta(0) == 0);
  CHECK(panel::android_meta(0x0001) == (0x1 | 0x40));      // left Shift: META_SHIFT_ON | META_SHIFT_LEFT_ON
  CHECK(panel::android_meta(0x0080) == (0x1000 | 0x4000));  // right Ctrl: META_CTRL_ON | META_CTRL_RIGHT_ON
  CHECK(panel::android_meta(0x2000) == 0x100000);           // Caps Lock
}

// ---- The headset view (panel_xr.cpp) ---------------------------------------------
namespace {
XrPosef pose(float x, float y, float z, XrQuaternionf q = {0, 0, 0, 1}) { return {q, {x, y, z}}; }
}  // namespace

TEST_CASE("2D panel opens ahead of the head along its heading, below eye level, facing it") {
  // Head at 1.6 m looking ahead (-Z): the panel 1.4 m ahead, 0.1 m lower.
  const auto ahead = panel::headset_panel_pose(pose(0, 1.6f, 0));
  CHECK(ahead.position.x == doctest::Approx(0).epsilon(1e-5));
  CHECK(ahead.position.y == doctest::Approx(1.5f));
  CHECK(ahead.position.z == doctest::Approx(-panel::kPanelDistance));
  // A head turned 90 degrees left (+Y) and pitched down: only the heading counts.
  const float s = std::sqrt(0.5f);
  const XrQuaternionf left{0, s, 0, s};
  const XrQuaternionf down{-0.2588f, 0, 0, 0.9659f};  // 30 degrees down
  const auto turned = panel::headset_panel_pose(pose(1, 1.6f, 0, venture::xr::multiply(left, down)));
  CHECK(turned.position.x == doctest::Approx(1 - panel::kPanelDistance).epsilon(1e-4));
  CHECK(turned.position.y == doctest::Approx(1.5f).epsilon(1e-4));
  CHECK(turned.position.z == doctest::Approx(0).epsilon(1e-4));
  // Its front (+Z) points back at the eye.
  const auto front = venture::xr::rotate(ahead.orientation, {0, 0, 1});
  CHECK(front.z > 0.99f);
  CHECK(front.y > 0);  // tilted up toward the eye above its centre
}

TEST_CASE("2D panel laser hits the picture in pixels from its top-left") {
  const auto panel_pose = panel::headset_panel_pose(pose(0, 1.6f, 0));
  const XrExtent2Df size{panel::kPanelWidth, panel::kPanelWidth * 1000.f / 1600.f};
  // A controller straight below the eye, pointing ahead: the panel's centre.
  auto centre = panel::panel_hit(panel_pose, size, pose(0, 1.5f, -0.3f), 1600, 1000);
  REQUIRE(centre);
  CHECK(centre->x == doctest::Approx(800).epsilon(0.01));
  CHECK(centre->y == doctest::Approx(500).epsilon(0.02));
  CHECK(centre->distance == doctest::Approx(1.1f).epsilon(0.01));
  // Up and to the left of centre: smaller pixel coordinates.
  auto corner = panel::panel_hit(panel_pose, size, pose(-0.5f, 1.75f, -0.3f), 1600, 1000);
  REQUIRE(corner);
  CHECK(corner->x < 300);
  CHECK(corner->y < 300);
  // Pointing away, from behind, or past the edge: no hit (unless unbounded, for a drag).
  const XrQuaternionf back{0, 1, 0, 0};  // turned around
  CHECK_FALSE(panel::panel_hit(panel_pose, size, pose(0, 1.5f, -0.3f, back), 1600, 1000));
  CHECK_FALSE(panel::panel_hit(panel_pose, size, pose(0, 1.5f, -2.f), 1600, 1000));
  CHECK_FALSE(panel::panel_hit(panel_pose, size, pose(2.f, 1.5f, -0.3f), 1600, 1000));
  auto past = panel::panel_hit(panel_pose, size, pose(2.f, 1.5f, -0.3f), 1600, 1000, false);
  REQUIRE(past);
  CHECK(past->x > 1600);
}

TEST_CASE("2D panel laser quad runs along the aim ray and faces the eye") {
  const auto aim = pose(0.2f, 1.2f, -0.3f);
  const auto laser = panel::laser_pose(aim, 1.f, {0, 1.6f, 0});
  // Centred half way along the ray (-Z), its +Y along the ray.
  CHECK(laser.position.x == doctest::Approx(0.2f));
  CHECK(laser.position.y == doctest::Approx(1.2f));
  CHECK(laser.position.z == doctest::Approx(-0.8f));
  const auto along = venture::xr::rotate(laser.orientation, {0, 1, 0});
  CHECK(along.z == doctest::Approx(-1).epsilon(1e-4));
  // Its face is perpendicular to the ray and turned toward the eye (up and left of it).
  const auto face = venture::xr::rotate(laser.orientation, {0, 0, 1});
  CHECK(std::abs(face.z) < 1e-4f);
  CHECK(face.y > 0);
  CHECK(face.x < 0);
}
