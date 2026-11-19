// SPDX-License-Identifier: GPL-3.0-or-later
// The runtime facilities the shared VrApi core and xr::Session use, as they
// are inside a game on an Android headset (libvrapi.so): every thread is the
// game's own (no guest threads, so frame waits take the host-thread paths),
// futexes are the kernel's, the GPU queues are the game's, and the PC
// runtime's diagnostics (profiles, timelines, dev dumps, GPU timing,
// telemetry) and its overlays (system keyboard) are absent.
#include <linux/futex.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <map>
#include <memory>
#include <mutex>

#include "common/log.h"
#include "core/av_telemetry.h"
#include "core/profile.h"
#include "core/session_timeline.h"
#include "core/thread.h"
#include "core/wait.h"
#include "gfx/gpu_device.h"
#include "gfx/queue_sync.h"
#include "hle/vrapi/drawn_layers.h"
#include "hle/vulkan/timing.h"
#include "hle/vulkan/vulkan.h"
#include "platform/profile.h"
#include "platform/thread_priority.h"
#include "xr/dev_dump.h"
#include "xr/system_keyboard.h"

namespace venture {
std::int64_t monotonic_ns() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return std::int64_t(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

namespace {
int futex(GuestAddr address, int op, std::uint32_t value, const timespec* timeout, std::uint32_t bitset) {
  return int(syscall(SYS_futex, g2h<std::uint32_t>(address), op | FUTEX_PRIVATE_FLAG, value, timeout, nullptr, bitset));
}
}  // namespace

// The game's threads are never guest threads here.
GuestThread* GuestThread::current() { return nullptr; }

int futex_wait(GuestThread&, GuestAddr address, std::uint32_t expected, std::int64_t deadline_ns, std::uint32_t bitset,
               bool) {
  timespec deadline{static_cast<time_t>(deadline_ns / 1'000'000'000), static_cast<long>(deadline_ns % 1'000'000'000)};
  const int r = futex(address, FUTEX_WAIT_BITSET, expected, deadline_ns < 0 ? nullptr : &deadline, bitset);
  return r < 0 ? -errno : 0;
}
int futex_wake(GuestAddr address, int count, std::uint32_t bitset, sync_census::WakeSource) {
  const int r = futex(address, FUTEX_WAKE_BITSET, std::uint32_t(count), nullptr, bitset);
  return r < 0 ? 0 : r;
}

namespace av {
bool enabled() { return false; }
void emit(nlohmann::json) noexcept {}
}  // namespace av

namespace profile {
extern const bool enabled = false;
void mark(std::string_view) {}
void frame(std::uint64_t) {}
void report_pipelines(std::string_view) {}
}  // namespace profile

namespace session_timeline {
void frame_submitted(std::int64_t, std::int64_t) {}
void host_wait(std::int64_t) {}
void frame_phases(std::int64_t, std::int64_t, std::int64_t) {}
void eye_rect(std::int32_t, std::int32_t) {}
}  // namespace session_timeline

namespace vulkan {
ScopedHostWait::ScopedHostWait(const char*) {}
ScopedHostWait::~ScopedHostWait() = default;
void timing_end_frame(std::int64_t, std::uint32_t, float) {}
// The game made its own Vulkan device; Venture has none of its own here.
VkPhysicalDevice shared_physical_device_in(VkInstance) { return nullptr; }
}  // namespace vulkan

namespace gfx {
// Venture renders nothing of its own in the game's process.
GpuDevice& gpu_device() {
  static GpuDevice none;
  return none;
}
// One lock per queue the session submits on, as the PC runtime keeps them.
std::shared_ptr<std::recursive_mutex> queue_mutex(VkQueue queue) {
  static std::mutex mutex;
  static std::map<VkQueue, std::shared_ptr<std::recursive_mutex>> queues;
  std::lock_guard lock(mutex);
  auto& entry = queues[queue];
  if (!entry) entry = std::make_shared<std::recursive_mutex>();
  return entry;
}
void register_queue(VkDevice, VkQueue queue) { queue_mutex(queue); }
}  // namespace gfx

namespace platform {
int set_thread_priority(std::int32_t tid, int nice) {
  return setpriority(PRIO_PROCESS, id_t(tid), nice) == 0 ? 0 : -errno;
}
std::uint32_t profile_thread_id() noexcept { return std::uint32_t(gettid()); }
}  // namespace platform

namespace xr {
void dev_dump_bind(const XrGraphicsBindingVulkanKHR&) {}
XrResult dev_create_swapchain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
  return ::xrCreateSwapchain(session, info, out);
}
XrResult dev_destroy_swapchain(XrSwapchain swapchain) { return ::xrDestroySwapchain(swapchain); }
std::size_t dev_forget_parked_swapchains() { return 0; }
XrResult dev_release_swapchain(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info, VkImage) {
  return ::xrReleaseSwapchainImage(swapchain, info);
}
void dev_dump_frame(const XrFrameEndInfo*, XrResult) {}
void dev_dump_end() {}

// Venture's system keyboard is a PC overlay; a headset has its own.
namespace keyboard {
std::shared_ptr<const Frame> extend(XrSession, const XrFrameEndInfo&, std::shared_ptr<const void>) { return {}; }
void reset_session(XrSession) {}
bool point(int, XrSpace, float, XrTime) { return false; }
void tick(std::string_view, XrSession, XrAction, XrPath) {}
}  // namespace keyboard
}  // namespace xr
}  // namespace venture

namespace venture::hle::vrapi {
// Layers drawn by Venture need its own images; the swapchains here hand the
// runtime the game's images directly (NativeImages::drawn() is null).
void DrawnLayers::add(const DrawnLayer &, std::vector<const XrCompositionLayerBaseHeader *> &) {}
}  // namespace venture::hle::vrapi
