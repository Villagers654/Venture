// Stall finder (common/stall_record.h): on a frame stall of 250 ms or more
// while the game has focus, snapshots where every guest thread is and, when
// frames resume, logs one "[stall]" record saying which threads were where and
// whether the time went to Venture (JIT, pipeline compiles, Java GC, page-ins).
#pragma once
#include "common/stall_record.h"
#include <cstdint>
#include <memory>

namespace venture::stall_finder {
class Finder {
 public:
  // gc_ns: the host JVM's cumulative GC time (-1 unknown), or null.
  explicit Finder(std::int64_t (*gc_ns)() = nullptr);
  ~Finder();
  // The supervisor's poll. Without a stall it only runs the detector: no
  // locks, no allocation. Snapshots try-lock the thread registry and never
  // suspend or wait for a guest thread; naming waits on the linker once, briefly.
  void poll(std::int64_t now_ms, std::uint64_t frames, bool excluded);

 private:
  struct Impl;
  stall::Detector detector_;
  std::unique_ptr<Impl> impl_;  // made at the first stall
  std::int64_t (*gc_ns_)();
};
}  // namespace venture::stall_finder
