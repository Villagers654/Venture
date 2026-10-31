#include <doctest/doctest.h>
#include "core/hle_runtime.h"
#include "core/thread_runtime.h"
#include "common/log.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace venture;
// Its own ctest partition: the abort status it records is process-wide.
TEST_CASE("a guest thread stopped by a runtime error is reported and ends the process") {
  const auto path = std::filesystem::temp_directory_path() /
      std::format("venture-thread-failure-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  log::set_file(path.string());
  GuestThread parent(91); parent.attach_to_current_host_thread();
  auto failing = hle::make_thunk("test", "failing call", [](hle::CallContext&) {
    throw std::runtime_error("injected HLE failure");
  });
  // The failure is inside a nested call, as in a game's thread loop.
  const std::array code{0xa9bf7bfdu, 0x910003fdu, 0xd63f0000u, 0xa8c17bfdu, 0xd65f03c0u};
  auto outer = hle::code_stub(code, "outer");  // stp x29,x30; mov x29,sp; blr x0; ldp; ret
  GuestThreadOptions options; options.detached = true;
  auto child = create_guest_thread(outer, failing, options);
  REQUIRE(child != nullptr);
  for (int i = 0; i < 500 && guest_process_exit_status() < 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(guest_process_exit_status() == 134);
  log::set_file("");
  std::ifstream input(path, std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(input), {}};
  std::error_code ignored; std::filesystem::remove(path, ignored);
  CHECK(text.find("stopped by a runtime error: injected HLE failure") != std::string::npos);
  CHECK(text.find("  #01 ") != std::string::npos);
}
