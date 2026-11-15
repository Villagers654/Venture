#pragma once
#include <optional>
#include <cstdint>
#include <string>
#include <vector>

// One log line a minute for the whole session ("[timeline] ..."), so a report sent
// after a slowdown shows when it began and what changed with it: frame rate and
// hitches, process and busiest-thread CPU, GPU time, process memory and GPU memory. The debug
// report collects these lines from the whole log, which it otherwise cuts in the
// middle. VENTURE_SESSION_TIMELINE=0 turns it off.
namespace venture::session_timeline {
// A frame was submitted; period_ns is the display period (0 when unknown). Called
// by the one thread that ends frames. Starts the timeline at the first frame.
void frame_submitted(std::int64_t now_ns, std::int64_t period_ns);
// Where a frame's time goes, to tell a slower host runtime from a slower game or Venture:
// the host's xrWaitFrame (blocked on the runtime's pacing), then at a successful submission
// the game's frame from that wait's return to its xrEndFrame (app), the host's xrEndFrame
// (end; 0 when submitted asynchronously) and how far ahead of the frame's predicted display
// time the submission started (margin; negative is late). Any thread; a few atomic adds.
void host_wait(std::int64_t ns);
void frame_phases(std::int64_t app_ns, std::int64_t end_ns, std::int64_t margin_ns);
// Venture's own share of a guest xrEndFrame before the host gets it (prep: layer
// translation, eye image copies and the waits they need); part of app.
void end_prep(std::int64_t ns);
// A guest timed wait or sleep expired `late_ns` after its deadline (host timer precision).
void timed_wait_expired(std::int64_t late_ns);
// The submitted projection layer's eye image rectangle, and the size Venture last answered
// for xrGetRecommendedLayerResolutionMETA (the size dynamic-resolution games render).
void eye_rect(std::int32_t width, std::int32_t height);
void recommended_size(std::int32_t width, std::int32_t height);

// One minute's frames, as the line reports them.
struct Frames {
  std::uint64_t count = 0, long_frames = 0;
  std::int64_t worst_ns = 0;
};
// One minute's per-frame phases (sums over `frames` submissions, `waits` host waits).
struct Phases {
  std::uint64_t waits = 0, frames = 0, late = 0;
  std::int64_t wait_ns = 0, app_ns = 0, prep_ns = 0, end_ns = 0, margin_ns = 0, min_margin_ns = 0;
};
// One minute's expired guest timed waits and sleeps: how many, and their lateness.
struct Timeouts {
  std::uint64_t count = 0;
  std::int64_t late_ns = 0, max_late_ns = 0;
};
struct ThreadCpu {
  std::string name;
  double percent = 0;  // of one core
};
struct Sample {
  double minutes = 0, seconds = 0;  // since the first frame; length of this window
  Frames frames;
  Phases phases;
  Timeouts timeouts;
  std::int32_t eye_width = 0, eye_height = 0, recommended_width = 0, recommended_height = 0;
  double cpu_percent = -1;          // whole process, of one core; < 0 unknown
  std::vector<ThreadCpu> top;       // busiest threads first
  std::uint64_t private_bytes = 0, gpu_used = 0, gpu_budget = 0;
  // Time this process kept its busiest GPU engine running in this window, as the OS
  // accounts it; < 0 unknown. With frames.count it gives GPU time per frame.
  std::int64_t gpu_busy_ns = -1;
  // The busiest engine's GPU clock (now and at most) and the GPU's temperature; 0 unknown.
  std::uint64_t gpu_mhz = 0, gpu_max_mhz = 0;
  double gpu_celsius = 0;
  std::size_t threads = 0;
};
std::string format(const Sample&);
// Growth of a cumulative counter between two reads, or nullopt when it went back. Unsigned:
// Windows' 3D engine Running Time can sit past 2^63, reading as negative while it counts up.
inline std::optional<std::uint64_t> counter_delta(std::uint64_t now, std::uint64_t before) {
  const std::uint64_t delta = now - before;
  if (delta >= (std::uint64_t{1} << 62)) return std::nullopt;
  return delta;
}
}  // namespace venture::session_timeline
