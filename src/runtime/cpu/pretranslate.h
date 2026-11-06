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

namespace detail {
// The function entries in the unwind information of PT_GNU_EH_FRAME [hdr, hdr + size)
// that lie in `code`, `segments` being the image's PT_LOADs ([begin, end) each).
std::vector<std::uint64_t> function_entries(std::uint64_t hdr, std::uint64_t size, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& code,
                                            const std::vector<std::pair<std::uint64_t, std::uint64_t>>& segments);
} // namespace detail
} // namespace venture::pretranslate
