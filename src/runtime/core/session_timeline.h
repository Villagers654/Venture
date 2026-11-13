#pragma once
#include <cstdint>
#include <string>
#include <vector>

// One log line a minute for the whole session ("[timeline] ..."), so a report sent
// after a slowdown shows when it began and what changed with it: frame rate and
// hitches, process and busiest-thread CPU, process memory and GPU memory. The debug
// report collects these lines from the whole log, which it otherwise cuts in the
// middle. VENTURE_SESSION_TIMELINE=0 turns it off.
namespace venture::session_timeline {
// A frame was submitted; period_ns is the display period (0 when unknown). Called
// by the one thread that ends frames. Starts the timeline at the first frame.
void frame_submitted(std::int64_t now_ns, std::int64_t period_ns);

// One minute's frames, as the line reports them.
struct Frames {
  std::uint64_t count = 0, long_frames = 0;
  std::int64_t worst_ns = 0;
};
struct ThreadCpu {
  std::string name;
  double percent = 0;  // of one core
};
struct Sample {
  double minutes = 0, seconds = 0;  // since the first frame; length of this window
  Frames frames;
  double cpu_percent = -1;          // whole process, of one core; < 0 unknown
  std::vector<ThreadCpu> top;       // busiest threads first
  std::uint64_t private_bytes = 0, gpu_used = 0, gpu_budget = 0;
  std::size_t threads = 0;
};
std::string format(const Sample&);
}  // namespace venture::session_timeline
