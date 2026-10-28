#pragma once
#include "control.h"
#include "scene_signature.h"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mock {
// VENTURE_MOCK_INPUT names a JSON object with an events array. Each event has time
// (seconds since the first xrSyncActions), and optional head/left/right updates.
// hand_tracking_active enables tracked hands independently of controllers;
// left_hand/right_hand set wrist poses, with the mock's open-hand joint model.
// Optional left_hand_joints/right_hand_joints replace that model with 26 poses
// in XR_HAND_JOINT_* order, relative to the wrist. Each needs position and
// orientation; null restores the open hand. Joint updates persist like poses.
// Poses use position [x,y,z] metres and orientation [x,y,z,w], in STAGE.
// LOCAL is calibrated at the initial head position/yaw, with pitch/roll removed.
// Updates persist until replaced. pose_interpolation:"linear" on an event
// interpolates its explicit poses from their previous pose keyframes (position
// lerp, shortest quaternion slerp). Buttons remain discrete at event.time.
// The runtime reloads edits once per second, keeping the original time origin.
// "wait" on an event holds the timeline at that event until the runtime opens the
// gate (true for the defaults, or an object overriding them): the eye image has
// changed from the moment the hold began by at least "change", then stayed within
// "still" of itself for "stable" seconds; or "timeout" seconds passed. Images are
// compared by a coarse luma signature (mean absolute difference, 0..1); change 0
// waits only for stillness. "match" (96 numbers: a signature captured from a
// known screen with tools/scene_signature.py; or a list of them, any of which
// counts) instead waits until the image is
// within "within" of that screen for "stable" seconds (an 8-bit UNORM image counts in
// either reading: sRGB-encoded as linear values, or its bytes as stored). Poses hold too, and later
// events keep their spacing behind the hold, so routes advance as fast as each
// runtime loads. "skip" makes the next seconds conditional: when the gate times
// out, the events after it up to its time plus "skip" are not applied (for a
// screen that only some runtimes show; their button changes still accumulate
// into later events' state, so keep such spans to press/release pairs).
// "watches" (optional, top level) answer a screen that can appear at any time, the same way
// whenever it does (a dismissable error dialog, say). Each watch has "match": an array of
// 96-number signatures, any of which counts; "within" (0.015) and "stable" (0.5 s) as for
// gates (an 8-bit UNORM image matches in either reading, see runtime.cpp linear8); optional "from"/"until" route seconds; "cooldown" (2 s) before it can fire again;
// and "press": events with "time" in seconds after the watch fires and only left/right
// controller updates. While a press runs, its controller state replaces the route's (the
// route keeps advancing underneath, holds included); 0.2 s after its last event the
// route's controllers return. Signatures are sampled while a watch is in its span.
struct Watch {
  std::vector<SceneSignature> match;
  double within = 0.015, stable = 0.5, from = 0, until = 1e300, cooldown = 2;
  std::vector<std::pair<double, std::string>> press;  // (seconds after firing, controller JSON)
};
// A half-open [begin, end) span of timeline seconds, written "begin:end".
struct TimeWindow {
  double begin = 0, end = 0;
  bool contains(double seconds) const { return seconds >= begin && seconds < end; }
};
// Empty text means no window; malformed or empty spans throw.
std::optional<TimeWindow> parse_time_window(std::string_view text);
struct Gate {
  double change = 0.04, still = 0.02, stable = 1.0, timeout = 120, within = 0.015, skip = 0;
  std::vector<float> match;  // empty, or 96 values per scene signature (any of them counts)
  bool last_projection = false; // projection:"last" selects a topmost menu; default "first" selects the world.
};
class InputTimeline {
 public:
  void load(const std::filesystem::path& path); // throws on malformed input; keeps holds already passed
  // Returns whether discrete events were crossed; poses are sampled every call.
  bool advance(double seconds, MockXrScript& out);
  const std::string& profile() const { return profile_; }
  const std::vector<Watch>& watches() const { return watches_; }
  // Timeline seconds minus time spent holding at gates: the route clock.
  double route_time(double seconds) const { return seconds - offset_; }
  // The gate the timeline holds at, or null; hold_time() is its event time.
  const Gate* holding() const { return holding_ ? &*events_[next_].gate : nullptr; }
  double hold_time() const { return holding_ ? events_[next_].time : 0; }
  // Releases the held gate; a timed-out gate skips the events in its "skip" span.
  void open(bool timed_out = false) {
    if (!holding_) return;
    opened_until_ = events_[next_].time;
    if (timed_out) skipped_ = {opened_until_, opened_until_ + events_[next_].gate->skip};
    holding_ = false;
  }
 private:
  struct Event { double time; MockXrScript state; std::optional<Gate> gate; };
  struct PoseKey { double time; XrPosef pose; bool interpolate; };
  std::vector<Event> events_;
  std::array<std::vector<PoseKey>, 7> poses_;
  std::size_t next_ = 0;
  double offset_ = 0, opened_until_ = -1;
  std::pair<double, double> skipped_{0, 0};  // (gate time, gate time + skip] after a timeout
  bool holding_ = false;
  std::vector<Watch> watches_;
  std::string profile_ = "/interaction_profiles/oculus/touch_controller";
};
// Runs a timeline's watches. Call restore() before InputTimeline::advance and update() after it.
class WatchRunner {
 public:
  // Whether a watch waits for its screen at this route time (the runtime then samples signatures).
  bool sampling(const std::vector<Watch>& watches, double route_seconds) const;
  // Puts the route's controllers back under a running press.
  void restore(MockXrScript& script) const;
  // Judges a new signature (null when there is none; "alternate" is a second reading of the same
  // image, matched too) and applies a running press. Returns the index of the watch that fired
  // at this call, or -1.
  int update(const std::vector<Watch>& watches, double route_seconds, double seconds,
             const SceneSignature* signature, const SceneSignature* alternate, MockXrScript& script);
  bool pressing() const { return active_ >= 0; }
 private:
  int active_ = -1;
  double fired_ = 0;
  std::size_t step_ = 0;
  MockXrController left_, right_, route_left_, route_right_;
  std::vector<double> still_since_, ready_at_;
};
}
