#pragma once
// Narrow, opt-in synchronization evidence. Existing result summaries merge all
// handles and callers; they cannot identify an unsatisfied queue dependency.
// Record API arguments only, never mapped memory or command-buffer contents.
#include "common/environment.h"
#include "common/log.h"
#include "core/shutdown_trace.h"
#include "core/thread.h"
#include "core/diagnostic_trace.h"
#include <chrono>
#include "hle/vulkan/vulkan.h"
#include <atomic>
#include <algorithm>
#include <format>
#include <tuple>
#include <type_traits>

namespace venture::vulkan::sync_trace {
// Opt-in compact sink uses the existing bounded per-thread diagnostic queues.
// Producers never compress or write files. The ordinary trace remains unchanged.
template<class... A> void write(std::format_string<A...> format, A&&... args) {
  static const bool compact = environment::getenv("VENTURE_DIAGNOSTIC_TRACE_ZLIB") != nullptr;
  if (!compact) { log::info("vk-sync", format, std::forward<A>(args)...); return; }
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  diagnostic_trace("vk-sync", std::format("t_ns={} {}", ns,
      std::format(format, std::forward<A>(args)...)));
}
template<class H> std::uint64_t handle(H h) {
  if constexpr (std::is_pointer_v<H>) return reinterpret_cast<std::uintptr_t>(h);
  else return static_cast<std::uint64_t>(h);
}
template<class F> inline constexpr bool supported =
    std::is_same_v<F, PFN_vkQueueSubmit> || std::is_same_v<F, PFN_vkQueueSubmit2> ||
    std::is_same_v<F, PFN_vkGetFenceStatus> || std::is_same_v<F, PFN_vkWaitForFences> ||
    std::is_same_v<F, PFN_vkResetFences> || std::is_same_v<F, PFN_vkAcquireNextImageKHR> ||
    std::is_same_v<F, PFN_vkAcquireNextImage2KHR> || std::is_same_v<F, PFN_vkQueuePresentKHR> ||
    std::is_same_v<F, PFN_vkSignalSemaphore> || std::is_same_v<F, PFN_vkCreateFence> ||
    std::is_same_v<F, PFN_vkCreateSemaphore>;

inline std::string list(std::uint32_t count, const auto* values) {
  std::string text;
  const auto n = std::min(count, 64u);
  if (values) for (std::uint32_t i = 0; i < n; ++i)
    text += std::format("{}0x{:x}", i ? "," : "", handle(values[i]));
  if (count > n) text += ",TRUNCATED";
  return text;
}
template<class F, class... A> std::string describe(A... args) {
  auto a = std::tuple(args...);
  std::string s = std::format("owner=0x{:x}", handle(std::get<0>(a)));
  if constexpr (std::is_same_v<F, PFN_vkQueueSubmit>) {
    auto count = std::get<1>(a); auto submits = std::get<2>(a);
    s += std::format(" fence=0x{:x} batches={}", handle(std::get<3>(a)), count);
    for (std::uint32_t i = 0; submits && i < std::min(count, 64u); ++i) {
      const auto& b = submits[i];
      s += std::format(" batch={} waits=[{}] signals=[{}]", i,
          list(b.waitSemaphoreCount, b.pWaitSemaphores), list(b.signalSemaphoreCount, b.pSignalSemaphores));
      auto* p = static_cast<const VkBaseInStructure*>(b.pNext);
      unsigned nodes = 0;
      for (; p && nodes++ < 64; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
          const auto& t = *reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(p);
          s += std::format(" wait_values=[{}] signal_values=[{}]",
              list(t.waitSemaphoreValueCount, t.pWaitSemaphoreValues), list(t.signalSemaphoreValueCount, t.pSignalSemaphoreValues));
        }
      if (p) s += " CHAIN_TRUNCATED";
    }
    if (count > 64) s += " TRUNCATED";
  } else if constexpr (std::is_same_v<F, PFN_vkQueueSubmit2>) {
    auto count = std::get<1>(a); auto submits = std::get<2>(a);
    s += std::format(" fence=0x{:x} batches={}", handle(std::get<3>(a)), count);
    for (std::uint32_t i = 0; submits && i < std::min(count, 64u); ++i) {
      const auto& b = submits[i]; s += std::format(" batch={}", i);
      for (std::uint32_t j = 0; j < std::min(b.waitSemaphoreInfoCount, 64u); ++j)
        s += std::format(" wait=0x{:x}:{}", handle(b.pWaitSemaphoreInfos[j].semaphore), b.pWaitSemaphoreInfos[j].value);
      for (std::uint32_t j = 0; j < std::min(b.signalSemaphoreInfoCount, 64u); ++j)
        s += std::format(" signal=0x{:x}:{}", handle(b.pSignalSemaphoreInfos[j].semaphore), b.pSignalSemaphoreInfos[j].value);
      if (b.waitSemaphoreInfoCount > 64 || b.signalSemaphoreInfoCount > 64) s += " TRUNCATED";
    }
    if (count > 64) s += " TRUNCATED";
  } else if constexpr (std::is_same_v<F, PFN_vkGetFenceStatus>) {
    s += std::format(" fence=0x{:x}", handle(std::get<1>(a)));
  } else if constexpr (std::is_same_v<F, PFN_vkWaitForFences> || std::is_same_v<F, PFN_vkResetFences>) {
    s += std::format(" fences=[{}]", list(std::get<1>(a), std::get<2>(a)));
    if constexpr (std::is_same_v<F, PFN_vkWaitForFences>)
      s += std::format(" all={} timeout={}", std::get<3>(a), std::get<4>(a));
  } else if constexpr (std::is_same_v<F, PFN_vkAcquireNextImageKHR>) {
    s += std::format(" swapchain=0x{:x} timeout={} semaphore=0x{:x} fence=0x{:x}",
        handle(std::get<1>(a)), std::get<2>(a), handle(std::get<3>(a)), handle(std::get<4>(a)));
  } else if constexpr (std::is_same_v<F, PFN_vkAcquireNextImage2KHR>) {
    if (auto* i = std::get<1>(a)) s += std::format(" swapchain=0x{:x} timeout={} semaphore=0x{:x} fence=0x{:x}",
        handle(i->swapchain), i->timeout, handle(i->semaphore), handle(i->fence));
  } else if constexpr (std::is_same_v<F, PFN_vkQueuePresentKHR>) {
    if (auto* i = std::get<1>(a)) s += std::format(" waits=[{}] swapchains=[{}]",
        list(i->waitSemaphoreCount, i->pWaitSemaphores), list(i->swapchainCount, i->pSwapchains));
  } else if constexpr (std::is_same_v<F, PFN_vkSignalSemaphore>) {
    if (auto* i = std::get<1>(a)) s += std::format(" semaphore=0x{:x} value={}", handle(i->semaphore), i->value);
  } else if constexpr (std::is_same_v<F, PFN_vkCreateFence> || std::is_same_v<F, PFN_vkCreateSemaphore>) {
    if (auto* i = std::get<1>(a)) s += std::format(" create_flags={}", i->flags);
  }
  return s;
}
template<class F, class... A> std::uint64_t begin(const char* name, A... args) noexcept {
  if constexpr (supported<F>) {
    static const bool enabled = environment::getenv("VENTURE_VULKAN_SYNC_TRACE") != nullptr;
    if (!enabled) return 0;
    try {
      // Process-wide budget, including all supported commands and aliases.
      extern std::atomic<std::uint64_t> serial;
      auto id = serial.fetch_add(1, std::memory_order_relaxed) + 1;
      if (id > 250000) { if (id == 250001) log::warn("vk-sync", "TRUNCATED call budget exhausted"); return 0; }
      auto* t = GuestThread::current();
      const auto lr = t && shutdown_trace::enabled ? shutdown_trace::read(*t).lr : 0;
      write("id={} begin={} guest_tid={} lr={:#x} {}", id, name,
          t ? t->tid() : 0, lr, describe<F>(args...));
      return id;
    } catch (...) {} // Diagnostic failure must not change driver execution.
  }
  return 0;
}
inline std::atomic<std::uint64_t> serial{0};
template<class F, class... A> void end(std::uint64_t id, VkResult result, A... args) noexcept {
  if (!id) return;
  try {
    auto a = std::tuple(args...);
    if constexpr (std::is_same_v<F, PFN_vkCreateFence> || std::is_same_v<F, PFN_vkCreateSemaphore>) {
      if (result == VK_SUCCESS && std::get<3>(a)) {
        write("id={} result={} created={:#x}", id, int(result), handle(*std::get<3>(a)));
        return;
      }
    } else if constexpr (std::is_same_v<F, PFN_vkAcquireNextImageKHR>) {
      if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && std::get<5>(a)) {
        write("id={} result={} index={}", id, int(result), *std::get<5>(a));
        return;
      }
    } else if constexpr (std::is_same_v<F, PFN_vkAcquireNextImage2KHR>) {
      if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && std::get<2>(a)) {
        write("id={} result={} index={}", id, int(result), *std::get<2>(a));
        return;
      }
    }
    write("id={} result={}", id, int(result));
  } catch (...) {}
}
} // namespace venture::vulkan::sync_trace
