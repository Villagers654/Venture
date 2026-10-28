// Android SYNC_FD semaphore and fence payloads. Android CDD 7.1.4.2 [C-3-1]
// requires them for Vulkan 1.1; Windows drivers have no Linux sync files, so
// Venture provides sync files as guest descriptors backed by host fences.
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "common/log.h"
#include "external.h"
#include "hle/libc/vfs.h"
#include "internal.h"
#include "core/wait.h"
#include "timing.h"
#include "chain.h"

namespace venture::vulkan {
namespace {
using hle::libc::FdTable;
constexpr auto semaphore_sync = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
constexpr auto fence_sync = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
template <class F>
F device_fn(VkDevice d, const char* name) {
  return reinterpret_cast<F>(raw_device_proc(d, name));
}
template <class F>
F physical_fn(VkPhysicalDevice p, const char* name) {
  return reinterpret_cast<F>(raw_physical_proc(p, name));
}
// Blocks the calling thread until `fence` signals (VENTURE_VULKAN_GPU_TIMING logs it).
VkResult wait_fence(VkDevice d, VkFence fence, const char* what) {
  const auto start = timing_enabled() ? monotonic_ns() : 0;
  auto r = device_fn<PFN_vkWaitForFences>(d, "vkWaitForFences")(d, 1, &fence, VK_TRUE, UINT64_MAX);
  if (start) timing_host_wait(what, monotonic_ns() - start);
  return r;
}

// A Linux sync file: POLLIN once its fence has signaled (kernel sync_file.c).
// A null fence is an already-signaled file.
class SyncFile final : public hle::libc::File {
 public:
  SyncFile(VkDevice device, VkFence fence) : device_(device), fence_(fence) {}
  ~SyncFile() override { retire(); }
  short ready(short events) override { return signaled() ? events & hle::libc::poll_in : 0; }
  bool signaled() {
    std::lock_guard lock(lock_);
    if (!fence_) return true;
    return device_fn<PFN_vkGetFenceStatus>(device_, "vkGetFenceStatus")(device_, fence_) == VK_SUCCESS;
  }
  // Blocks until signaled and releases the host fence.
  void retire() {
    std::lock_guard lock(lock_);
    if (!fence_) return;
    wait_fence(device_, fence_, "sync_file");
    device_fn<PFN_vkDestroyFence>(device_, "vkDestroyFence")(device_, fence_, nullptr);
    fence_ = VK_NULL_HANDLE;
  }
  VkDevice device() const { return device_; }

 private:
  std::mutex lock_;
  VkDevice device_;
  VkFence fence_;
};
// Live semaphore exports by device, with the semaphore their wait references.
struct Export {
  VkSemaphore semaphore;
  std::weak_ptr<SyncFile> file;
};
std::mutex files_lock;
std::multimap<VkDevice, Export> files;
std::atomic<std::uint64_t> import_signals{0};
// Binary semaphores whose host payload a temporary import signaled and no wait
// has consumed yet. Re-importing replaces it with an equally signaled payload,
// so no second host signal is issued: a signaled binary semaphore must not be
// signaled again (VUID-vkQueueSubmit-pSignalSemaphores-00067). A signaled
// permanent payload is not tracked; importing over one still signals twice.
std::mutex imports_lock;
std::set<std::pair<VkDevice, VkSemaphore>> imported;
std::atomic<std::size_t> imported_count{0};  // lets submissions skip the lock
// Waits consume the imported payloads of these semaphores.
template <class Each>
void consumed(VkDevice device, Each each) {
  if (!imported_count.load(std::memory_order_acquire)) return;
  std::lock_guard lock(imports_lock);
  each([&](VkSemaphore s) { imported.erase({device, s}); });
  imported_count.store(imported.size(), std::memory_order_release);
}
template <class Each>
void queue_consumed(VkQueue queue, Each each) {
  if (!imported_count.load(std::memory_order_acquire)) return;
  if (auto binding = queue_binding(queue)) consumed(binding->device, each);
}

VkResult submit(VkDevice device, const VkSubmitInfo* info, VkFence fence) {
  auto queue = internal_queue(device);
  if (!queue) return VK_ERROR_DEVICE_LOST;
  // The queue's own mutex, as guest queue calls take it (gfx/queue_sync.h);
  // for the shared graphics queue that is its ANGLE/XR queue_lock.
  if (info && info->waitSemaphoreCount) texture_queue_uncertain(queue->queue);
  const std::span<const VkSemaphore> waits = info ? std::span{info->pWaitSemaphores, info->waitSemaphoreCount}
                                               : std::span<const VkSemaphore>{};
  const std::span<const VkSemaphore> signals = info ? std::span{info->pSignalSemaphores, info->signalSemaphoreCount}
                                                 : std::span<const VkSemaphore>{};
  struct Owned { Chain storage; const VkSubmitInfo* info{}; };
  auto owned = std::make_shared<Owned>();
  if (info) {
    owned->storage.retain_input_arrays();
    owned->info = static_cast<const VkSubmitInfo*>(owned->storage.translate(info));
    if (!owned->info || owned->storage.result() != VK_SUCCESS) return VK_ERROR_INITIALIZATION_FAILED;
  }
  return external_queue_work(device, queue->queue, waits, signals, [device, queue, owned, fence](VkQueue native) {
    std::lock_guard lock(*queue->mutex);
    return device_fn<PFN_vkQueueSubmit>(device, "vkQueueSubmit")(native, owned->info ? 1 : 0, owned->info, fence);
  });
}
// Resolves an imported descriptor: -1 is an already-signaled sync file
// (VkImportSemaphoreFdInfoKHR/VkImportFenceFdInfoKHR). Waits for its payload.
VkResult wait_imported(int fd, std::shared_ptr<SyncFile>* file) {
  if (fd == -1) return VK_SUCCESS;
  *file = std::dynamic_pointer_cast<SyncFile>(FdTable::get().file(fd));
  if (!*file) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  (*file)->retire();
  return VK_SUCCESS;
}
// The layer below this one that intercepts `name` (internal.h Layer), if any.
template <class F, ProcedureName Name>
F next_layer() {
  return reinterpret_cast<F>(cached_layer_proc<Layer::sync_fd, Name>());
}
// The command below this layer: the next layer's, else the host's.
template <class F, ProcedureName Name>
F next_fn(VkDevice d) {
  if (auto f = next_layer<F, Name>()) return f;
  return device_fn<F>(d, Name.value);
}

VkResult VKAPI_CALL get_semaphore_fd(VkDevice d, const VkSemaphoreGetFdInfoKHR* info, int* out) {
  if (!info || !out) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  if (info->handleType != semaphore_sync) {
    auto f = next_layer<PFN_vkGetSemaphoreFdKHR, "vkGetSemaphoreFdKHR">();
    return f ? f(d, info, out) : VK_ERROR_INVALID_EXTERNAL_HANDLE;
  }
  *out = -1;
  // Copy transference: exporting has the side effects of a semaphore wait
  // (Vulkan "Importing Semaphore Payloads"). The wait signals the file's fence.
  // It runs on the internal queue, which may differ from the queue that
  // signaled the semaphore; a re-signal there can race the wait.
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence{};
  auto r = device_fn<PFN_vkCreateFence>(d, "vkCreateFence")(d, &fence_info, nullptr, &fence);
  if (r != VK_SUCCESS) return r;
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo wait{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  wait.waitSemaphoreCount = 1;
  wait.pWaitSemaphores = &info->semaphore;
  wait.pWaitDstStageMask = &stage;
  r = submit(d, &wait, fence);
  if (r != VK_SUCCESS) {
    device_fn<PFN_vkDestroyFence>(d, "vkDestroyFence")(d, fence, nullptr);
    return r;
  }
  consumed(d, [&](auto erase) { erase(info->semaphore); });
  // From here the submitted fence is owned (and retired) by the file.
  auto file = std::make_shared<SyncFile>(d, fence);
  const int fd = FdTable::get().insert(file, 0, true);  // close-on-exec, per vkGetSemaphoreFdKHR
  if (fd < 0) return VK_ERROR_TOO_MANY_OBJECTS;
  {
    std::lock_guard lock(files_lock);
    std::erase_if(files, [](const auto& entry) { return entry.second.file.expired(); });  // closed or imported
    files.emplace(d, Export{info->semaphore, file});
  }
  *out = fd;
  return VK_SUCCESS;
}
VkResult VKAPI_CALL import_semaphore_fd(VkDevice d, const VkImportSemaphoreFdInfoKHR* info) {
  if (!info) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  if (info->handleType != semaphore_sync) {
    auto f = next_layer<PFN_vkImportSemaphoreFdKHR, "vkImportSemaphoreFdKHR">();
    return f ? f(d, info) : VK_ERROR_INVALID_EXTERNAL_HANDLE;
  }
  // SYNC_FD supports only temporary (copy) imports: VUID-VkImportSemaphoreFdInfoKHR-handleType-07307.
  if (!(info->flags & VK_SEMAPHORE_IMPORT_TEMPORARY_BIT)) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  std::shared_ptr<SyncFile> file;
  if (auto r = wait_imported(info->fd, &file); r != VK_SUCCESS) return r;
  bool pending;
  {
    std::lock_guard lock(imports_lock);
    pending = imported.contains({d, info->semaphore});
  }
  // The payload has signaled. The binary semaphore has no pending operation
  // (VUID-vkImportSemaphoreFdKHR-semaphore-01142); signal it for its next wait.
  VkResult r = VK_SUCCESS;
  if (!pending) {
    VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    signal.signalSemaphoreCount = 1;
    signal.pSignalSemaphores = &info->semaphore;
    r = submit(d, &signal, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) {
      ++import_signals;
      std::lock_guard lock(imports_lock);
      imported.insert({d, info->semaphore});
      imported_count.store(imported.size(), std::memory_order_release);
    }
  }
  if (r == VK_SUCCESS && file) FdTable::get().close(info->fd);  // ownership transfers on success
  return r;
}
VkResult VKAPI_CALL get_fence_fd(VkDevice d, const VkFenceGetFdInfoKHR* info, int* out) {
  if (!info || !out) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  *out = -1;
  if (info->handleType != fence_sync) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  // The fence is signaled or has a pending signal operation
  // (VUID-VkFenceGetFdInfoKHR-handleType-01454). Once it has signaled,
  // vkGetFenceFdKHR may return -1; export then resets it (copy transference).
  auto r = wait_fence(d, info->fence, "get_fence_fd");
  if (r != VK_SUCCESS) return r;
  return device_fn<PFN_vkResetFences>(d, "vkResetFences")(d, 1, &info->fence);
}
VkResult VKAPI_CALL import_fence_fd(VkDevice d, const VkImportFenceFdInfoKHR* info) {
  if (!info || info->handleType != fence_sync) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  if (!(info->flags & VK_FENCE_IMPORT_TEMPORARY_BIT)) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
  std::shared_ptr<SyncFile> file;
  if (auto r = wait_imported(info->fd, &file); r != VK_SUCCESS) return r;
  // VUID-vkImportFenceFdKHR-fence-01463: no pending queue operation. A signaled
  // fence already carries the imported state; otherwise signal it now.
  VkResult r = device_fn<PFN_vkGetFenceStatus>(d, "vkGetFenceStatus")(d, info->fence);
  if (r == VK_NOT_READY) {
    r = submit(d, nullptr, info->fence);
    if (r == VK_SUCCESS)
      r = wait_fence(d, info->fence, "import_fence_fd");
  }
  if (r == VK_SUCCESS && file) FdTable::get().close(info->fd);
  return r;
}
// Presentation and sparse binding wait too. Presentation errors that still
// enqueue the request execute its waits (vkQueuePresentKHR).
VkResult VKAPI_CALL queue_present(VkQueue queue, const VkPresentInfoKHR* info) {
  auto binding = queue_binding(queue);
  if (!binding) return VK_ERROR_DEVICE_LOST;
  auto r = next_fn<PFN_vkQueuePresentKHR, "vkQueuePresentKHR">(binding->device)(queue, info);
  if (r != VK_ERROR_OUT_OF_HOST_MEMORY && r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_DEVICE_LOST)
    consumed(binding->device, [&](auto erase) {
      for (std::uint32_t i = 0; i < info->waitSemaphoreCount; ++i) erase(info->pWaitSemaphores[i]);
    });
  return r;
}
VkResult VKAPI_CALL bind_sparse(VkQueue queue, std::uint32_t count, const VkBindSparseInfo* infos, VkFence fence) {
  auto binding = queue_binding(queue);
  if (!binding) return VK_ERROR_DEVICE_LOST;
  auto r = next_fn<PFN_vkQueueBindSparse, "vkQueueBindSparse">(binding->device)(queue, count, infos, fence);
  if (r == VK_SUCCESS)
    consumed(binding->device, [&](auto erase) {
      for (std::uint32_t i = 0; i < count; ++i)
        for (std::uint32_t w = 0; w < infos[i].waitSemaphoreCount; ++w) erase(infos[i].pWaitSemaphores[w]);
    });
  return r;
}
// A recycled handle must not inherit an imported payload. An export's hidden
// wait may still reference the semaphore after the guest's own batches have
// completed (VUID-vkDestroySemaphore-semaphore-05149): retire those first. The
// guest's batches signaling it are done, so the waits are already satisfied.
void VKAPI_CALL destroy_semaphore(VkDevice d, VkSemaphore semaphore, const VkAllocationCallbacks* allocator) {
  consumed(d, [&](auto erase) { erase(semaphore); });
  std::vector<std::shared_ptr<SyncFile>> live;
  if (semaphore) {
    std::lock_guard lock(files_lock);
    auto [begin, end] = files.equal_range(d);
    for (auto it = begin; it != end;) {
      if (it->second.semaphore != semaphore) { ++it; continue; }
      if (auto file = it->second.file.lock()) live.push_back(std::move(file));
      it = files.erase(it);
    }
  }
  for (auto& file : live) file->retire();
  next_fn<PFN_vkDestroySemaphore, "vkDestroySemaphore">(d)(d, semaphore, allocator);
}
bool timeline(const void* next) {
  for (auto* p = static_cast<const VkBaseInStructure*>(next); p; p = p->pNext)
    if (p->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO)
      return reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(p)->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE;
  return false;
}
void VKAPI_CALL semaphore_properties(VkPhysicalDevice p, const VkPhysicalDeviceExternalSemaphoreInfo* info,
                                     VkExternalSemaphoreProperties* out) {
  out->exportFromImportedHandleTypes = out->compatibleHandleTypes = 0;
  out->externalSemaphoreFeatures = 0;
  if (info->handleType != semaphore_sync) {
    if (auto f = next_layer<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties,
            "vkGetPhysicalDeviceExternalSemaphoreProperties">())
      f(p, info, out);
    return;
  }
  // SYNC_FD has copy transference and is defined for binary semaphores only.
  if (timeline(info->pNext)) return;
  out->exportFromImportedHandleTypes = out->compatibleHandleTypes = semaphore_sync;
  out->externalSemaphoreFeatures =
      VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
}
void VKAPI_CALL fence_properties(VkPhysicalDevice, const VkPhysicalDeviceExternalFenceInfo* info,
                                 VkExternalFenceProperties* out) {
  const bool sync = info->handleType == fence_sync;
  out->exportFromImportedHandleTypes = out->compatibleHandleTypes = sync ? fence_sync : 0;
  out->externalFenceFeatures =
      sync ? VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT : 0;
}
}  // namespace

bool sync_fd_extension(std::string_view name) {
  return name == "VK_KHR_external_semaphore_fd" || name == "VK_KHR_external_fence_fd";
}
void sync_fd_extensions(std::vector<VkExtensionProperties>& values) {
  auto has = [&](std::string_view name) {
    return std::ranges::any_of(values, [&](auto& v) { return name == v.extensionName; });
  };
  // Each requires its Vulkan 1.1 external-object extension on the device.
  if (has("VK_KHR_external_semaphore") && !has("VK_KHR_external_semaphore_fd"))
    values.push_back({"VK_KHR_external_semaphore_fd", VK_KHR_EXTERNAL_SEMAPHORE_FD_SPEC_VERSION});
  if (has("VK_KHR_external_fence") && !has("VK_KHR_external_fence_fd"))
    values.push_back({"VK_KHR_external_fence_fd", VK_KHR_EXTERNAL_FENCE_FD_SPEC_VERSION});
}
VkResult sync_fd_chain(VkBaseOutStructure* p) {
  // Host objects need no export capability: sync files wrap separate host fences.
  if (p->sType == VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO)
    reinterpret_cast<VkExportSemaphoreCreateInfo*>(p)->handleTypes &= ~VkExternalSemaphoreHandleTypeFlags(semaphore_sync);
  if (p->sType == VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO)
    reinterpret_cast<VkExportFenceCreateInfo*>(p)->handleTypes &= ~VkExternalFenceHandleTypeFlags(fence_sync);
  return VK_SUCCESS;
}
void sync_fd_submitted(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits) {
  queue_consumed(queue, [&](auto erase) {
    for (std::uint32_t i = 0; i < count; ++i)
      for (std::uint32_t w = 0; w < submits[i].waitSemaphoreCount; ++w) erase(submits[i].pWaitSemaphores[w]);
  });
}
void sync_fd_submitted(VkQueue queue, std::uint32_t count, const VkSubmitInfo2* submits) {
  queue_consumed(queue, [&](auto erase) {
    for (std::uint32_t i = 0; i < count; ++i)
      for (std::uint32_t w = 0; w < submits[i].waitSemaphoreInfoCount; ++w)
        erase(submits[i].pWaitSemaphoreInfos[w].semaphore);
  });
}
void release_sync_fd_device(VkDevice device) {
  {
    std::lock_guard lock(imports_lock);
    std::erase_if(imported, [&](const auto& entry) { return entry.first == device; });
    imported_count.store(imported.size(), std::memory_order_release);
  }
  std::vector<std::shared_ptr<SyncFile>> live;
  {
    std::lock_guard lock(files_lock);
    auto [begin, end] = files.equal_range(device);
    for (auto it = begin; it != end; ++it)
      if (auto file = it->second.file.lock()) live.push_back(std::move(file));
    files.erase(begin, end);
  }
  // Applications complete device work before vkDestroyDevice; descriptors that
  // outlive the device remain valid, already-signaled sync files.
  for (auto& file : live) file->retire();
}
SyncFdState sync_fd_state(VkDevice device) {
  std::lock_guard lock(files_lock);
  return {files.count(device), import_signals.load()};
}
PFN_vkVoidFunction sync_fd_proc(std::string_view name) {
#define FN(vk, fn) \
  if (name == #vk) return reinterpret_cast<PFN_vkVoidFunction>(fn)
  FN(vkGetSemaphoreFdKHR, get_semaphore_fd);
  FN(vkImportSemaphoreFdKHR, import_semaphore_fd);
  FN(vkGetFenceFdKHR, get_fence_fd);
  FN(vkImportFenceFdKHR, import_fence_fd);
  FN(vkQueuePresentKHR, queue_present);
  FN(vkQueueBindSparse, bind_sparse);
  FN(vkDestroySemaphore, destroy_semaphore);
  FN(vkGetPhysicalDeviceExternalSemaphoreProperties, semaphore_properties);
  FN(vkGetPhysicalDeviceExternalFenceProperties, fence_properties);
#undef FN
  return nullptr;
}
}  // namespace venture::vulkan
