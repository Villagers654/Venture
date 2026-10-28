#include "core/address_space.h"
#include "core/process.h"
#include "core/thread_runtime.h"
#include "gfx/dispatch.h"
#include "hle/vulkan/vulkan.h"
#include "java/jvm.h"
#include "java/natives/graphics_bridge.h"
#include "java/natives/runtime_bridge.h"
#include "xr/native.h"
#include <cstdlib>
#include <doctest/doctest.h>
#include <jni.h>

TEST_CASE("Java EGL14 probe uses real guest EGL and GLES capabilities") {
  using namespace venture;
  // Never select an installed host XR runtime, even with an empty environment.
  REQUIRE(xr::set_runtime_manifest_for_process(VENTURE_TEST_MOCK_XR));
  AddressSpace::get().initialize(process_config().isa());
  gfx::register_graphics_libraries();
  vulkan::register_library();
  java::install_runtime_framework_bridges();
  java::register_graphics_natives();
  std::string error;
  REQUIRE_MESSAGE(
      java::start_jvm({VENTURE_TEST_JAVA_HOME,
                       {VENTURE_TEST_FRAMEWORK_JAR, VENTURE_TEST_JAVA_CLASSES},
                       {"-Xcheck:jni", "-Xverify:all"}},
                      &error),
      error);
  auto *env = java::host_env();
  auto fixture = env->FindClass("GraphicsTest");
  REQUIRE(fixture);
  // The validation-only CTest never asks for a display/device. Its JNI array
  // and EGL error paths can run while the shared GPU is occupied.
  const auto *validation = std::getenv("VENTURE_TEST_EGL_INPUTS_ONLY");
  auto run =
      env->GetStaticMethodID(fixture, validation ? "validate" : "run", "()V");
  REQUIRE(run);
  env->CallStaticVoidMethod(fixture, run);
  bool failed = env->ExceptionCheck();
  if (failed) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  CHECK_FALSE(failed);
  if (!validation && !failed) {
    // UE4 4.25's HasVulkanDriverSupport opens a Vulkan 1.0 instance without
    // extensions, even when PackageManager reports a newer device version.
    auto create = reinterpret_cast<PFN_vkCreateInstance>(
        vulkan::xr_host_proc(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance{};
    REQUIRE(create(&info, nullptr, &instance) == VK_SUCCESS);
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        vulkan::xr_host_proc(instance, "vkEnumeratePhysicalDevices"));
    std::uint32_t count{};
    auto result = enumerate(instance, &count, nullptr);
    INFO("UE4 Vulkan 1.0 device probe result=", result, " count=", count);
    if (result == VK_SUCCESS && count == 1) {
      VkPhysicalDevice physical{};
      CHECK(enumerate(instance, &count, &physical) == VK_SUCCESS);
      CHECK(physical != VK_NULL_HANDLE);
    }
    reinterpret_cast<PFN_vkDestroyInstance>(
        vulkan::xr_host_proc(instance, "vkDestroyInstance"))(instance, nullptr);
    CHECK(result == VK_SUCCESS);
    CHECK(count == 1);
  }
  env->DeleteLocalRef(fixture);
  REQUIRE_MESSAGE(java::stop_jvm(&error), error);
}
