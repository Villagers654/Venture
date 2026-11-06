#pragma once
// Deferred recording of guest command buffers behind pipelines still being
// created in the background. Used by vulkan.cpp's guest call boundary.
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "common/environment.h"
#include "common/log.h"
#include "core/pipeline_stats.h"
#include "core/wait.h"
#include "gfx/pipeline_trace.h"
#include "internal.h"

namespace venture::vulkan {
// ---------------------------------------------------------------------------
// Deferred recording behind pipelines that are still being created.
//
// Guest graphics pipelines are created by background workers (see
// gfx/pipeline_trace.cpp), but engines that create a pipeline at its first
// draw (UE4 without a precompiled PSO cache, for one) bind it right after
// creating it, so the render thread waited for each creation in turn and the
// workers never ran in parallel. When a command buffer binds a pipeline that
// is not ready, its later commands are therefore recorded here instead of
// reaching the layers below, until vkEndCommandBuffer: then every pipeline
// it binds is awaited together (the recording thread creates the queued ones
// itself) and the commands are replayed, in order and through the same layer
// chain they would have taken, before the real end. A command this cannot
// copy faithfully replays what was recorded first and then runs normally.
// Commands that only record (all vkCmd*) are invisible until submission, so
// recording them later at the end of the buffer is equivalent.
// VENTURE_VK_DEFERRED_RECORDING=0 disables it.
namespace deferred {
using Key = std::uintptr_t;  // a command buffer handle
template <class T>
concept Complete = requires { sizeof(T); };
// Values and handles (pointers to incomplete types) are copied by value.
template <class T>
constexpr bool plain_arg = !std::is_pointer_v<T> ||
    (!std::is_const_v<std::remove_pointer_t<T>> && !Complete<std::remove_pointer_t<T>>);
template <class F>
struct FirstArg {};
template <class R, class First, class... A>
struct FirstArg<R(VKAPI_PTR*)(First, A...)> { using type = First; };
template <class F>
constexpr bool command_buffer_call = [] {
  if constexpr (requires { typename FirstArg<F>::type; }) return std::is_same_v<typename FirstArg<F>::type, VkCommandBuffer>;
  else return false;
}();

template <class T>
struct Array {
  bool null = true;
  std::vector<T> items;
  Array() = default;
  Array(const T* p, std::size_t n) : null(!p) { if (p && n) items.assign(p, p + n); }
  const T* get() const { return null ? nullptr : items.empty() ? &dummy : items.data(); }
  inline static const T dummy{};
};
inline Array<std::byte> bytes(const void* p, std::size_t n) { return {static_cast<const std::byte*>(p), n}; }
template <class T>
bool bare(const T* p, std::size_t n = 1) {
  for (std::size_t i = 0; p && i < n; ++i)
    if (p[i].pNext) return false;
  return true;
}

struct Recording {
  std::vector<std::function<void()>> commands;
  std::vector<VkPipeline> pipelines;  // bound while not ready
  VkCommandPool pool{};
  std::int64_t started_ns = 0;
};
inline std::atomic<std::uint32_t> active{0};
inline std::mutex mutex;
inline std::unordered_map<Key, std::unique_ptr<Recording>> recordings;
inline bool enabled() {
  static const bool on = [] {
    const auto* e = environment::getenv("VENTURE_VK_DEFERRED_RECORDING");
    return !e || std::string_view(e) != "0";
  }();
  return on;
}
inline Recording* find(Key command_buffer) {
  if (!active.load(std::memory_order_acquire)) return nullptr;
  std::lock_guard lock(mutex);
  const auto it = recordings.find(command_buffer);
  return it == recordings.end() ? nullptr : it->second.get();
}
inline std::unique_ptr<Recording> take(Key command_buffer) {
  if (!active.load(std::memory_order_acquire)) return nullptr;
  std::lock_guard lock(mutex);
  const auto it = recordings.find(command_buffer);
  if (it == recordings.end()) return nullptr;
  auto r = std::move(it->second);
  recordings.erase(it);
  active.fetch_sub(1, std::memory_order_release);
  return r;
}
inline Recording* start(Key command_buffer, VkCommandPool pool) {
  auto r = std::make_unique<Recording>();
  r->started_ns = monotonic_ns();
  r->pool = pool;
  std::lock_guard lock(mutex);
  auto& slot = recordings[command_buffer];
  if (!slot) active.fetch_add(1, std::memory_order_release);
  slot = std::move(r);
  return slot.get();
}
// The buffer was reset, freed or begun again: what it recorded is gone.
inline void drop(Key command_buffer) { take(command_buffer); }
inline void drop_pool(VkCommandPool pool) {
  if (!active.load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex);
  for (auto it = recordings.begin(); it != recordings.end();)
    if (it->second->pool == pool) {
      it = recordings.erase(it);
      active.fetch_sub(1, std::memory_order_release);
    } else ++it;
}
inline void flush(Key command_buffer, std::string_view by) {
  auto r = take(command_buffer);
  if (!r) return;
  const auto start = monotonic_ns();
  gfx::await_pipelines(r->pipelines);
  for (auto& command : r->commands) {
    try {
      command();
    } catch (const std::exception& e) {
      log::error("vulkan", "deferred command replay: {}", e.what());
    }
  }
  if (profile::compile_trace_enabled())
    profile::trace_compile("vk_deferred_flush", start, static_cast<std::uint64_t>(monotonic_ns() - start),
                           static_cast<std::uint32_t>(r->commands.size()),
                           static_cast<std::uint32_t>(r->pipelines.size()),
                           std::format("\"by\":\"{}\",\"recorded_ms\":{:.1f}", by, (start - r->started_ns) / 1e6));
}

template <ProcedureName Name>
constexpr bool is(std::string_view name) { return std::string_view(Name.value) == name; }

// A replayable copy of one command, or an empty function when it cannot be
// copied faithfully (unknown pointer arguments or extension structures).
template <ProcedureName Name, class F, class C, class... A>
std::function<void()> capture(F fp, C cb, A... a) {
  if constexpr ((plain_arg<A> && ...)) {
    return [=] { fp(cb, a...); };
  } else if constexpr (is<Name>("vkCmdBindDescriptorSets")) {
    auto [point, layout, first, count, sets, dynamic_count, dynamic] = std::tuple(a...);
    return [=, s = Array(sets, count), d = Array(dynamic, dynamic_count)] {
      fp(cb, point, layout, first, count, s.get(), dynamic_count, d.get());
    };
  } else if constexpr (is<Name>("vkCmdBindVertexBuffers")) {
    auto [first, count, buffers, offsets] = std::tuple(a...);
    return [=, b = Array(buffers, count), o = Array(offsets, count)] { fp(cb, first, count, b.get(), o.get()); };
  } else if constexpr (is<Name>("vkCmdBindVertexBuffers2") || is<Name>("vkCmdBindVertexBuffers2EXT")) {
    auto [first, count, buffers, offsets, sizes, strides] = std::tuple(a...);
    return [=, b = Array(buffers, count), o = Array(offsets, count), z = Array(sizes, count), s = Array(strides, count)] {
      fp(cb, first, count, b.get(), o.get(), z.get(), s.get());
    };
  } else if constexpr (is<Name>("vkCmdPushConstants")) {
    auto [layout, stages, offset, size, values] = std::tuple(a...);
    return [=, v = bytes(values, size)] { fp(cb, layout, stages, offset, size, v.get()); };
  } else if constexpr (is<Name>("vkCmdSetViewport") || is<Name>("vkCmdSetScissor")) {
    auto [first, count, items] = std::tuple(a...);
    return [=, v = Array(items, count)] { fp(cb, first, count, v.get()); };
  } else if constexpr (is<Name>("vkCmdSetViewportWithCount") || is<Name>("vkCmdSetScissorWithCount") ||
                       is<Name>("vkCmdSetViewportWithCountEXT") || is<Name>("vkCmdSetScissorWithCountEXT")) {
    auto [count, items] = std::tuple(a...);
    return [=, v = Array(items, count)] { fp(cb, count, v.get()); };
  } else if constexpr (is<Name>("vkCmdSetBlendConstants")) {
    auto [constants] = std::tuple(a...);
    return [=, v = Array(constants, 4)] { fp(cb, v.get()); };
  } else if constexpr (is<Name>("vkCmdBeginRenderPass")) {
    auto [info, contents] = std::tuple(a...);
    if (!info || info->pNext) return {};
    return [=, i = *info, c = Array(info->pClearValues, info->clearValueCount)]() mutable {
      i.pClearValues = c.get();
      fp(cb, &i, contents);
    };
  } else if constexpr (is<Name>("vkCmdBeginRenderPass2") || is<Name>("vkCmdBeginRenderPass2KHR")) {
    auto [info, subpass] = std::tuple(a...);
    if (!info || info->pNext || !subpass || subpass->pNext) return {};
    return [=, i = *info, s = *subpass, c = Array(info->pClearValues, info->clearValueCount)]() mutable {
      i.pClearValues = c.get();
      fp(cb, &i, &s);
    };
  } else if constexpr (is<Name>("vkCmdNextSubpass2") || is<Name>("vkCmdNextSubpass2KHR")) {
    auto [begin, end] = std::tuple(a...);
    if (!begin || begin->pNext || !end || end->pNext) return {};
    return [=, b = *begin, e = *end] { fp(cb, &b, &e); };
  } else if constexpr (is<Name>("vkCmdEndRenderPass2") || is<Name>("vkCmdEndRenderPass2KHR")) {
    auto [end] = std::tuple(a...);
    if (!end || end->pNext) return {};
    return [=, e = *end] { fp(cb, &e); };
  } else if constexpr (is<Name>("vkCmdBeginRendering") || is<Name>("vkCmdBeginRenderingKHR")) {
    auto [info] = std::tuple(a...);
    if (!info || info->pNext || !bare(info->pColorAttachments, info->colorAttachmentCount) ||
        !bare(info->pDepthAttachment) || !bare(info->pStencilAttachment))
      return {};
    struct Copy {
      VkRenderingInfo info;
      Array<VkRenderingAttachmentInfo> color;
      std::optional<VkRenderingAttachmentInfo> depth, stencil;
    };
    auto copy = std::make_shared<Copy>(Copy{*info, Array(info->pColorAttachments, info->colorAttachmentCount),
        info->pDepthAttachment ? std::optional(*info->pDepthAttachment) : std::nullopt,
        info->pStencilAttachment ? std::optional(*info->pStencilAttachment) : std::nullopt});
    copy->info.pColorAttachments = copy->color.get();
    copy->info.pDepthAttachment = copy->depth ? &*copy->depth : nullptr;
    copy->info.pStencilAttachment = copy->stencil ? &*copy->stencil : nullptr;
    return [=] { fp(cb, &copy->info); };
  } else if constexpr (is<Name>("vkCmdPipelineBarrier")) {
    auto [src, dst, dependency, memory_count, memory, buffer_count, buffer, image_count, image] = std::tuple(a...);
    if (!bare(memory, memory_count) || !bare(buffer, buffer_count) || !bare(image, image_count)) return {};
    return [=, m = Array(memory, memory_count), b = Array(buffer, buffer_count), i = Array(image, image_count)] {
      fp(cb, src, dst, dependency, memory_count, m.get(), buffer_count, b.get(), image_count, i.get());
    };
  } else if constexpr (is<Name>("vkCmdPipelineBarrier2") || is<Name>("vkCmdPipelineBarrier2KHR")) {
    auto [info] = std::tuple(a...);
    if (!info || info->pNext || !bare(info->pMemoryBarriers, info->memoryBarrierCount) ||
        !bare(info->pBufferMemoryBarriers, info->bufferMemoryBarrierCount) ||
        !bare(info->pImageMemoryBarriers, info->imageMemoryBarrierCount))
      return {};
    return [=, i = *info, m = Array(info->pMemoryBarriers, info->memoryBarrierCount),
            b = Array(info->pBufferMemoryBarriers, info->bufferMemoryBarrierCount),
            g = Array(info->pImageMemoryBarriers, info->imageMemoryBarrierCount)]() mutable {
      i.pMemoryBarriers = m.get();
      i.pBufferMemoryBarriers = b.get();
      i.pImageMemoryBarriers = g.get();
      fp(cb, &i);
    };
  } else if constexpr (is<Name>("vkCmdCopyBuffer")) {
    auto [src, dst, count, regions] = std::tuple(a...);
    return [=, r = Array(regions, count)] { fp(cb, src, dst, count, r.get()); };
  } else if constexpr (is<Name>("vkCmdCopyImage") || is<Name>("vkCmdResolveImage")) {
    auto [src, src_layout, dst, dst_layout, count, regions] = std::tuple(a...);
    return [=, r = Array(regions, count)] { fp(cb, src, src_layout, dst, dst_layout, count, r.get()); };
  } else if constexpr (is<Name>("vkCmdBlitImage")) {
    auto [src, src_layout, dst, dst_layout, count, regions, filter] = std::tuple(a...);
    return [=, r = Array(regions, count)] { fp(cb, src, src_layout, dst, dst_layout, count, r.get(), filter); };
  } else if constexpr (is<Name>("vkCmdCopyBufferToImage") || is<Name>("vkCmdCopyImageToBuffer")) {
    auto [src, dst, layout, count, regions] = std::tuple(a...);
    return [=, r = Array(regions, count)] { fp(cb, src, dst, layout, count, r.get()); };
  } else if constexpr (is<Name>("vkCmdClearColorImage") || is<Name>("vkCmdClearDepthStencilImage")) {
    auto [image, layout, value, count, ranges] = std::tuple(a...);
    if (!value) return {};
    return [=, v = *value, r = Array(ranges, count)] { fp(cb, image, layout, &v, count, r.get()); };
  } else if constexpr (is<Name>("vkCmdClearAttachments")) {
    auto [count, attachments, rect_count, rects] = std::tuple(a...);
    return [=, at = Array(attachments, count), r = Array(rects, rect_count)] {
      fp(cb, count, at.get(), rect_count, r.get());
    };
  } else if constexpr (is<Name>("vkCmdUpdateBuffer")) {
    auto [buffer, offset, size, data] = std::tuple(a...);
    return [=, d = bytes(data, static_cast<std::size_t>(size))] { fp(cb, buffer, offset, size, d.get()); };
  } else if constexpr (is<Name>("vkCmdExecuteCommands")) {
    auto [count, buffers] = std::tuple(a...);
    return [=, b = Array(buffers, count)] { fp(cb, count, b.get()); };
  } else if constexpr (is<Name>("vkCmdBeginDebugUtilsLabelEXT") || is<Name>("vkCmdInsertDebugUtilsLabelEXT")) {
    auto [label] = std::tuple(a...);
    if (!label || label->pNext) return {};
    return [=, l = *label, name = std::string(label->pLabelName ? label->pLabelName : "")]() mutable {
      l.pLabelName = name.c_str();
      fp(cb, &l);
    };
  } else {
    return {};
  }
}

// The fast (in-JIT) path leaves these calls to the outer boundary.
template <ProcedureName Name, class F, class... A>
bool needs_slow_path(Key handle, A... args) {
  if constexpr (command_buffer_call<F>) {
    if constexpr (is<Name>("vkCmdBindPipeline")) {
      if (enabled() && gfx::pipeline_pending(std::get<2>(std::tuple(args...)))) return true;
    }
    return find(handle) != nullptr;
  } else if constexpr (is<Name>("vkFreeCommandBuffers") || is<Name>("vkResetCommandPool") ||
                       is<Name>("vkDestroyCommandPool")) {
    return active.load(std::memory_order_acquire) != 0;
  }
  return false;
}
// True when the call was recorded for later instead of run now.
// `pool_of(handle)`: the command pool a buffer was allocated from.
template <ProcedureName Name, class F, class PoolOf, class... A>
bool intercept(Key handle, PoolOf&& pool_of, F fp, A... args) {
  if constexpr (is<Name>("vkFreeCommandBuffers")) {
    if (active.load(std::memory_order_acquire)) {
      auto [device, pool, count, buffers] = std::tuple(args...);
      for (std::uint32_t i = 0; buffers && i < count; ++i) drop(reinterpret_cast<Key>(buffers[i]));
    }
    return false;
  } else if constexpr (is<Name>("vkResetCommandPool") || is<Name>("vkDestroyCommandPool")) {
    drop_pool(std::get<1>(std::tuple(args...)));
    return false;
  } else if constexpr (command_buffer_call<F>) {
    if constexpr (is<Name>("vkBeginCommandBuffer") || is<Name>("vkResetCommandBuffer")) {
      drop(handle);
      return false;
    } else {
      auto* r = find(handle);
      VkPipeline pending = VK_NULL_HANDLE;
      if constexpr (is<Name>("vkCmdBindPipeline")) {
        const auto pipeline = std::get<2>(std::tuple(args...));
        if (enabled() && gfx::pipeline_pending(pipeline)) pending = pipeline;
        if (!r && pending) r = start(handle, pool_of(handle));
      }
      if (!r) return false;
      if constexpr (std::string_view(Name.value).starts_with("vkCmd")) {
        if (auto command = capture<Name>(fp, args...)) {
          r->commands.push_back(std::move(command));
          if (pending) {
            r->pipelines.push_back(pending);
            gfx::prioritize_pipeline(pending);
          }
          return true;
        }
      }
      flush(handle, Name.value);
      return false;
    }
  }
  return false;
}
}  // namespace deferred
}  // namespace venture::vulkan
