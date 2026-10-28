#include "callbacks.h"
#include "terminal_allocator.h"
#include "terminal_allocator_ledger.h"
#include "terminal_allocator_admission.h"
#include <atomic>

#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>

#include "common/log.h"
#include "core/thread.h"
#include "core/os_quit.h"

namespace venture::vulkan {
namespace {
struct Allocator {
  VkAllocationCallbacks host{};
  std::array<GuestAddr, 6> guest{};
};
std::atomic<bool> terminal_allocators{false}, allocator_metadata_failed{false};
CallbackAdmission callback_admission;
// Compatible Vulkan allocators may have distinct callbacks/user data. Memory
// ownership is process-wide; callback dispatch remains per original tuple.
TerminalLedger allocation_ledger;
std::mutex callback_lock;
std::map<std::array<GuestAddr, 6>, std::unique_ptr<Allocator>> allocators;
std::map<std::pair<GuestAddr, GuestAddr>, std::shared_ptr<DebugCallback>> debug_callbacks;

std::uint64_t call(GuestAddr fn, std::initializer_list<std::uint64_t> args, bool* completed = nullptr) noexcept {
  if (os_quit::requested()) return 0;
  try {
    auto result = GuestThread::adopt_current().call(fn, args);
    if (completed) *completed = true;
    return result;
  } catch (const std::exception& e) {
    // A callback may have changed ownership before throwing. Only callbacks
    // rejected before guest entry can safely restore their reservation.
    if (completed) allocator_metadata_failed.store(true);
    log::error("vulkan", "guest callback failed: {}", e.what());
    return 0;
  } catch (...) {
    if (completed) allocator_metadata_failed.store(true);
    log::error("vulkan", "guest callback failed");
    return 0;
  }
}
// Once guest execution stops (OS quit), callbacks cannot enter the guest. Native
// calls in the window before the terminal bridge (a deferred submission pumped
// during the drain) are served by host-owned ledger memory, not failed.
bool guest_stopped(CallbackAdmission::Phase phase) {
  return phase == CallbackAdmission::Phase::terminal || os_quit::requested();
}
template <class F> auto ledger_call(F&& operation) {
  auto result = operation();
  if (!allocation_ledger.healthy()) allocator_metadata_failed.store(true);
  return result;
}
void* VKAPI_CALL allocate(void* user, std::size_t size, std::size_t alignment,
                          VkSystemAllocationScope scope) {
  auto admission = callback_admission.enter();
  if (admission.phase() == CallbackAdmission::Phase::closed) return nullptr;
  auto& record = *static_cast<Allocator*>(user);
  const auto host = [&] { return ledger_call([&] { return allocation_ledger.allocate(size, alignment, true); }); };
  if (guest_stopped(admission.phase())) return host();
  const auto& g = record.guest;
  bool completed = false;
  auto operation = allocation_ledger.begin();
  auto* result = g2h(call(g[1], {g[0], size, alignment, static_cast<std::uint64_t>(scope)}, &completed));
  if (!allocation_ledger.complete(std::move(operation), result, size, alignment, false, completed))
    allocator_metadata_failed.store(true);
  return !completed && os_quit::requested() ? host() : result;
}
void* VKAPI_CALL reallocate(void* user, void* old, std::size_t size, std::size_t alignment,
                            VkSystemAllocationScope scope) {
  auto admission = callback_admission.enter();
  if (admission.phase() == CallbackAdmission::Phase::closed) return nullptr;
  auto& record = *static_cast<Allocator*>(user);
  const auto host = [&] { return ledger_call([&] { return allocation_ledger.reallocate(old, size, alignment, true); }); };
  if (guest_stopped(admission.phase())) return host();
  const auto& g = record.guest;
  bool completed = false;
  auto operation = allocation_ledger.begin(old);
  auto* result = g2h(call(g[2], {g[0], h2g(old), size, alignment, static_cast<std::uint64_t>(scope)}, &completed));
  if (!allocation_ledger.complete(std::move(operation), result, size, alignment, false, completed))
    allocator_metadata_failed.store(true);
  return !completed && os_quit::requested() ? host() : result;
}
void VKAPI_CALL release(void* user, void* ptr) {
  auto admission = callback_admission.enter();
  if (admission.phase() == CallbackAdmission::Phase::closed) return;
  auto& record = *static_cast<Allocator*>(user);
  const auto host = [&] { if (!allocation_ledger.release(ptr, true)) allocator_metadata_failed.store(true); };
  if (guest_stopped(admission.phase())) return host();
  const auto& g = record.guest;
  bool completed = false;
  auto operation = allocation_ledger.begin(ptr);
  call(g[3], {g[0], h2g(ptr)}, &completed);
  if (!allocation_ledger.complete(std::move(operation), nullptr, 0, 0, true, completed)) allocator_metadata_failed.store(true);
  if (!completed && os_quit::requested()) host();
}
void VKAPI_CALL internal_allocate(void* user, std::size_t size, VkInternalAllocationType type,
                                  VkSystemAllocationScope scope) {
  auto admission = callback_admission.enter();
  if (admission.phase() != CallbackAdmission::Phase::normal) return;
  const auto& g = static_cast<Allocator*>(user)->guest;
  call(g[4], {g[0], size, static_cast<std::uint64_t>(type), static_cast<std::uint64_t>(scope)});
}
void VKAPI_CALL internal_free(void* user, std::size_t size, VkInternalAllocationType type,
                              VkSystemAllocationScope scope) {
  auto admission = callback_admission.enter();
  if (admission.phase() != CallbackAdmission::Phase::normal) return;
  const auto& g = static_cast<Allocator*>(user)->guest;
  call(g[5], {g[0], size, static_cast<std::uint64_t>(type), static_cast<std::uint64_t>(scope)});
}
}  // namespace

bool begin_terminal_allocator_bridge() {
  if (!os_quit::requested() || os_quit::active_calls.load() || os_quit::cleanup_failed.load() ||
      allocator_metadata_failed.load() || terminal_allocators.load()) return false;
  if (!callback_admission.close_and_drain(std::chrono::seconds(2))) return false;
  // A callback can publish failure after the initial pre-drain check.
  // Recheck once the complete callback/bookkeeping admission is drained.
  if (os_quit::cleanup_failed.load() || allocator_metadata_failed.load()) return false;
  std::lock_guard lock(callback_lock);
  if (!allocation_ledger.seal(true)) { allocator_metadata_failed.store(true); return false; }
  if (!callback_admission.activate_terminal()) return false;
  terminal_allocators.store(true, std::memory_order_release);
  log::info("vulkan", "Terminal allocator bridge active after guest quiescence; records={}", allocators.size());
  return true;
}
bool terminal_allocator_bridge_healthy() {
  return terminal_allocators.load(std::memory_order_acquire) && !allocator_metadata_failed.load();
}

const VkAllocationCallbacks* translate_allocator(const VkAllocationCallbacks* guest) {
  if (!guest) return nullptr;
  if (guest->pfnAllocation == allocate && guest->pfnReallocation == reallocate && guest->pfnFree == release)
    return guest;
  auto admission = callback_admission.enter();
  if (admission.phase() != CallbackAdmission::Phase::normal)
    throw std::logic_error("New guest allocator after terminal quiescence");
  // Vulkan-Headers: all members are pointer-sized on Android AArch64 and host
  // x64; size_t in the callback ABI is 64 bits on both (unlike Windows long).
  static_assert(sizeof(VkAllocationCallbacks) == 48 && sizeof(std::size_t) == 8);
  std::array<GuestAddr, 6> key{};
  std::memcpy(key.data(), guest, sizeof(*guest));
  if (!key[1] || !key[2] || !key[3] || (!!key[4] != !!key[5]))
    throw std::invalid_argument("incomplete VkAllocationCallbacks");
  std::lock_guard lock(callback_lock);
  auto& record = allocators[key];
  if (!record) {
    record = std::make_unique<Allocator>();
    record->guest = key;
    record->host = {record.get(),
                    allocate,
                    reallocate,
                    release,
                    key[4] ? internal_allocate : nullptr,
                    key[5] ? internal_free : nullptr};
  }
  return &record->host;
}

std::shared_ptr<DebugCallback> debug_callback(GuestAddr function, GuestAddr user) {
  if (!function) throw std::invalid_argument("null Vulkan debug callback");
  std::lock_guard lock(callback_lock);
  auto& record = debug_callbacks[{function, user}];
  if (!record) record = std::make_shared<DebugCallback>(function, user);
  return record;
}
VkBool32 VKAPI_CALL debug_utils_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
  const auto& g = *static_cast<DebugCallback*>(user);
  return static_cast<VkBool32>(
      call(g.function, {static_cast<std::uint64_t>(severity), type, h2g(data), g.user}));
}
VkBool32 VKAPI_CALL debug_report_callback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT type,
                                          std::uint64_t object, std::size_t location, std::int32_t code,
                                          const char* layer, const char* message, void* user) {
  const auto& g = *static_cast<DebugCallback*>(user);
  return static_cast<VkBool32>(call(g.function, {flags, static_cast<std::uint64_t>(type), object, location,
                                                 static_cast<std::uint64_t>(static_cast<std::int64_t>(code)),
                                                 h2g(layer), h2g(message), g.user}));
}
}  // namespace venture::vulkan
