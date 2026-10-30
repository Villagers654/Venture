// Only the host/transport and unrelated runtime services are scripted here.
// Passthrough lifecycle, validation, stereo translation, copies and shaders
// come from the production sources listed in CMakeLists.txt.
#include "support.h"
#include "hle/openxr/passthrough.h"
#include "xr/steam_link.h"
#include "xr/dev_dump.h"
#include "xr/queue.h"
#include "hle/vulkan/timing.h"
#include "core/process.h"
#include "common/log.h"
#include "android/framework_bridge.h"
#include "java/jvm.h"
#include <doctest/doctest.h>
#include <chrono>
#include <cstdio>

namespace venture {
const DeviceIdentity& native_device_identity() { static const DeviceIdentity identity{}; return identity; }
ProcessConfig& process_config() { static ProcessConfig config; return config; }
std::int64_t monotonic_ns() {
  return std::chrono::steady_clock::now().time_since_epoch().count();
}
namespace log {
Level level() { return Level::warn; }
void write(Level, std::string_view, std::string_view message) { std::fprintf(stderr, "%.*s\n", int(message.size()), message.data()); }
}
namespace java { JavaVM_* host_vm() { return nullptr; } }
namespace android { SurfaceBridge surface_bridge() { return {}; } }
namespace vulkan {
bool timing_enabled() { return false; }
std::uint64_t timing_frame() { return 0; }
void timing_xr_copy(const XrCopySample&) {}
void timing_host_wait(const char*, std::int64_t, std::int64_t) {}
void add_device_release_hook(void (*)(VkDevice)) {}
// Unused by these fixtures; State owns a nullable pointer to this service.
struct FullRateImage::Impl {};
FullRateImage::~FullRateImage() = default;
}
namespace gfx {
GpuDevice& gpu_device() {
  static GpuDevice gpu;
  static const bool initialized = [&] {
    gpu.get_instance_proc_addr = vkGetInstanceProcAddr;
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance.pApplicationInfo = &application;
    if (vkCreateInstance(&instance, nullptr, &gpu.instance) != VK_SUCCESS) throw std::runtime_error("No Vulkan driver for passthrough tests");
    std::uint32_t count{}; vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr);
    if (!count) throw std::runtime_error("No Vulkan GPU for passthrough tests");
    std::vector<VkPhysicalDevice> physical(count); vkEnumeratePhysicalDevices(gpu.instance, &count, physical.data());
    gpu.physical_device = physical.front();
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical_device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical_device, &count, families.data());
    while (gpu.queue_family < count && !(families[gpu.queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++gpu.queue_family;
    if (gpu.queue_family == count) throw std::runtime_error("No Vulkan graphics queue");
    float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = gpu.queue_family; queue.queueCount = 1; queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
    if (vkCreateDevice(gpu.physical_device, &device, nullptr, &gpu.device) != VK_SUCCESS) throw std::runtime_error("Cannot create Vulkan test device");
    vkGetDeviceQueue(gpu.device, gpu.queue_family, 0, &gpu.queue);
    return true;
  }();
  (void)initialized;
  return gpu;
}
std::shared_ptr<std::recursive_mutex> queue_mutex(VkQueue) { static auto mutex = std::make_shared<std::recursive_mutex>(); return mutex; }
}
namespace xr {
HostXr& host_xr() { static HostXr host; return host; }
std::uint64_t host_procedure_generation() { return 1; }
void cancel_sequence(XrSession, const XrHapticActionInfo*) {}
void clear_haptic_bindings(XrSession) {}
void forget_menu_object(std::uintptr_t) {}
Session& session() { static Session value; return value; }
void Session::finish_host_end() {}
XrImages* xr_images() { return nullptr; }
bool steam_link_passthrough_configured() { return test::passthrough::configured && process_config().steam_frame_passthrough && process_config().isa() == GuestIsa::a64; }
struct SteamLinkSource::Impl { std::uint64_t lease = 0; };
SteamLinkSource::SteamLinkSource() : impl_(std::make_unique<Impl>()) {}
SteamLinkSource::~SteamLinkSource() { test::passthrough::requested = false; }
void SteamLinkSource::request(bool active) {
  using namespace test::passthrough;
  if (active != requested) ++requests;
  requested = active;
  if (!active || !grant) impl_->lease = 0;
  else if (!impl_->lease) { static std::uint64_t serial = 0; impl_->lease = ++serial; }
}
std::uint64_t SteamLinkSource::permission() const { return test::passthrough::requested && test::passthrough::grant ? impl_->lease : 0; }
}
} // namespace venture

namespace {
struct HostChain {
  std::unique_ptr<venture::xr::GuestImages> backing;
  unsigned next = 0, released = 0;
  bool acquired = false, waited = false;
};
std::map<XrSwapchain, HostChain> host_chains;
XrResult XRAPI_CALL create_feature(XrSession, const XrPassthroughCreateInfoFB*, XrPassthroughFB* out) {
  ++venture::test::passthrough::native_creates; *out = reinterpret_cast<XrPassthroughFB>(0x300); return venture::test::passthrough::native_result;
}
XrResult XRAPI_CALL create_layer(XrSession, const XrPassthroughLayerCreateInfoFB*, XrPassthroughLayerFB* out) {
  *out = reinterpret_cast<XrPassthroughLayerFB>(0x400); return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_feature(XrPassthroughFB) { ++venture::test::passthrough::native_destroys; return XR_SUCCESS; }
XrResult XRAPI_CALL destroy_layer(XrPassthroughLayerFB) { ++venture::test::passthrough::native_destroys; return XR_SUCCESS; }
XrResult XRAPI_CALL feature_state(XrPassthroughFB) { return venture::test::passthrough::native_result; }
XrResult XRAPI_CALL layer_state(XrPassthroughLayerFB) { return venture::test::passthrough::native_result; }
XrResult XRAPI_CALL style(XrPassthroughLayerFB, const XrPassthroughStyleFB*) { return XR_SUCCESS; }
XrResult XRAPI_CALL resolve(XrInstance, const char* name, PFN_xrVoidFunction* out) {
  *out = nullptr;
#define ENTRY(n, f) if (std::strcmp(name, n) == 0) *out = reinterpret_cast<PFN_xrVoidFunction>(f)
  ENTRY("xrCreatePassthroughFB", create_feature);
  ENTRY("xrCreatePassthroughLayerFB", create_layer);
  ENTRY("xrDestroyPassthroughFB", destroy_feature);
  ENTRY("xrDestroyPassthroughLayerFB", destroy_layer);
  ENTRY("xrPassthroughStartFB", feature_state);
  ENTRY("xrPassthroughPauseFB", feature_state);
  ENTRY("xrPassthroughLayerPauseFB", layer_state);
  ENTRY("xrPassthroughLayerResumeFB", layer_state);
  ENTRY("xrPassthroughLayerSetStyleFB", style);
#undef ENTRY
  return *out ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
}
}
XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain handle, std::uint32_t capacity, std::uint32_t* count, XrSwapchainImageBaseHeader* out) {
  const auto images = host_chains.at(handle).backing->images(); *count = std::uint32_t(images.size());
  if (!capacity) return XR_SUCCESS;
  if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
  std::copy(images.begin(), images.end(), reinterpret_cast<XrSwapchainImageVulkanKHR*>(out)); return XR_SUCCESS;
}
XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain handle, const XrSwapchainImageAcquireInfo*, std::uint32_t* index) {
  auto& chain = host_chains.at(handle); CHECK_FALSE(chain.acquired); chain.acquired = true; chain.waited = false;
  *index = chain.next++ % chain.backing->images().size(); chain.released = *index; return XR_SUCCESS;
}
XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain handle, const XrSwapchainImageWaitInfo*) {
  auto& chain = host_chains.at(handle); CHECK(chain.acquired); chain.waited = true; return chain.backing->wait(chain.released);
}
namespace venture::xr {
XrResult dev_create_swapchain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* out) {
  auto backing = std::make_unique<GuestImages>(hle::openxr::state().graphics, vkGetInstanceProcAddr, *info);
  *out = reinterpret_cast<XrSwapchain>(backing.get()); host_chains[*out].backing = std::move(backing); return XR_SUCCESS;
}
XrResult create_host_swapchain(XrSession target, XrSwapchainCreateInfo& info, XrSwapchain* out) {
  return dev_create_swapchain(target, &info, out);
}
XrResult dev_destroy_swapchain(XrSwapchain handle) { host_chains.erase(handle); return XR_SUCCESS; }
XrResult dev_release_swapchain(XrSwapchain handle, const XrSwapchainImageReleaseInfo*, VkImage) {
  auto& chain = host_chains.at(handle); CHECK(chain.acquired); CHECK(chain.waited); chain.acquired = false; return XR_SUCCESS;
}
}
namespace venture::test::passthrough {
using namespace hle::openxr;
Fixture::Fixture() {
  configured = true; requested = grant = false; requests = native_creates = native_destroys = 0; native_result = XR_SUCCESS;
  process_config().abi = "arm64-v8a";
  process_config().steam_frame_passthrough = true;
  state().enabled = {"XR_FB_passthrough", "XR_FB_composition_layer_image_layout"};
  track_handle(target, "XrSession", reinterpret_cast<XrInstance>(1)); track_handle(space, "XrSpace", target);
  auto& gpu = gfx::gpu_device();
  state().graphics = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr, gpu.instance, gpu.physical_device, gpu.device, gpu.queue_family, 0};
  xr::session().queue = gpu.queue;
  xr::session().running = true; xr::session().phase = xr::Session::Phase::begun;
  xr::host_xr().enabled_extensions.clear(); xr::host_xr().available_extensions.clear();
  xr::host_xr().get_instance_proc_addr = reinterpret_cast<void*>(resolve);
}
Fixture::~Fixture() {
  CHECK(destroy_passthrough_session(target) == XR_SUCCESS);
  state().swapchains.clear(); state().objects.clear(); state().enabled.clear();
  CHECK(host_chains.empty());
}
void Fixture::create(bool native) {
  if (native) {
    xr::host_xr().enabled_extensions.insert("XR_FB_passthrough");
    xr::host_xr().available_extensions["XR_FB_passthrough"] = XR_FB_passthrough_SPEC_VERSION;
  }
  XrPassthroughCreateInfoFB info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB, nullptr, XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB};
  REQUIRE(guest::xrCreatePassthroughFB(target, &info, &feature) == XR_SUCCESS);
  XrPassthroughLayerCreateInfoFB li{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB, nullptr, feature,
      XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB, XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB};
  REQUIRE(guest::xrCreatePassthroughLayerFB(target, &li, &layer) == XR_SUCCESS);
}
XrSwapchain Fixture::chain(std::uint32_t layers, VkFormat format) {
  auto backing = std::make_unique<xr::GuestImages>(state().graphics, vkGetInstanceProcAddr,
      XrSwapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO, nullptr, 0,
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
        format, 1, 4, 4, 1, layers, 1});
  auto handle = reinterpret_cast<XrSwapchain>(backing.get());
  auto& chain = state().swapchains[handle];
  chain.info = {XR_TYPE_SWAPCHAIN_CREATE_INFO, nullptr, 0,
      XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
      format, 1, 4, 4, 1, layers, 1};
  chain.images.assign(backing->images().begin(), backing->images().end());
  chain.backing = std::move(backing); chain.released_index = 0; chain.release_serial = 1;
  track_handle(handle, "XrSwapchain", target); return handle;
}
void commands(const std::function<void(VkCommandBuffer)>& record) {
  auto& gpu = gfx::gpu_device();
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex = gpu.queue_family;
  VkCommandPool pool{}; REQUIRE(vkCreateCommandPool(gpu.device, &pi, nullptr, &pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer command{}; REQUIRE(vkAllocateCommandBuffers(gpu.device, &ai, &command) == VK_SUCCESS);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; REQUIRE(vkBeginCommandBuffer(command, &bi) == VK_SUCCESS);
  record(command); REQUIRE(vkEndCommandBuffer(command) == VK_SUCCESS);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
  REQUIRE(vkQueueSubmit(gpu.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS); REQUIRE(vkQueueWaitIdle(gpu.queue) == VK_SUCCESS);
  vkDestroyCommandPool(gpu.device, pool, nullptr);
}
static void barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to, std::uint32_t layers) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = from; barrier.newLayout = to; barrier.srcAccessMask = barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
void fill(XrSwapchain handle, std::array<std::array<float, 4>, 2> colors) {
  auto& chain = state().swapchains.at(handle); REQUIRE(chain.backing->wait(chain.released_index) == XR_SUCCESS);
  commands([&](VkCommandBuffer command) {
    const auto image = chain.images[chain.released_index].image;
    barrier(command, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, chain.info.arraySize);
    for (unsigned layer = 0; layer < chain.info.arraySize; ++layer) {
      VkClearColorValue value{}; std::copy(colors.at(layer).begin(), colors.at(layer).end(), value.float32);
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
      vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    }
    barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, chain.info.arraySize);
  });
}
std::array<unsigned char, 4> pixel(XrSwapchain handle, std::uint32_t layer, std::uint32_t row) {
  auto& gpu = gfx::gpu_device();
  VkImage image{}; std::uint32_t layers = 1;
  if (host_chains.contains(handle)) {
    auto& chain = host_chains.at(handle); image = chain.backing->images()[chain.released].image;
  } else { const auto& chain = state().swapchains.at(handle); image = chain.images[chain.released_index].image; layers = chain.info.arraySize; }
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = 4; ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer{}; REQUIRE(vkCreateBuffer(gpu.device, &ci, nullptr, &buffer) == VK_SUCCESS);
  VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(gpu.device, buffer, &requirements);
  VkPhysicalDeviceMemoryProperties properties{}; vkGetPhysicalDeviceMemoryProperties(gpu.physical_device, &properties);
  std::uint32_t type = 0;
  constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  while (type < properties.memoryTypeCount && (!(requirements.memoryTypeBits & (1u << type)) ||
         (properties.memoryTypes[type].propertyFlags & flags) != flags)) ++type;
  REQUIRE(type < properties.memoryTypeCount);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
  VkDeviceMemory memory{}; REQUIRE(vkAllocateMemory(gpu.device, &ai, nullptr, &memory) == VK_SUCCESS);
  REQUIRE(vkBindBufferMemory(gpu.device, buffer, memory, 0) == VK_SUCCESS);
  commands([&](VkCommandBuffer command) {
    barrier(command, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layers);
    VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1}; copy.imageOffset.y = row; copy.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, layers);
  });
  void* mapped{}; REQUIRE(vkMapMemory(gpu.device, memory, 0, 4, 0, &mapped) == VK_SUCCESS);
  std::array<unsigned char, 4> result{}; std::memcpy(result.data(), mapped, 4);
  vkUnmapMemory(gpu.device, memory); vkDestroyBuffer(gpu.device, buffer, nullptr); vkFreeMemory(gpu.device, memory, nullptr);
  return result;
}
} // namespace venture::test::passthrough
