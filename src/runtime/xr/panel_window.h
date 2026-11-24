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
#include "xr/api.h"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace venture::xr::panel {
// Registers com.venture.runtime.layoutlib.PanelHost's natives (before or after
// the JVM starts); a run without that class registers nothing.
void install_java_bridge();

// Event records, six values each, as PanelHost.java reads them.
enum Kind : std::int32_t { pointer = 0, scroll = 1, key = 2, text = 3 };
// Android's KeyEvent keycode for an SDL keycode, 0 for none. Printable keys
// map too; the window sends them as keys only with Ctrl or Alt held (shortcuts),
// as text otherwise.
std::int32_t android_keycode(std::uint32_t sdl_keycode);
// Android's KeyEvent meta state for SDL modifier bits.
std::int32_t android_meta(std::uint16_t sdl_mod);
// The person closed the app's window (or a panel script's "quit"): the app
// ends as a removed task does on a Quest, through the runtime's OS quit
// (pause, stop, then the process ends), like a stop from the launcher.
bool close_requested();
void request_close();

// ---- Shared with the headset view (panel_xr.cpp) ---------------------------------
// The newest panel picture (RGBA, premultiplied, row 0 at the top) if it is newer
// than `seen`: copies it into `out` and returns its generation; 0 when there is none.
std::uint64_t copy_frame(std::uint64_t seen, std::vector<std::uint8_t> &out, int &width, int &height);
// The panel's size in pixels (0 x 0 before its first frame).
std::array<int, 2> frame_size();
// Queues input events (the records above) as the desktop window does.
void post_events(const std::vector<std::array<std::int32_t, 6>> &events);

// ---- In the headset (panel_xr.cpp) -----------------------------------------------
// A 2D app's process that never creates an XR session of its own shows its panel
// in the headset, as a Quest shows a 2D app: Venture opens a host session itself
// and submits the panel as a quad in front of the player, with each controller's
// laser, its cursor and the system keyboard; the desktop window mirrors it. With
// no headset the panel stays on the desktop, and the session opens when a runtime
// and headset appear later. Started on the panel's first frame (java_present).
void start_headset();
// The app is about to create its own XR session (OpenXR or VrApi): the panel's
// session ends first and does not come back in this process. Blocks briefly.
void release_headset();
// The OS quit's XR shutdown: the panel's session and thread end (blocks briefly).
void stop_headset();
// The panel is shown in the headset (its session is running).
bool in_headset();

// ---- Geometry (pure, panel_xr.cpp) ------------------------------------------------
// Where a panel `width_m` wide opens: ahead of the head along its heading, a
// little below eye level, upright and turned to face it (as Horizon opens a 2D
// panel). `head` is in an upright space.
XrPosef headset_panel_pose(XrPosef head);
constexpr float kPanelDistance = 1.4f, kPanelWidth = 1.3f;
// Where the aim ray (-Z of `aim`) meets the panel's plane, in pixels of a
// width x height picture from its top-left, and how far along the ray; nullopt
// when the ray points away from its front or (bounded) misses the picture.
struct PanelHit { float x, y, distance; };
std::optional<PanelHit> panel_hit(XrPosef panel, XrExtent2Df size, XrPosef aim, std::uint32_t width,
                                  std::uint32_t height, bool bounded = true);
// The pose of a laser quad `length` long along the aim ray from `aim`: centred on
// the ray, its +Y along it and its face turned toward `eye` as far as it can.
XrPosef laser_pose(XrPosef aim, float length, XrVector3f eye);
} // namespace venture::xr::panel
