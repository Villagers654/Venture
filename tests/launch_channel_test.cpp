#include <doctest/doctest.h>
#include "common/launch_details.h"
#include "common/process.h"
#include <thread>
#include <future>

#include "async_wait.h"

TEST_CASE("Launcher pipe preserves deeplinks across an inherited child process") {
  const venture::LaunchDetails expected{4, "application", "message with spaces & unicode \xc3\xa9", "destination", "lobby", "match", 17};
  venture::Process process;
  std::promise<venture::Json> received;
  auto request = received.get_future();
  const auto log = venture::fs::temp_directory_path() / ("venture-launch-pipe-" + std::to_string(venture::current_process_id()) + ".log");
  process.spawn(VENTURE_LAUNCH_CHANNEL_CHILD, {}, log, false, [&](const venture::Json& frame) -> venture::Json {
    if (frame.at("op") == "launch_details") return {{"details", venture::launch_details_json(expected)}};
    received.set_value(frame);
    return {{"success", true}};
  });
  const auto ready = request.wait_for(venture::test::hang_timeout);
  INFO("launch request ready=", ready == std::future_status::ready,
       " child exited=", process.poll().has_value());
  REQUIRE(ready == std::future_status::ready);
  const auto frame = request.get();
  CHECK(frame.at("op") == "launch_other_app");
  CHECK(frame.at("app") == "123");
  CHECK(frame.at("details") == venture::launch_details_json(expected));
  REQUIRE(venture::test::wait_until([&] { return process.poll().has_value(); }));
  CHECK(*process.poll() == 0);
  venture::fs::remove(log);
}

TEST_CASE("Launcher frames reject malformed and oversized details") {
  auto json = venture::launch_details_json({});
  json["type"] = 99;
  CHECK_THROWS(venture::read_launch_details(json));
  json["type"] = std::uint64_t{0x100000004};
  CHECK_THROWS(venture::read_launch_details(json));
  json["type"] = 4;
  json["deeplink"] = std::string(4097, 'x');
  CHECK_THROWS(venture::read_launch_details(json));
  auto [a, b] = venture::PipeChannel::pair();
  std::jthread worker([b](std::stop_token stop) { b->serve([](const venture::Json& value) { return value; }, stop); });
  CHECK(a->call({{"op", "first"}}).at("op") == "first");
  CHECK(a->call({{"op", "second"}}).at("op") == "second");
  a->close(); worker.join();
  CHECK_THROWS(a->call({{"op", "closed"}}));
}
