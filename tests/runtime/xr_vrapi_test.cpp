#include "common/environment.h"
#include "xr/menu_chord.h"
#include "xr_menu_binding_fixture.h"
#include "xr/application_identity.h"
#include "../mock_openxr_runtime/control.h"
#include "core/hle.h"
#include "core/linker.h"
#include "core/thread.h"
#include "core/wait.h"
#include "common/log.h"
#include "fake_cpu.h"
#include "hle/openxr/openxr.h"
#include "hle/vrapi/integration.h"
#include "hle/vrapi/layers.h"
#include "hle/vrapi/vrapi.h"
#include "hle/vrapi/state.h"
#include "hle/vrapi/vulkan_images.h"
#include "vulkan_test_support.h"
#include "gfx/queue_sync.h"
#include "gfx/gpu_device.h"
#include "xr/vulkan_extensions.h"
#include "xr_images_fixture.h"
#include "xr_refresh_host.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <numbers>
#include <tuple>
#include "xr/hands.h"
#include "xr/math.h"
#include <doctest/doctest.h>
#include <thread>
#include <future>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../async_wait.h"

namespace vr = venture::hle::vrapi;
namespace vg = vr::guest;

TEST_CASE("Legacy XR Vulkan requirements separate Android capabilities from native creation") {
  using namespace venture;
  auto& host = xr::host_xr();
  auto& gpu = gfx::gpu_device();
  // Real software-driver physical device, but no device/background warm-up.
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  VkInstance instance{};
  REQUIRE(test::vk_call("vkCreateInstance", &ci, nullptr, &instance) == VK_SUCCESS);
  struct Destroy { VkInstance instance; ~Destroy() { test::vk_call("vkDestroyInstance", instance, nullptr); } } destroy{instance};
  VkPhysicalDevice physical{};
  std::uint32_t n = 1;
  REQUIRE(test::vk_call("vkEnumeratePhysicalDevices", instance, &n, &physical) == VK_SUCCESS);

  struct Fixture {
    xr::HostXr& host;
    gfx::GpuDevice& gpu;
    void* xr_original;
    PFN_vkGetInstanceProcAddr vk_original;
    bool legacy_enabled;
    std::string instance_names, device_names;
    std::vector<std::string> native_instance, native_device;
    XrResult error = XR_SUCCESS;
  };
  static Fixture* active;
  Fixture fixture{host, gpu, host.get_instance_proc_addr, gpu.get_instance_proc_addr,
                  host.enabled_extensions.contains("XR_KHR_vulkan_enable")};
  active = &fixture;
  struct Restore {
    Fixture& f;
    ~Restore() {
      f.host.get_instance_proc_addr = f.xr_original;
      f.gpu.get_instance_proc_addr = f.vk_original;
      if (!f.legacy_enabled) f.host.enabled_extensions.erase("XR_KHR_vulkan_enable");
      xr::invalidate_host_procedures(); active = nullptr;
    }
  } restore{fixture};
  host.enabled_extensions.insert("XR_KHR_vulkan_enable");
  static auto query = +[](bool device, std::uint32_t capacity, std::uint32_t* count, char* out) -> XrResult {
    if (XR_FAILED(active->error)) return active->error;
    const auto& text = device ? active->device_names : active->instance_names;
    *count = static_cast<std::uint32_t>(text.size() + 1);
    if (!capacity) return XR_SUCCESS;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    std::memcpy(out, text.c_str(), *count);
    return XR_SUCCESS;
  };
  host.get_instance_proc_addr = reinterpret_cast<void*>(+[](XrInstance i, const char* name, PFN_xrVoidFunction* out) -> XrResult {
    if (std::strcmp(name, "xrGetVulkanInstanceExtensionsKHR") == 0) {
      *out = reinterpret_cast<PFN_xrVoidFunction>(+[](XrInstance, XrSystemId, std::uint32_t c, std::uint32_t* n, char* out) -> XrResult { return query(false, c, n, out); });
      return XR_SUCCESS;
    }
    if (std::strcmp(name, "xrGetVulkanDeviceExtensionsKHR") == 0) {
      *out = reinterpret_cast<PFN_xrVoidFunction>(+[](XrInstance, XrSystemId, std::uint32_t c, std::uint32_t* n, char* out) -> XrResult { return query(true, c, n, out); });
      return XR_SUCCESS;
    }
    return reinterpret_cast<PFN_xrGetInstanceProcAddr>(active->xr_original)(i, name, out);
  });
  gpu.get_instance_proc_addr = +[](VkInstance i, const char* name) -> PFN_vkVoidFunction {
    if (std::strcmp(name, "vkCreateInstance") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(+[](const VkInstanceCreateInfo* ci, const VkAllocationCallbacks*, VkInstance*) -> VkResult {
        active->native_instance.assign(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
      });
    if (std::strcmp(name, "vkCreateDevice") == 0)
      return reinterpret_cast<PFN_vkVoidFunction>(+[](VkPhysicalDevice, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks*, VkDevice*) -> VkResult {
        active->native_device.assign(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
      });
    return active->vk_original(i, name);
  };
  xr::invalidate_host_procedures();
  SUBCASE("Windows WSI and native external handles") {
    fixture.instance_names = "VK_KHR_surface VK_KHR_win32_surface";
    fixture.device_names = "VK_KHR_external_memory VK_KHR_external_memory_win32 VK_KHR_external_semaphore_win32";
  }
  SUBCASE("Monado WSI and native external handles") {
    fixture.instance_names = "VK_KHR_surface VK_KHR_wayland_surface VK_KHR_xlib_surface";
    fixture.device_names = "VK_KHR_external_memory VK_KHR_external_memory_fd VK_KHR_external_semaphore_fd VK_EXT_image_drm_format_modifier";
  }
  SUBCASE("Empty requirements") {}
  SUBCASE("Host errors remain errors") {
    fixture.error = XR_ERROR_SYSTEM_INVALID;
  }
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  XrInstance guest{};
  REQUIRE(hle::openxr::guest::xrCreateInstance(&create, &guest) == XR_SUCCESS);
  struct Guest { XrInstance instance; ~Guest() { hle::openxr::guest::xrDestroyInstance(instance); } } guest_destroy{guest};
  for (bool device : {false, true}) {
    std::vector<std::string> native;
    const auto host_result = xr::vulkan_extension_requirements(host.instance, host.system, device, native);
    if (XR_FAILED(fixture.error)) { CHECK(host_result == fixture.error); continue; }
    REQUIRE(host_result == XR_SUCCESS);
    std::string expected;
    for (const auto& name : native) if (vulkan::extension_translatable(name)) {
      if (!expected.empty()) expected += ' ';
      expected += name;
    }
    auto get = [&](std::uint32_t cap, std::uint32_t* count, char* out) {
      return device ? hle::openxr::guest::xrGetVulkanDeviceExtensionsKHR(guest, host.system, cap, count, out)
                    : hle::openxr::guest::xrGetVulkanInstanceExtensionsKHR(guest, host.system, cap, count, out);
    };
    std::uint32_t count = 0;
    REQUIRE(get(0, &count, nullptr) == XR_SUCCESS);
    CHECK(count == expected.size() + 1);
    std::vector<char> text(count, '!');
    if (count > 1) { CHECK(get(count - 1, &count, text.data()) == XR_ERROR_SIZE_INSUFFICIENT); CHECK(text[0] == '!'); }
    REQUIRE(get(count, &count, text.data()) == XR_SUCCESS);
    CHECK(std::string(text.data()) == expected);
    CHECK(get(1, nullptr, text.data()) == XR_ERROR_VALIDATION_FAILURE);
    CHECK(get(1, &count, nullptr) == XR_ERROR_VALIDATION_FAILURE);
    count = 0;
    auto vr_get = device ? vg::vrapi_GetDeviceExtensionsVulkan : vg::vrapi_GetInstanceExtensionsVulkan;
    REQUIRE(vr_get(nullptr, &count) == 0);
    CHECK(count == expected.size() + 1);
    REQUIRE(vr_get(text.data(), &count) == 0);
    CHECK(std::string(text.data()) == expected);
  }
  if (XR_FAILED(fixture.error)) {
    std::uint32_t count = 0;
    CHECK(hle::openxr::guest::xrGetVulkanInstanceExtensionsKHR(guest, host.system, 0, &count, nullptr) == fixture.error);
    CHECK(hle::openxr::guest::xrGetVulkanDeviceExtensionsKHR(guest, host.system, 0, &count, nullptr) == fixture.error);
    CHECK(vg::vrapi_GetInstanceExtensionsVulkan(nullptr, &count) != 0);
    CHECK(vg::vrapi_GetDeviceExtensionsVulkan(nullptr, &count) != 0);
    VkInstance out{};
    CHECK(std::int32_t(test::vk_call("vkCreateInstance", &ci, nullptr, &out)) == VK_ERROR_INITIALIZATION_FAILED);
    return;
  }
  const char* surface = "VK_KHR_surface";
  ci.enabledExtensionCount = 1; ci.ppEnabledExtensionNames = &surface;
  VkInstance out{};
  CHECK(std::int32_t(test::vk_call("vkCreateInstance", &ci, nullptr, &out)) == VK_ERROR_OUT_OF_HOST_MEMORY);
  CHECK(ci.enabledExtensionCount == 1);
  CHECK(ci.ppEnabledExtensionNames == &surface);
  float priority = 1;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueCount = 1; queue.pQueuePriorities = &priority;
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &queue;
  const char* external = "VK_KHR_external_memory";
  di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = &external;
  VkDevice device{};
  CHECK(std::int32_t(test::vk_call("vkCreateDevice", physical, &di, nullptr, &device)) == VK_ERROR_OUT_OF_HOST_MEMORY);
  CHECK(di.enabledExtensionCount == 1);
  CHECK(di.ppEnabledExtensionNames == &external);
  for (bool is_device : {false, true}) {
    std::vector<std::string> required;
    REQUIRE(xr::vulkan_extension_requirements(host.instance, host.system, is_device, required) == XR_SUCCESS);
    const auto& received = is_device ? fixture.native_device : fixture.native_instance;
    for (const auto& name : required) CHECK(std::count(received.begin(), received.end(), name) == 1);
  }
  const char* foreign = "VK_KHR_win32_surface";
  ci.enabledExtensionCount = 1; ci.ppEnabledExtensionNames = &foreign;
  CHECK(std::int32_t(test::vk_call("vkCreateInstance", &ci, nullptr, &out)) == VK_ERROR_EXTENSION_NOT_PRESENT);
}

TEST_CASE("VrApi multiview capability follows stereo array image submission support") {
  CHECK(vg::vrapi_GetSystemPropertyInt(nullptr, 128) == 0);
  venture::test::XrImageFixture images;
  venture::xr::install_xr_images(&images);
  struct Restore { ~Restore() { venture::xr::install_xr_images(nullptr); } } restore;
  vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  struct Shutdown { ~Shutdown() { vg::vrapi_Shutdown(); } } shutdown;
  REQUIRE(vg::vrapi_GetSystemPropertyInt(nullptr, 128) == 1);
  auto ring = vg::vrapi_CreateTextureSwapChain3(2, 0x8058, 32, 32, 1, 3);
  REQUIRE(ring);
  CHECK(vr::state().rings.at(ring)->desc.layers == 2);
  vg::vrapi_DestroyTextureSwapChain(ring);
}

TEST_CASE("VrApi legacy stereo producers copy a shared slot only once") {
  venture::test::XrImageFixture images;
  venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  auto ring = vg::vrapi_CreateTextureSwapChain3(0, 0x8058, 32, 32, 6, 3);
  REQUIRE(ring);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto mobile = vg::vrapi_EnterVrMode(&mode);
  REQUIRE(mobile);
  vr::abi::LegacyFrame frame{};
  frame.type = 3;
  frame.swap_interval = 1;
  frame.frame_index = 1;
  frame.layer_count = 1;
  auto& layer = frame.layers[0];
  layer.color_scale = 1;
  layer.src_blend = 1;
  for (int eye = 0; eye < 2; ++eye) {
    auto& texture = layer.textures[eye];
    texture.color = ring;
    texture.rect = {0, 0, 1, 1};
    texture.head.pose.orientation.w = 1;
    texture.tan_angles.m[0][0] = texture.tan_angles.m[1][1] = .5f;
    texture.tan_angles.m[0][2] = texture.tan_angles.m[1][2] = -.5f;
    texture.tan_angles.m[2][2] = -1;
    texture.completion_fence = 100 + eye;
  }
  const auto before = ventureMockXrStats();
  images.batch_enabled = true;
  images.before_batch_submit = [&] {
    CHECK(images.copies == 1);
    CHECK(ventureMockXrStats().released == before.released);
  };
  vg::vrapi_SubmitFrame(mobile, &frame);
  CHECK(images.batch_submits == 1);
  CHECK(images.copies == 1);
  CHECK(images.last_source_mips == 6);
  CHECK(images.last_destination_mips == 1);
  CHECK(images.producer_syncs == std::vector<std::uint64_t>{100, 101});
  CHECK(ventureMockXrStats().released == before.released + 1);
  vg::vrapi_LeaveVrMode(mobile);
  vg::vrapi_DestroyTextureSwapChain(ring);
  vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr);
}

TEST_CASE("VrApi unavailable status polls report each getter and counter once per launch") {
  namespace log = venture::log;
  vr::abi::Init init{1, 1, 1, 51, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  struct Shutdown { ~Shutdown() { vg::vrapi_Shutdown(); } } shutdown;
  struct Capture {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        std::format("venture-vrapi-status-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    log::Level level = log::level();
    Capture() { log::set_level(log::Level::info); log::set_file(path.string()); }
    ~Capture() {
      log::set_file("");
      log::set_level(level);
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } capture;
  // These counters have no host telemetry source. Integer and float queries
  // are distinct causes even when they use the same numeric property.
  for (int i = 0; i < 500; ++i) {
    for (auto property : {262, 263}) {
      CHECK(vg::vrapi_GetSystemStatusFloat(nullptr, property) == 0);
      CHECK(vg::vrapi_GetSystemStatusInt(nullptr, property) == 0);
    }
  }
  log::set_file(""); // Flush the shared logger's pending repeat counts too.
  std::ifstream input(capture.path, std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(input), {}};
  for (auto getter : {"GetSystemStatusFloat unavailable counter", "GetSystemStatusInt unknown property"}) {
    for (auto property : {262, 263}) {
      auto message = std::format("[vrapi] {} failed: {}", getter, property);
      auto first = text.find(message);
      REQUIRE(first != text.npos);
      CHECK(text.find(message, first + message.size()) == text.npos);
    }
  }
  // Shared logger suppression alone would emit a summary for the 499 repeats.
  CHECK(text.find("times in the last") == text.npos);
}

// Run in its own process: std::exit intentionally leaves a live guest device
// and image ring, as an APK terminating before its Android teardown can do.
TEST_CASE("VrApi Vulkan process exit with live images" * doctest::skip()) {
  using namespace venture;
  test::VulkanDevice gpu;
  vr::abi::Init init{1, 1, 1, 51, 0, vr::abi::vulkan_api, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  vr::abi::SystemVulkan system{h2g(gpu.instance), h2g(gpu.physical), h2g(gpu.device)};
  REQUIRE(vg::vrapi_CreateSystemVulkan(&system) == 0);
  REQUIRE(vg::vrapi_CreateTextureSwapChain3(2, VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 2));
  std::exit(0);
}

// Also its own process. UE4 (RE4 without entitlement) destroys its device and
// then force-quits without DestroySystemVulkan: the eye images must go with the
// device, never be released on the freed device at exit.
TEST_CASE("VrApi Vulkan process exit after the game destroyed its device" * doctest::skip()) {
  using namespace venture;
  vr::abi::TextureSwapChain *ring = nullptr;
  {
    test::VulkanDevice gpu;
    vr::abi::Init init{1, 1, 1, 51, 0, vr::abi::vulkan_api, {1, 2, 3}};
    REQUIRE(vg::vrapi_Initialize(&init) == 0);
    vr::abi::SystemVulkan system{h2g(gpu.instance), h2g(gpu.physical), h2g(gpu.device)};
    REQUIRE(vg::vrapi_CreateSystemVulkan(&system) == 0);
    ring = vg::vrapi_CreateTextureSwapChain3(2, VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 2);
    REQUIRE(ring);
    auto source = vg::vrapi_GetTextureSwapChainBufferVulkan(ring, 0);
    REQUIRE(source != VK_NULL_HANDLE);
    vr::abi::ModeVulkan mode{{5, 0, {1, 2, 3}, 0, 0, 0}, h2g(gpu.queue)};
    XrGraphicsBindingVulkanKHR binding{};
    REQUIRE(vr::bind_vulkan_queue(mode, binding) == XR_SUCCESS);
    gpu.begin();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = source;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    test::vk_call("vkCmdPipelineBarrier", gpu.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    gpu.submit();
    auto &native_ring = *vr::state().rings.at(ring);
    // Both images belong to the device hook. Leave the copy slot pending so
    // device teardown must retire it before destroying its fence and images.
    REQUIRE(xr::xr_images()->copy_to_image(native_ring.textures[0],
        vg::vrapi_GetTextureSwapChainBufferVulkan(ring, 1), native_ring.desc, 0, false) == XR_SUCCESS);
  }
  CHECK(vg::vrapi_GetTextureSwapChainBufferVulkan(ring, 0) == VK_NULL_HANDLE);
  std::exit(0);
}

TEST_CASE("VrApi projection preserves asymmetric frusta with Vulkan inverted Y") {
  vr::abi::Projection projection{};
  projection.header = {1, 0, {1, 1, 1, 1}, 1, 0, 0};
  projection.head.pose.orientation.w = 1;
  for (auto &texture : projection.textures) {
    texture.tan_angles.m[0][0] = texture.tan_angles.m[1][1] = 0.5f;
    texture.tan_angles.m[0][2] = -0.5f;
    texture.tan_angles.m[1][2] = -0.25f;
    texture.tan_angles.m[2][2] = -1;
    texture.rect = {0, 0, 1, 1};
  }
  XrView eyes[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  for (auto &eye : eyes) eye.pose.orientation.w = 1;
  std::vector<bool> rows;
  auto image = [&](vr::abi::TextureSwapChain *, int, vr::abi::Rect, int, bool bottom_up,
                   XrSwapchainSubImage &) {
    rows.push_back(bottom_up);
    return XR_SUCCESS;
  };
  vr::LayerStorage gl, vk;
  std::vector<const XrCompositionLayerBaseHeader *> layers;
  REQUIRE(vr::translate_layer(projection.header, eyes, image, gl, layers) == XR_SUCCESS);
  for (auto &texture : projection.textures) {
    // Inverting normalized texture Y changes its center from .25 to .75,
    // while preserving the camera's asymmetric vertical frustum.
    texture.tan_angles.m[1][1] = -0.5f;
    texture.tan_angles.m[1][2] = -0.75f;
  }
  layers.clear();
  REQUIRE(vr::translate_layer(projection.header, eyes, image, vk, layers) == XR_SUCCESS);
  for (int eye = 0; eye < 2; ++eye) {
    CHECK(vk.views[eye].fov.angleLeft == doctest::Approx(gl.views[eye].fov.angleLeft));
    CHECK(vk.views[eye].fov.angleRight == doctest::Approx(gl.views[eye].fov.angleRight));
    CHECK(vk.views[eye].fov.angleUp == doctest::Approx(std::atan(1.5f)));
    CHECK(vk.views[eye].fov.angleDown == doctest::Approx(std::atan(-0.5f)));
  }
  // GL's T/Q=0 is the bottom of the view, so its memory rows reverse; the
  // inverted-Y matrix samples memory row 0 at the top.
  CHECK(rows == std::vector<bool>{true, true, false, false});
  projection.textures[0].tan_angles.m[1][1] = 0;
  CHECK(vr::translate_layer(projection.header, eyes, image, vk, layers) == XR_ERROR_VALIDATION_FAILURE);
}

TEST_CASE("VrApi projection FOV covers only each eye's texture rect") {
  vr::abi::Projection projection{};
  projection.header = {1, 0, {1, 1, 1, 1}, 1, 0, 0};
  projection.head.pose.orientation.w = 1;
  XrView eyes[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  for (auto &eye : eyes) eye.pose.orientation.w = 1;
  auto image = [](vr::abi::TextureSwapChain *, int, vr::abi::Rect, int, bool, XrSwapchainSubImage &) {
    return XR_SUCCESS;
  };
  // A 90-degree eye frustum (tangents -1..1) in each half of a shared texture:
  // side by side (S = .25 tan + .25 or + .75), then stacked (T likewise).
  for (bool stacked : {false, true}) {
    CAPTURE(stacked);
    for (int eye = 0; eye < 2; ++eye) {
      auto &t = projection.textures[eye];
      t.tan_angles = {};
      t.tan_angles.m[2][2] = -1;
      const int axis = stacked ? 1 : 0;
      t.tan_angles.m[axis][axis] = 0.25f;
      t.tan_angles.m[axis][2] = eye ? -0.75f : -0.25f;
      t.tan_angles.m[1 - axis][1 - axis] = 0.5f;
      t.tan_angles.m[1 - axis][2] = -0.5f;
      t.rect = stacked ? vr::abi::Rect{0, eye * 0.5f, 1, 0.5f} : vr::abi::Rect{eye * 0.5f, 0, 0.5f, 1};
    }
    vr::LayerStorage storage;
    std::vector<const XrCompositionLayerBaseHeader *> layers;
    REQUIRE(vr::translate_layer(projection.header, eyes, image, storage, layers) == XR_SUCCESS);
    const float quarter = std::numbers::pi_v<float> / 4;
    for (const auto &view : storage.views) {
      CHECK(view.fov.angleLeft == doctest::Approx(-quarter));
      CHECK(view.fov.angleRight == doctest::Approx(quarter));
      CHECK(view.fov.angleUp == doctest::Approx(quarter));
      CHECK(view.fov.angleDown == doctest::Approx(-quarter));
    }
  }
}

// VrApi_Helpers.h ovrMatrix4f_TanAngleMatrixFromUnitSquare for a square of
// half-extent `half`, `distance` metres ahead and yawed by `yaw`, evaluated
// from the inverse model-view the helper computes.
static vr::abi::Matrix unit_square(float yaw, float distance, float half) {
  const float c = std::cos(yaw), s = std::sin(yaw);
  const float inv[4][4]{{c / half, 0, -s / half, -distance * s / half},
                        {0, 1 / half, 0, 0},
                        {s, 0, c, distance * c},
                        {0, 0, 0, 1}};
  const float coef = inv[2][3] > 0 ? 1.f : -1.f;
  vr::abi::Matrix m{};
  for (int j = 0; j < 3; ++j) {
    m.m[0][j] = (.5f * (inv[0][j] * inv[2][3] - inv[0][3] * inv[2][j]) - .5f * inv[2][j]) * coef;
    m.m[1][j] = (-.5f * (inv[1][j] * inv[2][3] - inv[1][3] * inv[2][j]) - .5f * inv[2][j]) * coef;
    m.m[2][j] = -inv[2][j] * coef;
  }
  m.m[3][3] = 1;
  return m;
}

TEST_CASE("VrApi projection rows follow each layer's vertical sign in GLES and Vulkan") {
  auto &api = vr::state().init.graphics_api;
  const auto saved = api;
  vr::abi::Projection quad{};
  quad.header = {1, 0, {1, 1, 1, 1}, 2, 5, 0};
  quad.head.pose.orientation.w = 1;
  XrView eyes[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  for (auto &eye : eyes) eye.pose.orientation.w = 1;
  std::vector<bool> rows;
  auto image = [&](vr::abi::TextureSwapChain *, int, vr::abi::Rect, int, bool bottom_up,
                   XrSwapchainSubImage &) {
    rows.push_back(bottom_up);
    return XR_SUCCESS;
  };
  vr::LayerStorage storage;
  std::vector<const XrCompositionLayerBaseHeader *> layers;
  for (auto graphics : {0x10300, vr::abi::vulkan_api}) {
    CAPTURE(graphics);
    api = graphics;
    for (bool from_bottom : {false, true}) {
      CAPTURE(from_bottom);
      for (auto &texture : quad.textures) {
        texture.rect = {0, 0, 1, 1};
        // The helper's T grows down the quad, so memory row 0 is its top.
        texture.tan_angles = unit_square(.5f, 2, .5f);
        // Counting rows from the quad's bottom instead (T' = Q - T).
        if (from_bottom)
          for (int j = 0; j < 3; ++j)
            texture.tan_angles.m[1][j] = texture.tan_angles.m[2][j] - texture.tan_angles.m[1][j];
      }
      rows.clear();
      layers.clear();
      REQUIRE(vr::translate_layer(quad.header, eyes, image, storage, layers) == XR_SUCCESS);
      CHECK(rows == std::vector<bool>{from_bottom, from_bottom});
    }
  }
  api = saved;
}

TEST_CASE("VrApi unit-square projection preserves projective scale and tolerates inversion roundoff") {
  vr::abi::Projection projection{};
  projection.header = {1, 4, {1, 1, 1, 1}, 2, 5, 0};
  XrView eyes[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  for (int eye = 0; eye < 2; ++eye) {
    eyes[eye].pose.orientation.w = 1;
    auto &m = projection.textures[eye].tan_angles.m;
    m[0][0] = .21036558f;
    m[1][1] = .21036528f;
    m[0][2] = eye ? -1.0417235f : -.99172354f;
    m[1][2] = -.9990425f;
    m[2][2] = -2;
    m[1][0] = 2.977378e-7f;
    m[2][0] = 5.9604633e-7f;
    projection.textures[eye].rect = {0, 0, 1, 1};
  }
  auto image = [](vr::abi::TextureSwapChain *, int, vr::abi::Rect, int,
                  bool, XrSwapchainSubImage &) { return XR_SUCCESS; };
  vr::LayerStorage storage;
  std::vector<const XrCompositionLayerBaseHeader *> layers;
  REQUIRE(vr::translate_layer(projection.header, eyes, image, storage, layers) == XR_SUCCESS);
  for (int eye = 0; eye < 2; ++eye) {
    const auto &m = projection.textures[eye].tan_angles.m;
    const auto &fov = storage.views[eye].fov;
    // The rays at all four frustum corners must recover normalized UVs,
    // including the small inverse-matrix error in the actual guest packet.
    for (int x = 0; x < 2; ++x) for (int y = 0; y < 2; ++y) {
      const float tx = std::tan(x ? fov.angleRight : fov.angleLeft);
      const float ty = std::tan(y ? fov.angleUp : fov.angleDown);
      const float q = m[2][0] * tx + m[2][1] * ty - m[2][2];
      CHECK((m[0][0] * tx + m[0][1] * ty - m[0][2]) / q == doctest::Approx(x).epsilon(.00001));
      CHECK((m[1][0] * tx + m[1][1] * ty - m[1][2]) / q == doctest::Approx(y).epsilon(.00001));
    }
  }
  projection.textures[0].tan_angles.m[1][0] = .01f;
  CHECK(vr::translate_layer(projection.header, eyes, image, storage, layers) == XR_ERROR_VALIDATION_FAILURE);
}

TEST_CASE("VrApi rotated unit-square layers preserve their projective UV mapping") {
  vr::abi::Projection projection{};
  projection.header = {1, 0, {1, 1, 1, 1}, 2, 5, 0};
  projection.head.pose.orientation.w = 1;
  // The two 1080x1080 UI-eye packets submitted by UE4 after the TWD splash.
  const float matrices[2][3][3]{
      {{3.3588016f, .08012728f, -.7468902f},
       {-.030044967f, 3.4170253f, -.9411981f},
       {.009772968f, .1187805f, -1.9964455f}},
      {{3.3588014f, .08452252f, -.8208295f},
       {-.030044967f, 3.4166172f, -.94043446f},
       {.009772968f, .1187805f, -1.9964455f}}};
  XrView eyes[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  for (int eye = 0; eye < 2; ++eye) {
    eyes[eye].pose.orientation.w = 1;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
      projection.textures[eye].tan_angles.m[i][j] = matrices[eye][i][j];
    projection.textures[eye].rect = {0, 0, 1, 1};
  }
  unsigned bottom_up_eyes = 0;
  auto image = [&](vr::abi::TextureSwapChain *, int, vr::abi::Rect, int, bool bottom_up,
                   XrSwapchainSubImage &) {
    bottom_up_eyes += bottom_up;
    return XR_SUCCESS;
  };
  vr::LayerStorage storage;
  std::vector<const XrCompositionLayerBaseHeader *> layers;
  REQUIRE(vr::translate_layer(projection.header, eyes, image, storage, layers) == XR_SUCCESS);
  REQUIRE(layers.size() == 1);
  // UE4 renders these GLES eye buffers bottom-up: T grows upward (sy > 0).
  CHECK(bottom_up_eyes == 2);
  for (int eye = 0; eye < 2; ++eye) {
    const auto &view = storage.views[eye];
    CHECK(std::abs(view.pose.orientation.x) + std::abs(view.pose.orientation.y) > .01f);
    const auto &m = matrices[eye];
    for (float u : {0.f, .5f, 1.f}) for (float v : {0.f, .5f, 1.f}) {
      // Reconstruct an eye-space ray from the resulting OpenXR camera, then
      // evaluate the original homography, independently of the factorization.
      const auto ray = venture::xr::rotate(view.pose.orientation, {
          std::lerp(std::tan(view.fov.angleLeft), std::tan(view.fov.angleRight), u),
          std::lerp(std::tan(view.fov.angleDown), std::tan(view.fov.angleUp), v), -1});
      const float q = m[2][0]*ray.x + m[2][1]*ray.y + m[2][2]*ray.z;
      CHECK((m[0][0]*ray.x + m[0][1]*ray.y + m[0][2]*ray.z) / q == doctest::Approx(u).epsilon(.00001));
      CHECK((m[1][0]*ray.x + m[1][1]*ray.y + m[1][2]*ray.z) / q == doctest::Approx(v).epsilon(.00001));
    }
  }
}

TEST_CASE("VrApi Vulkan copies persistent guest images on the guest-owned device") {
  using namespace venture;
  REQUIRE(gfx::gpu_device().get_instance_proc_addr);
  test::VulkanDevice gpu;
  auto *previous = xr::xr_images();
  vr::abi::Init init{1, 1, 1, 51, 0, vr::abi::vulkan_api, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  CHECK(vg::vrapi_CreateSystemVulkan(nullptr) == vr::abi::invalid);
  vr::abi::SystemVulkan system{h2g(gpu.instance), h2g(gpu.physical), h2g(gpu.device)};
  REQUIRE(vg::vrapi_CreateSystemVulkan(&system) == 0);
  CHECK(vg::vrapi_CreateSystemVulkan(&system) == vr::abi::invalid_operation);
  auto *ring = vg::vrapi_CreateTextureSwapChain3(2, VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 2);
  REQUIRE(ring);
  auto source = vg::vrapi_GetTextureSwapChainBufferVulkan(ring, 0);
  REQUIRE(source);
  CHECK(vg::vrapi_GetTextureSwapChainBufferVulkan(ring, 1) != source);
  CHECK(vg::vrapi_GetTextureSwapChainBufferVulkan(ring, 2) == VK_NULL_HANDLE);
  VkImage density = source;
  std::uint32_t density_width = 999, density_height = 999;
  // An unavailable density resource is a successful empty query, before and
  // after binding the graphics queue. Stale caller values must be cleared.
  CHECK(vg::vrapi_GetSystemPropertyInt(nullptr, 130) == 0);
  CHECK(vg::vrapi_GetTextureSwapChainBufferFoveationVulkan(ring, 0, &density,
      &density_width, &density_height) == 0);
  CHECK(density == VK_NULL_HANDLE);
  CHECK(density_width == 0);
  CHECK(density_height == 0);
  vr::abi::ModeVulkan mode{{5, 0, {1, 2, 3}, 0, 0, 0}, h2g(gpu.queue)};
  auto *mobile = vg::vrapi_EnterVrMode(&mode.mode);
  REQUIRE(mobile);
  CHECK(vg::vrapi_GetSystemPropertyInt(nullptr, 130) == 0);
  for (int slot : {0, 1, 0}) {
    density = source;
    density_width = density_height = 999;
    CHECK(vg::vrapi_GetTextureSwapChainBufferFoveationVulkan(ring, slot, &density,
        &density_width, &density_height) == 0);
    CHECK(density == VK_NULL_HANDLE);
    CHECK(density_width == 0);
    CHECK(density_height == 0);
  }
  for (int slot : {-1, 2}) {
    density = source;
    density_width = density_height = 999;
    CHECK(vg::vrapi_GetTextureSwapChainBufferFoveationVulkan(ring, slot, &density,
        &density_width, &density_height) == vr::abi::invalid);
    CHECK(density == VK_NULL_HANDLE);
    CHECK(density_width == 0);
    CHECK(density_height == 0);
  }
  CHECK(vg::vrapi_GetTextureSwapChainBufferFoveationVulkan(ring, 0, nullptr,
      &density_width, &density_height) == vr::abi::invalid);
  CHECK_FALSE(xr::session().uses_shared_gpu);
  CHECK(vulkan::queue_binding(gpu.queue)->submission_mutex == gfx::queue_mutex(gpu.queue));
  auto barrier = [&](VkImage image, VkImageLayout old, VkImageLayout next) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = old; b.newLayout = next; b.image = image;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask = old == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    test::vk_call("vkCmdPipelineBarrier", gpu.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &b);
  };
  gpu.begin();
  barrier(source, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkClearColorValue color{{0.25f, 0.5f, 0.75f, 1}};
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
  test::vk_call("vkCmdClearColorImage", gpu.command, source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &color, 1u, &range);
  barrier(source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  gpu.submit();
  test::VulkanImage destination(gpu, VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 2);
  auto &native_ring = *vr::state().rings.at(ring);
  REQUIRE(xr::xr_images()->copy_to_image(native_ring.textures[0], destination.image, native_ring.desc, 0, false) == XR_SUCCESS);
  test::VulkanBuffer readback(gpu, 8);
  gpu.begin();
  barrier(destination.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy copies[2]{};
  for (std::uint32_t eye = 0; eye < 2; ++eye) {
    copies[eye].bufferOffset = eye * 4;
    copies[eye].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1};
    copies[eye].imageExtent = {1, 1, 1};
  }
  test::vk_call("vkCmdCopyImageToBuffer", gpu.command, destination.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 2u, copies);
  barrier(destination.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  gpu.submit();
  auto bytes = static_cast<const std::uint8_t *>(readback.mapped);
  for (int eye = 0; eye < 2; ++eye) {
    CHECK(bytes[eye * 4] == 64);
    CHECK((bytes[eye * 4 + 1] == 127 || bytes[eye * 4 + 1] == 128));
    CHECK(bytes[eye * 4 + 2] == 191);
    CHECK(bytes[eye * 4 + 3] == 255);
  }
  // Repeated copies retain both layouts and stable VkImage handles.
  REQUIRE(xr::xr_images()->copy_to_image(native_ring.textures[0], destination.image, native_ring.desc, 0, true) == XR_SUCCESS);
  CHECK(vg::vrapi_GetTextureSwapChainBufferVulkan(ring, 0) == source);
  vr::abi::Projection projection{};
  projection.header = {1, 2, {1, 1, 1, 1}, 1, 0, 0};
  auto time = vg::vrapi_GetPredictedDisplayTime(mobile, 1);
  projection.head = vg::vrapi_GetPredictedTracking2(mobile, time).head;
  for (auto &eye : projection.textures) {
    eye.chain = ring; eye.index = 0; eye.rect = {0, 0, 1, 1};
    eye.tan_angles.m[0][0] = 0.5f;
    eye.tan_angles.m[1][1] = -0.5f;
    eye.tan_angles.m[0][2] = eye.tan_angles.m[1][2] = -0.5f;
    eye.tan_angles.m[2][2] = -1;
  }
  const vr::abi::LayerHeader *layers[]{&projection.header};
  vr::abi::Submit submit{0, 2, 1, time, 0, 1, layers};
  std::promise<void> host_busy, release_host;
  auto released = release_host.get_future();
  std::atomic<bool> guard_expired{false};
  auto host_guard = std::async(std::launch::async, [&] {
    std::lock_guard lock(gfx::gpu_device().queue_lock);
    host_busy.set_value();
    guard_expired = released.wait_for(venture::test::hang_timeout) != std::future_status::ready;
  });
  host_busy.get_future().wait();
  const auto submitted = vg::vrapi_SubmitFrame2(mobile, &submit);
  const bool independent = !guard_expired.load();
  release_host.set_value();
  host_guard.get();
  INFO("vrapi_SubmitFrame2 result=", submitted, " queue guard expired=", guard_expired.load());
  REQUIRE(submitted == 0);
  CHECK(independent); // Submission completed while the unrelated queue stayed locked.
  vg::vrapi_DestroySystemVulkan(); // Live rings/session reject premature destruction.
  CHECK(xr::xr_images() != previous);
  vg::vrapi_LeaveVrMode(mobile);
  // Leave a copy pending when its source ring is destroyed. Ring teardown
  // must retire the slot before releasing the source image or its device.
  REQUIRE(xr::xr_images()->copy_to_image(native_ring.textures[0], destination.image,
      native_ring.desc, 0, false) == XR_SUCCESS);
  vg::vrapi_DestroyTextureSwapChain(ring);
  vg::vrapi_DestroySystemVulkan();
  CHECK(xr::xr_images() == previous);
  vg::vrapi_Shutdown();
  vr::abi::Event event{};
  CHECK(vg::vrapi_PollEvent(&event) == vr::abi::invalid);
}

TEST_CASE("VrApi Vulkan copies reverse rows only for bottom-up layers") {
  using namespace venture;
  REQUIRE(gfx::gpu_device().get_instance_proc_addr);
  test::VulkanDevice gpu;
  vr::abi::Init init{1, 1, 1, 51, 0, vr::abi::vulkan_api, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  vr::abi::SystemVulkan system{h2g(gpu.instance), h2g(gpu.physical), h2g(gpu.device)};
  REQUIRE(vg::vrapi_CreateSystemVulkan(&system) == 0);
  constexpr std::uint32_t size = 4, levels = 2, layers = 2;
  auto *ring = vg::vrapi_CreateTextureSwapChain3(2, VK_FORMAT_R8G8B8A8_UNORM, size, size, levels, 1);
  REQUIRE(ring);
  auto source = vg::vrapi_GetTextureSwapChainBufferVulkan(ring, 0);
  REQUIRE(source);
  vr::abi::ModeVulkan mode{{5, 0, {1, 2, 3}, 0, 0, 0}, h2g(gpu.queue)};
  auto *mobile = vg::vrapi_EnterVrMode(&mode.mode);
  REQUIRE(mobile);
  auto barrier = [&](VkImage image, VkImageLayout old, VkImageLayout next, std::uint32_t mip_levels) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = old; b.newLayout = next; b.image = image;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.srcAccessMask = old == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, layers};
    test::vk_call("vkCmdPipelineBarrier", gpu.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &b);
  };
  // Tightly packed level-major texels: red is the memory row, green the layer and level.
  std::vector<VkBufferImageCopy> regions;
  std::vector<std::uint8_t> texels;
  for (std::uint32_t level = 0; level < levels; ++level) {
    const auto extent = size >> level;
    VkBufferImageCopy region{};
    region.bufferOffset = texels.size();
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers};
    region.imageExtent = {extent, extent, 1};
    regions.push_back(region);
    for (std::uint32_t layer = 0; layer < layers; ++layer)
      for (std::uint32_t y = 0; y < extent; ++y)
        for (std::uint32_t x = 0; x < extent; ++x)
          texels.insert(texels.end(), {std::uint8_t(y * 40 + x), std::uint8_t(layer * 16 + level),
                                       0, 255});
  }
  test::VulkanBuffer upload(gpu, texels.size());
  std::memcpy(upload.mapped, texels.data(), texels.size());
  gpu.begin();
  barrier(source, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels);
  test::vk_call("vkCmdCopyBufferToImage", gpu.command, upload.buffer, source,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, std::uint32_t(regions.size()), regions.data());
  barrier(source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, levels);
  gpu.submit();
  for (bool single_host : {false, true}) {
  CAPTURE(single_host);
  const auto host_levels = single_host ? 1u : levels;
  test::VulkanImage destination(gpu, VK_FORMAT_R8G8B8A8_UNORM, size, size, host_levels, layers,
      0, nullptr, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  test::VulkanBuffer readback(gpu, texels.size());
  auto &native_ring = *vr::state().rings.at(ring);
  for (bool bottom_up : {true, false}) {
    CAPTURE(bottom_up);
    auto copy_desc = native_ring.desc;
    copy_desc.host_mip_count = host_levels;
    REQUIRE(xr::xr_images()->copy_to_image(native_ring.textures[0], destination.image,
                                           copy_desc, 0, bottom_up) == XR_SUCCESS);
    gpu.begin();
    barrier(destination.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, host_levels);
    test::vk_call("vkCmdCopyImageToBuffer", gpu.command, destination.image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
                  host_levels, regions.data());
    barrier(destination.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, host_levels);
    gpu.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    gpu.submit();
    const auto *bytes = static_cast<const std::uint8_t *>(readback.mapped);
    std::size_t mismatches = 0;
    for (std::uint32_t level = 0; level < host_levels; ++level) {
      const auto extent = size >> level;
      for (std::uint32_t layer = 0; layer < layers; ++layer)
        for (std::uint32_t y = 0; y < extent; ++y)
          for (std::uint32_t x = 0; x < extent; ++x) {
            // Destination row 0 is the displayed top; a bottom-up source shows
            // its memory row 0 at the bottom.
            const auto row = bottom_up ? extent - 1 - y : y;
            const auto *texel = bytes + regions[level].bufferOffset +
                                ((layer * extent + y) * extent + x) * 4;
            mismatches += texel[0] != row * 40 + x || texel[1] != layer * 16 + level;
          }
    }
    CHECK(mismatches == 0);
  }
  }
  vg::vrapi_LeaveVrMode(mobile);
  vg::vrapi_DestroyTextureSwapChain(ring);
  vg::vrapi_DestroySystemVulkan();
  vg::vrapi_Shutdown();
}

TEST_CASE("VrApi copies a slot shown with both row orders into separate host chains") {
  venture::test::XrImageFixture images;
  venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 51, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  auto *ring = vg::vrapi_CreateTextureSwapChain3(0, 0x8058, 4, 4, 1, 1);
  REQUIRE(ring);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto *mobile = vg::vrapi_EnterVrMode(&mode);
  REQUIRE(mobile);
  // A GLES eye projection (T grows upward) and a helper-built quad (T grows
  // downward) sampling the same image in one frame.
  vr::abi::Projection eye_buffer{}, quad{};
  eye_buffer.header = {1, 0, {1, 1, 1, 1}, 1, 0, 0};
  quad.header = {1, 0, {1, 1, 1, 1}, 2, 5, 0};
  for (auto *layer : {&eye_buffer, &quad}) {
    layer->head.pose.orientation.w = 1;
    for (auto &texture : layer->textures) {
      texture.chain = ring;
      texture.rect = {0, 0, 1, 1};
    }
  }
  for (auto &texture : eye_buffer.textures) {
    texture.tan_angles.m[0][0] = texture.tan_angles.m[1][1] = .5f;
    texture.tan_angles.m[0][2] = texture.tan_angles.m[1][2] = -.5f;
    texture.tan_angles.m[2][2] = -1;
  }
  for (auto &texture : quad.textures) texture.tan_angles = unit_square(.3f, 2, .5f);
  const vr::abi::LayerHeader *both[]{&eye_buffer.header, &quad.header};
  vr::abi::Submit submit{0, 1, 1, 0, 0x1234, 2, both};
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  REQUIRE(images.destinations.size() == 2);
  CHECK(images.destinations[0].second);
  CHECK_FALSE(images.destinations[1].second);
  CHECK(images.destinations[0].first != images.destinations[1].first);
  auto &native = *vr::state().rings.at(ring);
  const auto up = native.host[vr::Ring::host_chain(0, true)].handle;
  const auto down = native.host[vr::Ring::host_chain(0, false)].handle;
  CHECK(up);
  CHECK(down);
  CHECK(up != down);
  // Within a frame each (slot, row order) is copied once.
  const vr::abi::LayerHeader *same[]{&eye_buffer.header, &eye_buffer.header};
  submit = {0, 1, 2, 0, 0x1234, 2, same};
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  REQUIRE(images.destinations.size() == 3);
  CHECK(images.destinations[2].second);
  vg::vrapi_LeaveVrMode(mobile);
  vg::vrapi_DestroyTextureSwapChain(ring);
  vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr);
}

TEST_CASE("VrApi constant projective layers copy their texel across both eye frusta") {
  venture::test::XrImageFixture images;
  venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 51, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  auto *ring = vg::vrapi_CreateTextureSwapChain3(0, 0x8058, 1, 1, 1, 1);
  REQUIRE(ring);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto *mobile = vg::vrapi_EnterVrMode(&mode);
  REQUIRE(mobile);
  vr::abi::Projection projection{};
  projection.header = {1, 0, {1, 1, 1, 1}, 2, 5, 0};
  projection.head.pose.orientation.w = 1;
  XrView eyes[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  for (int eye = 0; eye < 2; ++eye) {
    // UE4/OVRPlugin's transition layer: a unit-square transform at the eye
    // produces a constant UV outside the texture, with no texture-rect clip.
    auto &texture = projection.textures[eye];
    texture.chain = ring;
    texture.rect = {0, 0, 1, 1};
    texture.tan_angles.m[0][2] = eye ? -7.4000006f : 5.4000006f;
    texture.tan_angles.m[1][2] = -1;
    texture.tan_angles.m[2][2] = -2;
    texture.tan_angles.m[3][3] = 1;
    eyes[eye].pose = {{0, 0, 0, 1}, {eye ? .032f : -.032f, 0, 0}};
    eyes[eye].fov = {-.8f - eye * .1f, .9f, .7f, -.6f};
  }
  // The guest's predicted head pose must not world-lock a full-view color.
  projection.head.pose.position = {.5f, 1.6f, -2};
  projection.head.pose.orientation = {0, .38268343f, 0, .92387953f};
  vr::LayerStorage storage;
  std::vector<const XrCompositionLayerBaseHeader *> layers;
  unsigned supplied = 0;
  auto image = [&](vr::abi::TextureSwapChain *source, int slot, vr::abi::Rect, int, bool,
                   XrSwapchainSubImage &) {
    CHECK(source == ring);
    CHECK(slot == 0);
    ++supplied;
    return XR_SUCCESS;
  };
  REQUIRE(vr::translate_layer(projection.header, eyes, image, storage, layers) == XR_SUCCESS);
  CHECK(supplied == 2);
  REQUIRE(layers.size() == 1);
  CHECK(storage.projection.layerFlags == (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                          XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT));
  CHECK(storage.projection.space == vr::state().view);
  for (int eye = 0; eye < 2; ++eye) {
    CHECK(std::memcmp(&storage.views[eye].fov, &eyes[eye].fov, sizeof(XrFovf)) == 0);
    CHECK(std::memcmp(&storage.views[eye].pose, &eyes[eye].pose, sizeof(XrPosef)) == 0);
  }
  const vr::abi::LayerHeader *headers[]{&projection.header};
  vr::abi::Submit submit{0, 1, 1, 0, 0x1234, 1, headers};
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  CHECK(images.copies == 1);
  CHECK(images.last_fence == 0x1234);
  const auto pixel = images.read_pixel();
  CHECK(pixel[0] == 64);
  CHECK(pixel[2] == 191);
  CHECK(pixel[3] == 255);
  // Other degenerate transforms cannot use this constant-texel conversion.
  auto &native = *vr::state().rings.at(ring);
  for (int invalid = 0; invalid < 4; ++invalid) {
    if (invalid == 0) native.desc.width = 2;
    if (invalid == 1) native.sampler.wrap_s = vr::abi::TextureWrap::clamp_border;
    if (invalid == 2) projection.header.flags = 16;
    if (invalid == 3) projection.textures[0].tan_angles.m[2][2] = 0;
    layers.clear();
    CHECK(vr::translate_layer(projection.header, eyes, image, storage, layers) == XR_ERROR_VALIDATION_FAILURE);
    CHECK(layers.empty());
    native.desc.width = 1;
    native.sampler.wrap_s = vr::abi::TextureWrap::clamp_edge;
    projection.header.flags = 0;
  }
  vg::vrapi_LeaveVrMode(mobile);
  vg::vrapi_DestroyTextureSwapChain(ring);
  vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr);
}

TEST_CASE("VrApi inverse alpha layers invert alpha through color scale and bias") {
  venture::test::XrImageFixture images;
  venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 52, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  auto *ring = vg::vrapi_CreateTextureSwapChain3(0, 0x8058, 32, 32, 1, 1);
  REQUIRE(ring);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto *mobile = vg::vrapi_EnterVrMode(&mode);
  REQUIRE(mobile);
  // RE4's fade layer: ONE_MINUS_SRC_ALPHA source, SRC_ALPHA destination.
  vr::abi::Projection projection{};
  projection.header = {1, 16, {1, 1, 1, .5f}, 5, 2, 0};
  projection.head.pose.orientation.w = 1;
  XrView eyes[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  for (int eye = 0; eye < 2; ++eye) {
    auto &texture = projection.textures[eye];
    texture.chain = ring;
    texture.rect = {0, 0, 1, 1};
    texture.tan_angles.m[0][0] = .5f;
    texture.tan_angles.m[1][1] = -.5f;
    texture.tan_angles.m[0][2] = texture.tan_angles.m[1][2] = -.5f;
    texture.tan_angles.m[2][2] = -1;
    eyes[eye].pose.orientation.w = 1;
    eyes[eye].fov = {-.8f, .8f, .8f, -.8f};
  }
  vr::LayerStorage storage;
  std::vector<const XrCompositionLayerBaseHeader *> layers;
  auto image = [](vr::abi::TextureSwapChain *, int, vr::abi::Rect, int, bool, XrSwapchainSubImage &) {
    return XR_SUCCESS;
  };
  REQUIRE(vr::translate_layer(projection.header, eyes, image, storage, layers) == XR_SUCCESS);
  REQUIRE(layers.size() == 1);
  CHECK(storage.projection.layerFlags == (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                          XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT));
  CHECK(storage.projection.next == &storage.color);
  // a' = 1 - a * scale: source-alpha blending then weighs source by 1 - a.
  CHECK(storage.color.colorScale.a == -.5f);
  CHECK(storage.color.colorBias.a == 1);
  CHECK(storage.color.colorScale.r == 1);
  CHECK(storage.color.colorBias.r == 0);
  // A layer the host cannot show is left out; the frame still displays.
  vr::abi::Projection unsupported = projection;
  unsupported.header.src_blend = 3;
  unsupported.header.dst_blend = 0;
  projection.header.color_scale = {1, 1, 1, 1};
  projection.header.src_blend = 1;
  projection.header.dst_blend = 0;
  const vr::abi::LayerHeader *headers[]{&unsupported.header, &projection.header};
  vr::abi::Submit submit{0, 1, 1, 0, 0, 2, headers};
  const auto before = ventureMockXrStats();
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  CHECK(images.copies == 1);
  CHECK(ventureMockXrStats().ends == before.ends + 1);
  CHECK(ventureMockXrStats().projection == before.projection + 1);
  vg::vrapi_LeaveVrMode(mobile);
  vg::vrapi_DestroyTextureSwapChain(ring);
  vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr);
}

namespace {
// Exercise the optional host sampler extension without changing the mock's
// advertised capabilities. All other XR calls still reach the real mock loader.
struct SamplerHost {
  void *previous = venture::xr::host_xr().get_instance_proc_addr;
  inline static PFN_xrGetInstanceProcAddr fallback{};
  inline static XrSwapchainStateSamplerVulkanFB last{};
  inline static XrResult response = XR_SUCCESS;
  inline static unsigned updates = 0;
  static XrResult XRAPI_CALL update(XrSwapchain chain, const XrSwapchainStateBaseHeaderFB *state) {
    CHECK(chain != XR_NULL_HANDLE);
    REQUIRE(state->type == XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB);
    last = *reinterpret_cast<const XrSwapchainStateSamplerVulkanFB *>(state);
    ++updates;
    return response;
  }
  static XrResult XRAPI_CALL get(XrInstance instance, const char *name, PFN_xrVoidFunction *out) {
    if (std::strcmp(name, "xrUpdateSwapchainFB") != 0) return fallback(instance, name, out);
    *out = reinterpret_cast<PFN_xrVoidFunction>(update);
    return XR_SUCCESS;
  }
  SamplerHost() {
    fallback = reinterpret_cast<PFN_xrGetInstanceProcAddr>(previous);
    response = XR_SUCCESS;
    updates = 0;
    auto &host = venture::xr::host_xr();
    host.enabled_extensions.insert("XR_FB_swapchain_update_state");
    host.enabled_extensions.insert("XR_FB_swapchain_update_state_vulkan");
    host.get_instance_proc_addr = reinterpret_cast<void *>(get);
  }
  ~SamplerHost() {
    auto &host = venture::xr::host_xr();
    host.get_instance_proc_addr = previous;
    host.enabled_extensions.erase("XR_FB_swapchain_update_state");
    host.enabled_extensions.erase("XR_FB_swapchain_update_state_vulkan");
  }
};
} // namespace

TEST_CASE(
    "VrApi owns persistent rings and reuses predictions across explicit and implicit frames") {
  venture::test::XrImageFixture images;
  venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  CHECK(vg::vrapi_GetSystemPropertyInt(nullptr, 12) == 256); // legacy Quest headset family
  CHECK(vg::vrapi_GetSystemPropertyInt(nullptr, 129) == 1); // sRGB layer bridge
  CHECK(vg::vrapi_GetSystemPropertyFloat(nullptr, 7) == doctest::Approx(90));
  CHECK(vg::vrapi_GetSystemPropertyFloat(nullptr, 8) == doctest::Approx(90));
  CHECK(vg::vrapi_GetSystemStatusInt(nullptr, 0) == 1); // attached before VR entry
  auto *ring = vg::vrapi_CreateTextureSwapChain3(0, 0x8058, 64, 64, 1, 3);
  REQUIRE(ring);
  auto texture = vg::vrapi_GetTextureSwapChainHandle(ring, 1);
  REQUIRE(texture);
  struct {
    vr::abi::TextureSamplerState sampler;
    std::uint32_t guard = 0xfeedface;
  } queried{};
  REQUIRE(vg::vrapi_GetTextureSwapChainSamplerState(ring, &queried.sampler) == 0);
  CHECK(queried.guard == 0xfeedface);
  CHECK(queried.sampler.min_filter == vr::abi::TextureFilter::linear);
  CHECK(queried.sampler.mag_filter == vr::abi::TextureFilter::linear);
  CHECK(queried.sampler.wrap_s == vr::abi::TextureWrap::clamp_edge);
  CHECK(queried.sampler.max_anisotropy == 1);
  auto sampler = queried.sampler;
  sampler.wrap_s = vr::abi::TextureWrap::clamp_border;
  sampler.wrap_t = vr::abi::TextureWrap::repeat;
  sampler.border_color[0] = 0.25f;
  REQUIRE(vg::vrapi_SetTextureSwapChainSamplerState(ring, &sampler) == 0);
  auto bad_sampler = sampler;
  bad_sampler.min_filter = vr::abi::TextureFilter(8);
  CHECK(vg::vrapi_SetTextureSwapChainSamplerState(ring, &bad_sampler) == vr::abi::invalid);
  bad_sampler = sampler;
  bad_sampler.mag_filter = vr::abi::TextureFilter::linear_mipmap_linear;
  CHECK(vg::vrapi_SetTextureSwapChainSamplerState(ring, &bad_sampler) == vr::abi::invalid);
  bad_sampler = sampler;
  bad_sampler.wrap_t = vr::abi::TextureWrap(-1);
  CHECK(vg::vrapi_SetTextureSwapChainSamplerState(ring, &bad_sampler) == vr::abi::invalid);
  CHECK(vg::vrapi_SetTextureSwapChainSamplerState(ring, nullptr) == vr::abi::invalid);
  CHECK(vg::vrapi_GetTextureSwapChainSamplerState(nullptr, &queried.sampler) == vr::abi::invalid);
  REQUIRE(vg::vrapi_GetTextureSwapChainSamplerState(ring, &queried.sampler) == 0);
  CHECK(std::memcmp(&sampler, &queried.sampler, sizeof(sampler)) == 0);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto *mobile = vg::vrapi_EnterVrMode(&mode);
  REQUIRE(mobile);
  CHECK(vg::vrapi_SetRemoteEmulation(nullptr, false) == vr::abi::invalid);
  CHECK(vg::vrapi_SetRemoteEmulation(mobile, false) == vr::abi::success);
  for (std::uint32_t id : {1u, 2u}) {
    vr::abi::RemoteCapabilities caps{};
    caps.header = {4, id};
    REQUIRE(vg::vrapi_GetInputDeviceCapabilities(mobile, &caps.header) == vr::abi::success);
    CHECK((caps.capabilities & 0x4000u) != 0); // ModelOculusTouch
    CHECK((caps.capabilities & 0x0800u) == 0); // no ModelGearVR emulation target
  }
  CHECK(vg::vrapi_SetRemoteEmulation(mobile, true) == vr::abi::success);
  CHECK(vg::vrapi_SetRemoteEmulation(mobile, false) == vr::abi::success);
  vr::abi::Event event{};
  REQUIRE(vg::vrapi_PollEvent(&event) == 0);
  CHECK(event.type == 2); // visibility gained
  REQUIRE(vg::vrapi_PollEvent(&event) == 0);
  CHECK(event.type == 4); // focus gained
  CHECK(vg::vrapi_PollEvent(&event) == vr::abi::event_unavailable);
  CHECK(event.type == 0);
  CHECK(vg::vrapi_PollEvent(&event) == vr::abi::event_unavailable);
  {
    RefreshHost host;
    CHECK(vg::vrapi_GetSystemPropertyFloat(nullptr, 4) == doctest::Approx(144));
    CHECK(vg::vrapi_GetSystemPropertyInt(nullptr, 64) == 1);
    float listed = 0;
    CHECK(vg::vrapi_GetSystemPropertyFloatArray(nullptr, 65, &listed, 1) == 1);
    CHECK(listed == doctest::Approx(144));
    CHECK(vg::vrapi_SetDisplayRefreshRate(mobile, 0) == vr::abi::success);
    CHECK(vg::vrapi_SetDisplayRefreshRate(mobile, 144) == vr::abi::success);
    CHECK(vg::vrapi_SetDisplayRefreshRate(mobile, 72) == vr::abi::unsupported);
    CHECK(RefreshHost::requests == 0);
    CHECK(RefreshHost::enumerations == 0);
  }
  venture::xr::session().display_timing.reset();
  auto before = ventureMockXrStats();
  double initial_prediction = vg::vrapi_GetPredictedDisplayTime(mobile, 0);
  REQUIRE(initial_prediction > 0);
  double time = vg::vrapi_GetPredictedDisplayTime(mobile, 1);
  REQUIRE(time > 0);
  CHECK(time > initial_prediction);
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 1) > 0);
  CHECK(ventureMockXrStats().waits == before.waits);
  CHECK(vg::vrapi_WaitFrame(mobile, 1) == 0);
  time = vg::vrapi_GetPredictedDisplayTime(mobile, 1); // Wait supplies the host prediction.
  CHECK(time > 0);
  std::promise<void> waiting;
  auto ready = waiting.get_future();
  auto pipelined_wait = std::async(std::launch::async, [&] {
    venture::GuestThread::adopt_current();
    waiting.set_value();
    return vg::vrapi_WaitFrame(mobile, 2);
  });
  ready.wait();
  CHECK(pipelined_wait.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  CHECK(ventureMockXrStats().waits == before.waits + 1);
  auto tracking = vg::vrapi_GetPredictedTracking2(mobile, time);
  CHECK(tracking.status == 143);
  CHECK(tracking.head.pose.position.y == doctest::Approx(0)); // Eye-level LOCAL origin.
  CHECK(vg::vrapi_BeginFrame(mobile, 2) == vr::abi::invalid_operation);
  CHECK(vg::vrapi_BeginFrame(mobile, 1) == 0);
  REQUIRE(pipelined_wait.get() == 0);
  CHECK(ventureMockXrStats().waits == before.waits + 2);
  CHECK(vg::vrapi_BeginFrame(mobile, 1) == vr::abi::invalid_operation);
  vr::abi::Projection projection{};
  projection.header = {1, 2, {1, 1, 1, 1}, 1, 0, 0};
  projection.head = tracking.head;
  for (auto &eye : projection.textures) {
    eye.chain = ring;
    eye.index = 1;
    eye.rect = {0, 0, 1, 1};
    eye.tan_angles.m[0][0] = eye.tan_angles.m[1][1] = 0.5f;
    eye.tan_angles.m[0][2] = eye.tan_angles.m[1][2] = -0.5f;
    eye.tan_angles.m[2][2] = -1;
  }
  const vr::abi::LayerHeader *layers[]{&projection.header};
  vr::abi::Submit submit{0, 1, 1, time, 0x1234, 1, layers};
  submit.frame_index = 2;
  CHECK(vg::vrapi_SubmitFrame2(mobile, &submit) == vr::abi::invalid_operation);
  submit.frame_index = 1;
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  CHECK(images.copies == 1);
  CHECK(images.last_texture == texture);
  CHECK(images.last_destination != VK_NULL_HANDLE);
  {
    SamplerHost host;
    auto changed = sampler;
    changed.min_filter = vr::abi::TextureFilter::cubic_mipmap_linear;
    changed.max_anisotropy = 2;
    REQUIRE(vg::vrapi_SetTextureSwapChainSamplerState(ring, &changed) == 0);
    CHECK(host.updates == 1);
    CHECK(host.last.minFilter == VK_FILTER_CUBIC_EXT);
    CHECK(host.last.magFilter == VK_FILTER_LINEAR);
    CHECK(host.last.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR);
    CHECK(host.last.wrapModeS == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    CHECK(host.last.wrapModeT == VK_SAMPLER_ADDRESS_MODE_REPEAT);
    CHECK(host.last.borderColor.r == 0.25f);
    CHECK(host.last.maxAnisotropy == 2);
    host.response = XR_ERROR_FEATURE_UNSUPPORTED;
    CHECK(vg::vrapi_SetTextureSwapChainSamplerState(ring, &sampler) == vr::abi::unsupported);
    REQUIRE(vg::vrapi_GetTextureSwapChainSamplerState(ring, &queried.sampler) == 0);
    CHECK(std::memcmp(&changed, &queried.sampler, sizeof(changed)) == 0);
    host.response = XR_SUCCESS;
    REQUIRE(vg::vrapi_SetTextureSwapChainSamplerState(ring, &sampler) == 0);
  }
  CHECK(images.last_fence == 0x1234);
  auto pixel = images.read_pixel();
  CHECK(pixel[0] == 64);
  CHECK((pixel[1] == 127 || pixel[1] == 128));
  CHECK(pixel[2] == 191);
  CHECK(pixel[3] == 255);
  CHECK(ventureMockXrStats().waits == before.waits + 2);
  CHECK(ventureMockXrStats().acquired == before.acquired + 1);
  submit.frame_index = 2;
  double second_prediction = vg::vrapi_GetPredictedDisplayTime(mobile, 2);
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 3) > second_prediction);
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 2) == second_prediction);
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0); // no explicit wait/begin
  CHECK(ventureMockXrStats().waits == before.waits + 2);
  CHECK(images.copies == 2);
  auto rejected = projection;
  rejected.textures[0].tan_angles.m[0][0] = 0;
  const vr::abi::LayerHeader *bad_layers[]{&rejected.header};
  auto bad_submit = submit;
  bad_submit.frame_index = 3;
  bad_submit.layers = bad_layers;
  auto ends = ventureMockXrStats().ends;
  CHECK(vg::vrapi_SubmitFrame2(mobile, &bad_submit) == vr::abi::invalid);
  CHECK(ventureMockXrStats().ends == ends + 1); // empty end balances the rejected host frame
  submit.frame_index = 4;
  std::int32_t next_result = vr::abi::unavailable;
  std::thread next_producer([&] { next_result = vg::vrapi_SubmitFrame2(mobile, &submit); });
  next_producer.join();
  CHECK(next_result == vr::abi::success);
  CHECK(images.copies == 3);
  // PhaseSync: main-thread prediction N+1 can finish while N is being
  // rendered. Beginning N twice must not consume the pending wait for N+1.
  REQUIRE(vg::vrapi_WaitFrame(mobile, 5) == 0);
  const double time5 = vg::vrapi_GetPredictedDisplayTime(mobile, 5);
  REQUIRE(vg::vrapi_BeginFrame(mobile, 5) == 0);
  REQUIRE(vg::vrapi_WaitFrame(mobile, 6) == 0);
  const double time6 = vg::vrapi_GetPredictedDisplayTime(mobile, 6);
  CHECK(time6 > time5);
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 5) == time5);
  CHECK(vg::vrapi_BeginFrame(mobile, 5) == vr::abi::invalid_operation);
  submit.frame_index = 5;
  submit.display_time = time5;
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  REQUIRE(vg::vrapi_BeginFrame(mobile, 6) == 0);
  submit.frame_index = 6;
  submit.display_time = time6;
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  // Prediction on a simulation thread must not bind/consume N+1 while N is
  // explicitly bound, even before Begin(N). Backward prediction uses period.
  REQUIRE(vg::vrapi_WaitFrame(mobile, 7) == 0);
  const auto time7 = vg::vrapi_GetPredictedDisplayTime(mobile, 7);
  const auto waits7 = ventureMockXrStats().waits;
  double future = 0, past = 0;
  std::thread simulation([&] {
    future = vg::vrapi_GetPredictedDisplayTime(mobile, 8);
    past = vg::vrapi_GetPredictedDisplayTime(mobile, 6);
    CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, -1) == 0);
  });
  simulation.join();
  CHECK(future - time7 == doctest::Approx(0.011111111).epsilon(0.0001));
  CHECK(time7 - past == doctest::Approx(0.011111111).epsilon(0.0001));
  CHECK(ventureMockXrStats().waits == waits7);
  REQUIRE(vg::vrapi_BeginFrame(mobile, 7) == 0);
  std::int32_t wait8 = vr::abi::unavailable;
  double time8 = 0;
  std::thread pipeline([&] {
    wait8 = vg::vrapi_WaitFrame(mobile, 8);
    time8 = vg::vrapi_GetPredictedDisplayTime(mobile, 8);
    CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 7) == time7);
    CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 9) > time8);
  });
  pipeline.join();
  REQUIRE(wait8 == 0);
  CHECK(ventureMockXrStats().waits == waits7 + 1);
  CHECK(vg::vrapi_BeginFrame(mobile, 7) == vr::abi::invalid_operation);
  bad_submit.frame_index = 7;
  bad_submit.display_time = time7;
  ends = ventureMockXrStats().ends;
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &bad_submit) == vr::abi::invalid);
  CHECK(ventureMockXrStats().ends == ends + 1);
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 8) == time8);
  submit.frame_index = 9;
  CHECK(vg::vrapi_SubmitFrame2(mobile, &submit) == vr::abi::invalid_operation);
  submit.frame_index = 8;
  submit.display_time = time8;
  std::thread recovery([&] {
    REQUIRE(vg::vrapi_BeginFrame(mobile, 8) == 0);
    next_result = vg::vrapi_SubmitFrame2(mobile, &submit);
  });
  recovery.join();
  CHECK(next_result == 0);
  CHECK(ventureMockXrStats().waits == waits7 + 1); // Reserved wait survived discard.
  CHECK(ventureMockXrStats().ends == ends + 2);
  vr::abi::InputHeader device{};
  CHECK(vg::vrapi_EnumerateInputDevices(mobile, 0, &device) == 0);
  CHECK(device.id == 1);
  vr::abi::RemoteState remote{};
  remote.header.type = 4;
  CHECK(vg::vrapi_GetCurrentInputState(mobile, 1, &remote.header) == 0);
  CHECK(remote.trigger == doctest::Approx(0.25));
  // ovrTracking is 104 bytes; nothing after it may be written (ovrTracking2 is 360).
  struct {
    vr::abi::TrackingHead tracking;
    std::array<std::uint8_t, 256> canary;
  } guarded{};
  guarded.canary.fill(0xA5);
  CHECK(vg::vrapi_GetInputTrackingState(mobile, 1, time, &guarded.tracking) == 0);
  CHECK(std::all_of(guarded.canary.begin(), guarded.canary.end(), [](auto b) { return b == 0xA5; }));
  // Controller motion must reach ovrRigidBodyPosef, not just the location.
  MockXrScript moving{};
  REQUIRE(vr::sync_input() == XR_SUCCESS);
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  moving.left.grip.position.x += 0.2f;
  moving.left.grip.orientation = {0, 0.70710678f, 0, 0.70710678f};
  ventureMockXrSetScript(&moving);
  REQUIRE(vr::sync_input() == XR_SUCCESS);
  REQUIRE(vg::vrapi_GetInputTrackingState(mobile, 1, time, &guarded.tracking) == 0);
  CHECK(guarded.tracking.head.linear_velocity.x > 0);
  CHECK(guarded.tracking.head.angular_velocity.y > 0);
  const auto motion = guarded.tracking.head.linear_velocity.x;
  REQUIRE(vg::vrapi_GetInputTrackingState(mobile, 1, time, &guarded.tracking) == 0);
  CHECK(guarded.tracking.head.linear_velocity.x == motion);
  REQUIRE(vr::sync_input() == XR_SUCCESS);
  REQUIRE(vg::vrapi_GetInputTrackingState(mobile, 1, time, &guarded.tracking) == 0);
  CHECK(guarded.tracking.head.linear_velocity.x == 0);
  CHECK(std::abs(guarded.tracking.head.angular_velocity.y) < 1e-3f);  // float noise through the legacy-pose offset
  MockXrScript stationary{};
  ventureMockXrSetScript(&stationary);
  REQUIRE(vr::sync_input() == XR_SUCCESS);
  auto pulses = ventureMockXrStats().haptic_pulses;
  std::array<std::uint8_t, 64> samples{};
  samples.fill(255);
  vr::abi::HapticBuffer buffer{0, std::uint32_t(samples.size()), true, samples.data()};
  CHECK(vg::vrapi_SetHapticVibrationBuffer(mobile, 1, &buffer) == 0);
  REQUIRE(ventureMockXrAwaitHaptics(pulses + 1, 200));
  vr::cancel_haptics();
  const auto expired_before = vr::expired_haptic_buffers();
  pulses = ventureMockXrStats().haptic_pulses;
  buffer.time = double(venture::monotonic_ns()) * 1e-9 - 1;
  buffer.count = 1;
  CHECK(vg::vrapi_SetHapticVibrationBuffer(mobile, 1, &buffer) == 0);
  const auto expiry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (vr::expired_haptic_buffers() == expired_before && std::chrono::steady_clock::now() < expiry_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(vr::expired_haptic_buffers() == expired_before + 1);
  CHECK(ventureMockXrStats().haptic_pulses == pulses);
  buffer.count = 257;
  CHECK(vg::vrapi_SetHapticVibrationBuffer(mobile, 1, &buffer) == vr::abi::invalid);
  CHECK(vg::vrapi_SetHapticVibrationSimple(mobile, 1, 0) == 0);
  pulses = ventureMockXrStats().haptic_pulses;
  const float overdrive = 96.3344497680664f;
  CHECK(vg::vrapi_SetHapticVibrationSimple(mobile, 1, overdrive) == 0);
  CHECK(ventureMockXrStats().haptic_pulses == pulses + 1);
  CHECK(vg::vrapi_SetHapticVibrationSimple(mobile, 1, -0.00035392120480537415f) == 0);
  CHECK(vg::vrapi_SetHapticVibrationSimple(mobile, 1, -0.5f) == 0);
  CHECK(vg::vrapi_SetHapticVibrationSimple(mobile, 1, std::nanf("")) == vr::abi::invalid);
  CHECK(ventureMockXrStats().haptic_pulses == pulses + 1);
  CHECK(vg::vrapi_SetHapticVibrationSimple(mobile, 1, 0) == 0);
  vr::abi::HandPose hand{};
  hand.header.version = vr::abi::hand_version;
  CHECK(vg::vrapi_GetHandPose(mobile, 3, time, &hand.header) == 0);
  CHECK(hand.status == 1);
  CHECK(hand.rotations[0].w == doctest::Approx(1));
  // OVRPlugin's hand-tracking-enabled query is driven by VrApi enumeration.
  MockXrScript hands_only{};
  hands_only.left.active = hands_only.right.active = false;
  ventureMockXrSetScript(&hands_only);
  REQUIRE(vg::vrapi_WaitFrame(mobile, 9) == 0); // snapshots controller action activity
  REQUIRE(vg::vrapi_EnumerateInputDevices(mobile, 0, &device) == 0);
  CHECK(device.type == 32);
  CHECK(device.id == 3);
  hands_only.hand_tracking_active = false;
  ventureMockXrSetScript(&hands_only);
  CHECK(vg::vrapi_EnumerateInputDevices(mobile, 0, &device) == vr::abi::no_device);
  CHECK(vg::vrapi_GetHandPose(mobile, 3, time, &hand.header) == 0);
  CHECK(hand.status == 0);
  MockXrScript restored{};
  ventureMockXrSetScript(&restored);
  auto skeleton = std::make_unique<vr::abi::HandSkeleton>();
  skeleton->version = vr::abi::hand_version;
  CHECK(vg::vrapi_GetHandSkeleton(mobile, 1, &skeleton->version) == 0);
  CHECK(skeleton->bone_count == 24);
  CHECK(skeleton->parents[0] == -1);
  auto mesh = std::make_unique<vr::abi::HandMesh>();
  mesh->version = vr::abi::hand_version;
  CHECK(vg::vrapi_GetHandMesh(mobile, 1, &mesh->version) == 0);
  REQUIRE(mesh->vertex_count > 0);
  for (std::uint32_t i = 0; i < mesh->index_count; ++i)
    CHECK(std::uint32_t(mesh->indices[i]) < mesh->vertex_count);
  // The native OVR wrist axes must not rotate the tracked fingertips or the
  // fallback mesh away from their OpenXR positions.
  auto xr_pose = [](vr::abi::Pose p) -> XrPosef {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
            {p.position.x, p.position.y, p.position.z}};
  };
  for (int side = 1; side <= 2; ++side) {
    REQUIRE(vg::vrapi_GetHandPose(mobile, side + 2, time, &hand.header) == 0);
    REQUIRE(vg::vrapi_GetHandSkeleton(mobile, side, &skeleton->version) == 0);
    const auto root = xr_pose(hand.root);
    const auto forward = venture::xr::rotate(root.orientation, {side == 1 ? 1.f : -1.f, 0, 0});
    CHECK(forward.x == doctest::Approx(0).epsilon(0.00001));
    CHECK(forward.z == doctest::Approx(-1));
    const auto &model = venture::xr::hand_model(side == 1);
    std::array<XrPosef, 24> bones;
    for (int bone = 0; bone < 24; ++bone) {
      auto parent = skeleton->parents[bone];
      bones[bone] = venture::xr::compose(parent < 0 ? root : bones[parent], xr_pose(skeleton->poses[bone]));
      const auto expected = model.joints[venture::xr::ovr_joint_map[bone]].position;
      CHECK(bones[bone].position.x - root.position.x == doctest::Approx(expected.x).epsilon(0.00001));
      CHECK(bones[bone].position.y - root.position.y == doctest::Approx(expected.y).epsilon(0.00001));
      CHECK(bones[bone].position.z - root.position.z == doctest::Approx(expected.z).epsilon(0.00001));
    }
    REQUIRE(vg::vrapi_GetHandMesh(mobile, side, &mesh->version) == 0);
    const auto vertex = mesh->positions[0];
    const auto transformed = venture::xr::rotate(root.orientation, {vertex.x, vertex.y, vertex.z});
    CHECK(transformed.x == doctest::Approx(model.positions[0].x).epsilon(0.00001));
    CHECK(transformed.y == doctest::Approx(model.positions[0].y).epsilon(0.00001));
    CHECK(transformed.z == doctest::Approx(model.positions[0].z).epsilon(0.00001));
  }
  // A runtime thumb that follows XR_EXT_hand_tracking (-Z along each bone) bent out of the palm:
  // the guest skeleton driven by GetHandPose must point its thumb bones where the runtime's do.
  {
    const auto &model = venture::xr::hand_model(true);
    auto joints = model.joints;
    auto toward = [](XrVector3f d) {  // shortest rotation taking -Z onto d
      const float n = venture::xr::length(d);
      d = {d.x / n, d.y / n, d.z / n};
      XrQuaternionf q{d.y, -d.x, 0, 1 - d.z};  // cross(-Z, d), 1 + dot(-Z, d)
      const float m = std::sqrt(q.x * q.x + q.y * q.y + q.w * q.w);
      return XrQuaternionf{q.x / m, q.y / m, q.z / m, q.w / m};
    };
    const XrVector3f bends[3]{{0.3f, 0.2f, -1}, {0.1f, 0.6f, -1}, {-0.2f, 0.9f, -1}};
    for (int k = 0; k < 3; ++k) {
      const int joint = XR_HAND_JOINT_THUMB_METACARPAL_EXT + k;
      const float n = venture::xr::length(bends[k]);
      const auto a = joints[joint].position;
      joints[joint].orientation = toward(bends[k]);
      joints[joint + 1].position = {a.x + 0.03f * bends[k].x / n, a.y + 0.03f * bends[k].y / n,
                                    a.z + 0.03f * bends[k].z / n};
    }
    joints[XR_HAND_JOINT_THUMB_TIP_EXT].orientation = joints[XR_HAND_JOINT_THUMB_DISTAL_EXT].orientation;
    MockXrScript bent{};
    bent.left_hand_joints = joints;
    ventureMockXrSetScript(&bent);
    REQUIRE(vg::vrapi_GetHandPose(mobile, 3, time, &hand.header) == 0);
    REQUIRE(hand.status == 1);
    REQUIRE(vg::vrapi_GetHandSkeleton(mobile, 1, &skeleton->version) == 0);
    const auto root = xr_pose(hand.root);
    std::array<XrPosef, 24> bones;
    for (int bone = 0; bone < 24; ++bone) {
      const auto parent = skeleton->parents[bone];
      const auto &r = hand.rotations[bone];
      bones[bone] = parent < 0 ? root
                               : venture::xr::compose(bones[parent], {{r.x, r.y, r.z, r.w},
                                                                      {skeleton->poses[bone].position.x,
                                                                       skeleton->poses[bone].position.y,
                                                                       skeleton->poses[bone].position.z}});
    }
    // OVR Thumb2, Thumb3 and ThumbTip end the runtime's metacarpal, proximal and distal bones.
    for (int bone : {4, 5, 19}) {
      CAPTURE(bone);
      const auto parent = skeleton->parents[bone];
      const auto g0 = bones[parent].position, g1 = bones[bone].position;
      const XrVector3f guest{g1.x - g0.x, g1.y - g0.y, g1.z - g0.z};
      const auto h0 = joints[venture::xr::ovr_joint_map[parent]].position,
                 h1 = joints[venture::xr::ovr_joint_map[bone]].position;
      // The default script's wrist is unrotated (checked above), so wrist-relative joint offsets
      // are already in the space of the guest root.
      const XrVector3f host{h1.x - h0.x, h1.y - h0.y, h1.z - h0.z};
      const float gl = venture::xr::length(guest), hl = venture::xr::length(host);
      CHECK(guest.x / gl == doctest::Approx(host.x / hl).epsilon(0.0001));
      CHECK(guest.y / gl == doctest::Approx(host.y / hl).epsilon(0.0001));
      CHECK(guest.z / gl == doctest::Approx(host.z / hl).epsilon(0.0001));
    }
    MockXrScript plain{};
    ventureMockXrSetScript(&plain);
  }
  std::uint32_t count = 0;
  CHECK(vg::vrapi_GetBoundaryGeometry(mobile, 0, &count, nullptr) == 0);
  CHECK(count == 4);
  MockXrScript lost{};
  lost.active = false;
  ventureMockXrSetScript(&lost);
  tracking = vg::vrapi_GetPredictedTracking2(mobile, time);
  CHECK((tracking.status & 15) == 0);
  MockXrScript active{};
  ventureMockXrSetScript(&active);
  vg::vrapi_RecenterPose(mobile);
  tracking = vg::vrapi_GetPredictedTracking2(mobile, time);
  CHECK(tracking.head.pose.position.y == doctest::Approx(0));
  vg::vrapi_LeaveVrMode(mobile);
  CHECK(vg::vrapi_SetRemoteEmulation(mobile, false) == vr::abi::invalid);
  CHECK(ventureMockXrStats().live_images == 0);
  CHECK(vg::vrapi_GetTextureSwapChainHandle(ring, 1) == texture); // survives session recreation
  REQUIRE(vg::vrapi_GetTextureSwapChainSamplerState(ring, &queried.sampler) == 0);
  CHECK(std::memcmp(&sampler, &queried.sampler, sizeof(sampler)) == 0);
  mobile = vg::vrapi_EnterVrMode(&mode);
  REQUIRE(mobile);
  submit.frame_index = 3;
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 0) > 0);
  CHECK(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  vg::vrapi_LeaveVrMode(mobile);
  vg::vrapi_DestroyTextureSwapChain(ring);
  vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr);
}
TEST_CASE("VrApi binds explicit waits and preserves queued waits on descriptor rejection") {
  venture::test::XrImageFixture images;
  venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto *mobile = vg::vrapi_EnterVrMode(&mode);
  REQUIRE(mobile);
  REQUIRE(vg::vrapi_GetPredictedDisplayTime(mobile, 0) > 0);
  CHECK(vg::vrapi_BeginFrame(mobile, 5) == vr::abi::invalid_operation);
  REQUIRE(vg::vrapi_WaitFrame(mobile, 5) == 0);
  const auto predicted = vg::vrapi_GetPredictedDisplayTime(mobile, 5);
  REQUIRE(vg::vrapi_BeginFrame(mobile, 5) == 0);
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 5) == predicted);
  vr::abi::Submit submit{0, 1, 5, predicted, 0, 0, nullptr};
  std::int32_t submitted = vr::abi::unavailable;
  std::thread renderer([&] { submitted = vg::vrapi_SubmitFrame2(mobile, &submit); });
  renderer.join();
  REQUIRE(submitted == 0); // UE4 begins on its game thread and submits on its RHI thread.
  for (int reject = 0; reject < 3; ++reject) {
    const auto index = std::uint64_t(6 + reject * 2);
    REQUIRE(vg::vrapi_WaitFrame(mobile, index) == 0);
    REQUIRE(vg::vrapi_BeginFrame(mobile, index) == 0);
    REQUIRE(vg::vrapi_WaitFrame(mobile, index + 1) == 0);
    const auto next_time = vg::vrapi_GetPredictedDisplayTime(mobile, index + 1);
    auto before = ventureMockXrStats();
    auto bad = submit;
    bad.frame_index = index;
    if (reject == 0) bad.layer_count = 1; // Missing layer array.
    if (reject == 1) bad.flags = 1; // Unsupported flag.
    if (reject == 2) bad.swap_interval = 2; // Host cadence: accepted, and must not steal the queued wait.
    std::thread reject_renderer([&] { submitted = vg::vrapi_SubmitFrame2(mobile, &bad); });
    reject_renderer.join();
    CHECK(submitted == (reject == 0 ? vr::abi::invalid : reject == 1 ? vr::abi::unsupported : 0));
    CHECK(ventureMockXrStats().ends == before.ends + 1);
    CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, index + 1) == next_time);
    submit.frame_index = index + 1;
    submit.display_time = next_time;
    REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
    CHECK(ventureMockXrStats().waits == before.waits);
    CHECK(ventureMockXrStats().ends == before.ends + 2);
  }
  // The host runtime sets the cadence: a larger swap interval renders every
  // host frame instead of waiting extra host frames on repeated content.
  const auto before = ventureMockXrStats();
  submit.frame_index = 12;
  submit.swap_interval = 2;
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  CHECK(ventureMockXrStats().waits == before.waits + 1);
  CHECK(ventureMockXrStats().ends == before.ends + 1);
  submit.frame_index = 13;
  submit.swap_interval = 1;
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  // Observed UE4 splash handoff: Wait(1197), then Wait(1336), with no
  // Begin(1197). The abandoned host wait must not strand the game thread once
  // 1197 is unbegun long past its display time.
  REQUIRE(vg::vrapi_WaitFrame(mobile, 1197) == 0);
  const auto handoff = ventureMockXrStats();
  const auto abandoned_after = std::chrono::steady_clock::now();
  auto resumed = std::async(std::launch::async, [&] {
    venture::GuestThread::adopt_current();
    return vg::vrapi_WaitFrame(mobile, 1336);
  });
  const bool ready = resumed.wait_for(venture::test::hang_timeout) == std::future_status::ready;
  if (!ready) vg::vrapi_LeaveVrMode(mobile); // Wake a broken implementation before asserting.
  const auto wait_result = resumed.get();
  INFO("abandoned wait resumed=", ready, " result=", wait_result,
       " host waits before=", handoff.waits, " now=", ventureMockXrStats().waits);
  REQUIRE(ready);
  REQUIRE(wait_result == 0);
  // Retired only after the abandonment bound, not on the index gap alone.
  CHECK(std::chrono::steady_clock::now() - abandoned_after >=
        std::chrono::nanoseconds(vr::abandoned_frame_ns - 100'000'000));
  CHECK(ventureMockXrStats().waits == handoff.waits + 1);
  CHECK(ventureMockXrStats().begins == handoff.begins + 1);
  CHECK(ventureMockXrStats().ends == handoff.ends + 1);
  CHECK(vg::vrapi_BeginFrame(mobile, 1197) == vr::abi::invalid_operation);
  REQUIRE(vg::vrapi_BeginFrame(mobile, 1336) == 0);
  submit.frame_index = 1336;
  submit.display_time = vg::vrapi_GetPredictedDisplayTime(mobile, 1336);
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  CHECK(ventureMockXrStats().ends == handoff.ends + 2);
  // Wait/Begin(N), Wait(N+1), Begin(N+1) deliberately abandons begun N.
  REQUIRE(vg::vrapi_WaitFrame(mobile, 1666) == 0);
  REQUIRE(vg::vrapi_BeginFrame(mobile, 1666) == 0);
  REQUIRE(vg::vrapi_WaitFrame(mobile, 1667) == 0);
  const auto abandoned = ventureMockXrStats();
  REQUIRE(vg::vrapi_BeginFrame(mobile, 1667) == 0);
  CHECK(ventureMockXrStats().begins == abandoned.begins + 1);
  CHECK(ventureMockXrStats().ends == abandoned.ends); // xrBeginFrame returns XR_FRAME_DISCARDED.
  submit.frame_index = 1666;
  CHECK(vg::vrapi_SubmitFrame2(mobile, &submit) == vr::abi::invalid_operation);
  REQUIRE(vg::vrapi_WaitFrame(mobile, 1668) == 0);
  submit.frame_index = 1667;
  submit.display_time = vg::vrapi_GetPredictedDisplayTime(mobile, 1667);
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  REQUIRE(vg::vrapi_BeginFrame(mobile, 1668) == 0);
  submit.frame_index = 1668;
  submit.display_time = vg::vrapi_GetPredictedDisplayTime(mobile, 1668);
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  vg::vrapi_LeaveVrMode(mobile);
  vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr);
}

namespace {
// One VrApi GLES session on the mock runtime, with frame helpers.
struct VrApiFrames {
  venture::test::XrImageFixture images;
  vr::abi::Mobile *mobile{};
  vr::abi::Submit submit{0, 1, 0, 0, 0, 0, nullptr};
  VrApiFrames() {
    venture::xr::install_xr_images(&images);
    vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
    REQUIRE(vg::vrapi_Initialize(&init) == 0);
    vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
    mobile = vg::vrapi_EnterVrMode(&mode);
    REQUIRE(mobile);
    drain();
  }
  ~VrApiFrames() {
    if (vr::state().mobile) vg::vrapi_LeaveVrMode(mobile);
    vg::vrapi_Shutdown();
    venture::xr::install_xr_images(nullptr);
  }
  void drain() {
    vr::abi::Event event{};
    while (vg::vrapi_PollEvent(&event) == vr::abi::success) {}
  }
  std::int32_t submit_frame(std::uint64_t index) {
    submit.frame_index = index;
    submit.display_time = vg::vrapi_GetPredictedDisplayTime(mobile, std::int64_t(index));
    return vg::vrapi_SubmitFrame2(mobile, &submit);
  }
  std::future<std::int32_t> wait_on_guest(std::uint64_t index) {
    return std::async(std::launch::async, [this, index] {
      venture::GuestThread::adopt_current();
      return vg::vrapi_WaitFrame(mobile, index);
    });
  }
  // Wakes a broken implementation's blocked waiter before asserting.
  bool settle(std::future<std::int32_t> &future) {
    const bool ready = future.wait_for(venture::test::hang_timeout) == std::future_status::ready;
    if (!ready) {
      const auto stats = ventureMockXrStats();
      FAIL_CHECK("VrApi waiter did not settle: host waits=", stats.waits,
                 " begins=", stats.begins, " ends=", stats.ends);
      vg::vrapi_LeaveVrMode(mobile);
    }
    return ready;
  }
};
} // namespace
TEST_CASE("VrApi concurrent predictions leave legacy pacing in submission") {
  using namespace std::chrono_literals;
  VrApiFrames frames;
  const auto before = ventureMockXrStats();
  const auto binding = vr::state().frame_index;
  frames.submit.frame_index = 1030;
  struct Prediction { double time; vr::abi::Tracking tracking; };
  std::future<std::int32_t> submitting;
  std::future<Prediction> queries[2];
  // Release before future destruction even when an assertion throws.
  struct ReleaseWait { ~ReleaseWait() { ventureMockXrGateWait(false); } } release;
  ventureMockXrGateWait(true);
  submitting = std::async(std::launch::async, [&] {
    venture::GuestThread::adopt_current();
    return vg::vrapi_SubmitFrame2(frames.mobile, &frames.submit);
  });
  const bool blocked = ventureMockXrAwaitGatedWait(2000);
  for (int slot = 0; slot < 2; ++slot) {
    queries[slot] = std::async(std::launch::async, [&, slot] {
      venture::GuestThread::adopt_current();
      const auto time = vg::vrapi_GetPredictedDisplayTime(frames.mobile, 1030 + slot);
      return Prediction{time, vg::vrapi_GetPredictedTracking2(frames.mobile, time)};
    });
  }
  const bool ready0 = queries[0].wait_for(2s) == std::future_status::ready;
  const bool ready1 = queries[1].wait_for(2s) == std::future_status::ready;
  bool unchanged = false;
  if (ready0 && ready1) {
    std::lock_guard lock(vr::state().mutex);
    std::lock_guard sl(venture::xr::session().mutex);
    unchanged = vr::state().wait_in_progress && !vr::state().token &&
        !vr::state().next_token && !vr::state().frame_bound &&
        vr::state().frame_index == binding;
  }
  const auto during = ventureMockXrStats();
  const bool submission_blocked = submitting.wait_for(0s) == std::future_status::timeout;
  ventureMockXrGateWait(false);
  const auto result = submitting.get();
  const auto first = queries[0].get(), second = queries[1].get();
  REQUIRE(blocked);
  CHECK(ready0);
  CHECK(ready1);
  CHECK(unchanged);
  CHECK(submission_blocked);
  CHECK(during.waits == before.waits);
  CHECK(during.begins == before.begins);
  CHECK(during.ends == before.ends);
  for (const auto &query : {first, second}) {
    CHECK(query.time > 0);
    CHECK(std::abs(query.tracking.head.time - query.time) <= 2e-9);
  }
  REQUIRE(result == 0);
  CHECK(vr::state().frame_index == 1030);
  CHECK(vr::state().prediction_index == 1030);
  CHECK_FALSE(vr::state().token);
  CHECK_FALSE(vr::state().next_token);
  CHECK(ventureMockXrStats().waits == before.waits + 1);
  CHECK(ventureMockXrStats().begins == before.begins + 1);
  CHECK(ventureMockXrStats().ends == before.ends + 1);
  REQUIRE(frames.submit_frame(1031) == 0);
  CHECK(ventureMockXrStats().waits == before.waits + 2);
  CHECK(ventureMockXrStats().begins == before.begins + 2);
  CHECK(ventureMockXrStats().ends == before.ends + 2);
}
TEST_CASE("VrApi submissions use their native waited frame time") {
  VrApiFrames frames;
  const auto before = ventureMockXrStats();
  const auto prediction = vg::vrapi_GetPredictedDisplayTime(frames.mobile, 9000);
  REQUIRE(prediction > 10);
  frames.submit.frame_index = 9000;
  frames.submit.display_time = prediction - 5;
  bool pipelined = false;
  XrTime current = 0, next = 0;
  SUBCASE("Legacy prediction became stale during rendering") {}
  SUBCASE("Guest prediction is further ahead than the native target") {
    frames.submit.display_time = prediction + 5;
  }
  SUBCASE("PhaseSync preserves the current target while the next wait finishes") {
    pipelined = true;
    REQUIRE(vg::vrapi_WaitFrame(frames.mobile, 9000) == 0);
    current = venture::xr::session().frame.predictedDisplayTime;
    REQUIRE(vg::vrapi_BeginFrame(frames.mobile, 9000) == 0);
    REQUIRE(vg::vrapi_WaitFrame(frames.mobile, 9001) == 0);
    next = venture::xr::session().pending_frame.predictedDisplayTime;
    REQUIRE(next > current);
  }
  const auto requested = frames.submit.display_time;
  REQUIRE(vg::vrapi_SubmitFrame2(frames.mobile, &frames.submit) == 0);
  if (!pipelined) current = venture::xr::session().frame.predictedDisplayTime;
  CHECK(ventureMockXrLastDisplayTime() == current);
  CHECK(frames.submit.display_time == requested); // Do not rewrite guest storage.
  CHECK_FALSE(vr::state().final_frame);
  CHECK(ventureMockXrStats().ends == before.ends + 1);
  frames.submit.frame_index = 9001;
  if (pipelined) REQUIRE(vg::vrapi_BeginFrame(frames.mobile, 9001) == 0);
  REQUIRE(vg::vrapi_SubmitFrame2(frames.mobile, &frames.submit) == 0);
  if (!pipelined) next = venture::xr::session().frame.predictedDisplayTime;
  CHECK(ventureMockXrLastDisplayTime() == next);
  CHECK(next > current);
  CHECK(ventureMockXrStats().waits == before.waits + 2);
  CHECK(ventureMockXrStats().begins == before.begins + 2);
  CHECK(ventureMockXrStats().ends == before.ends + 2);
  CHECK_FALSE(vr::state().token);
  CHECK_FALSE(vr::state().next_token);
}
TEST_CASE("VrApi discards host-rejected frames without stranding current or queued bindings") {
  MockXrScript script{};
  struct RestoreScript { ~RestoreScript() { MockXrScript defaults{}; ventureMockXrSetScript(&defaults); } } restore;
  VrApiFrames frames;
  const auto before = ventureMockXrStats();
  const auto before_errors = venture::xr::session().end_errors;
  bool pipelined = false;
  SUBCASE("Legacy runtime consumes rejected end") {}
  SUBCASE("Legacy runtime retains rejected end") { script.retain_failed_end = true; }
  SUBCASE("PhaseSync runtime consumes rejected end") { pipelined = true; }
  SUBCASE("PhaseSync runtime retains rejected end") { pipelined = true; script.retain_failed_end = true; }
  if (pipelined) {
    REQUIRE(vg::vrapi_WaitFrame(frames.mobile, 7000) == 0);
    REQUIRE(vg::vrapi_BeginFrame(frames.mobile, 7000) == 0);
    REQUIRE(vg::vrapi_WaitFrame(frames.mobile, 7001) == 0);
  }
  script.end_frame_result = XR_ERROR_TIME_INVALID;
  ventureMockXrSetScript(&script);
  frames.submit.frame_index = 7000;
  CHECK(vg::vrapi_SubmitFrame2(frames.mobile, &frames.submit) == vr::abi::unavailable);
  CHECK(ventureMockXrStats().ends == before.ends); // Never count a rejected frame.
  CHECK(venture::xr::session().end_errors == before_errors + 1); // No second EndFrame.
  CHECK(venture::xr::session().phase != venture::xr::Session::Phase::begun);
  CHECK(vr::state().token == pipelined);
  CHECK_FALSE(vr::state().next_token);
  script.end_frame_result = XR_SUCCESS;
  ventureMockXrSetScript(&script);
  // Keep the original failure visible, but recover on the next real frame.
  frames.submit.frame_index = 7001;
  if (pipelined) REQUIRE(vg::vrapi_BeginFrame(frames.mobile, 7001) == 0);
  REQUIRE(vg::vrapi_SubmitFrame2(frames.mobile, &frames.submit) == 0);
  CHECK(ventureMockXrStats().waits == before.waits + 2);
  CHECK(ventureMockXrStats().begins == before.begins + 2);
  CHECK(ventureMockXrStats().ends == before.ends + 1);
  CHECK_FALSE(vr::state().token);
  CHECK_FALSE(vr::state().next_token);
}
TEST_CASE("VrApi swap intervals keep the host cadence and its predictions") {
  MockXrScript script{};
  // Keep the queried interval ahead of the host clock without realtime sleeps.
  script.display_period = 100'000'000;
  ventureMockXrSetScript(&script);
  struct RestoreScript {
    ~RestoreScript() { MockXrScript defaults{}; ventureMockXrSetScript(&defaults); }
  } restore;
  VrApiFrames frames;
  for (std::uint32_t interval = 2; interval <= 4; ++interval) {
    CAPTURE(interval);
    const auto before = ventureMockXrStats();
    const auto index = 2000 + interval * 2;
    frames.submit.frame_index = index;
    frames.submit.swap_interval = interval;
    frames.submit.display_time = 0;
    REQUIRE(vg::vrapi_SubmitFrame2(frames.mobile, &frames.submit) == 0);
    const auto repeat = venture::xr::session().frame;
    CHECK(vr::state().frame_index == index);
    CHECK_FALSE(vr::state().token);
    CHECK(vr::state().prediction_index == index);
    CHECK(vr::state().predicted_time == repeat.predictedDisplayTime);
    CHECK(vr::state().predicted_period == repeat.predictedDisplayPeriod);
    const auto next = vg::vrapi_GetPredictedDisplayTime(frames.mobile, index + 1);
    CHECK(std::abs(next - vr::seconds_from_time(
        repeat.predictedDisplayTime + repeat.predictedDisplayPeriod)) <= 2e-9);
    CHECK(next > vr::seconds_from_time(repeat.predictedDisplayTime));
    CHECK(std::abs(vg::vrapi_GetPredictedTracking2(frames.mobile, next).head.time - next) <= 2e-9);
    REQUIRE(vg::vrapi_WaitFrame(frames.mobile, index + 1) == 0);
    CHECK(std::abs(next - vr::seconds_from_time(
        venture::xr::session().frame.predictedDisplayTime)) <= 2e-9);
    frames.submit.frame_index = index + 1;
    frames.submit.swap_interval = 1;
    frames.submit.display_time = next;
    REQUIRE(vg::vrapi_SubmitFrame2(frames.mobile, &frames.submit) == 0);
    // One host frame per application frame: the runtime sets the rate.
    CHECK(ventureMockXrStats().waits == before.waits + 2);
    CHECK(ventureMockXrStats().begins == before.begins + 2);
    CHECK(ventureMockXrStats().ends == before.ends + 2);
  }
}
TEST_CASE("VrApi waits resume after a runtime-initiated session restart") {
  using namespace std::chrono_literals;
  const auto before_mount = vg::vrapi_GetSystemStatusInt(nullptr, 13);
  VrApiFrames vr_frames;
  // STOPPING between WaitFrame(N) and BeginFrame(N), then READY: pump_events
  // ends and restarts the host session, which resets the shared frame state,
  // so the waited N no longer gates the next wait.
  REQUIRE(vg::vrapi_WaitFrame(vr_frames.mobile, 15) == 0);
  const auto mounted_recenter = vg::vrapi_GetSystemStatusInt(nullptr, 13);
  CHECK(mounted_recenter == before_mount + 1);
  // Polling a still-mounted headset is not another recenter. FOCUSED and
  // VISIBLE both describe a mounted headset, so focus gain is not one either.
  vr_frames.drain();
  CHECK(vg::vrapi_GetSystemStatusInt(nullptr, 13) == mounted_recenter);
  ventureMockXrRestartSession();
  vr_frames.drain();
  CHECK(vg::vrapi_GetSystemStatusInt(nullptr, 13) == mounted_recenter + 1);
  CHECK(venture::xr::session().running);
  CHECK_FALSE(venture::xr::session().pending_wait);
  CHECK(vg::vrapi_BeginFrame(vr_frames.mobile, 15) == vr::abi::invalid_operation);
  auto resumed = vr_frames.wait_on_guest(16);
  REQUIRE(vr_frames.settle(resumed));
  REQUIRE(resumed.get() == 0);
  REQUIRE(vr_frames.submit_frame(16) == 0);
}
TEST_CASE("Host Activity quit stops the VrApi session without EXITING") {
  VrApiFrames vr_frames;
  REQUIRE(venture::xr::session().request_host_stop() == XR_SUCCESS);
  vr_frames.drain();
  CHECK(venture::xr::session().state == XR_SESSION_STATE_IDLE);
  CHECK_FALSE(venture::xr::session().running);
}
TEST_CASE("VrApi keeps a lagging renderer's frame and retires only abandoned ones") {
  using namespace std::chrono_literals;
  VrApiFrames vr_frames;
  auto *mobile = vr_frames.mobile;
  // The simulation thread skips an index and waits N+2 before the renderer
  // begins N. N stays submittable; the newer wait queues behind Begin(N) as
  // in the consecutive PhaseSync case.
  REQUIRE(vg::vrapi_WaitFrame(mobile, 10) == 0);
  auto skipped = vr_frames.wait_on_guest(12);
  CHECK(skipped.wait_for(100ms) == std::future_status::timeout);
  const auto lagging = ventureMockXrStats();
  REQUIRE(vg::vrapi_BeginFrame(mobile, 10) == 0);
  REQUIRE(vr_frames.settle(skipped));
  REQUIRE(skipped.get() == 0);
  CHECK(ventureMockXrStats().ends == lagging.ends); // 10 was not retired empty.
  REQUIRE(vr_frames.submit_frame(10) == 0);
  REQUIRE(vr_frames.submit_frame(12) == 0);
  // A renderer slower than the abandonment bound: N-1 completes long after
  // N's predicted display time. N becomes ready only then and must survive
  // the simulation thread's Wait(N+1) until the renderer begins it.
  REQUIRE(vg::vrapi_WaitFrame(mobile, 20) == 0);
  REQUIRE(vg::vrapi_BeginFrame(mobile, 20) == 0);
  REQUIRE(vg::vrapi_WaitFrame(mobile, 21) == 0);
  auto behind = vr_frames.wait_on_guest(22);
  std::this_thread::sleep_for(std::chrono::nanoseconds(vr::abandoned_frame_ns) + 300ms);
  CHECK(behind.wait_for(0ms) == std::future_status::timeout);
  REQUIRE(vr_frames.submit_frame(20) == 0);
  const auto slow = ventureMockXrStats();
  CHECK(behind.wait_for(300ms) == std::future_status::timeout);
  CHECK(ventureMockXrStats().ends == slow.ends); // 21 was not retired empty.
  REQUIRE(vg::vrapi_BeginFrame(mobile, 21) == 0);
  REQUIRE(vr_frames.settle(behind));
  REQUIRE(behind.get() == 0);
  REQUIRE(vr_frames.submit_frame(21) == 0);
  REQUIRE(vr_frames.submit_frame(22) == 0);
  // An unbegun N followed by the consecutive Wait(N+1) is retired once N is
  // stale, instead of blocking the simulation thread forever.
  REQUIRE(vg::vrapi_WaitFrame(mobile, 13) == 0);
  const auto stranded = ventureMockXrStats();
  auto consecutive = vr_frames.wait_on_guest(14);
  REQUIRE(vr_frames.settle(consecutive));
  REQUIRE(consecutive.get() == 0);
  CHECK(ventureMockXrStats().begins == stranded.begins + 1);
  CHECK(ventureMockXrStats().ends == stranded.ends + 1);
  CHECK(vg::vrapi_BeginFrame(mobile, 13) == vr::abi::invalid_operation);
  REQUIRE(vr_frames.submit_frame(14) == 0);
}
TEST_CASE("VrApi cylinder sub-rects sit where the full layer shows their rows") {
  VrApiFrames vr_frames; // Host cylinder extension.
  auto &api = vr::state().init.graphics_api;
  const auto saved = api;
  vr::abi::Cylinder cylinder{};
  cylinder.header = {3, 0, {1, 1, 1, 1}, 1, 0, 0};
  cylinder.head.pose.orientation.w = 1;
  for (auto &t : cylinder.textures)
    for (int i = 0; i < 4; ++i) t.tan_angles.m[i][i] = t.texture_matrix.m[i][i] = 1;
  XrView eyes[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  for (auto &eye : eyes) eye.pose.orientation.w = 1;
  std::vector<bool> rows;
  auto image = [&](vr::abi::TextureSwapChain *, int, vr::abi::Rect, int, bool bottom_up,
                   XrSwapchainSubImage &) {
    rows.push_back(bottom_up);
    return XR_SUCCESS;
  };
  const float half = 0.5f * std::tan(std::numbers::pi_v<float> / 6);
  // GLES reverses rows, so memory row 0 (the first half) is the bottom of the
  // full layer; Vulkan keeps memory order, so the first half is its top.
  for (auto [graphics, bottom_up, first] :
       {std::tuple{0x10300, true, -half}, std::tuple{vr::abi::vulkan_api, false, half}}) {
    CAPTURE(graphics);
    api = graphics;
    for (auto [y, expected] : {std::pair{0.f, first}, std::pair{0.5f, -first}}) {
      CAPTURE(y);
      for (auto &t : cylinder.textures) t.rect = {0, y, 1, 0.5f};
      vr::LayerStorage storage;
      std::vector<const XrCompositionLayerBaseHeader *> layers;
      rows.clear();
      REQUIRE(vr::translate_layer(cylinder.header, eyes, image, storage, layers) == XR_SUCCESS);
      CHECK(rows == std::vector<bool>{bottom_up, bottom_up});
      for (const auto &layer : storage.cylinder) {
        CHECK(layer.pose.position.y == doctest::Approx(expected));
        CHECK(layer.centralAngle == doctest::Approx(std::numbers::pi_v<float>));
      }
    }
  }
  api = saved;
}
TEST_CASE("VrApi flipped texture rects stay inside odd-height images") {
  VrApiFrames vr_frames;
  auto *ring = vg::vrapi_CreateTextureSwapChain3(0, 0x8058, 32, 33, 1, 1);
  REQUIRE(ring);
  vr::abi::Projection projection{};
  projection.header = {1, 0, {1, 1, 1, 1}, 1, 0, 0};
  projection.head.pose.orientation.w = 1;
  for (auto &texture : projection.textures) {
    texture.chain = ring;
    texture.tan_angles.m[0][0] = texture.tan_angles.m[1][1] = 0.5f;
    texture.tan_angles.m[0][2] = texture.tan_angles.m[1][2] = -0.5f;
    texture.tan_angles.m[2][2] = -1;
    texture.rect = {0, 0, 1, 0.5f}; // GLES bottom half: memory rows 0-16.5.
  }
  const vr::abi::LayerHeader *layers[]{&projection.header};
  vr_frames.submit.layer_count = 1;
  vr_frames.submit.layers = layers;
  const auto before = ventureMockXrStats();
  REQUIRE(vr_frames.submit_frame(1) == 0);
  CHECK(ventureMockXrStats().projection == before.projection + 1);
  // Rounding both edges keeps the reversed rect's rows [17, 33) in the image;
  // rounding offset and extent separately gave 17 + 17 rows.
  XrSwapchainSubImage host[2]{};
  REQUIRE(ventureMockXrLastSubImages(host, 2) == 2);
  for (const auto &sub : host) {
    const auto &rect = sub.imageRect;
    CHECK(rect.offset.x == 0);
    CHECK(rect.extent.width == 32);
    CHECK(rect.offset.y == 17);
    CHECK(rect.extent.height == 16);
  }
}
TEST_CASE("VrApi gives the renderer the full abandonment bound once the next wait blocks") {
  using namespace std::chrono_literals;
  VrApiFrames vr_frames;
  auto *mobile = vr_frames.mobile;
  const auto stall = std::chrono::nanoseconds(vr::abandoned_frame_ns) + 300ms;
  // A prediction during a long load must not reserve a host frame. WaitFrame
  // obtains N, then simulation waits N+1 before the renderer begins N.
  REQUIRE(vg::vrapi_GetPredictedDisplayTime(mobile, 0) > 0);
  std::this_thread::sleep_for(stall);
  REQUIRE(vg::vrapi_WaitFrame(mobile, 1) == 0);
  const auto bound = ventureMockXrStats();
  auto pipelined = vr_frames.wait_on_guest(2);
  CHECK(pipelined.wait_for(300ms) == std::future_status::timeout);
  CHECK(ventureMockXrStats().ends == bound.ends);
  REQUIRE(vg::vrapi_BeginFrame(mobile, 1) == 0);
  REQUIRE(vr_frames.settle(pipelined));
  REQUIRE(pipelined.get() == 0);
  CHECK(ventureMockXrStats().ends == bound.ends); // 1 was not retired empty.
  REQUIRE(vr_frames.submit_frame(1) == 0);
  REQUIRE(vr_frames.submit_frame(2) == 0);
  // The simulation alone holds N past the bound (a synchronous load or GC),
  // then waits N+1; the renderer begins N right after.
  REQUIRE(vg::vrapi_WaitFrame(mobile, 10) == 0);
  std::this_thread::sleep_for(stall);
  const auto stalled = ventureMockXrStats();
  auto next = vr_frames.wait_on_guest(11);
  CHECK(next.wait_for(100ms) == std::future_status::timeout);
  CHECK(ventureMockXrStats().ends == stalled.ends);
  REQUIRE(vg::vrapi_BeginFrame(mobile, 10) == 0);
  REQUIRE(vr_frames.settle(next));
  REQUIRE(next.get() == 0);
  CHECK(ventureMockXrStats().ends == stalled.ends); // 10 was not retired empty.
  REQUIRE(vr_frames.submit_frame(10) == 0);
  REQUIRE(vr_frames.submit_frame(11) == 0);
}
TEST_CASE("VrApi legacy tracking transforms preserve repeated floor and eye selection") {
  venture::test::XrImageFixture images;
  venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto *mobile = vg::vrapi_EnterVrMode(&mode);
  REQUIRE(mobile);
  const auto time = vg::vrapi_GetPredictedDisplayTime(mobile, 0);
  REQUIRE(time > 0);

  // VrApi_Types.h: 0 identity, 1 current, 2 system eye, 3 system floor.
  // The mock starts with the head 1.6 m above STAGE and at the LOCAL origin.
  for (int repeat = 0; repeat < 3; ++repeat) {
    auto floor = vg::vrapi_GetTrackingTransform(mobile, 3);
    CHECK(floor.position.y == doctest::Approx(-1.6f));
    vg::set_tracking_transform(mobile, &floor);
    auto current = vg::vrapi_GetTrackingTransform(mobile, 1);
    CHECK(current.position.y == doctest::Approx(-1.6f));
    auto tracking = vg::vrapi_GetPredictedTracking2(mobile, time);
    CHECK(tracking.head.pose.position.y == doctest::Approx(1.6f));
    auto identity = vg::vrapi_GetTrackingTransform(mobile, 0);
    CHECK(identity.orientation.w == doctest::Approx(1));
    CHECK(identity.position.y == doctest::Approx(0));
  }
  auto eye = vg::vrapi_GetTrackingTransform(mobile, 2);
  CHECK(eye.position.y == doctest::Approx(0));
  vg::set_tracking_transform(mobile, &eye);
  CHECK(vg::vrapi_GetPredictedTracking2(mobile, time).head.pose.position.y == doctest::Approx(0));

  // Explicit tracking-space selection changes the base used by SetTrackingTransform.
  REQUIRE(vg::vrapi_SetTrackingSpace(mobile, 3) == 0);
  eye = vg::vrapi_GetTrackingTransform(mobile, 2);
  CHECK(eye.position.y == doctest::Approx(1.6f));
  vg::set_tracking_transform(mobile, &eye);
  CHECK(vg::vrapi_GetPredictedTracking2(mobile, time).head.pose.position.y == doctest::Approx(0));
  auto floor = vg::vrapi_GetTrackingTransform(mobile, 3);
  CHECK(floor.position.y == doctest::Approx(0));
  vg::set_tracking_transform(mobile, &floor);
  CHECK(vg::vrapi_GetPredictedTracking2(mobile, time).head.pose.position.y == doctest::Approx(1.6f));

  vg::vrapi_LeaveVrMode(mobile);
  vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr);
}
TEST_CASE("VrApi analytic layers use real host swapchains and final-frame state") {
  venture::test::XrImageFixture images;
  venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  auto ring = vg::vrapi_CreateTextureSwapChain3(0, 0x8058, 32, 32, 1, 3);
  auto cube_ring = vg::vrapi_CreateTextureSwapChain3(3, 0x8058, 32, 32, 1, 3);
  REQUIRE(ring);
  REQUIRE(cube_ring);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto mobile = vg::vrapi_EnterVrMode(&mode);
  REQUIRE(mobile);
  auto before = ventureMockXrStats();
  auto identity = [] {
    vr::abi::Matrix m{};
    for (int i = 0; i < 4; ++i)
      m.m[i][i] = 1;
    return m;
  };
  vr::abi::Cylinder cylinder{};
  cylinder.header = {3, 0, {1, 1, 1, 1}, 1, 0, 0};
  cylinder.head.pose.orientation.w = 1;
  for (auto &t : cylinder.textures) {
    t.chain = ring;
    t.index = 1;
    t.rect = {0, 0, 1, 1};
    t.tan_angles = identity();
    t.texture_matrix = identity();
  }
  vr::abi::Cube cube{};
  cube.header = {4, 0, {1, 1, 1, 1}, 1, 0, 0};
  cube.head.pose.orientation.w = 1;
  cube.tan_angles = identity();
  for (auto &t : cube.textures) {
    t.chain = cube_ring;
    t.index = 2;
  }
  vr::abi::Equirect eq{};
  eq.header = {5, 0, {1, 1, 1, 1}, 1, 0, 0};
  eq.head.pose.orientation.w = 1;
  eq.tan_angles = identity();
  for (auto &t : eq.textures) {
    t.chain = ring;
    t.index = 1;
    t.rect = {0, 0, 1, 1};
    t.texture_matrix = identity();
  }
  vr::abi::LoadingIcon icon{{6, 0, {1, 1, 1, 1}, 2, 5, 0}, 1, 16, ring, 1};
  const vr::abi::LayerHeader *layers[]{&cylinder.header, &cube.header, &eq.header, &icon.header};
  vr::abi::Submit submit{4, 1, 1, 0, 0, 4, layers};
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  auto after = ventureMockXrStats();
  CHECK(after.cylinder == before.cylinder + 2);
  CHECK(after.cube == before.cube + 2);
  CHECK(after.equirect == before.equirect + 2);
  CHECK(after.quad == before.quad + 1);
  CHECK(images.copies == 2);
  submit.frame_index = 2;
  CHECK(vg::vrapi_SubmitFrame2(mobile, &submit) == vr::abi::invalid_operation);
  CHECK(vg::vrapi_GetSystemStatusFloat(nullptr, 8) == 1);
  vg::vrapi_LeaveVrMode(mobile);
  vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr);
}
TEST_CASE("VrApi aliases and AAPCS64 aggregate returns are guest thunks") {
  vr::register_libraries();
  auto *library = venture::Linker::get().open("libvrapi.so", 0, nullptr);
  REQUIRE(library);
  CHECK(library->find_symbol("vrapi_GetHandPose") ==
        library->find_symbol("_Z17vrapi_GetHandPoseP9ovrMobilejdP18ovrHandPoseHeader_"));
  auto address = library->find_symbol("vrapi_GetPredictedTracking2");
  REQUIRE(address);
  auto code = venture::g2h<const std::uint32_t>(address);
  auto id = ((code[0] >> 5) & 0xffff) | (((code[1] >> 5) & 0xffff) << 16);
  venture::test::FakeCpu cpu;
  vr::abi::Tracking result;
  std::memset(&result, 0xff, sizeof(result));
  cpu.xs[8] = venture::h2g(&result);
  auto &info = venture::hle::function(id);
  venture::hle::CallContext ctx(*reinterpret_cast<venture::GuestThread *>(&cpu), cpu, info);
  info.handler(ctx);
  CHECK(result.status == 0);
  CHECK(result.head.pose.position.y == 0);
  vr::integration::Init init{1, 1, 16, 0, {1, 2, 3}};
  CHECK(vr::integration::initialize(&init) == 0);
  CHECK(vr::integration::set_thread_performance(42, 2) == 0);
  CHECK(vr::integration::set_thread_performance(42, 3) == vr::abi::invalid);
  vr::integration::shutdown();
}

TEST_CASE("host XR identity uses game label and preserves UTF-8 boundaries") {
  auto info = venture::xr::application_identity("Cook\u2019s \u4e16\u754c", "test.identity", 42);
  CHECK(std::string(info.applicationName) == "Cook\u2019s \u4e16\u754c");
  CHECK(std::string(info.engineName) == "test.identity");
  CHECK(info.applicationVersion == 42);
  CHECK(info.engineVersion == 0);
  CHECK(std::string(venture::xr::application_identity(std::string_view("\0ignored", 8), "test.fallback", 0).applicationName) == "test.fallback");
  CHECK(std::string(venture::xr::application_identity({}, "test.fallback", -1).applicationName) == "test.fallback");
  char small[5]{};
  venture::xr::identity_name(small, "ab\u4e16\u754c");
  CHECK(std::string(small) == "ab");
}

TEST_CASE("VrApi prediction followed by frame wait preserves the input snapshot") {
  venture::test::ScopedMenuMode automatic("");
  venture::test::XrImageFixture images; venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto* mobile = vg::vrapi_EnterVrMode(&mode); REQUIRE(mobile);
  venture::test::NativeMenuBindingOverride host_menu(vr::state().input.menu);
  CHECK(host_menu.bound_queries == 0);
  MockXrScript script{}; ventureMockXrSetScript(&script);
  REQUIRE(vr::sync_input() == XR_SUCCESS);
  script.left.thumbstick_click = script.right.thumbstick_click = true; ventureMockXrSetScript(&script);
  REQUIRE(vr::sync_input() == XR_SUCCESS); // Start the hold before this frame is sampled.
  std::this_thread::sleep_for(std::chrono::milliseconds(510));
  REQUIRE(vg::vrapi_GetPredictedDisplayTime(mobile, 3) > 0);
  REQUIRE(vg::vrapi_WaitFrame(mobile, 3) == 0);
  vr::abi::RemoteState left{}, right{}; left.header.type = right.header.type = 4;
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 2, &right.header) == 0);
  CHECK((left.buttons & 0x100000u) != 0); CHECK((right.buttons & 0x100000u) == 0);
  CHECK((left.buttons & 0x80000000u) != 0); CHECK((right.buttons & 0x80000000u) != 0);
  // Re-reading prediction/input for this same frame keeps one stable edge.
  REQUIRE(vg::vrapi_GetPredictedDisplayTime(mobile, 3) > 0);
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  CHECK((left.buttons & 0x100000u) != 0);
  // The next frame samples fresh axes; the still-held chord keeps the menu held.
  script.left.trigger = 0.75f;
  ventureMockXrSetScript(&script);
  REQUIRE(vg::vrapi_BeginFrame(mobile, 3) == 0);
  REQUIRE(vg::vrapi_GetPredictedDisplayTime(mobile, 4) > 0);
  REQUIRE(vg::vrapi_WaitFrame(mobile, 4) == 0);
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  CHECK((left.buttons & 0x100000u) != 0);  // the held chord keeps the menu held
  CHECK(left.trigger == doctest::Approx(0.75));
  host_menu.missing = false;
  venture::xr::note_menu_profiles_changed(vr::session().handle);
  REQUIRE(vr::sync_input() == XR_SUCCESS);
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  CHECK((left.buttons & 0x100000u) == 0);
  script.left.menu = true; ventureMockXrSetScript(&script);
  REQUIRE(vr::sync_input() == XR_SUCCESS);
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  CHECK((left.buttons & 0x100000u) != 0);
  CHECK(host_menu.bound_queries > 0);
  vg::vrapi_LeaveVrMode(mobile); vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr); MockXrScript restored{}; ventureMockXrSetScript(&restored);
}


TEST_CASE("VrApi future predictions preserve a waited input snapshot") {
  venture::test::ScopedMenuMode automatic("");
  venture::test::XrImageFixture images; venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto* mobile = vg::vrapi_EnterVrMode(&mode); REQUIRE(mobile);
  venture::test::NativeMenuBindingOverride host_menu(vr::state().input.menu);
  MockXrScript script{}; script.left.trigger = 0; ventureMockXrSetScript(&script);
  REQUIRE(vr::sync_input() == XR_SUCCESS);
  script.left.thumbstick_click = script.right.thumbstick_click = true;
  ventureMockXrSetScript(&script); REQUIRE(vr::sync_input() == XR_SUCCESS);
  std::this_thread::sleep_for(std::chrono::milliseconds(510));
  REQUIRE(vg::vrapi_WaitFrame(mobile, 1) == 0);
  const auto time1 = vg::vrapi_GetPredictedDisplayTime(mobile, 1);
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 2) > time1);
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 0) < time1);
  vr::abi::RemoteState left{}; left.header.type = 4;
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  CHECK((left.buttons & 0x100000u) != 0);
  script.left.trigger = 0.75f; ventureMockXrSetScript(&script);
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 3) > time1);
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  CHECK((left.buttons & 0x100000u) != 0);
  CHECK(left.trigger == doctest::Approx(0));
  REQUIRE(vg::vrapi_BeginFrame(mobile, 1) == 0);
  REQUIRE(vg::vrapi_WaitFrame(mobile, 2) == 0);
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  CHECK((left.buttons & 0x100000u) != 0);  // the held chord keeps the menu held
  CHECK(left.trigger == doctest::Approx(0.75));
  vg::vrapi_LeaveVrMode(mobile); vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr); MockXrScript restored{}; ventureMockXrSetScript(&restored);
}

TEST_CASE("A menu the host binds only to its system button still gets the stick-click menu") {
  // SteamVR with Index controllers reports the game's menu on the system
  // button, which opens SteamVR's dashboard and never reaches the game.
  venture::test::ScopedMenuMode automatic("");
  venture::test::XrImageFixture images; venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto* mobile = vg::vrapi_EnterVrMode(&mode); REQUIRE(mobile);
  venture::test::NativeMenuBindingOverride host_menu(vr::state().input.menu);
  host_menu.missing = false;
  REQUIRE(XR_SUCCEEDED(xrStringToPath(venture::xr::host_xr().instance, "/user/hand/left/input/system/click",
                                      &host_menu.appeared_source)));
  MockXrScript script{}; ventureMockXrSetScript(&script);
  REQUIRE(vr::sync_input() == XR_SUCCESS);
  script.left.thumbstick_click = script.right.thumbstick_click = true;
  ventureMockXrSetScript(&script); REQUIRE(vr::sync_input() == XR_SUCCESS);
  std::this_thread::sleep_for(std::chrono::milliseconds(510));
  REQUIRE(vg::vrapi_WaitFrame(mobile, 1) == 0);
  vr::abi::RemoteState left{}; left.header.type = 4;
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  CHECK(host_menu.bound_queries > 0);
  CHECK((left.buttons & 0x100000u) != 0);
  vg::vrapi_LeaveVrMode(mobile); vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr); MockXrScript restored{}; ventureMockXrSetScript(&restored);
}

TEST_CASE("A game that never binds stick clicks gets the stick-click menu even with a native menu") {
  // SteamVR maps Touch's menu to the Steam Frame controller's View button, so the
  // host reports it bound; with both sticks free in the game the chord costs nothing.
  venture::test::ScopedMenuMode automatic("");
  venture::test::XrImageFixture images; venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto* mobile = vg::vrapi_EnterVrMode(&mode); REQUIRE(mobile);
  venture::test::NativeMenuBindingOverride host_menu(vr::state().input.menu);
  host_menu.missing = false;
  REQUIRE(XR_SUCCEEDED(xrStringToPath(venture::xr::host_xr().instance, "/user/hand/left/input/view/click",
                                      &host_menu.appeared_source)));
  // Declare a game whose Touch bindings have a menu and no stick click.
  const auto instance = venture::xr::host_xr().instance;
  XrPath touch{}, menu{};
  REQUIRE(XR_SUCCEEDED(xrStringToPath(instance, "/interaction_profiles/oculus/touch_controller", &touch)));
  REQUIRE(XR_SUCCEEDED(xrStringToPath(instance, "/user/hand/left/input/menu/click", &menu)));
  const XrActionSuggestedBinding menu_only{vr::state().input.menu, menu};
  const XrInteractionProfileSuggestedBinding declared{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, touch, 1,
                                                      &menu_only};
  venture::xr::remember_menu_bindings(instance, touch, &declared);
  venture::xr::note_menu_profiles_changed(vr::session().handle);
  MockXrScript script{}; ventureMockXrSetScript(&script);
  REQUIRE(vr::sync_input() == XR_SUCCESS);
  script.left.thumbstick_click = script.right.thumbstick_click = true;
  ventureMockXrSetScript(&script); REQUIRE(vr::sync_input() == XR_SUCCESS);
  std::this_thread::sleep_for(std::chrono::milliseconds(510));
  REQUIRE(vg::vrapi_WaitFrame(mobile, 1) == 0);
  vr::abi::RemoteState left{}; left.header.type = 4;
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  CHECK((left.buttons & 0x100000u) != 0);
  vg::vrapi_LeaveVrMode(mobile); vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr); MockXrScript restored{}; ventureMockXrSetScript(&restored);
}

TEST_CASE("VrApi legacy predictions share one input sample until submission") {
  venture::test::ScopedMenuMode automatic("");
  venture::test::XrImageFixture images; venture::xr::install_xr_images(&images);
  vr::abi::Init init{1, 1, 1, 42, 0, 0x10300, {1, 2, 3}};
  REQUIRE(vg::vrapi_Initialize(&init) == 0);
  vr::abi::Mode mode{2, 0, {1, 2, 3}, 4, 5, 6};
  auto* mobile = vg::vrapi_EnterVrMode(&mode); REQUIRE(mobile);
  venture::test::NativeMenuBindingOverride host_menu(vr::state().input.menu);
  MockXrScript script{}; script.left.trigger = 0; ventureMockXrSetScript(&script);
  REQUIRE(vr::sync_input() == XR_SUCCESS);
  script.left.thumbstick_click = script.right.thumbstick_click = true;
  ventureMockXrSetScript(&script); REQUIRE(vr::sync_input() == XR_SUCCESS);
  std::this_thread::sleep_for(std::chrono::milliseconds(510));
  const auto prediction = vg::vrapi_GetPredictedDisplayTime(mobile, 100);
  REQUIRE(prediction > 0);
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 101) > prediction);
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 99) > 0);
  vr::abi::RemoteState left{}; left.header.type = 4;
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  CHECK((left.buttons & 0x100000u) != 0);
  script.left.trigger = 0.75f; ventureMockXrSetScript(&script);
  CHECK(vg::vrapi_GetPredictedDisplayTime(mobile, 102) > prediction);
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  CHECK((left.buttons & 0x100000u) != 0);
  CHECK(left.trigger == doctest::Approx(0));
  // Legacy submission performs its native wait implicitly, including a
  // skipped/nonconsecutive application frame index.
  vr::abi::Submit submit{0, 1, 100, prediction, 0, 0, nullptr};
  REQUIRE(vg::vrapi_SubmitFrame2(mobile, &submit) == 0);
  REQUIRE(vg::vrapi_GetPredictedDisplayTime(mobile, 103) > 0);
  REQUIRE(vg::vrapi_GetCurrentInputState(mobile, 1, &left.header) == 0);
  CHECK((left.buttons & 0x100000u) != 0);  // the held chord keeps the menu held
  CHECK(left.trigger == doctest::Approx(0.75));
  vg::vrapi_LeaveVrMode(mobile); vg::vrapi_Shutdown();
  venture::xr::install_xr_images(nullptr); MockXrScript restored{}; ventureMockXrSetScript(&restored);
}

TEST_CASE("A hands-only app sees the host's hands in place of its controllers") {
  using venture::xr::hand_replaces_controller;
  // Quest semantics for ordinary apps: a free hand replaces its controller; without the
  // data-source extension a held controller wins over a skeleton derived from it.
  CHECK(hand_replaces_controller(true, true, true, false));
  CHECK_FALSE(hand_replaces_controller(true, false, true, false));
  CHECK(hand_replaces_controller(true, false, false, false));
  // An app that requires hand tracking takes the host's hands, controller-derived or not.
  CHECK(hand_replaces_controller(true, false, true, true));
  CHECK_FALSE(hand_replaces_controller(false, true, true, true));
}
