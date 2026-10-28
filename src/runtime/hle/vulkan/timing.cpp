#include "core/frame_cpu.h"
#include "platform/thread_priority.h"
#include "common/json.h"
#include <chrono>
#include "common/environment.h"
#include "timing.h"
#include "timing_writer.h"
#include "internal.h"
#include <cstring>
#include <memory>
#include <functional>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include "common/log.h"
#include "core/wait.h"
#include "platform/profile.h"
#include "internal.h"

namespace venture::vulkan {
namespace {
constexpr std::uint32_t slots = 64;
std::atomic<bool> finished{false};
bool enabled() {
  static const bool value = [] {
    const auto* path = venture::environment::getenv("VENTURE_VULKAN_GPU_TIMING");
    return path && *path;
  }();
  return value && !finished.load(std::memory_order_relaxed);
}
template<class T> T vk(VkDevice d, const char* name) {
  return reinterpret_cast<T>(raw_device_proc(d, name));
}
struct Sample {
  VkFence fence{};
  std::uint64_t frame{};
  std::int64_t cpu_ns{};
  bool pending = false;
  bool published = false;
};
struct Queue {
  VkDevice device{};
  std::uint32_t family{}, valid_bits{};
  float period{};
  VkCommandPool pool{};
  VkQueryPool queries{};
  std::array<VkCommandBuffer, slots * 2> commands{};
  std::array<Sample, slots> samples{};
  bool initialized = false, available = false;
};
std::recursive_mutex mutex;
std::map<VkQueue, Queue> queues;
std::atomic<std::uint64_t> frame{1};
enum class Kind { gpu, copy, wait, gles, cpu, overhead, cadence, forwarding_header, forwarding_call };
struct ForwardingHeader {
  std::uint64_t generation{}, bumps{}, dropped{}, busy{};
  std::array<std::uint64_t, 4> events{};
};
struct Record {
  Kind kind{};
  std::uint64_t frame{}, queue{}, begin{}, end{}, cycles{}, host_tid{};
  std::int64_t ns{}, display_time{}, overhead{};
  std::int32_t guest_tid{};
  std::uint32_t interval{};
  double gpu_ns{}, cycles_per_ns{};
  float rate{};
  bool valid{};
  XrCopySample copy{};
  ForwardingHeader forwarding{};
  std::uint64_t calls{}, in_jit{};
  std::int64_t cpu_ns{-1};
  char text[128]{}, role[32]{}, selection[64]{};
};
template<std::size_t N> void text(char (&out)[N], std::string_view value) {
  auto n = std::min(N - 1, value.size());
  if (n < value.size()) while (n && (static_cast<unsigned char>(value[n]) & 0xc0) == 0x80) --n;
  std::memcpy(out, value.data(), n); out[n] = 0;
}
std::string serialize(const Record& r) {
  switch (r.kind) {
  case Kind::gpu:
    return std::format("{{\"frame\":{},\"queue\":{},\"cpu_submit_wall_ns\":{},\"gpu_ns\":{}}}", r.frame, r.queue, r.ns, r.gpu_ns);
  case Kind::copy: {
    const auto& s = r.copy;
    return std::format("{{\"frame\":{},\"xr_copy\":\"{}\",\"cpu_ns\":{},\"release_cpu_ns\":{},\"wait_ns\":{},"
      "\"gl_begin_ns\":{},\"gl_after_ns\":{},\"vk_begin_ns\":{},\"vk_end_ns\":{},\"thread_cpu_ns\":{},\"gpu_transfer\":{},\"gpu_ownership\":{}}}",
      r.frame, r.text, s.cpu_ns, s.release_cpu_ns, s.wait_ns, s.gl_begin_ns, s.gl_after_ns,
      s.vk_begin_ns, s.vk_end_ns, s.thread_cpu_ns, s.gpu_transfer, s.gpu_ownership);
  }
  case Kind::wait:
    if (r.cpu_ns >= 0)
      return std::format("{{\"frame\":{},\"host_wait\":\"{}\",\"ns\":{},\"cpu_ns\":{},\"host_tid\":{},\"begin_ns\":{},\"end_ns\":{}}}",
                         r.frame, r.text, r.ns, r.cpu_ns, r.host_tid, r.begin, r.end);
    return std::format("{{\"frame\":{},\"host_wait\":\"{}\",\"ns\":{},\"host_tid\":{},\"begin_ns\":{},\"end_ns\":{}}}", r.frame, r.text, r.ns, r.host_tid, r.begin, r.end);
  case Kind::gles: return std::format("{{\"frame\":{},\"guest_render\":\"gles\",\"begin_ns\":{},\"end_ns\":{},\"valid\":{},\"gpu_ns\":{}}}",
      r.frame, r.begin, r.end, r.valid, r.valid ? r.end - r.begin : 0);
  case Kind::cpu: {
    Json j{{"frame", r.frame}, {"frame_cpu", r.role}, {"host_tid", r.host_tid}, {"guest_tid", r.guest_tid},
      {"name", r.text}, {"selection", r.selection}, {"cycles", r.cycles},
      {"cycles_per_ns", r.cycles_per_ns}, {"method", platform::thread_cycles_method()}};
    j["cpu_ns_estimate"] = r.cycles_per_ns > 0 ? Json(double(r.cycles) / r.cycles_per_ns) : Json(nullptr);
    return j.dump();
  }
  case Kind::overhead: return Json{{"frame", r.frame}, {"frame_cpu_sampler_wall_ns", r.overhead}}.dump();
  case Kind::forwarding_header: {
    const auto& f = r.forwarding;
    constexpr const char* names[]{"vkAllocateCommandBuffers", "vkFreeCommandBuffers",
                                "vkCreateCommandPool", "vkDestroyCommandPool"};
    Json events = Json::object();
    for (std::size_t i = 0; i < f.events.size(); ++i) events[names[i]] = f.events[i];
    return Json{{"frame", r.frame}, {"vk_forwarding", true}, {"layer_generation", f.generation},
      {"generation_bumps", f.bumps}, {"lifetime_events", events},
      {"dropped_census_threads", f.dropped}, {"busy_census_threads", f.busy},
      {"calls", Json::array()}}.dump();
  }
  case Kind::forwarding_call:
    // Multiple rows retain one frame ID. Counts sum across rows, while census
    // coverage counts unique frames; lifetime/generation deltas live in the header.
    return Json{{"frame", r.frame}, {"vk_forwarding", true}, {"layer_generation", r.forwarding.generation},
      {"generation_bumps", 0}, {"lifetime_events", Json::object()},
      {"dropped_census_threads", 0}, {"busy_census_threads", 0},
      {"calls", Json::array({{{"guest_tid", r.guest_tid}, {"command", r.text},
                             {"calls", r.calls}, {"in_jit", r.in_jit}}})}}.dump();
  case Kind::cadence: return std::format("{{\"frame\":{},\"display_time_ns\":{},\"swap_interval\":{},\"display_rate_hz\":{},\"guest_cadence_hz\":{},\"budget_ms\":7}}",
      r.frame, r.display_time, r.interval, r.rate, r.rate / r.interval);
  }
  return {};
}
struct Output {
  using Sink = std::function<bool(const Record&)>;
  std::filesystem::path path;
  std::ofstream stream;
  bool opened = false;
  bool synchronous;
  std::uint64_t sync_written{}, sync_accepted{}, sync_lost{}, sync_failures{};
  bool sync_finished = false;
  std::chrono::steady_clock::time_point flushed{};
  struct Finish {
    Output* owner;
    template<class Status> void operator()(const Status& s) const { owner->finish_status(s.accepted, s.written, s.lost, s.write_failures, s.final_drain); }
  };
  using Writer = TimingWriter<Record, 8192, Sink, Finish>;
  std::unique_ptr<Writer> worker;
  Output() : path(std::filesystem::u8path(venture::environment::getenv("VENTURE_VULKAN_GPU_TIMING"))),
      synchronous([] { auto* p = venture::environment::getenv("VENTURE_TIMING_WRITER"); return p && std::string_view(p) == "sync"; }()) {
    if (!synchronous) worker = std::make_unique<Writer>([this](const Record& r) { return write(r); }, Finish{this});
  }
  ~Output() { finish(); }
  void status(std::uint64_t accepted, std::uint64_t written, std::uint64_t lost, std::uint64_t failures, bool drained) {
    auto status_path = path; status_path += ".status.json";
    std::ofstream out(status_path, std::ios::binary | std::ios::trunc);
    out << Json{{"timing_writer", synchronous ? "sync" : "async"}, {"accepted_records", accepted},
      {"written_records", written}, {"lost_records", lost}, {"write_failures", failures}, {"final_drain", drained}}.dump() << '\n';
    if (!out) log::warn("vulkan", "cannot write timing writer status");
  }
  bool write(const Record& r) {
    if (!opened) {
      opened = true; status(0, 0, 0, 0, false);
      stream.open(path, std::ios::binary | std::ios::trunc);
      stream.exceptions(std::ios::badbit | std::ios::failbit);
    }
    stream << serialize(r) << '\n';
    const auto now = std::chrono::steady_clock::now();
    if (r.kind == Kind::cadence || now - flushed >= std::chrono::milliseconds(250)) {
      stream.flush(); flushed = now;
    }
    return bool(stream);
  }
  void emit(const Record& r) {
    if (worker) { worker->emit(r); return; }
    std::lock_guard lock(mutex); // Original synchronous arm, comparison only.
    if (sync_finished) return;
    ++sync_accepted;
    try { if (write(r)) ++sync_written; else { ++sync_lost; ++sync_failures; } }
    catch (...) { ++sync_lost; ++sync_failures; log::warn("vulkan", "synchronous timing write failed"); }
  }
  void finish_status(std::uint64_t accepted, std::uint64_t written, std::uint64_t lost, std::uint64_t failures, bool drained) {
    try { if (stream.is_open()) stream.flush(); }
    catch (...) { ++failures; drained = false; }
    try { status(accepted, written, lost, failures, drained); } catch (...) {}
    if (failures) log::warn("vulkan", "timing writer failed: lost={} failures={} final_drain={}", lost, failures, drained);
  }
  void finish() {
    if (worker) worker->finish();
    else {
      std::lock_guard lock(mutex);
      if (sync_finished) return;
      sync_finished = true;
      finish_status(sync_accepted, sync_written, sync_lost, sync_failures, sync_failures == 0);
    }
  }
};
Output& output() { static Output value; return value; }
void emit(const Record& r) { output().emit(r); }
void telemetry_failure() noexcept {
  try { throw; }
  catch (const std::exception& e) {
    try { log::warn("vulkan", "GPU timing telemetry failed: {}; sample omitted", e.what()); } catch (...) {}
  } catch (...) {
    try { log::warn("vulkan", "GPU timing telemetry failed; sample omitted"); } catch (...) {}
  }
}
void check(VkResult r) {
  if (r != VK_SUCCESS) throw std::runtime_error(std::format("GPU timing VkResult {}", int(r)));
}
void collect(VkQueue handle, Queue& q) {
  if (finished.load(std::memory_order_relaxed)) return;
  for (std::uint32_t i = 0; i < slots; ++i) {
    auto& s = q.samples[i];
    if (!s.pending || !s.published || vk<PFN_vkGetFenceStatus>(q.device, "vkGetFenceStatus")(q.device, s.fence) != VK_SUCCESS)
      continue;
    std::uint64_t ticks[2]{};
    check(vk<PFN_vkGetQueryPoolResults>(q.device, "vkGetQueryPoolResults")(
        q.device, q.queries, 2*i, 2, sizeof(ticks), ticks, sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT));
    const auto mask = q.valid_bits == 64 ? UINT64_MAX : (std::uint64_t{1} << q.valid_bits) - 1;
    const auto ns = double((ticks[1] - ticks[0]) & mask) * q.period;
    Record record{.kind=Kind::gpu, .frame=s.frame, .queue=h2g(handle), .ns=s.cpu_ns, .gpu_ns=ns};
    emit(record);
    s.pending = false;
  }
}
void initialize(Queue& q) {
  q.initialized = true;
  auto physical = device_physical(q.device);
  auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      raw_physical_proc(physical, "vkGetPhysicalDeviceQueueFamilyProperties"));
  std::uint32_t count = 0;
  families(physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> values(count);
  families(physical, &count, values.data());
  q.valid_bits = values.at(q.family).timestampValidBits;
  if (!q.valid_bits) {
    log::warn("vulkan", "GPU timing unavailable on queue family {}", q.family);
    return;
  }
  VkPhysicalDeviceProperties properties{};
  reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(raw_physical_proc(physical, "vkGetPhysicalDeviceProperties"))(
      physical, &properties);
  q.period = properties.limits.timestampPeriod;
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.queueFamilyIndex = q.family;
  check(vk<PFN_vkCreateCommandPool>(q.device, "vkCreateCommandPool")(q.device, &pool, nullptr, &q.pool));
  VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  query.queryType = VK_QUERY_TYPE_TIMESTAMP;
  query.queryCount = slots * 2;
  check(vk<PFN_vkCreateQueryPool>(q.device, "vkCreateQueryPool")(q.device, &query, nullptr, &q.queries));
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = q.pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = slots * 2;
  check(vk<PFN_vkAllocateCommandBuffers>(q.device, "vkAllocateCommandBuffers")(q.device, &allocate, q.commands.data()));
  for (std::uint32_t i = 0; i < slots; ++i) {
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vk<PFN_vkCreateFence>(q.device, "vkCreateFence")(q.device, &fence, nullptr, &q.samples[i].fence));
    for (std::uint32_t end = 0; end < 2; ++end) {
      auto command = q.commands[2*i + end];
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      check(vk<PFN_vkBeginCommandBuffer>(q.device, "vkBeginCommandBuffer")(command, &begin));
      if (!end) vk<PFN_vkCmdResetQueryPool>(q.device, "vkCmdResetQueryPool")(command, q.queries, 2*i, 2);
      // BOTTOM_OF_PIPE excludes older queue work from the start timestamp and
      // includes completion of the enclosed commands in the end timestamp.
      // This measures queue elapsed time, including GPU semaphore stalls.
      vk<PFN_vkCmdWriteTimestamp>(q.device, "vkCmdWriteTimestamp")(
          command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, q.queries, 2*i + end);
      // Profiling deliberately prevents a later batch from overlapping this
      // marker, otherwise its work could start before the start timestamp.
      if (!end)
        vk<PFN_vkCmdPipelineBarrier>(q.device, "vkCmdPipelineBarrier")(
            command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 0, nullptr);
      check(vk<PFN_vkEndCommandBuffer>(q.device, "vkEndCommandBuffer")(command));
    }
  }
  q.available = true;
}
template<class Submit> VkResult measure(VkQueue handle, VkFence guest_fence, Submit submit) {
  if (!enabled()) return submit(nullptr, nullptr, guest_fence);
  std::unique_lock lock(mutex);
  auto it = queues.find(handle);
  if (it == queues.end()) {
    lock.unlock();
    return submit(nullptr, nullptr, guest_fence);
  }
  auto& q = it->second;
  std::uint32_t slot = slots;
  try {
    if (!q.initialized) initialize(q);
    if (q.available) {
      collect(handle, q);
      for (std::uint32_t i = 0; i < slots; ++i)
        if (!q.samples[i].pending) { slot = i; break; }
      if (slot == slots) log::warn("vulkan", "GPU timing ring full; sample omitted");
      else check(vk<PFN_vkResetFences>(q.device, "vkResetFences")(q.device, 1, &q.samples[slot].fence));
    }
  } catch (const std::exception& e) {
    log::warn("vulkan", "{}; disabling queue profiling", e.what());
    q.available = false;
    slot = slots;
  }
  if (slot == slots) {
    lock.unlock();
    return submit(nullptr, nullptr, guest_fence);
  }
  auto& s = q.samples[slot];
  s.frame = frame.load(std::memory_order_relaxed);
  const auto cpu_start = monotonic_ns();
  s.published = false;
  s.pending = true;
  // Reserve the slot before releasing the bookkeeping lock. A native submit
  // may block until another queue progresses; never hold a cross-queue mutex
  // across that call. The reset, unsignaled fence keeps collect() from reusing
  // this slot until its markers have actually completed.
  lock.unlock();
  VkResult result;
  try { result = submit(&q.commands[2*slot], &q.commands[2*slot+1], s.fence); }
  catch (...) {
    { std::lock_guard cleanup(mutex); s.pending = false; }
    throw; // Public boundary falls back only if native submission never ran.
  }
  if (result != VK_SUCCESS) {
    std::lock_guard cleanup(mutex);
    s.pending = false;
    return result;
  }
  {
    std::lock_guard publish(mutex);
    s.cpu_ns = monotonic_ns() - cpu_start;
    s.published = true;
  }
  // Our fence owns marker lifetime. Preserve the application's fence by
  // signaling it in queue order after the same work (Vulkan empty submission).
  if (guest_fence)
    return vk<PFN_vkQueueSubmit>(q.device, "vkQueueSubmit")(handle, 0, nullptr, guest_fence);
  return result;
}
}
void timing_queue(VkDevice device, VkQueue queue, std::uint32_t family) try {
  if (!enabled()) return;
  std::lock_guard lock(mutex);
  if (!enabled()) return;
  (void)output(); // Allocate the bounded ring before queue/frame producers start.
  queues.try_emplace(queue, Queue{.device=device, .family=family});
} catch (...) { telemetry_failure(); }
void release_timing_device(VkDevice device) try {
  if (!enabled()) return;
  // Device destruction excludes this device's queue users, but another device
  // may still need to submit. Do not hold profiling's cross-device mutex here.
  vk<PFN_vkDeviceWaitIdle>(device, "vkDeviceWaitIdle")(device);
  std::lock_guard lock(mutex);
  for (auto it = queues.begin(); it != queues.end();) {
    auto& q = it->second;
    if (q.device != device) { ++it; continue; }
    try { if (q.available) collect(it->first, q); }
    catch (...) { telemetry_failure(); }
    for (auto& s : q.samples)
      if (s.fence) vk<PFN_vkDestroyFence>(device, "vkDestroyFence")(device, s.fence, nullptr);
    if (q.pool) vk<PFN_vkDestroyCommandPool>(device, "vkDestroyCommandPool")(device, q.pool, nullptr);
    if (q.queries) vk<PFN_vkDestroyQueryPool>(device, "vkDestroyQueryPool")(device, q.queries, nullptr);
    it = queues.erase(it);
  }
} catch (...) { telemetry_failure(); }
void finish_timing() try {
  if (!enabled()) return;
  // Serialize the final producer transition, but never hold queue state while draining I/O.
  { std::lock_guard lock(mutex); if (!enabled()) return; finished.store(true, std::memory_order_relaxed); }
  output().finish();
} catch (...) { telemetry_failure(); }
bool timing_enabled() try { return enabled(); } catch (...) { telemetry_failure(); return false; }
std::uint64_t timing_frame() try { return frame.load(std::memory_order_relaxed); } catch (...) { telemetry_failure(); return 0; }
void timing_xr_copy(const XrCopySample& s) try {
  if (!enabled()) return;
  Record r{.kind=Kind::copy, .frame=s.frame, .copy=s};
  text(r.text, s.path); r.copy.path = nullptr;
  emit(r);
} catch (...) { telemetry_failure(); }
void timing_host_wait(const char* what, std::int64_t ns, std::int64_t cpu_ns) try {
  if (!enabled()) return;
  const auto end = monotonic_ns();
  Record r{.kind=Kind::wait, .frame=timing_frame(),
    .begin=static_cast<std::uint64_t>(end - ns), .end=static_cast<std::uint64_t>(end),
    .host_tid=platform::profile_thread_id(), .ns=ns};
  r.cpu_ns = cpu_ns;
  text(r.text, what); emit(r);
} catch (...) { telemetry_failure(); }
// Thread cycles, not thread times: Windows thread times advance per scheduler
// quantum (~15.6 ms), so a frame-sized span would read zero.
ScopedHostWait::ScopedHostWait(const char* what) : what(what) {
  if (!timing_enabled() || platform::thread_cycles_per_ns() <= 0) return;
  wall = monotonic_ns(); cpu = platform::thread_cpu_cycles();
}
ScopedHostWait::~ScopedHostWait() {
  if (!wall) return;
  const auto cycles = platform::thread_cpu_cycles();
  timing_host_wait(what, monotonic_ns() - wall,
                   cpu < 0 || cycles < 0 ? -1 : std::int64_t(double(cycles - cpu) / platform::thread_cycles_per_ns()));
}
void timing_gles_render(std::uint64_t sample_frame, std::uint64_t begin, std::uint64_t end, bool valid) try {
  if (!enabled()) return;
  emit(Record{.kind=Kind::gles, .frame=sample_frame, .begin=begin, .end=end, .valid=valid});
} catch (...) { telemetry_failure(); }
void timing_end_frame(std::int64_t display_time, std::uint32_t swap_interval, float display_rate) try {
  if (!enabled()) return;
  const auto current = timing_frame();
  static const bool census = [] {
    const auto* value = venture::environment::getenv("VENTURE_VULKAN_FORWARD_CENSUS");
    return value && std::string_view(value) == "1";
  }();
  if (census) {
    // Snapshot on the frame boundary, but keep JSON construction and I/O on the
    // bounded writer. Fixed records own no vectors, pointers or guest memory.
    std::lock_guard lock(mutex);
    const auto sample = forwarding_sample();
    static std::array<std::uint64_t, 4> previous_events{};
    static std::uint64_t previous_generation{};
    Record header{.kind=Kind::forwarding_header, .frame=current};
    auto& f = header.forwarding;
    f.generation = sample.layer_generation;
    f.bumps = previous_generation ? sample.layer_generation - previous_generation : 0;
    f.dropped = sample.dropped_census_threads; f.busy = sample.busy_census_threads;
    for (std::size_t i = 0; i < f.events.size(); ++i)
      f.events[i] = sample.lifetime_events[i] - previous_events[i];
    previous_events = sample.lifetime_events; previous_generation = sample.layer_generation;
    emit(header);
    for (const auto& call : sample.calls) {
      Record record{.kind=Kind::forwarding_call, .frame=current, .guest_tid=call.guest_tid};
      record.forwarding.generation = sample.layer_generation;
      record.calls = call.calls; record.in_jit = call.in_jit;
      text(record.text, call.command); emit(record);
    }
  }
  if (frame_cpu::enabled) {
    const auto cpu_begin = std::chrono::steady_clock::now();
    const auto cpu_samples = frame_cpu::boundary();
    const auto overhead = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - cpu_begin).count();
    for (const auto& s : cpu_samples) {
      Record r{.kind=Kind::cpu, .frame=current, .cycles=s.cycles, .host_tid=s.host_tid,
        .guest_tid=s.guest_tid, .cycles_per_ns=s.cycles_per_ns};
      text(r.text, s.name); text(r.role, s.role); text(r.selection, s.selection); emit(r);
    }
    emit(Record{.kind=Kind::overhead, .frame=current, .overhead=overhead});
  }
  { std::lock_guard lock(mutex);
    if (!enabled()) return;
    for (auto& [handle, q] : queues) if (q.available) {
      try { collect(handle, q); }
      catch (...) { q.available = false; telemetry_failure(); }
    }
  }
  emit(Record{.kind=Kind::cadence, .frame=current, .display_time=display_time, .interval=swap_interval, .rate=display_rate});
  frame.fetch_add(1, std::memory_order_relaxed);
} catch (...) { telemetry_failure(); }
VkResult timed_submit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits,
                      VkFence fence, PFN_vkQueueSubmit fn) {
  bool submitted = false;
  VkResult result = VK_ERROR_UNKNOWN;
  auto native = [&](VkQueue q, std::uint32_t n, const VkSubmitInfo* batch, VkFence done) {
    submitted = true;
    result = fn(q, n, batch, done);
    return result;
  };
  try {
    if (!enabled() || !count) return native(queue, count, submits, fence);
    for (std::uint32_t i = 0; i < count; ++i)
      for (auto* p = static_cast<const VkBaseInStructure*>(submits[i].pNext); p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO || p->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO)
          return native(queue, count, submits, fence);
    return measure(queue, fence, [&](const VkCommandBuffer* begin, const VkCommandBuffer* end, VkFence done) {
      if (!begin) return native(queue, count, submits, done);
      std::vector<VkSubmitInfo> batch(count + 2, {VK_STRUCTURE_TYPE_SUBMIT_INFO});
      batch.front().commandBufferCount = batch.back().commandBufferCount = 1;
      batch.front().pCommandBuffers = begin;
      batch.back().pCommandBuffers = end;
      std::copy_n(submits, count, batch.begin() + 1);
      return native(queue, static_cast<std::uint32_t>(batch.size()), batch.data(), done);
    });
  } catch (...) {
    telemetry_failure();
    if (submitted) return result; // No duplicate guest work after a post-submit failure.
    try { return native(queue, count, submits, fence); }
    catch (...) { telemetry_failure(); return VK_ERROR_UNKNOWN; }
  }
}
VkResult timed_submit2(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* submits,
                       VkFence fence, PFN_vkQueueSubmit2 fn) {
  bool submitted = false;
  VkResult result = VK_ERROR_UNKNOWN;
  auto native = [&](VkQueue q, std::uint32_t n, const VkSubmitInfo2* batch, VkFence done) {
    submitted = true;
    result = fn(q, n, batch, done);
    return result;
  };
  try {
    if (!enabled() || !count) return native(queue, count, submits, fence);
    for (std::uint32_t i = 0; i < count; ++i) {
      if (submits[i].flags) return native(queue, count, submits, fence);
      for (std::uint32_t c = 0; c < submits[i].commandBufferInfoCount; ++c)
        if (submits[i].pCommandBufferInfos[c].deviceMask > 1) return native(queue, count, submits, fence);
    }
    return measure(queue, fence, [&](const VkCommandBuffer* begin, const VkCommandBuffer* end, VkFence done) {
      if (!begin) return native(queue, count, submits, done);
      VkCommandBufferSubmitInfo first{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, *begin, 1};
      VkCommandBufferSubmitInfo last{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, *end, 1};
      std::vector<VkSubmitInfo2> batch(count + 2, {VK_STRUCTURE_TYPE_SUBMIT_INFO_2});
      batch.front().commandBufferInfoCount = batch.back().commandBufferInfoCount = 1;
      batch.front().pCommandBufferInfos = &first;
      batch.back().pCommandBufferInfos = &last;
      std::copy_n(submits, count, batch.begin() + 1);
      return native(queue, static_cast<std::uint32_t>(batch.size()), batch.data(), done);
    });
  } catch (...) {
    telemetry_failure();
    if (submitted) return result; // No duplicate guest work after a post-submit failure.
    try { return native(queue, count, submits, fence); }
    catch (...) { telemetry_failure(); return VK_ERROR_UNKNOWN; }
  }
}
}
