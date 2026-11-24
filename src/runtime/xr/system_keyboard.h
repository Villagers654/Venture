// The system keyboard. On a Quest, Horizon OS shows its own keyboard over a game
// that asks for text (InputMethodManager.showSoftInput); a game waits for it
// (issue 160: Pinball Universe's name prompt). com.venture.runtime.SystemKeyboard
// edits the field and draws the keyboard; this shows that picture as a panel in
// front of the player on top of the game's layers, for OpenXR and VrApi games
// on any host runtime, finds the key each controller's aim ray points at, and
// queues hovers, trigger presses and the desktop mirror's PC typing for Java to
// read on the app's main Looper. While it is up the game sees no controller
// input, as on a Quest where the keyboard takes input focus; head tracking stays.
//
// Threads: Java (show, hide, take_events), the guest's input sync (point), the
// mirror thread (type_*) and the frame thread (extend, under the session lock)
// meet under one mutex; the frame thread never waits on the GPU or a swapchain:
// an upload that is not ready is tried again next frame. A closed keyboard costs
// a frame one relaxed load (shown()).
#pragma once
#include "xr/api.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace venture::xr::keyboard {
// Event kinds, as SystemKeyboard.java reads them: kind, two values, layout.
enum Event : std::int32_t { hover = 0, press = 1, release = 2, text = 3, key = 4 };
struct Rect { std::int32_t x, y, w, h; };

// ---- com.venture.runtime.SystemKeyboard ------------------------------------------
// Registers the Java natives (before or after the JVM starts).
void install_java_bridge();
// A new picture (premultiplied 0xAARRGGBB, row 0 at the top) and its keys'
// rectangles; opens the keyboard in front of the player if it was closed.
void show(std::vector<std::uint32_t> argb, std::uint32_t width, std::uint32_t height, std::vector<Rect> keys,
          std::int32_t layout);
void hide();
// The queued events, four values each; empties the queue.
std::vector<std::int32_t> take_events();

namespace detail { inline std::atomic<bool> shown{false}; }
inline bool shown() { return detail::shown.load(std::memory_order_relaxed); }

// ---- The frame thread (Session::end) -----------------------------------------------
// The game's End with the keyboard's layers on top. It owns everything `info`
// points to (keeping `game`, the caller's owner), so an asynchronous host End
// may keep it. Null while there is nothing to show yet.
struct Frame {
  std::shared_ptr<const void> game;
  std::vector<const XrCompositionLayerBaseHeader *> layers;
  std::array<XrCompositionLayerQuad, 3> quads{};
  XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
};
std::shared_ptr<const Frame> extend(XrSession, const XrFrameEndInfo &, std::shared_ptr<const void> game);
// Session::destroy, under the session and queue locks.
void reset_session(XrSession);

// ---- The guest's input sync ----------------------------------------------------------
// One controller's aim pose space and trigger value at `time`; true when it
// pressed a key (the caller ticks that hand's vibration).
bool point(int hand, XrSpace aim, float trigger, XrTime time);
void tick(std::string_view api, XrSession, XrAction haptic, XrPath hand);
// How far along that hand's aim ray the keyboard was hit at its last point();
// nullopt while it is closed or the ray misses it (a 2D panel behind it then
// takes the ray, panel_xr.cpp).
std::optional<float> hit_distance(int hand);

// ---- The desktop mirror's keyboard ---------------------------------------------------
void type_text(std::string_view utf8);
void type_key(std::int32_t android_keycode);

// ---- Geometry and pixels (pure) ------------------------------------------------------
// The panel's pose in an upright space: in front of and below the head, facing
// it, turned with the head's heading only.
XrPosef panel_pose(XrPosef head);
// Where an aim ray (-Z of `aim`) meets the front of a quad of `size` at `panel`,
// in pixels of a width x height picture from its top-left.
struct Hit { float x, y, distance; };
std::optional<Hit> hit(XrPosef panel, XrExtent2Df size, XrPosef aim, std::uint32_t width, std::uint32_t height);
int key_at(const std::vector<Rect> &, float x, float y);
// Premultiplied ARGB into a 4-byte RGBA or BGRA swapchain format; a UNORM image
// holds linear values, an sRGB one the encoded bytes.
void convert(const std::uint32_t *argb, std::size_t count, std::int64_t format, std::uint8_t *out);
// Room for `wanted` more layers under the host's limit (0: none known): drops
// the game's last non-projection layers first, never a projection. The result
// is how many of the wanted layers fit.
std::uint32_t fit_layers(std::vector<const XrCompositionLayerBaseHeader *> &, std::uint32_t limit,
                         std::uint32_t wanted);
} // namespace venture::xr::keyboard
