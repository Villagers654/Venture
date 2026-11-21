// A 2D app's desktop window (xr/panel_window.h): the PC keyboard's keys and
// modifiers as Android's KeyEvent codes and meta state.
#include "xr/panel_window.h"
#include <doctest/doctest.h>

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
