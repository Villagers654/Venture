// Meta's Android OpenXR loader, run as guest code, finds Venture's runtime the
// way it finds a Quest's: through the com.oculus.systemdriver package context and
// its DriverLoader class. Games bundle that loader; one shipped under a file name
// other than libopenxr_loader.so is loaded as is (its API layers keep working),
// so this path is its only route to the runtime.
//
// Meta's loader cannot be committed to this GPL repository. Point
// VENTURE_TEST_META_XR_LOADER at an arm64 copy (any game's lib/arm64-v8a/
// libopenxr_loader.so that names com.oculus.systemdriver.DriverLoader); without
// it the test reports itself skipped.
#include "android/package_assets.h"
#include "common/environment.h"
#include "common/paths.h"
#include "core/address_space.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/thread.h"
#include "hle/openxr/openxr.h"
#include "hle/registry.h"
#include "java/jvm.h"
#include "java/natives/framework_natives.h"
#include "java/natives/runtime_bridge.h"
#include <openxr/openxr.h>
#include <jni.h>
#include <miniz.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <doctest/doctest.h>

namespace {
namespace fs = std::filesystem;
constexpr const char* kPackage = "org.venture.metaxrloader";

// A binary AndroidManifest.xml (AOSP ResXMLTree) holding <manifest package=...>
// <application/></manifest>, all RuntimeEnvironment needs for the app identity.
std::vector<std::uint8_t> manifest(const std::string& package) {
  std::vector<std::uint8_t> b;
  auto put = [&](std::uint32_t value) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
  };
  auto patch = [&](std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
  };
  put(0x00080003); put(0);
  const auto pool = b.size();
  for (std::uint32_t value : {0x001c0001u, 0u, 4u, 0u, 0x100u, 44u, 0u}) put(value);
  for (int i = 0; i < 4; ++i) put(0);
  const auto start = b.size();
  int index = 0;
  for (const std::string word : {std::string("manifest"), std::string("package"), package, std::string("application")}) {
    patch(pool + 28 + 4 * index++, static_cast<std::uint32_t>(b.size() - start));
    b.push_back(static_cast<std::uint8_t>(word.size()));
    b.push_back(static_cast<std::uint8_t>(word.size()));
    b.insert(b.end(), word.begin(), word.end());
    b.push_back(0);
  }
  while (b.size() % 4) b.push_back(0);
  patch(pool + 4, static_cast<std::uint32_t>(b.size() - pool));
  for (std::uint32_t value : {0x00100102u, 56u, 1u, 0xffffffffu, 0xffffffffu, 0u, 0x00140014u, 1u, 0u,
                              0xffffffffu, 1u, 2u, 0x03000008u, 2u,
                              0x00100102u, 36u, 1u, 0xffffffffu, 0xffffffffu, 3u, 0x00140014u, 0u, 0u})
    put(value);
  for (std::uint32_t name : {3u, 0u})
    for (std::uint32_t value : {0x00100103u, 24u, 1u, 0xffffffffu, 0xffffffffu, name}) put(value);
  patch(4, static_cast<std::uint32_t>(b.size()));
  return b;
}

std::string text(JNIEnv* env, jthrowable error) {
  auto cls = env->FindClass("java/lang/Object");
  auto string = static_cast<jstring>(env->CallObjectMethod(error, env->GetMethodID(cls, "toString", "()Ljava/lang/String;")));
  if (!string) return "?";
  const char* chars = env->GetStringUTFChars(string, nullptr);
  std::string result = chars ? chars : "?";
  if (chars) env->ReleaseStringUTFChars(string, chars);
  return result;
}
void require_no_exception(JNIEnv* env) {
  if (!env->ExceptionCheck()) return;
  auto error = env->ExceptionOccurred();
  env->ExceptionClear();
  FAIL("Java exception: " << text(env, error));
}

struct AndroidLoaderInit {  // XrLoaderInitInfoAndroidKHR as an A64 guest lays it out
  XrStructureType type;
  const void* next;
  venture::GuestAddr application_vm;
  venture::GuestAddr application_context;
};
struct AndroidInstanceCreate {  // XrInstanceCreateInfoAndroidKHR
  XrStructureType type;
  const void* next;
  venture::GuestAddr application_vm;
  venture::GuestAddr application_activity;
};
}  // namespace

TEST_CASE("Meta's OpenXR loader under another name finds Venture's runtime through the system driver") {
  using namespace venture;
  const char* source = environment::getenv("VENTURE_TEST_META_XR_LOADER");
  if (!source || !*source) {
    std::puts("skipped: VENTURE_TEST_META_XR_LOADER is not set");
    return;
  }
  REQUIRE(fs::is_regular_file(paths::from_utf8(source)));

  const auto root = fs::temp_directory_path() /
      ("venture-meta-xr-loader-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup { fs::path path; ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); } } cleanup{root};
  for (const char* dir : {"data", "cache", "obb", "lib"}) fs::create_directories(root / dir);
  const auto apk = root / "base.apk";
  {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_file(&zip, paths::utf8(apk).c_str(), 0));
    const auto bytes = manifest(kPackage);
    REQUIRE(mz_zip_writer_add_mem(&zip, "AndroidManifest.xml", bytes.data(), bytes.size(), 0));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    REQUIRE(mz_zip_writer_end(&zip));
  }
  // A name no Venture library answers to, as a game ships a renamed loader.
  const auto loader_path = root / "lib" / "libgame_openxr_loader.so";
  fs::copy_file(paths::from_utf8(source), loader_path);

  auto& config = process_config();
  config.package = kPackage;
  config.apk = apk;
  config.data_dir = root / "data";
  config.cache_dir = root / "cache";
  config.guest_cache_dir = config.data_dir / "cache";
  config.obb_dir = root / "obb";
  config.native_lib_dir = root / "lib";
  REQUIRE(android::PackageAssets::get().open_apk(paths::utf8(apk)));
  AddressSpace::get().initialize(GuestIsa::a64);
  register_all_hle_libraries();
  hle::openxr::register_library();

  java::install_runtime_framework_bridges();
  java::register_framework_natives();
  std::string error;
  REQUIRE_MESSAGE(java::start_jvm({VENTURE_TEST_JAVA_HOME, {VENTURE_TEST_FRAMEWORK_JAR}, {"-Xcheck:jni", "-Xverify:all"}}, &error),
                  error);
  auto* env = java::host_env();
  REQUIRE(env->PushLocalFrame(32) == JNI_OK);
  auto string_class = env->FindClass("java/lang/String");
  const std::string sdk = "32";
  const std::string values[] = {kPackage, paths::utf8(apk), paths::utf8(root / "data"), paths::utf8(root / "cache"),
                                paths::utf8(root / "obb"), paths::utf8(root / "lib"), sdk, paths::utf8(apk)};
  auto args = env->NewObjectArray(8, string_class, nullptr);
  for (jsize i = 0; i < 8; ++i) env->SetObjectArrayElement(args, i, env->NewStringUTF(values[i].c_str()));
  auto runtime = env->FindClass("com/venture/runtime/RuntimeEnvironment");
  REQUIRE(runtime);
  env->CallStaticVoidMethod(runtime, env->GetStaticMethodID(runtime, "configure", "([Ljava/lang/String;)V"), args);
  require_no_exception(env);
  auto activity_class = env->FindClass("android/app/Activity");
  auto activity = env->NewGlobalRef(env->NewObject(activity_class, env->GetMethodID(activity_class, "<init>", "()V")));
  require_no_exception(env);
  REQUIRE(activity);

  auto& thread = GuestThread::adopt_current();  // runs the loader's initializers
  auto* loader = Linker::get().open(paths::utf8(loader_path), kRtldNow, nullptr);
  INFO(Linker::get().take_error());
  REQUIRE(loader);
  const auto symbol = [&](const char* name) {
    const auto address = Linker::get().symbol(loader, name);
    INFO(name);
    REQUIRE(address);
    return address;
  };
  const auto call = [&](GuestAddr function, std::initializer_list<std::uint64_t> args) {
    return static_cast<XrResult>(static_cast<std::int32_t>(thread.call(function, args)));
  };

  // The loader takes the VM and context, then the runtime: createPackageContext,
  // DriverLoader.loadXrRuntime, getProcAddr("xrInitializeLoaderKHR") and
  // getProcAddr("xrNegotiateLoaderRuntimeInterface").
  // Like every app, the loader's caller reaches xrInitializeLoaderKHR through
  // xrGetInstanceProcAddr(XR_NULL_HANDLE, ...): the loader does not export it.
  GuestAddr initialize = 0;
  REQUIRE(call(symbol("xrGetInstanceProcAddr"), {0, h2g("xrInitializeLoaderKHR"), h2g(&initialize)}) == XR_SUCCESS);
  REQUIRE(initialize);
  AndroidLoaderInit init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, nullptr, java::guest_vm(), h2g(activity)};
  REQUIRE(call(initialize, {h2g(&init)}) == XR_SUCCESS);
  require_no_exception(env);

  const char* extensions[]{"XR_KHR_android_create_instance"};
  AndroidInstanceCreate android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, nullptr, java::guest_vm(), h2g(activity)};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO, &android};
  std::strcpy(create.applicationInfo.applicationName, "Meta loader fixture");
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionCount = 1;
  create.enabledExtensionNames = extensions;
  XrInstance instance{};
  REQUIRE(call(symbol("xrCreateInstance"), {h2g(&create), h2g(&instance)}) == XR_SUCCESS);
  CHECK(instance != XR_NULL_HANDLE);
  XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
  REQUIRE(call(symbol("xrGetInstanceProperties"), {reinterpret_cast<std::uint64_t>(instance), h2g(&properties)}) ==
          XR_SUCCESS);
  CHECK(std::string(properties.runtimeName) == "Oculus");  // Venture reports a Quest's runtime
  CHECK(call(symbol("xrDestroyInstance"), {reinterpret_cast<std::uint64_t>(instance)}) == XR_SUCCESS);
  require_no_exception(env);
  env->DeleteGlobalRef(activity);
  env->PopLocalFrame(nullptr);
}
