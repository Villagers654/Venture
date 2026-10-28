#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <thread>

#include "core/libc_lifecycle.h"
#include "common/log.h"
#include "hle/libc/libc_a.h"
#include "libc_a_test_support.h"
using namespace venture;
TEST_CASE("libc A properties, callbacks, process defaults and auxv") {
  test::LibcFixture f;
  char out[92]{};
  CHECK(f.invoke("__system_property_get", {h2g("ro.product.model"), h2g(out)}) == 7);
  CHECK(std::string(out) == "Quest 3");
  for (auto name : {"ro.product.device", "ro.product.name", "ro.product.board", "ro.hardware",
                    "ro.product.vendor.device", "ro.product.system.name"}) {
    CHECK(f.invoke("__system_property_get", {h2g(name), h2g(out)}) == 6);
    CHECK(std::string(out) == "eureka");
  }
  CHECK(f.invoke("__system_property_get", {h2g("test.venture"), h2g(out)}) == 10);
  CHECK(std::string(out) == "configured");
  CHECK(f.invoke("__system_property_get", {h2g("ro.kernel.qemu"), h2g(out)}) == 0);
  CHECK(out[0] == 0);
  auto p = f.invoke("__system_property_find", {h2g("test.venture")});
  REQUIRE(p != 0);
  bool called = false;
  auto cb = test::callback([&](auto args) {
    CHECK(args[0] == 123);
    CHECK(std::string(g2h<char>(args[1])) == "test.venture");
    CHECK(std::string(g2h<char>(args[2])) == "configured");
    CHECK(args[3] == 10u << 24);
    called = true;
    return 0;
  });
  f.invoke("__system_property_read_callback", {p, cb, 123});
  CHECK(called);
  CHECK(f.invoke("android_get_device_api_level") == 32);
  CHECK(f.invoke("getauxval", {6}) == 4096);
  CHECK(f.invoke("getauxval", {16}) == cpu_backend().info().hwcap);
  CHECK(f.invoke("getauxval", {26}) == cpu_backend().info().hwcap2);
  auto random = f.invoke("getauxval", {25});
  CHECK(random != 0);
  CHECK(f.invoke("getauxval", {25}) == random);
  CHECK(std::string(g2h<char>(f.invoke("getauxval", {15}))) == "aarch64");
  CHECK(f.invoke("getauxval", {9999}) == 0);
  CHECK(*hle::libc::error_location() == hle::err::kENOENT);
  CHECK(f.invoke("__errno") == h2g(f.thread.errno_location()));
  CHECK(f.invoke("__libc_current_sigrtmin") == 41);
}
TEST_CASE("libc A guest environment putenv aliasing and stable getenv storage") {
  test::LibcFixture f;
  f.invoke("clearenv");
  CHECK(f.invoke("setenv", {h2g("VENTURE_TEST"), h2g("first"), 1}) == 0);
  auto original = f.invoke("getenv", {h2g("VENTURE_TEST")});
  CHECK(std::string(g2h<char>(original)) == "first");
  f.invoke("setenv", {h2g("VENTURE_TEST"), h2g("second"), 0});
  CHECK(f.invoke("getenv", {h2g("VENTURE_TEST")}) == original);
  f.invoke("setenv", {h2g("VENTURE_TEST"), h2g("second"), 1});
  CHECK(std::string(g2h<char>(original)) == "first");
  auto guest_environment = *g2h<char**>(f.data("environ"));
  CHECK(std::string(guest_environment[0]) == "VENTURE_TEST=second");
  CHECK(guest_environment[1] == nullptr);
  char storage[] = "VENTURE_LIVE=old";
  f.invoke("putenv", {h2g(storage)});
  storage[std::string_view("VENTURE_LIVE=").size()] = 'n';
  CHECK(std::string(g2h<char>(f.invoke("getenv", {h2g("VENTURE_LIVE")}))) == "nld");
  CHECK(f.invoke("unsetenv", {h2g("VENTURE_TEST")}) == 0);
  CHECK(f.invoke("getenv", {h2g("VENTURE_TEST")}) == 0);
  f.invoke("clearenv");
}
TEST_CASE("libc A atexit reentrancy, TLS destructors and C++ initialization guards") {
  test::LibcFixture f;
  std::vector<int> order;
  auto cb = test::callback([&](auto args) {
    order.push_back(static_cast<int>(args[0]));
    return 0;
  });
  f.invoke("__cxa_atexit", {cb, 1, 11});
  f.invoke("__cxa_atexit", {cb, 2, 22});
  f.invoke("__cxa_atexit", {cb, 3, 11});
  f.invoke("__cxa_finalize", {11});
  CHECK(order == std::vector<int>{3, 1});
  run_guest_exit_handlers(f.thread);
  CHECK(order == std::vector<int>{3, 1, 2});
  f.invoke("__cxa_thread_atexit_impl", {cb, 4, 0});
  f.invoke("__cxa_thread_atexit_impl", {cb, 5, 0});
  run_guest_thread_destructors(f.thread);
  CHECK(order == std::vector<int>{3, 1, 2, 5, 4});
  alignas(8) std::uint64_t guard = 0;
  CHECK(f.invoke("__cxa_guard_acquire", {h2g(&guard)}, "libstdc++.so") == 1);
  f.invoke("__cxa_guard_abort", {h2g(&guard)}, "libstdc++.so");
  CHECK(guard == 0);
  std::atomic<int> initialized{0};
  int object = 0;
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i)
    threads.emplace_back([&] {
      test::LibcFixture worker;
      if (worker.invoke("__cxa_guard_acquire", {h2g(&guard)}, "libstdc++.so")) {
        object = 42;
        initialized.fetch_add(1);
        worker.invoke("__cxa_guard_release", {h2g(&guard)}, "libstdc++.so");
      }
      CHECK(object == 42);
    });
  for (auto& t : threads) t.join();
  CHECK(initialized == 1);
  CHECK(guard == 1);
}
TEST_CASE("liblog host output redacts Horizon response payloads") {
  test::LibcFixture f;
  auto path = std::filesystem::temp_directory_path() /
      ("venture-horizon-log-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Restore {
    std::filesystem::path path;
    ~Restore() {
      log::set_file("");
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } restore{path};
  log::set_file(path.string());
  const char* text = "Message data: {\"id\":\"fixture-private-id\",\"alias\":\"fixture-private-alias\","
                     "\"nonce\":\"fixture-private-proof\"}\nfixture-private-tail";
  CHECK(f.invoke("__android_log_write", {4, h2g("Unity"), h2g(text)}, "liblog.so") == 1);
  for (auto message : {"Oculus: Success getting user proof. fixture-private-proof",
                       "[AuthenticationState] UserID: fixture-private-id",
                       "[Analytics] Analytics ID Retrieval Success. Game Specific RID: fixture-private-rid "
                       "Platform RID: fixture-private-rid Game Specific ID: fixture-private-id "
                       "Platform ID: fixture-private-id"})
    CHECK(f.invoke("__android_log_write", {4, h2g("Unity"), h2g(message)}, "liblog.so") == 1);
  CHECK(f.invoke("__android_log_write", {4, h2g("Unity"), h2g("ordinary diagnostic")}, "liblog.so") == 1);
  log::set_file("");
  std::ifstream input(path, std::ios::binary);
  const std::string output((std::istreambuf_iterator<char>(input)), {});
  CHECK(output.find("Message data: [Platform value redacted]") != std::string::npos);
  CHECK(output.find("Oculus: Success getting user proof. [Platform value redacted]") != std::string::npos);
  CHECK(output.find("[AuthenticationState] UserID: [Platform value redacted]") != std::string::npos);
  CHECK(output.find("[Analytics] Analytics ID Retrieval Success.[Platform value redacted]") != std::string::npos);
  CHECK(output.find("ordinary diagnostic") != std::string::npos);
  CHECK(output.find("fixture-private-") == std::string::npos);
}
