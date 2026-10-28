#include <doctest/doctest.h>
#include "core/engine_policy.h"
#include "core/linker.h"
#include "core/thread.h"
#include "core/os_quit.h"
#include <thread>
#include <chrono>
using namespace venture;
TEST_CASE("engine pacing uses the guest SDK without changing fixed game time or entry arguments") {
  GuestThread thread(61001); thread.attach_to_current_host_thread();
  auto& linker = Linker::get();
  auto* library = linker.open(VENTURE_ENGINE_POLICY_FIXTURE, kRtldNow, nullptr);
  REQUIRE(library != nullptr);
  auto symbol = [&](const char* name) { auto p = library->find_symbol(name); REQUIRE(p != 0); return p; };
  auto configure = symbol("fixture_configure"), update = symbol("_ZN9CXConsole6UpdateEv");
  auto cap = symbol("fixture_cap"), fixed = symbol("fixture_fixed_step"), writes = symbol("fixture_writes");
  const auto console = thread.call(symbol("fixture_console"), {});
  REQUIRE(console != 0);
  engine_policy::on_vrapi_session(thread);
  for (int mode : {1, 0, 6, 2, 3, 4, 5}) {
    INFO("fixture mode ", mode);
    thread.call(configure, {static_cast<unsigned>(mode)});
    CHECK(thread.call(update, {console}) == console + 7);
    CHECK(thread.call(fixed, GuestCallArgs{}).f32() == doctest::Approx(mode == 1 || mode == 6 ? 1.f/72 : 0));
    CHECK(thread.call(cap, GuestCallArgs{}).f32() == (mode == 1 || mode >= 4 ? 72 : -1));
    CHECK(thread.call(writes, {}) == (mode == 0 || mode == 2 || mode == 6 ? 1 : 0));
    // The periodic policy should not keep writing an already released cap.
    CHECK(thread.call(update, {console}) == console + 7);
    CHECK(thread.call(writes, {}) == (mode == 0 || mode == 2 || mode == 6 ? 1 : 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
  }
  thread.call(configure, {0});
  os_quit::host_requested.store(true);
  CHECK(thread.call(update, {console}) == console + 7);
  CHECK(thread.call(cap, GuestCallArgs{}).f32() == 72);
  CHECK(thread.call(writes, {}) == 0);
  CHECK(thread.call(symbol("fixture_queries"), {}) == 0);
  os_quit::host_requested.store(false);
  CHECK(linker.close(library) == 0);
}
