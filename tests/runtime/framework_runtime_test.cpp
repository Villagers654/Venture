#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <chrono>
#include <thread>
#include <tuple>
#include <stdexcept>
#include <doctest/doctest.h>
#include <jni.h>
#include "android/framework_bridge.h"
#include "android/native_window.h"
#include "android/package_assets.h"
#include "android/looper.h"
#include "audio/audio.h"
#ifdef _WIN32
#include <cstdint>
#include <vector>
#include "platform/change_cache.h"
#include "platform/win32/network_watch.h"
#endif
#include "core/thread.h"
#include "core/thread_runtime.h"
#include "core/process.h"
#include "common/json.h"
#include "java/jvm.h"
#include "java/natives/framework_natives.h"
#include "java/natives/native_activity_bridge.h"
#include "java/natives/runtime_bridge.h"
#include "java/natives/graphics_bridge.h"
#include "java/natives/display_bridge.h"
#include "java/natives/display_clock.h"

#include "hle/libc/libc_parts.h"
#include "hle/libc/libc_a.h"
#include "hle/gles/gles_state.h"
#include "java/natives/horizon_bridge.h"
#include "ovrplatform_support.h"
#include "platform/network.h"

#include "../async_wait.h"

#ifdef VENTURE_ANDROID_FIXTURES
TEST_CASE("AssetManager openFd owns a real bounded archive descriptor through production JNI") {
  using namespace venture;
  auto& assets = android::PackageAssets::get();
  const auto fixtures = std::filesystem::path(VENTURE_ANDROID_FIXTURES);
  REQUIRE(assets.open_apk((fixtures / "test.apk").string()));
  java::install_runtime_framework_bridges();
  java::register_framework_natives();
  std::string error;
  REQUIRE(java::start_jvm({VENTURE_TEST_JAVA_HOME, {VENTURE_TEST_FRAMEWORK_JAR}, {"-Xcheck:jni", "-Xverify:all"}}, &error));
  auto* env = java::host_env();
  REQUIRE(env->PushLocalFrame(32) == JNI_OK);
  auto cls = env->FindClass("android/content/res/AssetManager");
  REQUIRE(cls);
  auto manager = env->NewObject(cls, env->GetMethodID(cls, "<init>", "()V"));
  REQUIRE_FALSE(env->ExceptionCheck());
  auto open = env->GetMethodID(cls, "openFd", "(Ljava/lang/String;)Landroid/content/res/AssetFileDescriptor;");
  REQUIRE(open);
  for (const char* name : {"plain.bin", "large-offset.bin"}) {
    if (std::string_view(name) == "large-offset.bin")
      REQUIRE(assets.open_apk((fixtures / "zip64.apk").string()));
    auto entry = assets.find(std::string("assets/") + name);
    REQUIRE(entry);
    auto afd = env->CallObjectMethod(manager, open, env->NewStringUTF(name));
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(afd);
    auto afd_class = env->GetObjectClass(afd);
    CHECK(env->CallLongMethod(afd, env->GetMethodID(afd_class, "getStartOffset", "()J")) == entry->data_offset);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(env->CallLongMethod(afd, env->GetMethodID(afd_class, "getLength", "()J")) == entry->size);
    REQUIRE_FALSE(env->ExceptionCheck());
    auto fd = env->CallObjectMethod(afd, env->GetMethodID(afd_class, "getFileDescriptor", "()Ljava/io/FileDescriptor;"));
    REQUIRE_FALSE(env->ExceptionCheck());
    auto fd_class = env->FindClass("java/io/FileDescriptor");
    auto valid = env->GetMethodID(fd_class, "valid", "()Z");
    CHECK(env->CallBooleanMethod(fd, valid));
    REQUIRE_FALSE(env->ExceptionCheck());
    auto stream = env->CallObjectMethod(afd, env->GetMethodID(afd_class, "createInputStream", "()Ljava/io/FileInputStream;"));
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(stream);
    auto stream_class = env->GetObjectClass(stream);
    auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(stream, env->GetMethodID(stream_class, "readAllBytes", "()[B")));
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(bytes);
    REQUIRE(env->GetArrayLength(bytes) == entry->size);
    std::vector<jbyte> actual(entry->size), expected(entry->size);
    env->GetByteArrayRegion(bytes, 0, actual.size(), actual.data());
    auto oracle = assets.open(entry->name);
    REQUIRE(oracle->read(expected.data(), expected.size()) == expected.size());
    CHECK(actual == expected);
    CHECK(env->CallIntMethod(stream, env->GetMethodID(stream_class, "read", "()I")) == -1);
    REQUIRE_FALSE(env->ExceptionCheck());
    env->CallVoidMethod(stream, env->GetMethodID(stream_class, "close", "()V"));
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK_FALSE(env->CallBooleanMethod(fd, valid));
    REQUIRE_FALSE(env->ExceptionCheck());
  }
  REQUIRE(assets.open_apk((fixtures / "test.apk").string()));
  auto io_exception = env->FindClass("java/io/IOException");
  REQUIRE(io_exception);
  for (const char* name : {"deflated.bin", "absent.bin"}) {
    CHECK(env->CallObjectMethod(manager, open, env->NewStringUTF(name)) == nullptr);
    REQUIRE(env->ExceptionCheck());
    auto failure = env->ExceptionOccurred();
    env->ExceptionClear();
    CHECK(env->IsInstanceOf(failure, io_exception));
  }
  env->PopLocalFrame(nullptr);
}
TEST_CASE("AssetManager streams stored and deflated content through production JNI") {
  using namespace venture;
  auto& assets = android::PackageAssets::get();
  const auto fixtures = std::filesystem::path(VENTURE_ANDROID_FIXTURES);
  REQUIRE(assets.open_apk((fixtures / "test.apk").string()));
  java::install_runtime_framework_bridges();
  java::register_framework_natives();
  std::string error;
  REQUIRE(java::start_jvm({VENTURE_TEST_JAVA_HOME, {VENTURE_TEST_FRAMEWORK_JAR}, {"-Xcheck:jni", "-Xverify:all"}}, &error));
  auto* env = java::host_env();
  REQUIRE(env->PushLocalFrame(64) == JNI_OK);
  auto cls = env->FindClass("android/content/res/AssetManager");
  REQUIRE(cls);
  auto manager = env->NewObject(cls, env->GetMethodID(cls, "<init>", "()V"));
  auto open = env->GetMethodID(cls, "open", "(Ljava/lang/String;I)Ljava/io/InputStream;");
  auto input = env->FindClass("java/io/InputStream");
  auto read = env->GetMethodID(input, "read", "()I");
  auto bulk = env->GetMethodID(input, "read", "([BII)I");
  auto all = env->GetMethodID(input, "readAllBytes", "()[B");
  auto skip = env->GetMethodID(input, "skip", "(J)J");
  auto available = env->GetMethodID(input, "available", "()I");
  auto mark = env->GetMethodID(input, "mark", "(I)V");
  auto reset = env->GetMethodID(input, "reset", "()V");
  auto close = env->GetMethodID(input, "close", "()V");
  auto missing = env->CallObjectMethod(manager, open, env->NewStringUTF("absent.bin"), 2);
  CHECK(missing == nullptr);
  REQUIRE(env->ExceptionCheck());
  auto failure = env->ExceptionOccurred();
  env->ExceptionClear();
  CHECK(env->IsInstanceOf(failure, env->FindClass("java/io/FileNotFoundException")));
  for (const char* name : {"plain.bin", "deflated.bin", "large-offset.bin"}) {
    REQUIRE(assets.open_apk((fixtures / (std::string_view(name) == "large-offset.bin" ? "zip64.apk" : "test.apk")).string()));
    auto oracle = assets.open(std::string("assets/") + name);
    REQUIRE(oracle);
    std::vector<jbyte> expected(oracle->length());
    REQUIRE(oracle->read(expected.data(), expected.size()) == expected.size());
    auto stream = env->CallObjectMethod(manager, open, env->NewStringUTF(name), 2);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(stream);
    CHECK(env->CallBooleanMethod(stream, env->GetMethodID(input, "markSupported", "()Z")));
    CHECK(env->CallIntMethod(stream, available) == expected.size());
    // The stream owns its archive record independently of manager/archive reload.
    env->CallVoidMethod(manager, env->GetMethodID(cls, "close", "()V"));
    REQUIRE(assets.open_apk((fixtures / "test.apk").string()));
    CHECK(env->CallIntMethod(stream, read) == (static_cast<unsigned char>(expected[0])));
    env->CallVoidMethod(stream, reset); // Default mark is start.
    CHECK(env->CallIntMethod(stream, available) == expected.size());
    CHECK(env->CallLongMethod(stream, skip, jlong{1}) == 1);
    env->CallVoidMethod(stream, mark, 0); // readlimit does not invalidate an asset mark.
    CHECK(env->CallLongMethod(stream, skip, jlong{INT64_MAX}) == expected.size() - 1);
    CHECK(env->CallIntMethod(stream, read) == -1);
    auto buffer = env->NewByteArray(4);
    CHECK(env->CallIntMethod(stream, bulk, buffer, 4, 0) == 0);
    CHECK(env->CallLongMethod(stream, skip, jlong{-1}) == 0);
    env->CallVoidMethod(stream, reset);
    CHECK(env->CallIntMethod(stream, read) == static_cast<unsigned char>(expected[1]));
    env->CallVoidMethod(stream, reset);
    auto tail = static_cast<jbyteArray>(env->CallObjectMethod(stream, all));
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(tail);
    REQUIRE(env->GetArrayLength(tail) == expected.size() - 1);
    std::vector<jbyte> actual(expected.size() - 1);
    env->GetByteArrayRegion(tail, 0, actual.size(), actual.data());
    CHECK(std::equal(actual.begin(), actual.end(), expected.begin() + 1));
    env->CallVoidMethod(stream, close);
    env->CallVoidMethod(stream, close);
    REQUIRE_FALSE(env->ExceptionCheck());
    env->CallIntMethod(stream, available);
    REQUIRE(env->ExceptionCheck());
    auto closed = env->ExceptionOccurred();
    env->ExceptionClear();
    CHECK(env->IsInstanceOf(closed, env->FindClass("java/lang/IllegalStateException")));
  }
  env->PopLocalFrame(nullptr);
}
#endif

TEST_CASE("Android connectivity reflects the real host network through production JNI") {
  using namespace venture;
  java::install_runtime_framework_bridges();
  java::register_framework_natives();
  std::string error;
  REQUIRE(java::start_jvm({VENTURE_TEST_JAVA_HOME, {VENTURE_TEST_FRAMEWORK_JAR}, {"-Xcheck:jni", "-Xverify:all"}}, &error));
  auto* env = java::host_env();
  auto manager_class = env->FindClass("android/net/ConnectivityManager");
  auto info_class = env->FindClass("android/net/NetworkInfo");
  REQUIRE(manager_class); REQUIRE(info_class);
  auto manager = env->NewObject(manager_class, env->GetMethodID(manager_class, "<init>", "()V"));
  REQUIRE(manager);
  const auto expected = platform::active_network_type();
  auto active = env->CallObjectMethod(manager, env->GetMethodID(manager_class, "getActiveNetworkInfo", "()Landroid/net/NetworkInfo;"));
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK((active != nullptr) == (expected != -2));
  if (active) {
    CHECK(env->CallBooleanMethod(active, env->GetMethodID(info_class, "isConnected", "()Z")));
    CHECK(env->CallIntMethod(active, env->GetMethodID(info_class, "getType", "()I")) == expected);
  }
  REQUIRE_FALSE(env->ExceptionCheck());
  for (auto ref : {static_cast<jobject>(manager_class), static_cast<jobject>(info_class), manager, active})
    if (ref) env->DeleteLocalRef(ref);
}

#ifdef __linux__
#include <fstream>
#include <unistd.h>
#include "platform/posix/network_linux.h"
#endif

#ifdef _WIN32
namespace network_events {
PIPFORWARD_CHANGE_CALLBACK route_changed = nullptr;
PIPINTERFACE_CHANGE_CALLBACK interface_changed = nullptr;
PUNICAST_IPADDRESS_CHANGE_CALLBACK address_changed = nullptr;
void* context = nullptr;
unsigned failing = 0;  // 1-based registration to refuse; 0 admits all three
std::vector<HANDLE> live, cancelled;
DWORD admit(unsigned index, HANDLE* out) {
  if (failing == index) return ERROR_NOT_SUPPORTED;
  *out = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(index));
  live.push_back(*out);
  return NO_ERROR;
}
DWORD WINAPI route(ADDRESS_FAMILY, PIPFORWARD_CHANGE_CALLBACK callback, PVOID c, BOOLEAN, HANDLE* out) {
  route_changed = callback; context = c;
  return admit(1, out);
}
DWORD WINAPI iface(ADDRESS_FAMILY, PIPINTERFACE_CHANGE_CALLBACK callback, PVOID c, BOOLEAN, HANDLE* out) {
  interface_changed = callback; context = c;
  return admit(2, out);
}
DWORD WINAPI address(ADDRESS_FAMILY, PUNICAST_IPADDRESS_CHANGE_CALLBACK callback, PVOID c, BOOLEAN, HANDLE* out) {
  address_changed = callback; context = c;
  return admit(3, out);
}
DWORD WINAPI cancel(HANDLE h) {
  cancelled.push_back(h);
  return NO_ERROR;
}
void reset(unsigned fail) {
  route_changed = nullptr; interface_changed = nullptr; address_changed = nullptr; context = nullptr;
  failing = fail; live.clear(); cancelled.clear();
}
bool watch(venture::platform::WatchedQuery<std::int32_t>::Changed changed, void* c) {
  return venture::platform::watch_network_changes(changed, c, {route, iface, address, cancel});
}
}  // namespace network_events

#endif

TEST_CASE("Horizon Android facade delivers shared Platform results through registered JNI") {
  using namespace venture;
  using namespace venture::ovrplatform;
  // Keep the process-installed facade's dependency alive until process exit.
  namespace ot = venture::ovrplatform::test;
  static ot::Arena arena;
  static Session session(ot::app(), std::make_shared<ot::FakeProvider>(), arena.memory());
  java::install_horizon_bridge(session);
  std::string error;
  REQUIRE(java::start_jvm({VENTURE_TEST_JAVA_HOME, {VENTURE_TEST_FRAMEWORK_JAR}, {"-Xcheck:jni", "-Xverify:all"}}, &error));
  auto* env = java::host_env();
  REQUIRE(env->PushLocalFrame(32) == JNI_OK);
  auto core = env->FindClass("horizonos/supplement/hzplatformclientcore/HorizonPlatformCore");
  REQUIRE(core);
  auto instance = env->NewObject(core, env->GetMethodID(core, "<init>", "()V"));
  REQUIRE_FALSE(env->ExceptionCheck());
  auto config = env->FindClass("horizonos/supplement/hzplatformclientcore/AndroidHorizonPlatformConfig");
  auto activity = env->FindClass("android/app/Activity");
  auto context = env->NewObject(activity, env->GetMethodID(activity, "<init>", "()V"));
  REQUIRE_FALSE(env->ExceptionCheck());
  auto app_id = env->NewStringUTF(ot::app().app_id.c_str());
  auto configuration = env->NewObject(config, env->GetMethodID(config, "<init>",
      "(Landroid/content/Context;Ljava/lang/String;)V"), context, app_id);
  REQUIRE_FALSE(env->ExceptionCheck());
  env->CallVoidMethod(instance, env->GetMethodID(core, "setup",
      "(Lhorizonos/supplement/hzplatformclientcore/AndroidHorizonPlatformConfig;)V"), configuration);
  REQUIRE_FALSE(env->ExceptionCheck());
  const auto route = env->CallLongMethod(instance, env->GetMethodID(core, "getGlobalSessionId", "()J"));
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(route != 0);
  auto module = env->NewStringUTF("users");
  auto method = env->NewStringUTF("get_logged_in_user");
  auto data = env->NewStringUTF("{}");
  auto make = env->GetMethodID(core, "makeRequest", "(Ljava/lang/String;Ljava/lang/String;ILjava/lang/String;I)J");
  auto request = env->CallLongMethod(instance, make, module, method, 1, data, -13);
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(request != 0);
  auto count = env->GetMethodID(core, "getMessageCount", "(J)I");
  auto deadline = std::chrono::steady_clock::now() + venture::test::hang_timeout;
  jint queued = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    queued = env->CallIntMethod(instance, count, route);
    REQUIRE_FALSE(env->ExceptionCheck());
    if (queued) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  INFO("Horizon Java message count=", queued, " JNI exception=", bool(env->ExceptionCheck()));
  REQUIRE(queued == 1);
  auto pop = env->GetMethodID(core, "popMessage", "(JZ)Ljava/lang/Object;");
  auto message = env->CallObjectMethod(instance, pop, route, JNI_FALSE);
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(message);
  auto message_class = env->GetObjectClass(message);
  CHECK(env->GetLongField(message, env->GetFieldID(message_class, "requestId", "J")) == request);
  CHECK(env->GetIntField(message, env->GetFieldID(message_class, "statusCode", "I")) == 0);
  CHECK(env->GetIntField(message, env->GetFieldID(message_class, "cookie", "I")) == -13);
  auto json = static_cast<jstring>(env->GetObjectField(message, env->GetFieldID(message_class, "data", "Ljava/lang/String;")));
  auto chars = env->GetStringUTFChars(json, nullptr);
  const auto value = Json::parse(chars);
  env->ReleaseStringUTFChars(json, chars);
  CHECK(value.at("id").get<std::uint64_t>() == 0xfedcba9876543210);
  CHECK(value.at("alias") == "fixture-alias");
  CHECK(env->CallObjectMethod(instance, pop, route, JNI_FALSE) == nullptr);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(arena.live == 0);
  env->PopLocalFrame(nullptr);
}

TEST_CASE("NativeActivity termination crosses HotSpot as ThreadDeath while faults remain errors") {
  using namespace venture;
  java::install_runtime_framework_bridges();
  java::register_framework_natives();
  std::string error;
  REQUIRE(java::start_jvm({VENTURE_TEST_JAVA_HOME, {VENTURE_TEST_FRAMEWORK_JAR}, {"-Xcheck:jni", "-Xverify:all"}}, &error));
  auto* env = java::host_env();
  auto cls = env->FindClass("com/venture/runtime/NativeHost");
  REQUIRE(cls);
  auto ctor = env->GetMethodID(cls, "<init>", "()V");
  REQUIRE(ctor);
  auto host = env->NewObject(cls, ctor);
  REQUIRE(host);
  auto event = env->GetMethodID(cls, "nativeActivityEvent", "(JIJ)V");
  REQUIRE(event);
  static int failure;
  java::set_native_activity_bridge({nullptr,
      [](GuestAddr, java::NativeActivityEvent, GuestAddr, std::string*) -> bool {
        if (failure == 0) throw ThreadExit(0);
        if (failure == 1) throw ProcessExit(23);
        throw std::runtime_error("native lifecycle regression fault");
      }});
  struct Restore { ~Restore() { java::install_runtime_framework_bridges(); } } restore;
  for (failure = 0; failure != 3; ++failure) {
    env->CallVoidMethod(host, event, jlong(1), jint(11), jlong(0));
    REQUIRE(env->ExceptionCheck());
    auto exception = env->ExceptionOccurred();
    env->ExceptionClear();
    auto expected = env->FindClass(failure == 2 ? "java/lang/IllegalStateException" : "java/lang/ThreadDeath");
    REQUIRE(expected);
    CHECK(env->IsInstanceOf(exception, expected));
    env->DeleteLocalRef(expected);
    env->DeleteLocalRef(exception);
  }
  env->DeleteLocalRef(host);
  env->DeleteLocalRef(cls);
}

TEST_CASE("Java Build fields share the immutable native launch identity") {
  std::string error;
  REQUIRE(venture::java::start_jvm({VENTURE_TEST_JAVA_HOME, {VENTURE_TEST_FRAMEWORK_JAR}, {"-Xcheck:jni", "-Xverify:all"}}, &error));
  auto* env = venture::java::host_env();
  auto cls = env->FindClass("android/os/Build");
  REQUIRE(cls);
  const auto& device = *venture::process_config().device_identity;
  for (auto name : {"MODEL", "DEVICE", "PRODUCT", "HARDWARE", "BOARD"}) {
    auto field = env->GetStaticFieldID(cls, name, "Ljava/lang/String;");
    REQUIRE(field);
    auto value = static_cast<jstring>(env->GetStaticObjectField(cls, field));
    REQUIRE(value);
    auto text = env->GetStringUTFChars(value, nullptr);
    CHECK(std::string_view(text) == (std::strcmp(name, "MODEL") == 0 ? device.model : device.device));
    env->ReleaseStringUTFChars(value, text);
    env->DeleteLocalRef(value);
  }
  auto display = static_cast<jstring>(env->GetStaticObjectField(cls,
      env->GetStaticFieldID(cls, "DISPLAY", "Ljava/lang/String;")));
  REQUIRE(display);
  auto text = env->GetStringUTFChars(display, nullptr);
  CHECK(std::string_view(text) == "Venture");
  env->ReleaseStringUTFChars(display, text);
  env->DeleteLocalRef(display);
  CHECK_FALSE(env->ExceptionCheck());
  env->DeleteLocalRef(cls);
}

TEST_CASE("Android class loader reuses app definitions for direct JNI slash lookups") {
  using namespace venture;
  java::install_runtime_framework_bridges();
  java::register_framework_natives();
  std::string error;
  REQUIRE(java::start_jvm({VENTURE_TEST_JAVA_HOME, {VENTURE_TEST_FRAMEWORK_JAR}, {"-Xcheck:jni", "-Xverify:all"}}, &error));
  auto* env = java::host_env();
  auto class_loader = env->FindClass("java/lang/ClassLoader");
  auto system_loader = env->GetStaticMethodID(class_loader, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
  auto parent = env->CallStaticObjectMethod(class_loader, system_loader);
  REQUIRE_FALSE(env->ExceptionCheck());
  auto path_loader = env->FindClass("dalvik/system/PathClassLoader");
  auto constructor = env->GetMethodID(path_loader, "<init>", "(Ljava/lang/String;Ljava/lang/ClassLoader;)V");
  auto path = env->NewStringUTF(VENTURE_TEST_FRAMEWORK_JAR);
  auto loader = env->NewObject(path_loader, constructor, path, parent);
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(loader);
  auto find = env->GetMethodID(path_loader, "findClass", "(Ljava/lang/String;)Ljava/lang/Class;");
  REQUIRE(find);
  auto slash_name = env->NewStringUTF("android/graphics/Rect");
  auto dotted_name = env->NewStringUTF("android.graphics.Rect");
  auto first = env->CallObjectMethod(loader, find, slash_name);
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(first);
  auto second = env->CallObjectMethod(loader, find, dotted_name);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(env->IsSameObject(first, second));
  auto third = env->CallObjectMethod(loader, find, slash_name);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(env->IsSameObject(first, third));
  auto cls = env->FindClass("java/lang/Class");
  auto get_loader = env->GetMethodID(cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
  auto owner = env->CallObjectMethod(first, get_loader);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(env->IsSameObject(owner, loader));
  auto close = env->GetMethodID(path_loader, "close", "()V");
  env->CallVoidMethod(loader, close);
  REQUIRE_FALSE(env->ExceptionCheck());
  for (jobject ref : {owner, static_cast<jobject>(cls), third, second, first,
                     static_cast<jobject>(dotted_name), static_cast<jobject>(slash_name), loader,
                     static_cast<jobject>(path), static_cast<jobject>(path_loader), parent,
                     static_cast<jobject>(class_loader)}) env->DeleteLocalRef(ref);
}

TEST_CASE("production framework JNI owns real NativeWindow references") {
  using namespace venture;
  java::install_runtime_framework_bridges();
  java::register_framework_natives();
  std::string error;
  REQUIRE(java::start_jvm({VENTURE_TEST_JAVA_HOME, {VENTURE_TEST_FRAMEWORK_JAR}, {"-Xcheck:jni", "-Xverify:all"}}, &error));
  auto* env = java::host_env();
  auto& config = process_config();
  config.package = "org.venture.pathbridge";
  config.data_dir = std::filesystem::temp_directory_path() / "Venture-path-bridge-test";
  auto& thread = GuestThread::adopt_current();
  const auto jni = java::guest_env(thread);
  const auto* table = g2h<GuestAddr>(*g2h<GuestAddr>(jni));
  auto file_class = env->FindClass("java/io/File");
  auto file_ctor = env->GetMethodID(file_class, "<init>", "(Ljava/lang/String;)V");
  auto host_path = (config.data_dir / "files/example").generic_string();
  auto path_text = env->NewStringUTF(host_path.c_str());
  auto file = env->NewObject(file_class, file_ctor, path_text);
  REQUIRE_FALSE(env->ExceptionCheck());
  auto path_method = thread.call(table[33], {jni, h2g(file_class), h2g("getAbsolutePath"), h2g("()Ljava/lang/String;")});
  REQUIRE(path_method);
  auto guest_text = g2h<_jstring>(thread.call(table[36], {jni, h2g(file), path_method, 0}));
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(guest_text);
  const auto* guest_path = env->GetStringUTFChars(guest_text, nullptr);
  CHECK(std::string(guest_path) == config.guest_data_dir() + "/files/example");
  env->ReleaseStringUTFChars(guest_text, guest_path);
  // File.toString is another spelling of getPath. Native middleware must not
  // receive a potentially much longer host path through this JNI entry point.
  auto string_method = thread.call(table[33], {jni, h2g(file_class), h2g("toString"), h2g("()Ljava/lang/String;")});
  REQUIRE(string_method);
  auto string_text = g2h<_jstring>(thread.call(table[36], {jni, h2g(file), string_method, 0}));
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(string_text);
  const auto* string_path = env->GetStringUTFChars(string_text, nullptr);
  CHECK(std::string(string_path) == config.guest_data_dir() + "/files/example");
  env->ReleaseStringUTFChars(string_text, string_path);
  env->DeleteLocalRef(string_text);
  // Restrict adaptation to File, leaving arbitrary objects' toString intact.
  auto string_class = env->FindClass("java/lang/String");
  auto identity_method = thread.call(table[33], {jni, h2g(string_class), h2g("toString"), h2g("()Ljava/lang/String;")});
  REQUIRE(identity_method);
  auto identity_text = g2h<_jstring>(thread.call(table[36], {jni, h2g(path_text), identity_method, 0}));
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(identity_text);
  const auto* identity_path = env->GetStringUTFChars(identity_text, nullptr);
  CHECK(std::string(identity_path) == host_path);
  env->ReleaseStringUTFChars(identity_text, identity_path);
  env->DeleteLocalRef(identity_text);
  env->DeleteLocalRef(string_class);
  auto java_text = static_cast<jstring>(env->CallObjectMethod(file, reinterpret_cast<jmethodID>(path_method)));
  REQUIRE_FALSE(env->ExceptionCheck());
  const auto* java_path = env->GetStringUTFChars(java_text, nullptr);
  CHECK(std::filesystem::path(java_path).lexically_normal() == std::filesystem::path(host_path).lexically_normal());
  env->ReleaseStringUTFChars(java_text, java_path);
  // Unity 2019 asks File.getParent for its external-data/cache root. Returning
  // a Windows drive path makes the native engine treat it as APK-relative.
  auto parent_method = thread.call(table[33], {jni, h2g(file_class), h2g("getParent"), h2g("()Ljava/lang/String;")});
  REQUIRE(parent_method);
  auto guest_parent = g2h<_jstring>(thread.call(table[36], {jni, h2g(file), parent_method, 0}));
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(guest_parent);
  const auto* parent_path = env->GetStringUTFChars(guest_parent, nullptr);
  CHECK(std::string(parent_path) == config.guest_data_dir() + "/files");
  env->ReleaseStringUTFChars(guest_parent, parent_path);
  auto java_parent = static_cast<jstring>(env->CallObjectMethod(file, reinterpret_cast<jmethodID>(parent_method)));
  REQUIRE_FALSE(env->ExceptionCheck());
  const auto* host_parent = env->GetStringUTFChars(java_parent, nullptr);
  CHECK(std::filesystem::path(host_parent).lexically_normal() == (config.data_dir / "files").lexically_normal());
  env->ReleaseStringUTFChars(java_parent, host_parent);
  env->DeleteLocalRef(java_parent);
  env->DeleteLocalRef(guest_parent);
  auto guest_ctor = thread.call(table[33], {jni, h2g(file_class), h2g("<init>"), h2g("(Ljava/lang/String;)V")});
  REQUIRE(guest_ctor);
  jvalue constructor_args[1]{};
  constructor_args[0].l = guest_text;
  auto roundtrip = g2h<_jobject>(thread.call(table[30], {jni, h2g(file_class), guest_ctor, h2g(constructor_args)}));
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(roundtrip);
  auto roundtrip_text = static_cast<jstring>(env->CallObjectMethod(roundtrip, reinterpret_cast<jmethodID>(path_method)));
  REQUIRE_FALSE(env->ExceptionCheck());
  const auto* roundtrip_path = env->GetStringUTFChars(roundtrip_text, nullptr);
  CHECK(std::filesystem::path(roundtrip_path).lexically_normal() == std::filesystem::path(host_path).lexically_normal());
  env->ReleaseStringUTFChars(roundtrip_text, roundtrip_path);
  env->DeleteLocalRef(roundtrip_text);
  env->DeleteLocalRef(roundtrip);
  // Unity's AndroidJavaObject finds constructors by reflection; Batman sizes
  // its language downloads with StatFs on getExternalFilesDir's Android path.
  std::filesystem::create_directories(config.data_dir / "files");
  auto statfs_class = env->FindClass("android/os/StatFs");
  REQUIRE(statfs_class);
  auto reflected = env->ToReflectedMethod(statfs_class, env->GetMethodID(statfs_class, "<init>", "(Ljava/lang/String;)V"), JNI_FALSE);
  REQUIRE(reflected);
  auto statfs_ctor = thread.call(table[7], {jni, h2g(reflected)});
  REQUIRE(statfs_ctor);
  auto files_text = env->NewStringUTF((config.guest_data_dir() + "/files").c_str());
  constructor_args[0].l = files_text;
  auto statfs = g2h<_jobject>(thread.call(table[30], {jni, h2g(statfs_class), statfs_ctor, h2g(constructor_args)}));
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(statfs);
  CHECK(env->CallLongMethod(statfs, env->GetMethodID(statfs_class, "getTotalBytes", "()J")) > 0);
  for (jobject ref : std::initializer_list<jobject>{statfs, files_text, reflected, statfs_class}) env->DeleteLocalRef(ref);
  for (jobject ref : std::initializer_list<jobject>{java_text, guest_text, path_text, file, file_class}) env->DeleteLocalRef(ref);
  auto cls = env->FindClass("android/view/Surface");
  REQUIRE(cls);
  auto ctor = env->GetMethodID(cls, "<init>", "(II)V");
  REQUIRE(ctor);
  auto surface = env->NewObject(cls, ctor, 320, 240);
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(surface);
  const auto bridge = android::surface_bridge();
  // The opaque env argument is a guest address, never a host JNIEnv*.
  auto* window = bridge.from_surface(reinterpret_cast<void*>(0x1234), surface);
  REQUIRE(window);
  CHECK(window->width == 320);
  CHECK(window->height == 240);
  CHECK(window->refs == 2);
  auto duplicate = bridge.to_surface(reinterpret_cast<void*>(0x1234), window);
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(duplicate);
  CHECK(window->refs == 3);
  REQUIRE(android::set_window_geometry(window, 2, 2, 1) == 0);
  android::WindowBuffer buffer;
  REQUIRE(android::lock_window(window, &buffer, nullptr) == 0);
  const std::uint32_t rgba[]{0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu};
  std::memcpy(g2h(buffer.bits), rgba, sizeof(rgba));
  REQUIRE(android::unlock_window(window) == 0);
  auto host = env->FindClass("com/venture/runtime/NativeHost");
  REQUIRE(host);
  auto prepare = env->GetStaticMethodID(host, "prepareLooper", "()J");
  auto poll = env->GetStaticMethodID(host, "pollLooper", "(JI)V");
  REQUIRE(prepare);
  REQUIRE(poll);
  auto native_looper = env->CallStaticLongMethod(host, prepare);
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(native_looper);
  CHECK(android::thread_looper() == g2h<android::Looper>(native_looper));
  bool dispatched = false;
  android::thread_looper()->post([&] { dispatched = true; });
  env->CallStaticVoidMethod(host, poll, native_looper, 0);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(dispatched);
  auto handler_thread_class = env->FindClass("android/os/HandlerThread");
  auto thread_name = env->NewStringUTF("native-looper-regression");
  auto handler_thread_ctor = env->GetMethodID(handler_thread_class, "<init>", "(Ljava/lang/String;)V");
  auto handler_thread = env->NewObject(handler_thread_class, handler_thread_ctor, thread_name);
  auto start = env->GetMethodID(handler_thread_class, "start", "()V");
  env->CallVoidMethod(handler_thread, start);
  REQUIRE_FALSE(env->ExceptionCheck());
  auto get_looper = env->GetMethodID(handler_thread_class, "getLooper", "()Landroid/os/Looper;");
  auto java_looper = env->CallObjectMethod(handler_thread, get_looper);
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(java_looper);
  auto handler_class = env->FindClass("android/os/Handler");
  auto handler_ctor = env->GetMethodID(handler_class, "<init>", "(Landroid/os/Looper;)V");
  auto handler = env->NewObject(handler_class, handler_ctor, java_looper);
  auto send = env->GetMethodID(handler_class, "sendEmptyMessage", "(I)Z");
  auto has = env->GetMethodID(handler_class, "hasMessages", "(I)Z");
  CHECK(env->CallBooleanMethod(handler, send, 42));
  REQUIRE_FALSE(env->ExceptionCheck());
  bool pending = true;
  CHECK(test::wait_until([&] {
    pending = env->CallBooleanMethod(handler, has, 42);
    REQUIRE_FALSE(env->ExceptionCheck());
    return !pending;
  }));
  CHECK_FALSE(pending);
  auto quit = env->GetMethodID(handler_thread_class, "quitSafely", "()Z");
  CHECK(env->CallBooleanMethod(handler_thread, quit));
  REQUIRE_FALSE(env->ExceptionCheck());
  auto join = env->GetMethodID(handler_thread_class, "join", "(J)V");
  env->CallVoidMethod(handler_thread, join, jlong(2000));
  REQUIRE_FALSE(env->ExceptionCheck());
  auto alive = env->GetMethodID(handler_thread_class, "isAlive", "()Z");
  CHECK_FALSE(env->CallBooleanMethod(handler_thread, alive));
  REQUIRE_FALSE(env->ExceptionCheck());
  for (jobject ref : std::initializer_list<jobject>{handler_thread_class, static_cast<jobject>(thread_name), handler_thread, java_looper,
                     static_cast<jobject>(handler_class), handler}) env->DeleteLocalRef(ref);
  auto copy = env->GetStaticMethodID(host, "copySurfacePixels", "(JIIIIII[I)I");
  REQUIRE(copy);
  auto pixels = env->NewIntArray(9);
  REQUIRE(pixels);
  CHECK(env->CallStaticIntMethod(host, copy, static_cast<jlong>(h2g(window)), 0, 0, 0, 0, 3, 3, pixels) == 0);
  REQUIRE_FALSE(env->ExceptionCheck());
  jint colors[9]; env->GetIntArrayRegion(pixels, 0, 9, colors);
  CHECK(static_cast<std::uint32_t>(colors[0]) == 0xffff0000u);
  CHECK(static_cast<std::uint32_t>(colors[2]) == 0xff00ff00u);
  CHECK(static_cast<std::uint32_t>(colors[6]) == 0xff0000ffu);
  CHECK(static_cast<std::uint32_t>(colors[8]) == 0xffffffffu);
  env->DeleteLocalRef(pixels);
  env->DeleteLocalRef(host);
  auto release = env->GetMethodID(cls, "release", "()V");
  REQUIRE(release);
  env->CallVoidMethod(surface, release);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(window->refs == 2);
  env->CallVoidMethod(duplicate, release);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(window->refs == 1);
  const auto address = h2g(window);
  android::release(window);
  CHECK_FALSE(android::pin_native_window(address));
  env->DeleteLocalRef(duplicate);
  env->DeleteLocalRef(surface);
  env->DeleteLocalRef(cls);
  REQUIRE(audio::Backend::get().use_null_device(false));
  auto audio_class = env->FindClass("android/media/AudioTrack");
  REQUIRE(audio_class);
  auto audio_ctor = env->GetMethodID(audio_class, "<init>", "(IIIIII)V");
  auto audio_track = env->NewObject(audio_class, audio_ctor, 3, 48000, 12, 2, 3840, 1);
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(audio_track);
  auto audio_state = env->GetMethodID(audio_class, "getState", "()I");
  CHECK(env->CallIntMethod(audio_track, audio_state) == 1);
  REQUIRE_FALSE(env->ExceptionCheck());
  auto audio_play = env->GetMethodID(audio_class, "play", "()V");
  auto audio_stop = env->GetMethodID(audio_class, "stop", "()V");
  env->CallVoidMethod(audio_track, audio_stop);
  REQUIRE_FALSE(env->ExceptionCheck());
  env->CallVoidMethod(audio_track, audio_stop);
  REQUIRE_FALSE(env->ExceptionCheck());
  env->CallVoidMethod(audio_track, audio_play);
  REQUIRE_FALSE(env->ExceptionCheck());
  auto samples = env->NewByteArray(3840);
  auto audio_write = env->GetMethodID(audio_class, "write", "([BIII)I");
  std::vector<jbyte> pcm(3840);
  for (std::size_t i = 0; i < pcm.size(); i += 2) { pcm[i] = 0; pcm[i + 1] = 32; }
  env->SetByteArrayRegion(samples, 0, 3840, pcm.data());
  CHECK(env->CallIntMethod(audio_track, audio_write, samples, 0, 3840, 1) == 3840);
  REQUIRE_FALSE(env->ExceptionCheck());
  const auto stop_begin = std::chrono::steady_clock::now();
  env->CallVoidMethod(audio_track, audio_stop);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(std::chrono::steady_clock::now() - stop_begin < std::chrono::milliseconds(100));
  auto play_state = env->GetMethodID(audio_class, "getPlayState", "()I");
  CHECK(env->CallIntMethod(audio_track, play_state) == 1);
  REQUIRE_FALSE(env->ExceptionCheck());
  // A stalled endpoint has consumed nothing at stop return. Its next pull
  // must still contain every written sample, including the last PCM frame.
  std::vector<float> output(1920);
  audio::Backend::get().process(audio::Direction::output, output.data(), nullptr, 960);
  CHECK(std::all_of(output.begin(), output.end(), [](float x) { return std::abs(x - .25f) < .0001f; }));
  env->CallVoidMethod(audio_track, audio_play);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(env->CallIntMethod(audio_track, audio_write, samples, 0, 3840, 1) == 3840);
  REQUIRE_FALSE(env->ExceptionCheck());
  env->CallVoidMethod(audio_track, audio_stop);
  REQUIRE_FALSE(env->ExceptionCheck());
  // Immediate play must cancel drain without waiting on a device pull.
  env->CallVoidMethod(audio_track, audio_play);
  REQUIRE_FALSE(env->ExceptionCheck());
  env->CallVoidMethod(audio_track, audio_stop);
  REQUIRE_FALSE(env->ExceptionCheck());
  auto audio_flush = env->GetMethodID(audio_class, "flush", "()V");
  env->CallVoidMethod(audio_track, audio_flush);
  REQUIRE_FALSE(env->ExceptionCheck());
  audio::Backend::get().process(audio::Direction::output, output.data(), nullptr, 960);
  CHECK(std::all_of(output.begin(), output.end(), [](float x) { return x == 0; }));
  env->CallVoidMethod(audio_track, audio_play);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(env->CallIntMethod(audio_track, audio_write, samples, 0, 3840, 1) == 3840);
  REQUIRE_FALSE(env->ExceptionCheck());
  auto shared_track = env->NewGlobalRef(audio_track);
  auto shared_samples = env->NewGlobalRef(samples);
  std::promise<void> writer_entered;
  auto entered = writer_entered.get_future();
  auto writer = std::async(std::launch::async, [=, &writer_entered] {
    auto* writer_env = java::host_env();
    writer_entered.set_value();
    const auto count = writer_env->CallIntMethod(shared_track, audio_write, shared_samples, 0, 4, 0);
    const bool failed = writer_env->ExceptionCheck();
    if (failed) writer_env->ExceptionClear();
    return std::pair{count, failed};
  });
  REQUIRE(entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  CHECK(writer.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout);
  env->CallVoidMethod(audio_track, audio_stop);
  REQUIRE_FALSE(env->ExceptionCheck());
  env->CallVoidMethod(audio_track, audio_play);
  REQUIRE_FALSE(env->ExceptionCheck());
  // A writer that had not reached its wait when stop ran (a loaded machine) is not interrupted, as on Android: it
  // waits for space, which the next mixer pull makes.
  const bool interrupted = writer.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  if (!interrupted) audio::Backend::get().process(audio::Direction::output, output.data(), nullptr, 960);
  CHECK(writer.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  env->CallVoidMethod(audio_track, audio_stop);
  REQUIRE_FALSE(env->ExceptionCheck());
  auto audio_release = env->GetMethodID(audio_class, "release", "()V");
  const auto release_begin = std::chrono::steady_clock::now();
  env->CallVoidMethod(audio_track, audio_release);
  REQUIRE_FALSE(env->ExceptionCheck());
  const auto writer_result = writer.get();
  CHECK(writer_result.first == (interrupted ? 0 : 4));
  CHECK_FALSE(writer_result.second);
  env->DeleteGlobalRef(shared_track);
  env->DeleteGlobalRef(shared_samples);
  CHECK(std::chrono::steady_clock::now() - release_begin < std::chrono::milliseconds(100));
  env->CallVoidMethod(audio_track, audio_release);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(env->CallIntMethod(audio_track, audio_state) == 0);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(env->CallIntMethod(audio_track, audio_write, samples, 0, 4, 1) == -3);
  REQUIRE_FALSE(env->ExceptionCheck());
  env->DeleteLocalRef(samples); env->DeleteLocalRef(audio_track); env->DeleteLocalRef(audio_class);
  // AudioRecord reads the default input through the same mixer (IMMERROCK).
  auto record_class = env->FindClass("android/media/AudioRecord");
  REQUIRE(record_class);
  auto record = env->NewObject(record_class, env->GetMethodID(record_class, "<init>", "(IIIII)V"), 1, 48000, 16, 2, 3840);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(env->CallIntMethod(record, env->GetMethodID(record_class, "getState", "()I")) == 1);
  env->CallVoidMethod(record, env->GetMethodID(record_class, "startRecording", "()V"));
  REQUIRE_FALSE(env->ExceptionCheck());
  std::vector<float> input(1920, .5f);  // 960 stereo frames: the mixer is two-channel
  audio::Backend::get().process(audio::Direction::input, nullptr, input.data(), 960);
  auto captured = env->NewShortArray(960);
  const auto read = env->CallIntMethod(record, env->GetMethodID(record_class, "read", "([SIII)I"), captured, 0, 960, 1);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(read > 0);
  if (read > 0) {
    jshort first = 0;
    env->GetShortArrayRegion(captured, 0, 1, &first);
    CHECK(std::abs(first - 16384) < 64);
  }
  env->CallVoidMethod(record, env->GetMethodID(record_class, "stop", "()V"));
  env->CallVoidMethod(record, env->GetMethodID(record_class, "release", "()V"));
  REQUIRE_FALSE(env->ExceptionCheck());
  env->DeleteLocalRef(captured); env->DeleteLocalRef(record); env->DeleteLocalRef(record_class);
  // Effects: pre-processing reports unavailable; output effects keep settings.
  auto suppressor = env->FindClass("android/media/audiofx/NoiseSuppressor");
  REQUIRE(suppressor);
  CHECK_FALSE(env->CallStaticBooleanMethod(suppressor, env->GetStaticMethodID(suppressor, "isAvailable", "()Z")));
  auto equalizer_class = env->FindClass("android/media/audiofx/Equalizer");
  REQUIRE(equalizer_class);
  auto equalizer = env->NewObject(equalizer_class, env->GetMethodID(equalizer_class, "<init>", "(II)V"), 0, 1);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(env->CallShortMethod(equalizer, env->GetMethodID(equalizer_class, "getNumberOfBands", "()S")) == 5);
  REQUIRE_FALSE(env->ExceptionCheck());
  env->DeleteLocalRef(equalizer); env->DeleteLocalRef(equalizer_class); env->DeleteLocalRef(suppressor);
  CHECK(java::stop_jvm(&error));
}
