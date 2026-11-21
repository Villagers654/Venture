// The desktop window of a 2D app (an Android app without a VR launch activity,
// run on layoutlib's framework: docs/design/layoutlib.md). The app's panel
// (com.venture.runtime.layoutlib.Panel, its windows composited) is shown
// letterboxed in a normal resizable window; the mouse taps and drags as a
// finger does on a Quest's panel, hovers as a mouse, and scrolls with the
// wheel; the PC keyboard types into the focused field and Escape is Back.
// Closing the window quits the app.
//
// The window opens on the first frame the panel presents, on its own thread;
// a game that never presents a panel (every VR game) creates nothing.
//
// Threads: Java's main thread presents (a copy under the mutex), a Java input
// thread waits for events, and the window thread owns SDL.
#pragma once
#include <cstdint>
#include <vector>

namespace venture::xr::panel {
// Registers com.venture.runtime.layoutlib.PanelHost's natives (before or after
// the JVM starts); a run without that class registers nothing.
void install_java_bridge();

// Event records, six values each, as PanelHost.java reads them.
enum Kind : std::int32_t { pointer = 0, scroll = 1, key = 2, text = 3, close = 4 };
// Android's KeyEvent keycode for an SDL keycode, 0 for none. Printable keys
// map too; the window sends them as keys only with Ctrl or Alt held (shortcuts),
// as text otherwise.
std::int32_t android_keycode(std::uint32_t sdl_keycode);
// Android's KeyEvent meta state for SDL modifier bits.
std::int32_t android_meta(std::uint16_t sdl_mod);
} // namespace venture::xr::panel
