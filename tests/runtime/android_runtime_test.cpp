#include "android/errors.h"
#include <doctest/doctest.h>
#include "android_test_support.h"
#include "android/native_window.h"
#include "android/native_activity.h"
#include "android/input.h"
#include "android/looper.h"
#include "android/libc_bridge.h"
#include "core/shutdown_trace.h"
#include "android/choreographer.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/wait.h"
#include "core/signals.h"
#include <nlohmann/json.hpp>
#include <bit>
#include <array>
#include <chrono>
#include <fstream>
#include <future>
#include <thread>

#include "../async_wait.h"

using namespace venture;
using namespace venture::android;
namespace {
std::uint64_t call(const char* name, std::initializer_list<std::uint64_t> args = {}) {
  return test::android_call("libandroid.so", name, args).x0;
}
}  // namespace
TEST_CASE("android windows: invalid pointers, lifetime, geometry and CPU lock ABI") {
  CHECK(native_window_from_guest(1) == nullptr);
  CHECK(native_window_from_guest(UINT64_MAX) == nullptr);
  auto* window = create_native_window(128, 64);
  REQUIRE(window);
  auto address = h2g(window);
  CHECK(call("ANativeWindow_getWidth", {address}) == 128);
  CHECK(set_window_geometry(window, 0, 4, 1) == -err::invalid);
  CHECK(set_window_geometry(window, 32, 16, 4) == 0);
  WindowBuffer buffer{};
  Rect dirty{};
  CHECK(lock_window(window, &buffer, &dirty) == 0);
  CHECK(buffer.width == 32);
  CHECK(buffer.height == 16);
  CHECK(buffer.format == 4);
  CHECK(buffer.bits != 0);
  CHECK(sizeof(buffer) == 48);
  CHECK(offsetof(WindowBuffer, bits) == 16);
  CHECK(lock_window(window, &buffer, nullptr) == -err::busy);
  CHECK(set_window_geometry(window, 8, 8, 1) == -err::busy);
  CHECK(unlock_window(window) == 0);
  CHECK(unlock_window(window) == -err::invalid);
  CHECK(set_window_geometry(window, 0, 0, 1) == 0);
  CHECK(window->width == 128);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i)
    threads.emplace_back([=] {
      for (int j = 0; j < 1000; ++j) {
        acquire(window);
        release(window);
      }
    });
  for (auto& thread : threads) thread.join();
  CHECK(window->refs == 1);
  auto pin = pin_native_window(address);
  release(window);
  CHECK(native_window_from_guest(address) == nullptr);
  CHECK(pin->width == 128);
  CHECK(static_cast<std::int64_t>(call("ANativeWindow_getWidth", {address})) == -err::invalid);
}
TEST_CASE("android AConfiguration ABI and Quest resource policy") {
  auto config = call("AConfiguration_new");
  REQUIRE(config);
  auto manager = call("AAssetManager_fromJava", {0, 1});
  call("AConfiguration_fromAssetManager", {config, manager});
  CHECK(call("AConfiguration_getSdkVersion", {config}) == 32);
  CHECK(call("AConfiguration_getDensity", {config}) == 320);
  CHECK(call("AConfiguration_getOrientation", {config}) == 2);
  CHECK(call("AConfiguration_getTouchscreen", {config}) == 1);
  CHECK(call("AConfiguration_getKeyboard", {config}) == 1);
  CHECK(call("AConfiguration_getUiModeType", {config}) == 7);
  char language[3]{0, 0, '!'}, country[3]{0, 0, '!'};
  call("AConfiguration_getLanguage", {config, h2g(language)});
  call("AConfiguration_getCountry", {config, h2g(country)});
  CHECK(std::string(language, 3) == "en!");
  CHECK(std::string(country, 3) == "US!");
  auto other = call("AConfiguration_new");
  call("AConfiguration_copy", {other, config});
  CHECK(call("AConfiguration_diff", {config, other}) == 0);
  call("AConfiguration_setDensity", {other, 240});
  CHECK(call("AConfiguration_diff", {config, other}) == 256);
  call("AConfiguration_delete", {config});
  call("AConfiguration_delete", {other});
}
TEST_CASE("android ALooper fd readiness, callback removal, timeout, wake, interruption") {
  Looper looper(1);
  auto& thread = GuestThread::adopt_current();
  int fd = test::android_test_fd(), out_fd = -1, events = -1;
  GuestAddr data = 0;
  CHECK(looper.add_fd(fd, 42, 3, 0, 1234) == 1);
  auto begin = monotonic_ns();
  CHECK(looper.poll(8, nullptr, nullptr, nullptr) == -3);
  CHECK(monotonic_ns() - begin >= 7000000);
  test::android_fd_ready(fd, 4);  // Linux POLLOUT becomes ALOOPER_EVENT_OUTPUT=2
  CHECK(looper.poll(0, &out_fd, &events, &data) == 42);
  CHECK(out_fd == fd);
  CHECK(events == 2);
  CHECK(data == 1234);
  test::android_fd_ready(fd, 0);
  int callbacks = 0;
  auto cb = test::android_callback([&](const GuestCallArgs& args) {
    CHECK(args.integers[0] == static_cast<std::uint64_t>(fd));
    CHECK(args.integers[1] == 1);
    ++callbacks;
    return GuestCallResult{};
  });
  CHECK(looper.add_fd(fd, 99, 1, cb, 1234) == 1);
  test::android_fd_ready(fd, 1);
  CHECK(looper.poll(0, nullptr, nullptr, nullptr) == -2);
  CHECK(callbacks == 1);
  CHECK(looper.poll(0, nullptr, nullptr, nullptr) == -3);
  CHECK(looper.remove_fd(fd) == 0);
  test::android_remove_callback(cb);
  auto waking = std::async(std::launch::async, [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    looper.wake();
  });
  CHECK(looper.poll(-1, nullptr, nullptr, nullptr) == -1);
  waking.get();
  auto signal_cb = test::android_callback([](const GuestCallArgs&) { return GuestCallResult{}; });
  signals::Action action{signal_cb}, old_action;
  REQUIRE(signals::set_action(10, &action, &old_action) == 0);
  REQUIRE(signals::send_to_thread(thread.tid(), {10}) == 0);
  CHECK(looper.poll(100, nullptr, nullptr, nullptr) == -1);
  signals::set_action(10, &old_action, nullptr);
  test::android_remove_callback(signal_cb);
  CHECK(looper.add_fd(fd, 42, 1, 0, 0) == 1);
  test::android_close_fd(fd);
  CHECK(looper.poll(0, &out_fd, &events, nullptr) == 42);
  CHECK(events == 16);
  CHECK(looper.remove_fd(fd) == 1);
}
TEST_CASE("android input queue delivers once, finishes once and preserves motion coordinates") {
  auto* l = prepare_looper(1);
  auto* q = create_input_queue();
  attach_input_queue(q, l, 71, 0, 99);
  InputEvent e;
  e.type = 2;
  e.source = 0x1002;
  e.event_time = 123456;
  e.pointers.resize(1);
  e.pointers[0].id = 9;
  e.pointers[0].axes[0] = 12.5f;
  e.pointers[0].axes[1] = 3;
  int completions = 0;
  REQUIRE(enqueue_input(q, std::move(e), [&](bool handled) {
    CHECK(handled);
    ++completions;
  }));
  CHECK(l->poll(0, nullptr, nullptr, nullptr) == 71);
  GuestAddr event = 0;
  CHECK(input_get_event(q, &event) == 0);
  REQUIRE(event);
  CHECK(call("AInputEvent_getType", {event}) == 2);
  CHECK(call("AMotionEvent_getPointerCount", {event}) == 1);
  CHECK(call("AMotionEvent_getPointerId", {event, 0}) == 9);
  auto x = test::android_call("libandroid.so", "AMotionEvent_getX", {event, 0});
  CHECK(std::bit_cast<float>(static_cast<std::uint32_t>(x.v0.lo)) == 12.5f);
  CHECK(input_has_events(q) == 0);
  CHECK(input_get_event(q, &event) == -err::again);
  input_finish_event(q, event, true);
  CHECK(completions == 1);
  CHECK_FALSE(pin_input_event(event));
  input_finish_event(q, event, true);
  CHECK(completions == 1);
  destroy_input_queue(q);
}
TEST_CASE("android NativeActivity AArch64 fields and ordered lifecycle callbacks") {
  const auto root = std::filesystem::temp_directory_path() /
      ("venture-native-files-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(std::filesystem::create_directory(root));
  struct RestoreData {
    std::filesystem::path old, root;
    ~RestoreData() { process_config().data_dir = old; std::filesystem::remove_all(root); }
  } restore{process_config().data_dir, root};
  process_config().data_dir = root;
  process_config().package = "com.venture.test";
  HleLibrary library("libtest.so");
  std::vector<std::string> events;
  std::vector<GuestAddr> callback_addresses;
  GuestAddr address = 0;
  auto make = [&](const char* name) {
    auto cb = test::android_callback([&, name](const GuestCallArgs& args) {
      CHECK(args.integers[0] == address);
      events.emplace_back(name);
      return GuestCallResult{};
    });
    callback_addresses.push_back(cb);
    return cb;
  };
  auto entry = test::android_callback([&](const GuestCallArgs& args) {
    address = args.integers[0];
    auto* a = g2h<GuestNativeActivity>(address);
    CHECK(a->vm == 0x100);
    CHECK(a->env == 0x200);
    CHECK(a->clazz == 0x300);
    CHECK(a->sdkVersion == 32);
    CHECK(std::filesystem::is_directory(root / "files"));
    CHECK(std::filesystem::is_directory(root / "external-data" / "files"));
    CHECK(std::string(g2h<char>(a->internalDataPath)) == "/data/user/0/com.venture.test/files");
    CHECK(std::string(g2h<char>(a->externalDataPath)) == "/storage/emulated/0/Android/data/com.venture.test/files");
    CHECK(std::string(g2h<char>(a->obbPath)) == "/storage/emulated/0/Android/obb/com.venture.test");
    CHECK(a->instance == 0);
    CHECK(a->assetManager != 0);
    auto* c = g2h<GuestActivityCallbacks>(a->callbacks);
    c->onStart = make("start");
    c->onResume = make("resume");
    c->onPause = make("pause");
    c->onStop = make("stop");
    c->onNativeWindowCreated = make("window+");
    c->onNativeWindowDestroyed = make("window-");
    c->onInputQueueCreated = make("input+");
    c->onInputQueueDestroyed = make("input-");
    c->onWindowFocusChanged = make("focus");
    c->onDestroy = make("destroy");
    return GuestCallResult{};
  });
  library.add_function("customEntry", entry);
  std::string error;
  auto a = NativeActivity::create(library, 0x100, 0x200, 0x300, &error, "customEntry");
  REQUIRE(a);
  auto* window = create_native_window(128, 128);
  auto* input = create_input_queue();
  a->event(ActivityEvent::start);
  a->event(ActivityEvent::resume);
  a->window(window);
  a->input_queue(input);
  a->focus(true);
  std::byte saved_bytes[]{std::byte{1}, std::byte{2}, std::byte{3}};
  bool freed = false;
  auto save_cb = test::android_callback([&](const GuestCallArgs& args) {
    CHECK(args.integers[0] == a->address());
    *g2h<std::uint64_t>(args.integers[1]) = sizeof(saved_bytes);
    return GuestCallResult{h2g(saved_bytes)};
  });
  auto free_cb = test::android_callback([&](const GuestCallArgs& args) {
    CHECK(args.integers[0] == h2g(saved_bytes));
    freed = true;
    return GuestCallResult{};
  });
  auto* guest_activity = g2h<GuestNativeActivity>(a->address());
  g2h<GuestActivityCallbacks>(guest_activity->callbacks)->onSaveInstanceState = save_cb;
  CHECK(a->save_state(free_cb) == std::vector<std::byte>(std::begin(saved_bytes), std::end(saved_bytes)));
  CHECK(freed);
  test::android_remove_callback(save_cb);
  test::android_remove_callback(free_cb);
  call("ANativeActivity_setWindowFormat", {a->address(), 4});
  CHECK(window->format == 4);
  a->event(ActivityEvent::pause);
  a->event(ActivityEvent::stop);
  a->destroy();
  a->destroy();
  CHECK(events == std::vector<std::string>{"start", "resume", "window+", "input+", "focus", "pause", "stop",
                                           "input-", "window-", "destroy"});
  CHECK(a->address() == 0);
  CHECK(window->refs == 1);
  release(window);
  destroy_input_queue(input);
  test::android_remove_callback(entry);
  for (auto cb : callback_addresses) test::android_remove_callback(cb);
}
TEST_CASE("android choreographer executes on looper and refresh carries the int64 vsync period") {
  auto* l = prepare_looper(1);
  display_vsync(monotonic_ns(), 11111111);
  auto c = call("AChoreographer_getInstance");
  REQUIRE(c);
  bool frame = false, refresh = false;
  auto frame_cb = test::android_callback([&](const GuestCallArgs& args) {
    CHECK(args.integers[0] <= static_cast<std::uint64_t>(monotonic_ns()));
    CHECK(args.integers[1] == 17);
    frame = true;
    return GuestCallResult{};
  });
  auto refresh_cb = test::android_callback([&](const GuestCallArgs& args) {
    // NDK android/choreographer.h: callback(int64_t vsyncPeriodNanos, void*).
    REQUIRE(args.integers.size() >= 2); // test CPU snapshots all eight GP registers
    CHECK(args.integers[0] == 11111111);
    CHECK(args.integers[1] == 18);
    refresh = true;
    return GuestCallResult{};
  });
  call("AChoreographer_registerRefreshRateCallback", {c, refresh_cb, 18});
  call("AChoreographer_postFrameCallback64", {c, frame_cb, 17});
  CHECK(test::wait_until([&] {
    l->poll(20, nullptr, nullptr, nullptr);
    return frame && refresh;
  }));
  CHECK(frame);
  CHECK(refresh);
  call("AChoreographer_unregisterRefreshRateCallback", {c, refresh_cb, 18});
  test::android_remove_callback(frame_cb);
  test::android_remove_callback(refresh_cb);
}
