// Zero-copy eye chains (docs/design/zero-copy-eyes.md): a guest chain that can
// be the host's gets the host runtime's images and is presented without a copy.
#include "../mock_openxr_runtime/control.h"
#include "common/environment.h"
#include "gfx/gpu_device.h"
#include "hle/openxr/openxr.h"
#include "hle/openxr/state.h"
#include "hle/vulkan/internal.h"
#include "xr/capture.h"
#include "xr/guest_images.h"
#include "xr/host_xr.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <doctest/doctest.h>
#include "xr_zero_copy_setting.h"
#include <string>
#include <vector>

namespace {
using venture::test::ZeroCopySetting;

// One guest session on the mock runtime, presenting a two-slice chain as a
// projection or quad layer, each slice cleared to its own color.
struct ZeroCopyScene {
  XrInstance instance{};
  XrSession session{};
  XrSpace space{};
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  PFN_vkGetInstanceProcAddr get{};
  VkCommandPool pool{};
  template <class T> T fn(const char* name) { return reinterpret_cast<T>(get(binding.instance, name)); }
  // `color_space`: the game enables XR_FB_color_space.
  explicit ZeroCopyScene(bool color_space = false) {
    namespace g = venture::hle::openxr::guest;
    const char* names[]{"XR_KHR_vulkan_enable2", "XR_FB_composition_layer_image_layout", "XR_FB_color_space"};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = color_space ? 3 : 2;
    ci.enabledExtensionNames = names;
    REQUIRE(g::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
    auto& gpu = venture::gfx::gpu_device();
    binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, nullptr, gpu.instance, gpu.physical_device, gpu.device,
               gpu.queue_family, gpu.queue_index};
    get = gpu.get_instance_proc_addr;
    XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO, &binding, 0, venture::xr::host_xr().system};
    REQUIRE(g::xrCreateSession(instance, &sc, &session) == XR_SUCCESS);
    XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO, nullptr,
                                         XR_REFERENCE_SPACE_TYPE_LOCAL, {{0, 0, 0, 1}, {0, 0, 0}}};
    REQUIRE(g::xrCreateReferenceSpace(session, &reference, &space) == XR_SUCCESS);
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (g::xrPollEvent(instance, &event) == XR_SUCCESS) event = {XR_TYPE_EVENT_DATA_BUFFER};
    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    REQUIRE(g::xrBeginSession(session, &begin) == XR_SUCCESS);
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.queueFamilyIndex = gpu.queue_family;
    REQUIRE(fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(gpu.device, &cp, nullptr, &pool) == VK_SUCCESS);
  }
  ~ZeroCopyScene() {
    namespace g = venture::hle::openxr::guest;
    fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(binding.device, pool, nullptr);
    CHECK(g::xrDestroySession(session) == XR_SUCCESS);
    // Every host chain, parked or not, went with the session.
    CHECK(ventureMockXrStats().live_swapchains == 0);
    CHECK(g::xrDestroyInstance(instance) == XR_SUCCESS);
  }
  static XrSwapchainCreateInfo info() {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.format = VK_FORMAT_R8G8B8A8_SRGB;
    info.width = info.height = 8;
    info.sampleCount = info.faceCount = info.mipCount = 1;
    info.arraySize = 2;
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    return info;
  }
  // The game's rendering: slice l of `image` cleared to (seed, .25 + .5 l, .6, .8).
  void render(VkImage image, float seed) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer command{};
    REQUIRE(fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(binding.device, &ai, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    REQUIRE(fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command, &bi) == VK_SUCCESS);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = image;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    const auto transition = fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    transition(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
               nullptr, 1, &barrier);
    for (std::uint32_t layer = 0; layer < 2; ++layer) {
      const VkClearColorValue color{{seed, .25f + .5f * float(layer), .6f, .8f}};
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
      fn<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                           &color, 1, &range);
    }
    barrier.oldLayout = barrier.newLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    transition(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
               nullptr, 1, &barrier);
    REQUIRE(fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command) == VK_SUCCESS);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    auto& gpu = venture::gfx::gpu_device();
    REQUIRE(fn<PFN_vkQueueSubmit>("vkQueueSubmit")(gpu.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    REQUIRE(fn<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(gpu.queue) == VK_SUCCESS);
    fn<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(binding.device, pool, 1, &command);
  }
  // Acquire, wait, render and release one image; returns its index.
  std::uint32_t draw(XrSwapchain chain, float seed) {
    namespace g = venture::hle::openxr::guest;
    std::uint32_t index{};
    REQUIRE(g::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    REQUIRE(g::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
    render(venture::hle::openxr::state().swapchains.at(chain).images.at(index).image, seed);
    REQUIRE(g::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
    return index;
  }
  // One frame presenting `chain`: both slices as a projection, or slice 1 as
  // a quad (flipped if asked).
  XrResult present(XrSwapchain chain, bool quad = false, bool flip = false) {
    namespace g = venture::hle::openxr::guest;
    XrFrameState frame{XR_TYPE_FRAME_STATE};
    REQUIRE(g::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
    REQUIRE(g::xrBeginFrame(session, nullptr) == XR_SUCCESS);
    XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (std::uint32_t i = 0; i < 2; ++i) {
      views[i].pose.orientation.w = 1;
      views[i].fov = {-.5f, .5f, .5f, -.5f};
      views[i].subImage = {chain, {{0, 0}, {8, 8}}, i};
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, space, 2, views};
    XrCompositionLayerImageLayoutFB layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB, nullptr,
                                           XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB};
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD, flip ? &layout : nullptr, 0, space,
                                 XR_EYE_VISIBILITY_BOTH, {chain, {{0, 0}, {8, 8}}, 1},
                                 {{0, 0, 0, 1}, {0, 0, -1}}, {1, 1}};
    const auto* header = quad ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)
                              : reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, frame.predictedDisplayTime,
                       XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, &header};
    return g::xrEndFrame(session, &end);
  }
  std::vector<unsigned char> read(VkImage image, XrSwapchain chain, std::uint32_t slice) {
    std::vector<unsigned char> pixels;
    std::string error;
    const XrSwapchainSubImage sub{chain, {{0, 0}, {8, 8}}, slice};
    REQUIRE(venture::xr::read_eye(binding, get, image, VK_FORMAT_R8G8B8A8_SRGB, sub, pixels, &error));
    return pixels;
  }
};
} // namespace

TEST_CASE("XR zero-copy chains give the game the host's images and present them without a copy") {
  namespace g = venture::hle::openxr::guest;
  auto& state = venture::hle::openxr::state();
  // What each path shows the host for the same rendering, per slice.
  std::array<std::vector<std::vector<unsigned char>>, 2> shown;
  for (const bool zero_copy : {true, false}) {
    CAPTURE(zero_copy);
    ZeroCopySetting setting(zero_copy ? "1" : "0");
    ZeroCopyScene scene;
    const auto live = ventureMockXrStats().live_swapchains;
    XrSwapchain chain{};
    const auto info = ZeroCopyScene::info();
    REQUIRE(g::xrCreateSwapchain(scene.session, &info, &chain) == XR_SUCCESS);
    auto& record = state.swapchains.at(chain);
    std::uint32_t count{};
    REQUIRE(g::xrEnumerateSwapchainImages(chain, 0, &count, nullptr) == XR_SUCCESS);
    std::vector<XrSwapchainImageVulkanKHR> guest(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    REQUIRE(g::xrEnumerateSwapchainImages(chain, count, &count,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(guest.data())) == XR_SUCCESS);
    if (zero_copy) {
      // The host chain exists from creation and its images are the game's.
      REQUIRE(record.direct_handle != XR_NULL_HANDLE);
      CHECK(record.backing->external());
      CHECK(ventureMockXrStats().live_swapchains == live + 1);
      std::uint32_t host_count{};
      REQUIRE(::xrEnumerateSwapchainImages(record.direct_handle, 0, &host_count, nullptr) == XR_SUCCESS);
      std::vector<XrSwapchainImageVulkanKHR> host(host_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
      REQUIRE(::xrEnumerateSwapchainImages(record.direct_handle, host_count, &host_count,
                                           reinterpret_cast<XrSwapchainImageBaseHeader*>(host.data())) == XR_SUCCESS);
      REQUIRE(host_count == count);
      for (std::uint32_t i = 0; i < count; ++i) CHECK(host[i].image == guest[i].image);
    } else {
      CHECK(record.direct_handle == XR_NULL_HANDLE);
      CHECK_FALSE(record.backing->external());
      CHECK(ventureMockXrStats().live_swapchains == live);
    }
    for (int frame = 0; frame < 5; ++frame) {
      CAPTURE(frame);
      const auto index = scene.draw(chain, .1f + .1f * float(frame));
      REQUIRE(scene.present(chain) == XR_SUCCESS);
      XrSwapchainSubImage host[2]{};
      REQUIRE(ventureMockXrLastSubImages(host, 2) == 2);
      for (std::uint32_t slice = 0; slice < 2; ++slice) {
        CHECK(host[slice].imageArrayIndex == slice);
        CHECK(host[slice].imageRect.offset.y == 0);
        CHECK(host[slice].imageRect.extent.height == 8);
      }
      if (zero_copy) {
        // Presented as it is: the host chain is the game's, and there is no
        // presentation chain to copy into.
        CHECK(record.present_direct);
        CHECK(host[0].swapchain == record.direct_handle);
        CHECK(record.host_handle == XR_NULL_HANDLE);
        CHECK(record.direct_frames == std::uint64_t(frame + 1));
        CHECK(ventureMockXrStats().live_swapchains == live + 1);
        // The image went back to the host before the frame was ended.
        CHECK_FALSE(record.held);
        CHECK_FALSE(record.backing->owned(index));
      } else {
        CHECK(host[0].swapchain == record.host_handle);
      }
      if (frame == 4) {
        const auto rendered = scene.read(guest[index].image, chain, 0);
        const auto source = zero_copy ? guest[index].image : record.host_images.at(record.host_index).image;
        const auto presented = zero_copy ? record.direct_handle : record.host_handle;
        for (std::uint32_t slice = 0; slice < 2; ++slice)
          shown[zero_copy].push_back(scene.read(source, presented, slice));
        CHECK(shown[zero_copy][0] == rendered);
        CHECK(shown[zero_copy][0] != shown[zero_copy][1]);
      }
    }
    // Destroying a chain whose image the game holds gives every image back
    // to the host chain, which is parked for reuse (dev_dump.cpp): a chain
    // like it takes it back and runs as before.
    std::uint32_t index{};
    REQUIRE(g::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    REQUIRE(g::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
    REQUIRE(g::xrDestroySwapchain(chain) == XR_SUCCESS);
    const auto parked = ventureMockXrStats().live_swapchains;
    REQUIRE(g::xrCreateSwapchain(scene.session, &info, &chain) == XR_SUCCESS);
    CHECK(ventureMockXrStats().live_swapchains == parked);
    for (int frame = 0; frame < 4; ++frame) {
      scene.draw(chain, .9f);
      REQUIRE(scene.present(chain) == XR_SUCCESS);
    }
    CHECK(state.swapchains.at(chain).present_direct == zero_copy);
    REQUIRE(g::xrDestroySwapchain(chain) == XR_SUCCESS);
  }
  // What the host gets does not depend on the path.
  CHECK(shown[0] == shown[1]);
}

TEST_CASE("XR zero-copy chains copy for a frame that needs it, and for good after a late write") {
  namespace g = venture::hle::openxr::guest;
  auto& state = venture::hle::openxr::state();
  ZeroCopySetting setting("1");
  ZeroCopyScene scene;
  const auto live = ventureMockXrStats().live_swapchains;
  XrSwapchain chain{};
  const auto info = ZeroCopyScene::info();
  REQUIRE(g::xrCreateSwapchain(scene.session, &info, &chain) == XR_SUCCESS);
  auto& record = state.swapchains.at(chain);
  REQUIRE(record.direct_handle != XR_NULL_HANDLE);
  XrSwapchainSubImage host{}, views[2]{};

  SUBCASE("a flipped layer presents through a copy, the next frame directly again") {
    auto index = scene.draw(chain, .3f);
    const auto rendered = scene.read(record.images.at(index).image, chain, 1);
    REQUIRE(scene.present(chain, true, true) == XR_SUCCESS);
    REQUIRE(ventureMockXrLastSubImages(&host, 1) == 1);
    CHECK_FALSE(record.present_direct);
    REQUIRE(record.host_handle != XR_NULL_HANDLE);
    CHECK(host.swapchain == record.host_handle);
    CHECK(ventureMockXrStats().live_swapchains == live + 2);
    // The copy was made before the image went back to the host.
    CHECK_FALSE(record.held);
    // Every row of a clear is the same, so the flipped copy matches.
    CHECK(scene.read(record.host_images.at(record.host_index).image, record.host_handle, 1) == rendered);
    index = scene.draw(chain, .4f);
    REQUIRE(scene.present(chain, true) == XR_SUCCESS);
    REQUIRE(ventureMockXrLastSubImages(&host, 1) == 1);
    CHECK(record.present_direct);
    CHECK(host.swapchain == record.direct_handle);
    CHECK(record.direct_frames == 1);
  }

  SUBCASE("two releases in one frame present the second") {
    // The first goes back to the host before the second's wait.
    const auto first = scene.draw(chain, .2f);
    const auto second = scene.draw(chain, .7f);
    CHECK(first != second);
    CHECK(record.held == second);
    REQUIRE(scene.present(chain) == XR_SUCCESS);
    REQUIRE(ventureMockXrLastSubImages(views, 2) == 2);
    CHECK(record.present_direct);
    CHECK(views[0].swapchain == record.direct_handle);
    CHECK_FALSE(record.held);
    // The game's next frame waits as usual.
    scene.draw(chain, .8f);
    REQUIRE(scene.present(chain) == XR_SUCCESS);
    CHECK(record.direct_frames == 2);
  }

  SUBCASE("a game that changes an image the host has presents through copies from then on") {
    const auto index = scene.draw(chain, .5f);
    REQUIRE(scene.present(chain) == XR_SUCCESS);
    CHECK(record.present_direct);
    // The frame went to the host with the image. The game then submits a
    // layout change on it, and one back (so the copies below read it in its
    // attachment layout).
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = record.images.at(index).image;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    const auto command = reinterpret_cast<VkCommandBuffer>(std::uintptr_t(0x5eed));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    for (int pass = 0; pass < 2; ++pass) {
      venture::vulkan::note_layouts(command, 1, &barrier);
      venture::vulkan::guest_submitting_layouts(1, &submit);
      venture::vulkan::submitted_layouts(1, &submit);
      venture::vulkan::forget_command_layouts(command);
      std::swap(barrier.oldLayout, barrier.newLayout);
    }
    scene.draw(chain, .55f);
    REQUIRE(scene.present(chain) == XR_SUCCESS);
    CHECK(record.copy_always);
    CHECK_FALSE(record.present_direct);
    REQUIRE(ventureMockXrLastSubImages(views, 2) == 2);
    CHECK(views[0].swapchain == record.host_handle);
    // From now on every frame is copied and the zero-copy chain is never
    // composited, so a late write cannot reach what the host shows.
    for (int frame = 0; frame < 4; ++frame) {
      CAPTURE(frame);
      const auto next = scene.draw(chain, .6f + .05f * float(frame));
      REQUIRE(scene.present(chain) == XR_SUCCESS);
      REQUIRE(ventureMockXrLastSubImages(views, 2) == 2);
      CHECK(views[0].swapchain == record.host_handle);
      CHECK(scene.read(record.host_images.at(record.host_index).image, record.host_handle, 0) ==
            scene.read(record.images.at(next).image, chain, 0));
    }
  }

  SUBCASE("work the game submits after its release, before the frame ends, keeps zero-copy") {
    // Engines that submit at the end of their frame, and a deferred queue
    // facade, reach the queue after xrReleaseSwapchainImage. The image is
    // still Venture's, and the game's work is flushed before the host gets it.
    namespace g = venture::hle::openxr::guest;
    std::uint32_t index{};
    REQUIRE(g::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    REQUIRE(g::xrWaitSwapchainImage(chain, &wait) == XR_SUCCESS);
    scene.render(record.images.at(index).image, .5f);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = record.images.at(index).image;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    const auto command = reinterpret_cast<VkCommandBuffer>(std::uintptr_t(0x5eee));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    venture::vulkan::note_layouts(command, 1, &barrier);
    REQUIRE(g::xrReleaseSwapchainImage(chain, nullptr) == XR_SUCCESS);
    venture::vulkan::guest_submitting_layouts(1, &submit);
    venture::vulkan::submitted_layouts(1, &submit);
    venture::vulkan::forget_command_layouts(command);
    REQUIRE(scene.present(chain) == XR_SUCCESS);
    CHECK_FALSE(record.copy_always);
    CHECK(record.present_direct);
  }

  SUBCASE("acquiring more images than the chain has is refused before it reaches the host") {
    std::uint32_t count{};
    REQUIRE(g::xrEnumerateSwapchainImages(chain, 0, &count, nullptr) == XR_SUCCESS);
    std::vector<std::uint32_t> held;
    for (std::uint32_t i = 0; i < count; ++i) {
      std::uint32_t index{};
      REQUIRE(g::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_SUCCESS);
      held.push_back(index);
    }
    std::uint32_t extra{};
    CHECK(XR_FAILED(g::xrAcquireSwapchainImage(chain, nullptr, &extra)));
    std::sort(held.begin(), held.end());
    CHECK(std::unique(held.begin(), held.end()) == held.end());
  }
  REQUIRE(g::xrDestroySwapchain(chain) == XR_SUCCESS);
}

TEST_CASE("XR chains that cannot be the host's keep private images") {
  namespace g = venture::hle::openxr::guest;
  ZeroCopySetting setting("1");
  ZeroCopyScene scene;
  auto& state = venture::hle::openxr::state();
  auto direct = [&](XrSwapchainCreateInfo info) {
    XrSwapchain chain{};
    REQUIRE(g::xrCreateSwapchain(scene.session, &info, &chain) == XR_SUCCESS);
    const bool result = state.swapchains.at(chain).direct_handle != XR_NULL_HANDLE;
    REQUIRE(g::xrDestroySwapchain(chain) == XR_SUCCESS);
    return result;
  };
  const auto info = ZeroCopyScene::info();
  CHECK(direct(info));
  auto mips = info;
  mips.mipCount = 3;  // the host keeps one level
  CHECK_FALSE(direct(mips));
  auto still = info;
  still.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
  CHECK_FALSE(direct(still));
  auto large = info;
  large.width = 8192;  // past the mock's 4096 maximum
  CHECK_FALSE(direct(large));
}

TEST_CASE("XR a game's color space keeps zero-copy with or without the host's XR_FB_color_space") {
  namespace g = venture::hle::openxr::guest;
  // The host instance is the process's: its enabled list is set as if the
  // mock had offered the extension at startup.
  struct HostColorSpace {
    explicit HostColorSpace(const char* value) {
      set(value);
      if (*value) venture::xr::host_xr().enabled_extensions.insert("XR_FB_color_space");
    }
    ~HostColorSpace() {
      venture::xr::host_xr().enabled_extensions.erase("XR_FB_color_space");
      set("");
    }
    static void set(const char* value) {
#ifdef _WIN32
      _putenv_s("VENTURE_MOCK_COLOR_SPACE", value);
#else
      if (*value) setenv("VENTURE_MOCK_COLOR_SPACE", value, 1); else unsetenv("VENTURE_MOCK_COLOR_SPACE");
#endif
      venture::environment::refresh();
      ventureMockXrRefreshEnvironment();
    }
  };
  ZeroCopySetting setting("1");
  auto& state = venture::hle::openxr::state();
  for (const bool host_has_it : {true, false}) {
    CAPTURE(host_has_it);
    HostColorSpace host(host_has_it ? "1" : "");
    ZeroCopyScene scene(true);
    REQUIRE(g::xrSetColorSpaceFB(scene.session, XR_COLOR_SPACE_P3_FB) == XR_SUCCESS);
    XrSwapchain chain{};
    const auto info = ZeroCopyScene::info();
    REQUIRE(g::xrCreateSwapchain(scene.session, &info, &chain) == XR_SUCCESS);
    auto& record = state.swapchains.at(chain);
    // The host shows P3 itself, or has no colour management and is given the
    // game's colours as they are: either way nothing converts the eye.
    CHECK(record.direct_handle != XR_NULL_HANDLE);
    scene.draw(chain, .5f);
    REQUIRE(scene.present(chain) == XR_SUCCESS);
    CHECK(record.present_direct);
    REQUIRE(g::xrDestroySwapchain(chain) == XR_SUCCESS);
  }
}
