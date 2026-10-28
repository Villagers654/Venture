#pragma once
#include "vulkan.h"

namespace venture::vulkan {
// Opt-in diagnostic (VENTURE_VULKAN_GPU_TIMING=<jsonl path>). Timestamps bracket
// submitted GPU work, not CPU time between XR frames. Disabled by default.
void timing_queue(VkDevice, VkQueue, std::uint32_t family);
void release_timing_device(VkDevice);
void timing_end_frame(std::int64_t display_time, std::uint32_t swap_interval = 1, float display_rate = 0);
bool timing_enabled();
// Publish collected telemetry only; never wait for pending GPU work at exit.
void finish_timing();
std::uint64_t timing_frame();
// One XR eye-image hand-off done by Venture (GLES bridge or VrApi copy):
// Elapsed ns on the submitting thread (of which `wait_ns` blocked on the GPU) and
// GPU timestamps in ns; zero means unavailable. gl_begin/gl_after bracket the
// GLES stream position before the copy and after its release.
struct XrCopySample {
  const char* path;
  std::uint64_t frame;
  std::int64_t cpu_ns, release_cpu_ns, wait_ns;
  std::uint64_t gl_begin_ns, gl_after_ns, vk_begin_ns, vk_end_ns;
  // OS thread CPU time excludes blocked/runnable time in cpu_ns (wall time).
  // -1 means unavailable; aggregate across frames because OS accounting is coarse.
  std::int64_t thread_cpu_ns = -1;
  // Native OpenXR host images need no bridge transfer. Distinguish zero work
  // from a transfer whose GPU timestamps are unavailable.
  bool gpu_transfer = true;
  bool gpu_ownership = false;
};
void timing_xr_copy(const XrCopySample&);
void timing_gles_render(std::uint64_t frame, std::uint64_t begin_ns,
                        std::uint64_t end_ns, bool valid);
// A CPU block on GPU progress in Venture's Vulkan translation (sync-file
// export or import), `ns` long on the calling thread.
// Records caller identity and host-monotonic bounds for joining CPU profiles.
// cpu_ns, when given, is the waiting thread's own CPU time over the same span.
void timing_host_wait(const char* what, std::int64_t ns, std::int64_t cpu_ns = -1);
// Records the enclosing scope on the calling thread (wall and own CPU) as a host
// wait named `what`; inert unless timing is on.
class ScopedHostWait {
public:
  explicit ScopedHostWait(const char* what);
  ~ScopedHostWait();
  ScopedHostWait(const ScopedHostWait&) = delete;
  ScopedHostWait& operator=(const ScopedHostWait&) = delete;
private:
  const char* what;
  std::int64_t wall = 0, cpu = 0;
};
VkResult timed_submit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence, PFN_vkQueueSubmit);
VkResult timed_submit2(VkQueue, std::uint32_t, const VkSubmitInfo2*, VkFence, PFN_vkQueueSubmit2);
}
