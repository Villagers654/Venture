#include "xr/system_keyboard.h"
#include "common/log.h"
#include "java/jvm.h"
#include "xr/dev_dump.h"
#include "xr/haptic_output.h"
#include "xr/haptic_sequence.h"
#include "xr/host_xr.h"
#include "xr/math.h"
#include "xr/queue.h"
#include "xr/reference_spaces.h"
#include "xr/session.h"
#include <jni.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace venture::xr::keyboard {
namespace {
constexpr float kWidth = 0.6f;                     // meters across
constexpr XrVector3f kOffset{0, -0.3f, -0.55f};    // from the head, along its heading
constexpr float kPress = 0.55f, kRelease = 0.35f;  // trigger, with hysteresis
constexpr std::uint32_t kCursor = 32;              // cursor picture, pixels
constexpr float kCursorSize = 0.014f;              // meters

struct Hand {
  int hover = -1, pressed = -1;
  bool down = true;  // a trigger held as the keyboard opens must release before it presses
  std::optional<Hit> at;
};
// Shared with Java, the input sync and the mirror.
struct Shared {
  std::mutex mutex;
  bool open = false, placed = false;
  std::vector<std::uint32_t> pixels;
  std::uint32_t width = 0, height = 0;
  std::uint64_t generation = 0;
  std::vector<Rect> keys;
  std::int32_t layout = 0;
  std::vector<std::int32_t> events;
  std::array<Hand, 2> hands;
  XrSession session{};
  XrSpace local{};  // upright LOCAL: the panel stays where it opened
  XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
  XrExtent2Df size{};
};
Shared &shared() {
  static Shared value;
  return value;
}
void queue(Shared &s, Event kind, std::int32_t a, std::int32_t b) {
  // A keyboard nobody reads (no main Looper) must not grow without bound.
  if (s.events.size() >= 4096) return;
  s.events.insert(s.events.end(), {std::int32_t(kind), a, b, s.layout});
}

// Frame-thread state, under the session lock.
struct Image {
  XrSwapchain chain{};
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  std::vector<XrSwapchainImageVulkanKHR> images;
  std::uint32_t index = 0;
  bool acquired = false, waited = false, content = false;
};
struct Render {
  XrSession session{};
  bool failed = false;
  XrSpace view{};
  std::uint32_t layer_limit = 0;
  Image panel, cursor;
  std::uint64_t uploaded = 0;
  XrGraphicsBindingVulkanKHR binding{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  bool in_flight = false;
  VkBuffer staging{};
  VkDeviceMemory memory{};
  void *mapped = nullptr;
  bool coherent = false;
  VkDeviceSize capacity = 0;
};
Render &render() {
  static Render value;
  return value;
}
template <class F> F vk(const char *name) {
  return reinterpret_cast<F>(gfx::gpu_device().get_instance_proc_addr(render().binding.instance, name));
}
bool checked(VkResult result, const char *what) {
  if (result == VK_SUCCESS) return true;
  log::warn("keyboard", "{} failed: VkResult={}", what, int(result));
  return false;
}
void destroy_image(Image &image) {
  if (image.chain) {
    if (image.acquired) {
      if (!image.waited) {
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
        ::xrWaitSwapchainImage(image.chain, &wait);
      }
      XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      ::xrReleaseSwapchainImage(image.chain, &release);
    }
    dev_destroy_swapchain(image.chain);
  }
  image = {};
}
// The caller holds the session lock and the queue lock, and no host End is in flight.
void destroy_render() {
  auto &r = render();
  if (r.binding.device) {
    if (r.in_flight) vk<PFN_vkWaitForFences>("vkWaitForFences")(r.binding.device, 1, &r.fence, VK_TRUE, 1'000'000'000);
    if (r.mapped) vk<PFN_vkUnmapMemory>("vkUnmapMemory")(r.binding.device, r.memory);
    if (r.staging) vk<PFN_vkDestroyBuffer>("vkDestroyBuffer")(r.binding.device, r.staging, nullptr);
    if (r.memory) vk<PFN_vkFreeMemory>("vkFreeMemory")(r.binding.device, r.memory, nullptr);
    if (r.fence) vk<PFN_vkDestroyFence>("vkDestroyFence")(r.binding.device, r.fence, nullptr);
    if (r.pool) vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(r.binding.device, r.pool, nullptr);
  }
  destroy_image(r.panel);
  destroy_image(r.cursor);
  if (r.view) ::xrDestroySpace(r.view);
  r = {};
}
// After a failure the session goes on without the panel (Java and the mirror
// still type); what was made goes with the session (reset_session), as the
// caller may hold the queue lock here.
void fail(const char *what) {
  log::warn("keyboard", "The keyboard panel is unavailable this session: {}", what);
  render().failed = true;
}
bool create_gpu(VkDeviceSize bytes) {
  auto &r = render();
  r.binding = xr::session().binding;
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = r.binding.queueFamilyIndex;
  if (!checked(vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(r.binding.device, &pci, nullptr, &r.pool),
               "vkCreateCommandPool"))
    return false;
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, r.pool,
                                  VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  if (!checked(vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(r.binding.device, &cai, &r.command),
               "vkAllocateCommandBuffers"))
    return false;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (!checked(vk<PFN_vkCreateFence>("vkCreateFence")(r.binding.device, &fci, nullptr, &r.fence), "vkCreateFence"))
    return false;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (!checked(vk<PFN_vkCreateBuffer>("vkCreateBuffer")(r.binding.device, &bci, nullptr, &r.staging), "vkCreateBuffer"))
    return false;
  VkMemoryRequirements requirements{};
  vk<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(r.binding.device, r.staging, &requirements);
  VkPhysicalDeviceMemoryProperties properties{};
  vk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(r.binding.physicalDevice,
                                                                                    &properties);
  std::uint32_t type = properties.memoryTypeCount;
  for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
    const auto flags = properties.memoryTypes[i].propertyFlags;
    if ((requirements.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      type = i;
      if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
    }
  }
  if (type == properties.memoryTypeCount) return false;
  r.coherent = properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, type};
  if (!checked(vk<PFN_vkAllocateMemory>("vkAllocateMemory")(r.binding.device, &mai, nullptr, &r.memory),
               "vkAllocateMemory") ||
      !checked(vk<PFN_vkBindBufferMemory>("vkBindBufferMemory")(r.binding.device, r.staging, r.memory, 0),
               "vkBindBufferMemory") ||
      !checked(vk<PFN_vkMapMemory>("vkMapMemory")(r.binding.device, r.memory, 0, VK_WHOLE_SIZE, 0, &r.mapped),
               "vkMapMemory"))
    return false;
  r.capacity = bytes;
  return true;
}
bool create_image(XrSession session, Image &image, std::uint32_t width, std::uint32_t height) {
  image.info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
  image.info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                          XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  // sRGB first: the picture's bytes are sRGB, so they go in unchanged; a host
  // without 8-bit sRGB gets linear values (create_host_swapchain's fallback).
  image.info.format = VK_FORMAT_R8G8B8A8_SRGB;
  image.info.sampleCount = image.info.faceCount = image.info.arraySize = image.info.mipCount = 1;
  image.info.width = width;
  image.info.height = height;
  XrResult result;
  {
    HostQueueLock queue(xr::session().uses_shared_gpu);
    if (!queue) return false;
    result = create_host_swapchain(session, image.info, &image.chain);
  }
  if (XR_FAILED(result)) return false;
  std::uint32_t count = 0;
  if (XR_FAILED(::xrEnumerateSwapchainImages(image.chain, 0, &count, nullptr)) || !count || count > 32) return false;
  image.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  return XR_SUCCEEDED(::xrEnumerateSwapchainImages(image.chain, count, &count,
      reinterpret_cast<XrSwapchainImageBaseHeader *>(image.images.data())));
}
// Acquired and waited for: ready to write. False to try again next frame; a
// failure is reported through `failed`.
bool ready(Image &image, bool &failed) {
  if (!image.acquired) {
    HostQueueLock queue(xr::session().uses_shared_gpu);
    if (!queue || XR_FAILED(::xrAcquireSwapchainImage(image.chain, nullptr, &image.index)) ||
        image.index >= image.images.size())
      return !(failed = true);
    image.acquired = true;
    image.waited = false;
  }
  if (!image.waited) {
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, 0};
    const auto result = ::xrWaitSwapchainImage(image.chain, &wait);
    if (result == XR_TIMEOUT_EXPIRED) return false;
    if (XR_FAILED(result)) return !(failed = true);
    image.waited = true;
  }
  return true;
}
void record_copy(Image &image, VkDeviceSize offset) {
  auto &r = render();
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // the whole image is written
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.images[image.index].image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(r.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy copy{};
  copy.bufferOffset = offset;
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {image.info.width, image.info.height, 1};
  vk<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(r.command, r.staging, barrier.image,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  // OpenXR: a released Vulkan color image is in COLOR_ATTACHMENT_OPTIMAL.
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(r.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
// A white dot with a dark rim, premultiplied.
void cursor_pixels(std::uint32_t *out) {
  const float c = (kCursor - 1) / 2.f;
  for (std::uint32_t y = 0; y < kCursor; ++y)
    for (std::uint32_t x = 0; x < kCursor; ++x) {
      const float d = std::hypot(x - c, y - c);
      const float alpha = std::clamp(c - d + 0.5f, 0.f, 1.f);
      const float white = std::clamp(c - 3.f - d + 0.5f, 0.f, 1.f);
      const auto a = std::uint32_t(std::lround(alpha * 255)), v = std::uint32_t(std::lround(white * alpha * 255));
      out[y * kCursor + x] = (a << 24) | (v << 16) | (v << 8) | v;
    }
}
// Uploads a new picture (and the cursor once). Never waits: a copy still in
// flight or a swapchain image not yet free leaves it for a later frame.
void upload(XrSession session) {
  auto &r = render();
  std::uint32_t width, height;
  std::uint64_t generation;
  {
    auto &s = shared();
    std::lock_guard lock(s.mutex);
    width = s.width;
    height = s.height;
    generation = s.generation;
  }
  if (!width || !height) return;
  if (r.panel.chain && (r.panel.info.width != width || r.panel.info.height != height)) {
    HostQueueLock queue(xr::session().uses_shared_gpu);
    if (r.in_flight)
      vk<PFN_vkWaitForFences>("vkWaitForFences")(r.binding.device, 1, &r.fence, VK_TRUE, 1'000'000'000);
    r.in_flight = false;
    destroy_image(r.panel);
  }
  const VkDeviceSize bytes = VkDeviceSize(width) * height * 4 + kCursor * kCursor * 4;
  if (r.capacity < bytes) {
    if (r.binding.device) return fail("the picture grew");  // Java draws one fixed size
    if (!create_gpu(bytes)) return fail("staging memory");
  }
  if (!r.panel.chain && !create_image(session, r.panel, width, height)) return fail("panel swapchain");
  if (!r.cursor.chain && !create_image(session, r.cursor, kCursor, kCursor)) return fail("cursor swapchain");
  const bool picture = r.uploaded != generation, dot = !r.cursor.content;
  if (!picture && !dot) return;
  if (r.in_flight) {
    if (vk<PFN_vkGetFenceStatus>("vkGetFenceStatus")(r.binding.device, r.fence) != VK_SUCCESS) return;
    r.in_flight = false;
  }
  bool failed = false;
  const bool panel = picture && ready(r.panel, failed);
  const bool cursor = dot && !failed && ready(r.cursor, failed);
  if (failed) return fail("swapchain image");
  if (!panel && !cursor) return;
  auto *staging = static_cast<std::uint8_t *>(r.mapped);
  const VkDeviceSize cursor_offset = VkDeviceSize(width) * height * 4;
  if (panel) {
    auto &s = shared();
    std::lock_guard lock(s.mutex);
    if (s.width != width || s.height != height || s.pixels.size() != std::size_t(width) * height) return;
    convert(s.pixels.data(), s.pixels.size(), r.panel.info.format, staging);
    generation = s.generation;
  }
  if (cursor) {
    std::vector<std::uint32_t> dot_pixels(kCursor * kCursor);
    cursor_pixels(dot_pixels.data());
    convert(dot_pixels.data(), dot_pixels.size(), r.cursor.info.format, staging + cursor_offset);
  }
  if (!r.coherent) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, r.memory, 0, VK_WHOLE_SIZE};
    vk<PFN_vkFlushMappedMemoryRanges>("vkFlushMappedMemoryRanges")(r.binding.device, 1, &range);
  }
  HostQueueLock queue(xr::session().uses_shared_gpu);
  if (!queue) return fail("queue lock");
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (!checked(vk<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(r.command, 0), "vkResetCommandBuffer") ||
      !checked(vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(r.command, &begin), "vkBeginCommandBuffer"))
    return fail("command buffer");
  if (panel) record_copy(r.panel, 0);
  if (cursor) record_copy(r.cursor, cursor_offset);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &r.command;
  if (!checked(vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(r.command), "vkEndCommandBuffer") ||
      !checked(vk<PFN_vkResetFences>("vkResetFences")(r.binding.device, 1, &r.fence), "vkResetFences") ||
      !checked(vk<PFN_vkQueueSubmit>("vkQueueSubmit")(xr::session().queue, 1, &submit, r.fence), "vkQueueSubmit"))
    return fail("upload");
  r.in_flight = true;
  // The copy is queued ahead of the release on the binding's queue, as OpenXR
  // requires; the runtime waits for it, not this thread.
  for (auto *image : {panel ? &r.panel : nullptr, cursor ? &r.cursor : nullptr}) {
    if (!image) continue;
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(dev_release_swapchain(image->chain, &release, image->images[image->index].image)))
      return fail("swapchain release");
    image->acquired = image->waited = false;
    image->content = true;
  }
  if (panel) r.uploaded = generation;
}
std::uint32_t host_layer_limit() {
  XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
  auto &host = host_xr();
  if (host.available && XR_SUCCEEDED(::xrGetSystemProperties(host.instance, host.system, &properties)))
    return properties.graphicsProperties.maxLayerCount;
  return 0;
}
bool valid(const XrSpaceLocation &location) {
  constexpr auto both = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  return (location.locationFlags & both) == both;
}

// ---- Java natives ----
void JNICALL java_show(JNIEnv *env, jclass, jintArray pixels, jint width, jint height, jintArray keys, jint layout) {
  try {
    if (!pixels || !keys || width <= 0 || height <= 0 || width > 4096 || height > 4096) return;
    const auto count = std::size_t(width) * std::size_t(height);
    if (std::size_t(env->GetArrayLength(pixels)) != count) return;
    std::vector<std::uint32_t> argb(count);
    env->GetIntArrayRegion(pixels, 0, jsize(count), reinterpret_cast<jint *>(argb.data()));
    static_assert(sizeof(Rect) == 4 * sizeof(jint));
    std::vector<Rect> rects(std::size_t(env->GetArrayLength(keys) / 4));
    env->GetIntArrayRegion(keys, 0, jsize(rects.size() * 4), reinterpret_cast<jint *>(rects.data()));
    if (env->ExceptionCheck()) return;
    show(std::move(argb), std::uint32_t(width), std::uint32_t(height), std::move(rects), layout);
  } catch (const std::bad_alloc &) {
    log::warn("keyboard", "No memory for the keyboard picture");
  }
}
void JNICALL java_hide(JNIEnv *, jclass) { hide(); }
jintArray JNICALL java_events(JNIEnv *env, jclass) {
  try {
    const auto events = take_events();
    if (events.empty()) return nullptr;
    auto out = env->NewIntArray(jsize(events.size()));
    if (out) env->SetIntArrayRegion(out, 0, jsize(events.size()), events.data());
    return out;
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
}
void register_natives(JNIEnv *env) {
  auto cls = env->FindClass("com/venture/runtime/SystemKeyboard");
  if (!cls) {  // runs without venture-framework.jar (guest tests, NDK fixtures)
    env->ExceptionClear();
    return;
  }
  JNINativeMethod methods[] = {
      {const_cast<char *>("nativeShow"), const_cast<char *>("([III[II)V"), reinterpret_cast<void *>(java_show)},
      {const_cast<char *>("nativeHide"), const_cast<char *>("()V"), reinterpret_cast<void *>(java_hide)},
      {const_cast<char *>("nativeEvents"), const_cast<char *>("()[I"), reinterpret_cast<void *>(java_events)},
  };
  if (env->RegisterNatives(cls, methods, jint(std::size(methods))) != JNI_OK) {
    env->ExceptionClear();
    log::warn("keyboard", "System keyboard natives were not registered");
  }
  env->DeleteLocalRef(cls);
}
} // namespace

void install_java_bridge() {
  static std::once_flag once;
  std::call_once(once, [] {
    try {
      java::add_framework_natives(register_natives);
    } catch (const std::exception &e) {
      log::warn("keyboard", "System keyboard unavailable: {}", e.what());
    }
  });
}

void show(std::vector<std::uint32_t> argb, std::uint32_t width, std::uint32_t height, std::vector<Rect> keys,
          std::int32_t layout) {
  auto &s = shared();
  std::lock_guard lock(s.mutex);
  if (!s.open) {
    s.open = true;
    s.placed = false;  // opens in front of the player, wherever they look now
    s.events.clear();
    s.hands = {};
    log::info("keyboard", "Shown");
  }
  s.pixels = std::move(argb);
  s.width = width;
  s.height = height;
  ++s.generation;
  if (layout != s.layout)
    for (auto &hand : s.hands) hand.hover = -1;  // the next pointer update reports the new layout's key
  s.keys = std::move(keys);
  s.layout = layout;
  detail::shown.store(true, std::memory_order_relaxed);
}
void hide() {
  auto &s = shared();
  std::lock_guard lock(s.mutex);
  if (!s.open) return;
  s.open = false;
  s.events.clear();
  s.pixels = {};
  ++s.generation;
  detail::shown.store(false, std::memory_order_relaxed);
  log::info("keyboard", "Hidden");
}
std::vector<std::int32_t> take_events() {
  auto &s = shared();
  std::lock_guard lock(s.mutex);
  return std::exchange(s.events, {});
}

std::shared_ptr<const Frame> extend(XrSession session, const XrFrameEndInfo &info, std::shared_ptr<const void> game) {
  auto &r = render();
  if (r.session != session) {
    if (r.session) {
      HostQueueLock queue(xr::session().uses_shared_gpu);
      reset_session(r.session);
    }
    r.session = session;
    r.layer_limit = host_layer_limit();
  }
  if (r.failed) return nullptr;
  if (!r.view) {
    XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr, XR_REFERENCE_SPACE_TYPE_VIEW,
                                  {{0, 0, 0, 1}, {0, 0, 0}}};
    if (XR_FAILED(::xrCreateReferenceSpace(session, &ci, &r.view))) {
      fail("view space");
      return nullptr;
    }
  }
  XrPosef pose;
  XrExtent2Df size;
  XrSpace local;
  std::array<std::optional<Hit>, 2> cursors;
  std::uint32_t width;
  {
    auto &s = shared();
    std::lock_guard lock(s.mutex);
    if (!s.open || !s.width) return nullptr;
    if (s.session != session) {
      if (s.local) ::xrDestroySpace(s.local);
      s.local = {};
      s.session = session;
      s.placed = false;
    }
    if (!s.local) {
      auto ci = upright_reference_space(session, {XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
                                                  XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}});
      if (XR_FAILED(::xrCreateReferenceSpace(session, &ci, &s.local))) return nullptr;
    }
    if (!s.placed) {
      XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
      if (XR_FAILED(::xrLocateSpace(r.view, s.local, info.displayTime, &head)) || !valid(head)) return nullptr;
      s.pose = panel_pose(head.pose);
      s.size = {kWidth, kWidth * float(s.height) / float(s.width)};
      s.placed = true;
    }
    pose = s.pose;
    size = s.size;
    local = s.local;
    width = s.width;
    for (int h = 0; h < 2; ++h) cursors[std::size_t(h)] = s.hands[std::size_t(h)].at;
  }
  upload(session);
  if (r.failed || !r.panel.content) return nullptr;
  auto frame = std::make_shared<Frame>();
  frame->game = std::move(game);
  frame->layers.assign(info.layers, info.layers + info.layerCount);
  std::uint32_t wanted = 1;
  for (const auto &at : cursors) wanted += at && r.cursor.content;
  const auto room = fit_layers(frame->layers, r.layer_limit, wanted);
  if (!room) return nullptr;
  auto quad = [&](XrSwapchain chain, const XrSwapchainCreateInfo &ci, XrPosef at, XrExtent2Df extent) {
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    // Premultiplied alpha: the picture is drawn that way.
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = local;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage = {chain, {{0, 0}, {std::int32_t(ci.width), std::int32_t(ci.height)}}, 0};
    layer.pose = at;
    layer.size = extent;
    return layer;
  };
  std::size_t used = 0;
  frame->quads[used++] = quad(r.panel.chain, r.panel.info, pose, size);
  const float meters = size.width / float(width);
  for (const auto &at : cursors) {
    if (used >= room || !at || !r.cursor.content) continue;
    // Just in front of the panel, at the hit, so the dot sits on the key.
    const XrPosef offset{{0, 0, 0, 1}, {(at->x - float(width) / 2) * meters,
                                        (float(r.panel.info.height) / 2 - at->y) * meters, 0.002f}};
    frame->quads[used++] = quad(r.cursor.chain, r.cursor.info, compose(pose, offset), {kCursorSize, kCursorSize});
  }
  for (std::size_t i = 0; i < used; ++i)
    frame->layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&frame->quads[i]));
  frame->info = info;
  frame->info.layerCount = std::uint32_t(frame->layers.size());
  frame->info.layers = frame->layers.data();
  return frame;
}

void reset_session(XrSession session) {
  auto &r = render();
  if (r.session == session) destroy_render();
  auto &s = shared();
  std::lock_guard lock(s.mutex);
  if (s.session != session) return;
  if (s.local) ::xrDestroySpace(s.local);
  s.local = {};
  s.session = {};
  s.placed = false;
  for (auto &hand : s.hands) hand.at.reset();
}

bool point(int hand, XrSpace aim, float trigger, XrTime time) {
  if (hand < 0 || hand > 1) return false;
  auto &s = shared();
  std::lock_guard lock(s.mutex);
  auto &h = s.hands[std::size_t(hand)];
  if (!s.open) return false;
  std::optional<Hit> at;
  if (s.placed && s.local && aim) {
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(::xrLocateSpace(aim, s.local, time, &location)) && valid(location))
      at = hit(s.pose, s.size, location.pose, s.width, s.height);
  }
  h.at = at;
  const int key = at ? key_at(s.keys, at->x, at->y) : -1;
  if (key != h.hover) {
    h.hover = key;
    queue(s, Event::hover, hand, key);
  }
  const bool down = h.down ? trigger > kRelease : trigger > kPress;
  bool pressed = false;
  if (down && !h.down) {
    h.pressed = key;
    if (key >= 0) {
      queue(s, Event::press, hand, key);
      pressed = true;
    }
  } else if (!down && h.down && h.pressed >= 0) {
    queue(s, Event::release, hand, h.pressed);
    h.pressed = -1;
  }
  h.down = down;
  return pressed;
}
std::optional<float> hit_distance(int hand) {
  if (hand < 0 || hand > 1) return std::nullopt;
  auto &s = shared();
  std::lock_guard lock(s.mutex);
  const auto &at = s.hands[std::size_t(hand)].at;
  if (!s.open || !at) return std::nullopt;
  return at->distance;
}
void tick(std::string_view api, XrSession session, XrAction haptic, XrPath hand) {
  if (!haptic || !hand) return;
  const XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, haptic, hand};
  const XrHapticVibration pulse{XR_TYPE_HAPTIC_VIBRATION, nullptr, 15'000'000, XR_FREQUENCY_UNSPECIFIED, 0.35f};
  queue_vibration(api, session, info, pulse, "keyboard_key");
}

void type_text(std::string_view utf8) {
  auto &s = shared();
  std::lock_guard lock(s.mutex);
  if (!s.open) return;
  for (std::size_t i = 0; i < utf8.size();) {
    const auto c = static_cast<unsigned char>(utf8[i]);
    const int length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!length || i + std::size_t(length) > utf8.size()) break;  // SDL hands over valid UTF-8
    std::int32_t code = length == 1 ? c : c & (0x7f >> length);
    for (int k = 1; k < length; ++k) code = (code << 6) | (static_cast<unsigned char>(utf8[i + std::size_t(k)]) & 0x3f);
    queue(s, Event::text, code, 0);
    i += std::size_t(length);
  }
}
void type_key(std::int32_t android_keycode) {
  auto &s = shared();
  std::lock_guard lock(s.mutex);
  if (s.open) queue(s, Event::key, android_keycode, 0);
}

XrPosef panel_pose(XrPosef head) {
  const auto heading = yaw_only(head.orientation);
  const auto at = rotate(heading, kOffset);
  // Tilted back about its horizontal axis (its top away from the player) to
  // face the eyes above it: a negative turn about +X lifts its front (+Z).
  const float half = std::atan2(-kOffset.y, -kOffset.z) / 2;
  const XrQuaternionf back{-std::sin(half), 0, 0, std::cos(half)};
  return {multiply(heading, back), {head.position.x + at.x, head.position.y + at.y, head.position.z + at.z}};
}
std::optional<Hit> hit(XrPosef panel, XrExtent2Df size, XrPosef aim, std::uint32_t width, std::uint32_t height) {
  const auto to_panel = inverse(panel);
  const auto origin = compose(to_panel, aim).position;
  const auto direction = rotate(multiply(to_panel.orientation, aim.orientation), {0, 0, -1});
  // The front faces +Z: the ray must come from in front and travel toward it.
  if (origin.z <= 0 || direction.z >= -1e-6f) return std::nullopt;
  const float t = -origin.z / direction.z;
  const float x = origin.x + direction.x * t, y = origin.y + direction.y * t;
  if (std::abs(x) > size.width / 2 || std::abs(y) > size.height / 2) return std::nullopt;
  return Hit{(x / size.width + 0.5f) * float(width), (0.5f - y / size.height) * float(height), t};
}
int key_at(const std::vector<Rect> &keys, float x, float y) {
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const auto &k = keys[i];
    if (x >= float(k.x) && y >= float(k.y) && x < float(k.x + k.w) && y < float(k.y + k.h)) return int(i);
  }
  return -1;
}
void convert(const std::uint32_t *argb, std::size_t count, std::int64_t format, std::uint8_t *out) {
  const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
  const bool linear = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM;
  static const auto to_linear = [] {
    std::array<float, 256> table{};
    for (int i = 0; i < 256; ++i) {
      const float c = float(i) / 255.f;
      table[std::size_t(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    return table;
  }();
  for (std::size_t i = 0; i < count; ++i) {
    const auto p = argb[i];
    std::uint8_t a = std::uint8_t(p >> 24), r = std::uint8_t(p >> 16), g = std::uint8_t(p >> 8), b = std::uint8_t(p);
    if (linear && a) {
      // Premultiplied sRGB to premultiplied linear, through the straight colour.
      auto channel = [&](std::uint8_t v) {
        const auto straight = std::min(255, (int(v) * 255 + a / 2) / a);
        return std::uint8_t(std::lround(to_linear[std::size_t(straight)] * float(a)));
      };
      r = channel(r);
      g = channel(g);
      b = channel(b);
    }
    auto *o = out + i * 4;
    o[0] = bgra ? b : r;
    o[1] = g;
    o[2] = bgra ? r : b;
    o[3] = a;
  }
}
std::uint32_t fit_layers(std::vector<const XrCompositionLayerBaseHeader *> &layers, std::uint32_t limit,
                         std::uint32_t wanted) {
  if (!limit) return wanted;
  while (layers.size() + wanted > limit) {
    const auto last = std::find_if(layers.rbegin(), layers.rend(), [](const XrCompositionLayerBaseHeader *layer) {
      return layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    });
    if (last == layers.rend()) break;
    layers.erase(std::next(last).base());
  }
  return layers.size() >= limit ? 0 : std::min<std::uint32_t>(wanted, limit - std::uint32_t(layers.size()));
}
} // namespace venture::xr::keyboard
