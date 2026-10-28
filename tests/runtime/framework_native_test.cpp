// Contract fakes stay in tests: this target does not link runtime core or JVM.
#include <doctest/doctest.h>
#include <jni.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "android/native_window.h"
#include "android/package_assets.h"
#include "core/process.h"
#include "core/thread.h"
#include "core/shutdown_trace.h"
#include "platform/timed_wait.h"
#include "fake_cpu.h"
#include "java/jvm.h"
#include "java/natives/framework_natives.h"
#include "java/natives/native_activity_bridge.h"
#include "java/natives/path_bridge.h"
#include "java/natives/surface_bridge.h"
#include "platform/network.h"

namespace venture::platform {
struct TimedWait::State {};
TimedWait::~TimedWait() = default;
}

namespace {
JNINativeInterface_ table{};
JNIEnv env{&table};
std::map<std::string, void*> registered;
bool exception_pending = false;
std::string exception_type;
int monitors = 0;
std::vector<jbyte> array;
std::map<venture::GuestAddr, std::unique_ptr<venture::android::NativeWindow>> windows;
std::string loaded_name;
void* loaded_loader = nullptr;
int network_type = -2;
bool network_failure = false;
bool internet_connected = false;
std::vector<std::string> lifecycle_samples;

void setup_jni() {
  table.FindClass = [](JNIEnv*, const char* name) -> jclass {
    exception_type = name;
    return reinterpret_cast<jclass>(1);
  };
  table.RegisterNatives = [](JNIEnv*, jclass, const JNINativeMethod* methods,
                             jint count) -> jint {
    for (int i = 0; i < count; ++i)
      registered[std::string(methods[i].name) + methods[i].signature] =
          methods[i].fnPtr;
    return JNI_OK;
  };
  table.DeleteLocalRef = [](JNIEnv*, jobject) {};
  table.ExceptionCheck = [](JNIEnv*) -> jboolean { return exception_pending; };
  table.ThrowNew = [](JNIEnv*, jclass, const char*) -> jint {
    exception_pending = true;
    return JNI_OK;
  };
  table.GetStringLength = [](JNIEnv*, jstring text) -> jsize {
    return static_cast<jsize>(reinterpret_cast<std::u16string*>(text)->size());
  };
  table.GetStringRegion = [](JNIEnv*, jstring text, jsize start, jsize count,
                             jchar* out) {
    std::copy_n(reinterpret_cast<std::u16string*>(text)->data() + start, count,
                out);
  };
  table.NewGlobalRef = [](JNIEnv*, jobject object) -> jobject {
    return object;
  };
  table.DeleteGlobalRef = [](JNIEnv*, jobject) {};
  table.NewByteArray = [](JNIEnv*, jsize n) -> jbyteArray {
    array.resize(n);
    return reinterpret_cast<jbyteArray>(&array);
  };
  table.SetByteArrayRegion = [](JNIEnv*, jbyteArray, jsize start, jsize count,
                                const jbyte* bytes) {
    std::copy_n(bytes, count, array.begin() + start);
  };
  table.IsInstanceOf = [](JNIEnv*, jobject, jclass) -> jboolean {
    return JNI_TRUE;
  };
  table.GetMethodID = [](JNIEnv*, jclass, const char*,
                         const char*) -> jmethodID {
    return reinterpret_cast<jmethodID>(1);
  };
  table.CallLongMethodV = [](JNIEnv*, jobject surface, jmethodID,
                             va_list) -> jlong {
    return *reinterpret_cast<jlong*>(surface);
  };
  table.MonitorEnter = [](JNIEnv*, jobject) -> jint {
    ++monitors;
    return JNI_OK;
  };
  table.MonitorExit = [](JNIEnv*, jobject) -> jint {
    --monitors;
    return JNI_OK;
  };
  venture::java::register_framework_natives();
}

template <class F>
F native(const char* name) {
  setup_jni();
  auto it = registered.find(name);
  REQUIRE(it != registered.end());
  return reinterpret_cast<F>(it->second);
}
}  // namespace

namespace venture::shutdown_trace {
void sample_watch(std::string_view step) noexcept { lifecycle_samples.emplace_back(step); }
}

namespace venture {
ProcessConfig& process_config() {
  static ProcessConfig config;
  return config;
}
GuestThread::GuestThread(std::int32_t tid)
    : cpu_(std::make_unique<test::FakeCpu>()), tid_(tid) {}
GuestThread::~GuestThread() = default;
GuestThread& GuestThread::adopt_current() {
  static thread_local GuestThread thread(123);
  return thread;
}

TEST_CASE("framework host paths map only inside configured guest mounts") {
  auto& config = venture::process_config();
  config.package = "com.test.game";
  const auto root = std::filesystem::current_path() / "framework-path-test";
  config.data_dir = root / "data";
  config.cache_dir = root / "cache";
  config.guest_cache_dir = config.data_dir / "cache";
  config.native_lib_dir = root / "lib";
  config.apk = root / "base.apk";
  config.obb_dir = root / "obb";
  CHECK(venture::java::framework_path_to_guest(
            (config.data_dir / "files/save.xml").string()) ==
        "/data/user/0/com.test.game/files/save.xml");
  CHECK(venture::java::framework_path_to_guest(config.apk.string()) ==
        "/data/app/com.test.game/base.apk");
  CHECK(venture::java::framework_path_to_guest(
            (config.guest_cache_dir / "shader.bin").string()) ==
        "/data/user/0/com.test.game/cache/shader.bin");
  CHECK_FALSE(venture::java::framework_path_to_guest((root / "outside.txt").string())
                  .has_value());
  CHECK_FALSE(venture::java::framework_path_to_guest(
                  (config.data_dir / "../outside.txt").string())
                  .has_value());
  CHECK_FALSE(
      venture::java::framework_path_to_guest((root / "database/save.xml").string())
          .has_value());
}
}  // namespace venture
namespace venture::java {
void throw_thread_death(JNIEnv* target) noexcept {
  auto cls = target->FindClass("java/lang/ThreadDeath");
  target->ThrowNew(cls, nullptr);
  target->DeleteLocalRef(cls);
}
void add_framework_natives(NativeRegistrar registrar) { registrar(&env); }
bool load_guest_library(const std::string& name, void* loader, std::string*) {
  loaded_name = name;
  loaded_loader = loader;
  return true;
}
}  // namespace venture::java
namespace venture::platform {
std::int32_t active_network_type() {
  if (network_failure) throw std::runtime_error("private host query details");
  return network_type;
}
bool has_internet_connectivity() {
  if (network_failure) throw std::runtime_error("private host validation details");
  return internet_connected;
}
}
namespace venture::android {
// This isolated ownership fixture never allocates a guest CPU backing buffer.
NativeWindow::~NativeWindow() = default;
NativeWindow* create_native_window(std::int32_t width, std::int32_t height) {
  auto window = std::make_unique<NativeWindow>();
  window->width = width;
  window->height = height;
  auto* ptr = window.get();
  windows.emplace(h2g(ptr), std::move(window));
  return ptr;
}
NativeWindow* native_window_from_guest(GuestAddr address) {
  auto it = windows.find(address);
  return it == windows.end() ? nullptr : it->second.get();
}
std::shared_ptr<NativeWindow> pin_native_window(GuestAddr address) {
  auto* window = native_window_from_guest(address);
  return window ? std::shared_ptr<NativeWindow>(window, [](NativeWindow*) {}) : nullptr;
}
void acquire(NativeWindow* window) { ++window->refs; }
void release(NativeWindow* window) {
  if (--window->refs == 0) windows.erase(h2g(window));
}
class TestStream final : public AssetStream {
  std::size_t offset_ = 0;

 public:
  std::int64_t read(void* out, std::uint64_t size) override {
    auto n =
        std::min<std::size_t>({static_cast<std::size_t>(size), 3, 9 - offset_});
    std::memcpy(out, &"assetdata"[offset_], n);
    offset_ += n;
    return static_cast<std::int64_t>(n);
  }
  std::int64_t seek(std::int64_t, int) override { return -1; }
  std::uint64_t length() const override { return 9; }
  bool file_range(std::string*, std::uint64_t*) const override { return false; }
};
PackageAssets& PackageAssets::get() {
  static PackageAssets assets;
  return assets;
}
std::unique_ptr<AssetStream> PackageAssets::open(std::string_view name) const {
  return name == "assets/test" ? std::make_unique<TestStream>() : nullptr;
}
std::vector<std::string> PackageAssets::list(std::string_view) const {
  return {};
}
}  // namespace venture::android

TEST_CASE(
    "framework Surface reference ownership across JNI and ANativeWindow") {
  auto create = native<jlong(JNICALL*)(JNIEnv*, jobject, jint, jint, jint)>(
      "createSurface(III)J");
  auto release =
      native<void(JNICALL*)(JNIEnv*, jobject, jlong)>("releaseSurface(J)V");
  jlong surface = create(&env, nullptr, 1920, 1080, 4);
  auto* window = venture::java::acquire_surface_window(&env, &surface);
  REQUIRE(window);
  CHECK(window->refs == 2);
  CHECK(window->width == 1920);
  CHECK(window->format == 4);
  CHECK(monitors == 0);
  release(&env, nullptr, surface);
  CHECK(window->refs == 1);
  venture::android::release(window);
  CHECK(windows.empty());
  surface = 0;
  CHECK(venture::java::acquire_surface_window(&env, &surface) == nullptr);
  CHECK(monitors == 0);
}

TEST_CASE(
    "framework native asset reading handles short chunks and missing entries") {
  auto read = native<jbyteArray(JNICALL*)(JNIEnv*, jobject, jstring)>(
      "readAsset(Ljava/lang/String;)[B");
  std::u16string path = u"assets/test";
  REQUIRE(read(&env, nullptr, reinterpret_cast<jstring>(&path)) != nullptr);
  CHECK(std::string(array.begin(), array.end()) == "assetdata");
  path = u"assets/missing";
  CHECK(read(&env, nullptr, reinterpret_cast<jstring>(&path)) == nullptr);
}

namespace venture::platform { int set_thread_priority(std::int32_t, int) { return 0; } }  // namespace venture::platform
