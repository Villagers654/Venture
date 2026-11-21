// Stall finder (common/stall_record.h): on a frame stall of 250 ms or more
// while the game has focus, snapshots where every guest thread is and, when
// frames resume, logs one "[stall]" record saying which threads were where and
// whether the time went to Venture (JIT, pipeline compiles, Java GC, page-ins).
// From 250 ms it also gathers the freeze evidence core/stall_watch.h keeps
// (calls in progress, fences polled in vain and their submissions, the last
// Vulkan submits and waits, the last XR calls), the GPU's position from its
// breadcrumbs and held queue locks; from 1 s every thread's host stack; at 4 s
// it logs the record without waiting for the end, so a freeze that never ends
// is still reported. Calls in progress past 500 ms while frames flow (a worker's
// fence wait, a JIT compile) get a "[stall-op]" line each.
#pragma once
#include "common/stall_record.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace venture::stall_finder {
// Where the GPU's guest work stands, from the Vulkan layer; read only with
// try-locks, never waiting on a lock a stalled thread may hold.
struct GpuEvidence {
  std::string summary;  // "GPU idle: ...", "GPU busy: ..."; empty when unknown
  std::vector<std::string> lines;
  std::vector<std::pair<std::uint64_t, std::string>> held_queue_locks;  // queue, owner ("unknown")
};
struct Sources {
  std::int64_t (*gc_ns)() = nullptr;      // the host JVM's cumulative GC time (-1 unknown)
  GpuEvidence (*gpu)() = nullptr;
  bool (*xr_session_lock_held)() = nullptr;
  bool host_stacks = true;               // every thread's host stack at the 1 s recheck
};
class Finder {
 public:
  explicit Finder(Sources sources = {});
  ~Finder();
  // The supervisor's poll. Without a stall it runs the detector and scans the
  // watched calls for one past 500 ms: no locks, no allocation. Snapshots
  // try-lock the thread registry and never suspend or wait for a guest thread;
  // naming waits on the linker once, briefly. Host stacks park each thread for
  // a few microseconds while its stack words are copied.
  void poll(std::int64_t now_ms, std::uint64_t frames, bool excluded);

 private:
  struct Impl;
  stall::Detector detector_;
  std::unique_ptr<Impl> impl_;  // made at the first stall
  Sources sources_;
  std::int64_t next_op_check_ms_ = 0;
};
}  // namespace venture::stall_finder
