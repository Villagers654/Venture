#include <doctest/doctest.h>
#include <jni.h>
#include <zlib.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "platform/file_activity.h"
#include <thread>
#include "core/address_space.h"
#include "core/guest_heap.h"
#include "core/linker.h"
#include "core/os_quit.h"
#include "core/process.h"
#include "core/thread_api.h"
#include "core/thread_runtime.h"
#include "hle/registry.h"
#include "hle/libc/libc_parts.h"
#include "java/execution_freeze.h"
#include "java/jvm.h"
#include "java/natives/framework_natives.h"

TEST_CASE("OS menu quit stops native and Java execution and preserves unclosed gzip output") {
  using namespace venture;
  REQUIRE(os_quit::enabled);
  process_config().package = "test.menu.quit";
  auto& guest = GuestThread::adopt_current();
  register_all_hle_libraries();
  java::register_framework_natives();
  std::string error;
  REQUIRE_MESSAGE(java::start_jvm({VENTURE_TEST_JAVA_HOME,
      {VENTURE_TEST_FRAMEWORK_JAR, VENTURE_TEST_JAVA_CLASSES}, {"-Xcheck:jni", "-Xverify:all"}}, &error), error);
  auto* env = java::host_env();
  auto fixture = env->FindClass("MenuQuitTest");
  REQUIRE(fixture);
  const auto start = env->GetStaticMethodID(fixture, "start", "()V");
  const auto stop = env->GetStaticMethodID(fixture, "stop", "()V");
  const auto counter = env->GetStaticFieldID(fixture, "counter", "J");
  REQUIRE(start); REQUIRE(stop); REQUIRE(counter);
  env->CallStaticVoidMethod(fixture, start);
  REQUIRE_FALSE(env->ExceptionCheck());
  const auto await_java = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!env->GetStaticLongField(fixture, counter) && std::chrono::steady_clock::now() < await_java)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE(env->GetStaticLongField(fixture, counter) > 0);
  {
    java::ExecutionFreeze freeze;
    bool saving = true;
    CHECK_FALSE(freeze.application_quiet(&error, &saving));
    CHECK_FALSE(saving);  // a Java loop is not a save
    REQUIRE_MESSAGE(freeze.freeze(&error), error);
    const auto frozen = env->GetStaticLongField(fixture, counter);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(env->GetStaticLongField(fixture, counter) == frozen);
  }
  env->CallStaticVoidMethod(fixture, stop);
  REQUIRE_FALSE(env->ExceptionCheck());
  {
    // A thread blocked inside the application's own native method runs guest
    // code; the guest census, not the Java census, classifies it. The fixture
    // is loaded by its own loader, as a game's classes are, so JDK threads that
    // carry the system loader stay outside the application.
    static std::atomic<bool> parked{false}, release{false};
    auto file_class = env->FindClass("java/io/File");
    auto dir = env->NewObject(file_class, env->GetMethodID(file_class, "<init>", "(Ljava/lang/String;)V"),
                              env->NewStringUTF(VENTURE_TEST_JAVA_CLASSES));
    auto uri = env->CallObjectMethod(dir, env->GetMethodID(file_class, "toURI", "()Ljava/net/URI;"));
    auto url = env->CallObjectMethod(uri, env->GetMethodID(env->FindClass("java/net/URI"), "toURL", "()Ljava/net/URL;"));
    auto url_class = env->FindClass("java/net/URL");
    auto urls = env->NewObjectArray(1, url_class, url);
    auto loader_class = env->FindClass("java/net/URLClassLoader");
    auto loader = env->NewObject(loader_class, env->GetMethodID(loader_class, "<init>", "([Ljava/net/URL;Ljava/lang/ClassLoader;)V"),
                                 urls, nullptr);
    REQUIRE(loader);
    auto app = static_cast<jclass>(env->CallObjectMethod(
        loader, env->GetMethodID(loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), env->NewStringUTF("MenuQuitTest")));
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(app);
    JNINativeMethod park{const_cast<char*>("parkNative"), const_cast<char*>("()V"),
                         reinterpret_cast<void*>(+[](JNIEnv*, jclass) {
                           parked = true;
                           while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                         })};
    REQUIRE(env->RegisterNatives(app, &park, 1) == JNI_OK);
    java::set_app_class_loader(loader);
    env->CallStaticVoidMethod(app, env->GetStaticMethodID(app, "startParked", "()V"));
    REQUIRE_FALSE(env->ExceptionCheck());
    const auto await_park = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!parked && std::chrono::steady_clock::now() < await_park) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(parked);
    java::ExecutionFreeze census;
    CHECK_MESSAGE(census.application_quiet(&error), error);
    {
      // Once guest execution is stopped, the freeze accepts it too.
      java::ExecutionFreeze parked_freeze;
      CHECK_MESSAGE(parked_freeze.freeze(&error), error);
    }
    release = true;
    env->CallStaticVoidMethod(app, env->GetStaticMethodID(app, "joinParked", "()V"));
    REQUIRE_FALSE(env->ExceptionCheck());
    // NativeHost's NativeActivity bridge runs the game's callbacks the same way.
    parked = false; release = false;
    auto host = env->FindClass("com/venture/runtime/NativeHost");
    REQUIRE(host);
    JNINativeMethod event{const_cast<char*>("nativeActivityEvent"), const_cast<char*>("(JIJ)V"),
                          reinterpret_cast<void*>(+[](JNIEnv*, jobject, jlong, jint, jlong) {
                            parked = true;
                            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                          })};
    REQUIRE(env->RegisterNatives(host, &event, 1) == JNI_OK);
    env->CallStaticVoidMethod(fixture, env->GetStaticMethodID(fixture, "startActivityEvent", "()V"));
    REQUIRE_FALSE(env->ExceptionCheck());
    const auto await_event = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!parked && std::chrono::steady_clock::now() < await_event) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(parked);
    CHECK_MESSAGE(census.application_quiet(&error), error);
    {
      java::ExecutionFreeze bridge_freeze;
      CHECK_MESSAGE(bridge_freeze.freeze(&error), error);
    }
    release = true;
    env->CallStaticVoidMethod(fixture, env->GetStaticMethodID(fixture, "joinActivityEvent", "()V"));
    REQUIRE_FALSE(env->ExceptionCheck());
    env->DeleteLocalRef(host);
    // A save on a slow disk: the application's thread waits in the JDK's
    // fsync. Its census reports a save, which extends the quit window.
    parked = false; release = false;
    auto descriptor = env->FindClass("java/io/FileDescriptor");
    JNINativeMethod slow_sync{const_cast<char*>("sync0"), const_cast<char*>("()V"),
                              reinterpret_cast<void*>(+[](JNIEnv*, jobject) {
                                parked = true;
                                while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                              })};
    REQUIRE(env->RegisterNatives(descriptor, &slow_sync, 1) == JNI_OK);
    const auto synced = std::filesystem::temp_directory_path() / ("venture-slow-sync-" + std::to_string(os_quit::now_ns()));
    env->CallStaticVoidMethod(app, env->GetStaticMethodID(app, "startSync", "(Ljava/lang/String;)V"),
                              env->NewStringUTF(synced.string().c_str()));
    REQUIRE_FALSE(env->ExceptionCheck());
    const auto await_sync = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!parked && std::chrono::steady_clock::now() < await_sync) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(parked);
    bool syncing = false;
    CHECK_FALSE(census.application_quiet(&error, &syncing));
    CHECK(syncing);
    CHECK(error.find("sync0") != std::string::npos);
    release = true;
    env->CallStaticVoidMethod(app, env->GetStaticMethodID(app, "joinSync", "()V"));
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK_MESSAGE(census.application_quiet(&error, &syncing), error);
    CHECK_FALSE(syncing);
    std::filesystem::remove(synced);
    // A thread waiting on a socket (JVMTI: runnable in native Net.poll) is not
    // a save; neither the census nor the freeze waits for its peer.
    env->CallStaticVoidMethod(app, env->GetStaticMethodID(app, "startNetWait", "()V"));
    REQUIRE_FALSE(env->ExceptionCheck());
    const auto net_waiting = env->GetStaticMethodID(app, "netWaiting", "()Z");
    const auto await_net = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!env->CallStaticBooleanMethod(app, net_waiting) && std::chrono::steady_clock::now() < await_net)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(env->CallStaticBooleanMethod(app, net_waiting));
    CHECK_MESSAGE(census.application_quiet(&error, &syncing), error);
    CHECK_FALSE(syncing);
    {
      java::ExecutionFreeze net_freeze;
      CHECK_MESSAGE(net_freeze.freeze(&error), error);
    }
    env->CallStaticVoidMethod(app, env->GetStaticMethodID(app, "joinNetWait", "()V"));
    REQUIRE_FALSE(env->ExceptionCheck());
    env->DeleteLocalRef(descriptor);
    java::set_app_class_loader(nullptr);
    for (jobject ref : {static_cast<jobject>(app), loader, static_cast<jobject>(loader_class), static_cast<jobject>(urls),
                        static_cast<jobject>(url_class), url, uri, dir, static_cast<jobject>(file_class)})
      env->DeleteLocalRef(ref);
  }
  env->DeleteLocalRef(fixture);

  const auto root = std::filesystem::temp_directory_path() /
      ("venture-os-save-" + std::to_string(os_quit::now_ns()));
  std::filesystem::create_directories(root / "files");
  process_config().data_dir = root;
  // A quiet filesystem does not imply a Java BufferedOutputStream is drained.
  fixture = env->FindClass("MenuQuitTest");
  REQUIRE(fixture);
  const auto buffered = env->GetStaticMethodID(fixture, "bufferedSave", "(Ljava/lang/String;)V");
  const auto close_save = env->GetStaticMethodID(fixture, "closeSave", "()V");
  REQUIRE(buffered); REQUIRE(close_save);
  auto path = env->NewStringUTF((root / "files/buffered-save.dat").string().c_str());
  platform::FileActivity files(root);
  env->CallStaticVoidMethod(fixture, buffered, path);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(std::filesystem::file_size(root / "files/buffered-save.dat") == 0);
  auto quiet_since = std::chrono::steady_clock::now();
  const auto quiet_deadline = quiet_since + std::chrono::seconds(20);
  for (;;) {
    const auto change = files.poll();
    REQUIRE(change != platform::FileActivity::Poll::failed);
    const auto now = std::chrono::steady_clock::now();
    if (change == platform::FileActivity::Poll::changed) quiet_since = now;
    if (now-quiet_since >= std::chrono::milliseconds(1100)) break;
    REQUIRE(now < quiet_deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  {
    java::ExecutionFreeze barrier;
    CHECK_FALSE(barrier.save_work_quiet(root, &error));
    CHECK(platform::data_writers(root) == platform::DataWriters::open);
    env->CallStaticVoidMethod(fixture, close_save);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK_MESSAGE(barrier.save_work_quiet(root, &error), error);
  }
  env->DeleteLocalRef(path);
  std::ifstream saved_java(root / "files/buffered-save.dat");
  std::string java_bytes((std::istreambuf_iterator<char>(saved_java)), {});
  CHECK(java_bytes == "pause-buffered-save");
  saved_java.close();
  env->DeleteLocalRef(fixture);
  const auto name = process_config().guest_data_dir() + "/files/save.gz";
  auto* libz = Linker::get().open("libz.so", kRtldNow, nullptr);
  REQUIRE(libz);
  auto invoke = [&](const char* symbol, std::initializer_list<std::uint64_t> args) {
    return guest.call(libz->find_symbol(symbol), GuestCallArgs{std::span(args.begin(), args.size()), {}}).x0;
  };
  const auto file = invoke("gzopen", {h2g(name.c_str()), h2g("wb")});
  REQUIRE(file);
  constexpr char contents[] = "save-without-gzclose";
  REQUIRE(invoke("gzwrite", {file, h2g(contents), sizeof(contents) - 1}) == sizeof(contents) - 1);
  const auto code = AddressSpace::get().allocate(4096, kProtRead | kProtWrite | kProtExec, "[os-stop-test]");
  REQUIRE(code);
  *g2h<std::uint32_t>(code) = 0x14000000; // B .; interrupt must retire the running JIT.
  auto worker = create_guest_thread(code, 0);
  REQUIRE(worker);
  const auto await_native = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!os_quit::active_calls.load() && std::chrono::steady_clock::now() < await_native)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE(os_quit::active_calls.load());
  set_guest_thread_native_cleanup([] { std::abort(); });
  os_quit::stop_guest_execution();
  const auto drain = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while ((os_quit::active_calls.load() || threads::count() > 1) && std::chrono::steady_clock::now() < drain)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE(os_quit::active_calls.load() == 0);
  REQUIRE(threads::count() == 1);
  CHECK_THROWS_AS(guest.call(code, {}), ProcessExit);
  CHECK(os_quit::active_calls.load() == 0);
  REQUIRE(hle::libc::close_gzip_for_os_quit());
  auto* saved = ::gzopen((root / "files/save.gz").string().c_str(), "rb");
  REQUIRE(saved);
  char bytes[64]{};
  CHECK(::gzread(saved, bytes, sizeof(bytes)) == sizeof(contents) - 1);
  CHECK(std::strcmp(bytes, contents) == 0);
  CHECK(::gzclose(saved) == Z_OK);
  // Even an already-detached A32 heap must not enter allocator collection.
  guest_heap::ThreadHeapDeleter{}(reinterpret_cast<mi_heap_s*>(1));
  fixture = env->FindClass("MenuQuitTest");
  REQUIRE(fixture);
  const auto crash = env->GetStaticMethodID(fixture, "crashWhileDestroyBlocked", "()V");
  const auto release_destroy = env->GetStaticMethodID(fixture, "releaseDestroy", "()V");
  REQUIRE(crash); REQUIRE(release_destroy);
  env->CallStaticVoidMethod(fixture, crash);
  REQUIRE_FALSE(env->ExceptionCheck());
  {
    java::ExecutionFreeze boundary;
    REQUIRE(boundary.recorded_failure().has_value());
    CHECK(*boundary.recorded_failure() == 1);
    REQUIRE_MESSAGE(boundary.freeze(&error), error);
    // Same volatile field remains readable after the blocked destroy thread
    // is suspended: no Java invocation can hide this failure at the boundary.
    REQUIRE(boundary.recorded_failure().has_value());
    record_guest_process_failure(*boundary.recorded_failure());
    CHECK(recorded_guest_process_failure() == 1);
    CHECK(guest_process_exit_status() == 1);
  }
  env->CallStaticVoidMethod(fixture, release_destroy);
  REQUIRE_FALSE(env->ExceptionCheck());
  env->DeleteLocalRef(fixture);
  std::filesystem::remove_all(root);
}
