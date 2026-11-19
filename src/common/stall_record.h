// Stall finder: where a game's threads were when its frames stopped for a
// noticeable time (>= 250 ms) while it had focus. The runtime's supervisor
// detects a stall, snapshots every guest thread once (and again at 1 s), and
// when frames resume logs one "[stall]" record: a header line and a line per
// thread worth reading. The debug report parses the records back. Shared so
// the writer and the reader cannot drift apart.
#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace venture::stall {
// The supervisor's stall state machine, polled every few tens of ms with the
// count of submitted frames. Pure: no clock, no I/O. A stall that ends in a
// pause or quit (`excluded`) is dropped, since the game was not expected to draw.
class Detector {
 public:
  static constexpr std::int64_t threshold_ms = 250;    // one snapshot once frames stop this long
  static constexpr std::int64_t recheck_ms = 1000;     // a second one tells stuck from slow
  static constexpr std::int64_t spacing_ms = 10'000;   // at most one capture per this
  static constexpr int max_captures = 30;              // per run
  enum class Action { none, snapshot, recheck, record, discard };
  Action poll(std::int64_t now_ms, std::uint64_t frames, bool excluded) {
    if (excluded) {
      since_ = now_ms;
      frames_ = frames;
      const bool pending = snapshots_ != 0;
      snapshots_ = 0;
      return pending ? Action::discard : Action::none;
    }
    if (!frames) return Action::none;  // nothing drawn yet: startup has its own probe
    if (frames != frames_) {
      frames_ = frames;
      duration_ms_ = now_ms - since_;
      since_ = now_ms;
      if (!snapshots_) return Action::none;
      snapshots_ = 0;
      return Action::record;
    }
    const auto stalled = now_ms - since_;
    if (!snapshots_) {
      if (stalled < threshold_ms || captures_ >= max_captures || now_ms - last_capture_ < spacing_ms)
        return Action::none;
      snapshots_ = 1;
      ++captures_;
      last_capture_ = now_ms;
      return Action::snapshot;
    }
    if (snapshots_ == 1 && stalled >= recheck_ms) {
      snapshots_ = 2;
      return Action::recheck;
    }
    return Action::none;
  }
  // The stall's length, from the last poll that saw a frame to the one that saw
  // frames again (so within one poll interval); valid after Action::record.
  std::int64_t duration_ms() const { return duration_ms_; }

 private:
  std::uint64_t frames_ = 0;
  std::int64_t since_ = 0, duration_ms_ = 0;
  std::int64_t last_capture_ = std::numeric_limits<std::int64_t>::min() / 2;
  int snapshots_ = 0, captures_ = 0;
};

// "main" (UnityMain, GameThread), "render" (UnityGfxDeviceW, RenderThread*),
// "rhi" (RHIThread) or "" for a thread name.
std::string thread_role(std::string_view name);

// What a thread was doing, coarsely. The first four are Venture's own work.
enum class Cause {
  jit,       // translating guest code (the JIT compiling a block)
  pipeline,  // a shader or pipeline compile (vkCreate*Pipelines, glLinkProgram...)
  gc,        // the host JVM collecting garbage
  paging,    // the process reading pages back from disk
  host,      // inside another Venture implementation of a guest API
  java,      // in a JNI call into Java (framework or game Java)
  gpu,       // waiting for the GPU (fences, submits, presents)
  xr,        // inside the host OpenXR runtime
  io,        // file I/O
  wait,      // waiting on another thread or sleeping
  game,      // running the game's own code
  unknown,
};
std::string_view cause_name(Cause);
Cause cause_from_name(std::string_view);
bool venture_side(Cause);
// The cause of a thread inside host function `library!name`.
Cause host_call_cause(std::string_view library, std::string_view name);

struct Thread {
  std::int32_t tid = 0;
  std::string name, role;
  std::string where;  // "libvulkan.so!vkQueueSubmit", "JIT compile", "libunity.so+0x1234 (sym)"
  Cause cause = Cause::unknown;
  int moved = -1;     // between the two snapshots: 1 moved, 0 the same, -1 one snapshot
  std::string stack;  // the next few guest frames, "a < b < c"
};
struct Record {
  int id = 0;
  double at_s = 0;       // since the runtime started
  std::int64_t ms = 0;   // the stall's length
  int snapshots = 0;
  Cause cause = Cause::unknown;
  // Over the stall from the first snapshot to its end; negative when unknown.
  double cpu_percent = -1;      // of one core
  std::int64_t faults = -1, hard_faults = -1, read_kib = -1, gc_ms = -1;
  std::int64_t private_mib = -1;
  int threads = 0;              // guest threads in the snapshot
  std::vector<Thread> shown;    // the threads worth reading, most telling first
};
// The stall's cause from its threads and host counters.
Cause classify(const Record&);
// The record as log lines for the "stall" tag: a header and one per shown thread.
std::vector<std::string> format(const Record&);
// Records from a runtime log's text, in order; lines it cannot read are skipped.
std::vector<Record> parse_log(std::string_view text);
// The debug report's "=== Stalls ===" section, empty without records.
std::string report_section(const std::vector<Record>&);
// The report's likely cause and next step when most stalls share one cause on
// Venture's side; nothing otherwise.
struct Verdict { std::string cause, advice; };
std::optional<Verdict> venture_cause(const std::vector<Record>&);
}  // namespace venture::stall
