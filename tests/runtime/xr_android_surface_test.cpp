#include "../mock_openxr_runtime/control.h"
#include "android/native_window.h"
#include "gfx/gpu_device.h"
#include "hle/openxr/state.h"
#include "java/jvm.h"
#include "java/natives/framework_natives.h"
#include "java/natives/runtime_bridge.h"
#include "xr_images_fixture.h"
#include <jni.h>
#include <atomic>
#include <future>
#include <doctest/doctest.h>

TEST_CASE("XR Android Surface consumes real producer pixels and owns JNI lifetime") {
  using namespace venture;
  namespace xg = hle::openxr::guest;
  java::install_runtime_framework_bridges();
  java::register_framework_natives();
  std::string error;
  REQUIRE(java::start_jvm({VENTURE_TEST_JAVA_HOME, {VENTURE_TEST_FRAMEWORK_JAR},
      {"-Xcheck:jni", "-Xverify:all"}}, &error));
  auto* env = java::host_env();
  const char* extensions[]{"XR_KHR_vulkan_enable2", "XR_KHR_android_surface_swapchain"};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ci.enabledExtensionCount = std::size(extensions);
  ci.enabledExtensionNames = extensions;
  XrInstance instance{};
  REQUIRE(xg::xrCreateInstance(&ci, &instance) == XR_SUCCESS);
  auto& gpu = gfx::gpu_device();
  gfx::GpuDevice producer_gpu;
  producer_gpu.instance = gpu.instance;
  producer_gpu.physical_device = gpu.physical_device;
  producer_gpu.get_instance_proc_addr = gpu.get_instance_proc_addr;
  producer_gpu.queue_family = gpu.queue_family;
  float priority = 1;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = gpu.queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  auto create_device = reinterpret_cast<PFN_vkCreateDevice>(gpu.get_instance_proc_addr(gpu.instance, "vkCreateDevice"));
  REQUIRE(create_device(gpu.physical_device, &device_info, nullptr, &producer_gpu.device) == VK_SUCCESS);
  auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(gpu.get_instance_proc_addr(gpu.instance, "vkGetDeviceQueue"));
  get_queue(producer_gpu.device, gpu.queue_family, 0, &producer_gpu.queue);
  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  binding.instance = gpu.instance;
  binding.physicalDevice = gpu.physical_device;
  binding.device = producer_gpu.device;
  binding.queueFamilyIndex = gpu.queue_family;
  binding.queueIndex = 0;
  XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO, &binding};
  si.systemId = xr::host_xr().system;
  XrSession session{};
  REQUIRE(xg::xrCreateSession(instance, &si, &session) == XR_SUCCESS);
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (xg::xrPollEvent(instance, &event) == XR_SUCCESS)
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
  begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  REQUIRE(xg::xrBeginSession(session, &begin) == XR_SUCCESS);
  XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  reference.poseInReferenceSpace.orientation.w = 1;
  XrSpace space{};
  REQUIRE(xg::xrCreateReferenceSpace(session, &reference, &space) == XR_SUCCESS);
  XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  sc.width = 4; sc.height = 2;
  XrSwapchain chain{};
  void* surface{};
  sc.format = VK_FORMAT_R8G8B8A8_UNORM;
  CHECK(xg::xrCreateSwapchainAndroidSurfaceKHR(session, &sc, &chain, &surface) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(chain == XR_NULL_HANDLE);
  CHECK(surface == nullptr);
  sc.format = 0;
  sc.createFlags = XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT;
  CHECK(xg::xrCreateSwapchainAndroidSurfaceKHR(session, &sc, &chain, &surface) == XR_ERROR_FEATURE_UNSUPPORTED);
  sc.createFlags = 0;
  REQUIRE(xg::xrCreateSwapchainAndroidSurfaceKHR(session, &sc, &chain, &surface) == XR_SUCCESS);
  CHECK(hle::openxr::state().swapchains.at(chain).info.format == VK_FORMAT_R8G8B8A8_SRGB);
  REQUIRE(surface);
  CHECK(env->GetObjectRefType(static_cast<jobject>(surface)) == JNILocalRefType);
  auto cls = env->GetObjectClass(static_cast<jobject>(surface));
  REQUIRE(cls);
  auto valid = env->GetMethodID(cls, "isValid", "()Z");
  auto native = env->GetMethodID(cls, "ventureNativeWindow", "()J");
  REQUIRE(valid); REQUIRE(native);
  CHECK(env->CallBooleanMethod(static_cast<jobject>(surface), valid));
  REQUIRE_FALSE(env->ExceptionCheck());
  auto address = env->CallLongMethod(static_cast<jobject>(surface), native);
  REQUIRE_FALSE(env->ExceptionCheck());
  auto window = android::pin_native_window(address);
  REQUIRE(window);
  CHECK(window->width == 4);
  CHECK(window->height == 2);
  CHECK(window->fixed_consumer_extent);
  std::uint32_t count = 17, index{};
  CHECK(xg::xrEnumerateSwapchainImages(chain, 0, &count, nullptr) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(xg::xrAcquireSwapchainImage(chain, nullptr, &index) == XR_ERROR_VALIDATION_FAILURE);
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
  CHECK(xg::xrWaitSwapchainImage(chain, &wait) == XR_ERROR_VALIDATION_FAILURE);
  CHECK(xg::xrReleaseSwapchainImage(chain, nullptr) == XR_ERROR_VALIDATION_FAILURE);
  android::WindowBuffer pixels{};
  REQUIRE(android::lock_window(window.get(), &pixels, nullptr) == 0);
  auto* bytes = g2h<std::uint8_t>(pixels.bits);
  for (int i = 0; i < 8; ++i) {
    bytes[i*4] = 63; bytes[i*4+1] = 127; bytes[i*4+2] = 191; bytes[i*4+3] = 255;
  }
  REQUIRE(android::unlock_window(window.get()) == 0);
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  quad.space = space;
  quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  quad.subImage = {chain, {{0, 1}, {4, 1}}, 0};
  quad.pose.orientation.w = 1;
  quad.pose.position.z = -1;
  quad.size = {1, .5f};
  auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  REQUIRE(xg::xrWaitFrame(session, nullptr, &frame) == XR_SUCCESS);
  REQUIRE(xg::xrBeginFrame(session, nullptr) == XR_SUCCESS);
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
  end.displayTime = frame.predictedDisplayTime;
  end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end.layerCount = 1; end.layers = &layer;
  // A private XR queue must progress while an unrelated ANGLE queue is busy.
  // The bounded guard also makes this regression fail rather than deadlock.
  std::promise<void> host_busy;
  std::promise<void> release_host;
  auto released = release_host.get_future();
  std::atomic<bool> guard_expired{false};
  auto guard = std::async(std::launch::async, [&] {
    std::lock_guard lock(gpu.queue_lock);
    host_busy.set_value();
    guard_expired = released.wait_for(std::chrono::seconds(30)) != std::future_status::ready;
  });
  host_busy.get_future().wait();
  const auto submitted = xg::xrEndFrame(session, &end);
  const bool independent = !guard_expired.load();
  release_host.set_value();
  guard.get();
  INFO("xrEndFrame result=", submitted, " queue guard expired=", guard_expired.load());
  REQUIRE(submitted == XR_SUCCESS);
  CHECK(independent); // Submission completed while the unrelated queue stayed locked.
  // Surface swapchains are not GLES-bridged images: rects reach the host as submitted.
  XrSwapchainSubImage host{};
  CHECK(ventureMockXrLastSubImages(&host, 1) == 1);
  CHECK(host.imageRect.offset.y == 1);
  CHECK(host.imageRect.extent.height == 1);
  // Verify the actual host image chosen by the runtime at lazy publication.
  {
    test::XrImageFixture reader(producer_gpu);
    reader.last_destination = hle::openxr::state().swapchains.at(chain).host_images.at(hle::openxr::state().swapchains.at(chain).host_index).image;
    const std::array<std::uint8_t, 4> expected{63, 127, 191, 255};
    CHECK(reader.read_pixel() == expected);
  }
  // Movie buffers can resize independently of the XR consumer. Verify a
  // nonuniform corner after each change, including cached sampling offsets.
  for(auto shape:{std::array<int,3>{8,4,0},std::array<int,3>{3,6,2}}) {
    auto [width,height,image_index]=shape;
    std::vector<std::uint8_t> rgba(width*height*4);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
      auto* pixel=rgba.data()+(y*width+x)*4;pixel[0]=x*17;pixel[1]=y*31;pixel[2]=77;pixel[3]=255;
    }
    REQUIRE(android::publish_window_rgba(window.get(),rgba,width,height)==0);
    REQUIRE(xg::xrWaitFrame(session,nullptr,&frame)==XR_SUCCESS);
    REQUIRE(xg::xrBeginFrame(session,nullptr)==XR_SUCCESS);end.displayTime=frame.predictedDisplayTime;
    REQUIRE(xg::xrEndFrame(session,&end)==XR_SUCCESS);
    test::XrImageFixture reader(producer_gpu);
    reader.last_destination=hle::openxr::state().swapchains.at(chain).host_images.at(hle::openxr::state().swapchains.at(chain).host_index).image;
    const std::array<std::uint8_t,4> expected{std::uint8_t((3*width/4)*17),std::uint8_t((height/2)*31),77,255};
    CHECK(reader.read_pixel(3,1)==expected);
  }
  REQUIRE(xg::xrDestroySwapchain(chain) == XR_SUCCESS);
  CHECK_FALSE(env->CallBooleanMethod(static_cast<jobject>(surface), valid));
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK_FALSE(android::pin_native_window(address));
  env->DeleteLocalRef(static_cast<jobject>(surface));
  env->DeleteLocalRef(cls);
  CHECK(xg::xrDestroySpace(space) == XR_SUCCESS);
  CHECK(xg::xrDestroySession(session) == XR_SUCCESS);
  CHECK(xg::xrDestroyInstance(instance) == XR_SUCCESS);
  auto destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(gpu.get_instance_proc_addr(gpu.instance, "vkDestroyDevice"));
  destroy_device(producer_gpu.device, nullptr);
}
