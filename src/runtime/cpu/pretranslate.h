#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace venture::pretranslate {
// Background translation into the portable block cache, ahead of execution.
// Low-priority workers, each with a JIT that never runs guest code, translate
// the direct successors (links and return addresses) of every block a guest
// thread translated or installed, nearest first, then the function entries of
// loaded images (.eh_frame_hdr), those near executed code first. A guest thread
// reaching that code later installs it instead of translating it on its own
// frame. Whenever nothing is queued, the cache file records which blocks have
// had their successors explored, and later runs skip those instead of reading
// every cached block again. A64 only; on with the block cache, VENTURE_JIT_AOT=0 disables it.

// A guest JIT translated or installed this block (any thread; cheap).
void reached(std::uint64_t descriptor) noexcept;
// The calling thread's batch of reached blocks goes to the workers now (it is about to wait).
void flush_reached() noexcept;
// Process exit: workers finish their current block and stop.
void stop() noexcept;

// Before a game's first launch (venture-runtime --prepare), with its libraries loaded and no
// guest code running: translates the function entries of every loaded image and their
// successors until nothing is left, `block_limit` blocks are explored (0: no limit) or `stop`
// returns true, then writes the cache. A pass that ran out of work records every block
// explored, as an idle game run does; a limited or stopped one leaves the rest to the game's
// own background translation (complete is true for a limited pass). False if background
// translation is off (no block cache, A32, VENTURE_JIT_AOT=0).
struct PrepareResult {
  std::uint64_t translated = 0, failed = 0, explored = 0;
  bool complete = false, limited = false;
};
bool prepare(bool (*stop)(void*), void* context, std::uint64_t block_limit, PrepareResult& result) noexcept;

namespace detail {
// The function entries in the unwind information of PT_GNU_EH_FRAME [hdr, hdr + size)
// that lie in `code`, `segments` being the image's PT_LOADs ([begin, end) each).
std::vector<std::uint64_t> function_entries(std::uint64_t hdr, std::uint64_t size, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& code,
                                            const std::vector<std::pair<std::uint64_t, std::uint64_t>>& segments);
} // namespace detail
} // namespace venture::pretranslate
