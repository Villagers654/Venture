#include "platform/runtime_teardown_grace.h"
#include <doctest/doctest.h>
#include "platform/runtime_child.h"
#include "common/ipc.h"
#include "common/paths.h"
#include "common/environment.h"
#include <chrono>
#include <thread>

#include <fstream>
#include <sstream>

TEST_CASE("OS termination outcome survives the worker's final pipe record") {
  const auto child = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD,
      {"os-outcome"}, {}, [] { return false; }, {}, true);
  CHECK(child.status == 0);
  CHECK(child.os_termination);
  CHECK_FALSE(child.supervisor_stopped);
}
TEST_CASE("Guest abort remains fatal when its reporter exits zero") {
  const auto child = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD,
      {"abort-reporter-exit-zero"}, {}, [] { return false; }, {}, true);
  CHECK(child.status == 134);
  CHECK_FALSE(child.os_termination);
}
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

#ifdef _WIN32
#endif
TEST_CASE("App supervisor preserves worker failure after a stop or elapsed bound") {
  using venture::platform::RuntimeChildResult;
  for (const auto status : {1, 85, 124, -1073741819}) {
    for (const auto restart : {false, true}) {
      const RuntimeChildResult failed{status, restart, {}};
      REQUIRE(failed.completion_status(true).has_value());
      CHECK(*failed.completion_status(true) == status);
      CHECK(*failed.completion_status(false) == status);
    }
  }
  CHECK(RuntimeChildResult{0, false, {}}.completion_status(true) == 0);
  CHECK(RuntimeChildResult{0, true, {}}.completion_status(true) == 0);
  CHECK_FALSE(RuntimeChildResult{0, true, {}}.completion_status(false).has_value());
}
TEST_CASE("Host quit backstop ends the worker without per-thread native teardown") {
  for (const auto* mode : {"host-quit-backstop", "host-quit-backstop-signal"}) {
    const auto child = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD,
        {mode}, {}, [] { return false; }, {}, true);
    CHECK(child.status == (std::string_view(mode) == "host-quit-backstop" ? 124 : 139));
    CHECK_FALSE(child.restart);
    CHECK_FALSE(child.supervisor_stopped);
  }
}
TEST_CASE("Supervisor allows a configured host quit backstop plus evidence flush margin") {
  using namespace std::chrono_literals;
  using venture::platform::RuntimeTeardownGrace;
  RuntimeTeardownGrace grace(true, venture::quit_backstop("15000") + 5s);
  const auto begin = RuntimeTeardownGrace::Clock::time_point{};
  grace.request(begin);
  grace.posted(begin + 8s);
  CHECK_FALSE(grace.expired(begin + 18s));
  CHECK_FALSE(grace.expired(begin + 27999ms));
  CHECK(grace.expired(begin + 28s));
}

TEST_CASE("Supervisor extends its deadline when the worker waits for a file save") {
  using namespace std::chrono_literals;
  using venture::platform::RuntimeTeardownGrace;
  RuntimeTeardownGrace grace(true, venture::quit_backstop(nullptr) + 5s);
  const auto begin = RuntimeTeardownGrace::Clock::time_point{};
  grace.request(begin);
  const char posted = 1, saving = 3;
  grace.consume(std::span(&posted, 1), begin, [](auto, auto, auto) {});
  CHECK(grace.expired(begin + 10s));
  grace.consume(std::span(&saving, 1), begin + 6s, [](auto, auto, auto) {});
  // The worker's 30 s save window plus its 8 s destroy settle bound, and 5 s.
  CHECK_FALSE(grace.expired(begin + 42999ms));
  CHECK(grace.expired(begin + 43s));
}

TEST_CASE("Supervisor gives committed exit stages their own budget past the posted deadline") {
  using namespace std::chrono_literals;
  using venture::platform::RuntimeTeardownGrace;
  using venture::platform::RuntimeExitWait;
  RuntimeTeardownGrace grace(true, venture::quit_backstop(nullptr) + 5s);
  const auto begin = RuntimeTeardownGrace::Clock::time_point{};
  grace.request(begin);
  const char posted = 1;
  grace.consume(std::span(&posted, 1), begin, [](auto, auto, auto) {});
  // The worker's destroy settle used its 8 s bound; then it starts exiting.
  const char stage[10] = {2, static_cast<char>(RuntimeExitWait::vulkan_shutdown), 0, 0, 0, 0, 0, 0, 0, 0};
  grace.consume(std::span(stage, sizeof(stage)), begin + 8300ms, [](auto, auto, auto) {});
  CHECK_FALSE(grace.expired(begin + 11s));  // past posted + 10 s
  CHECK_FALSE(grace.expired(begin + 18s));
  CHECK(grace.expired(begin + 18300ms));
}

TEST_CASE("App worker restart uses a private channel and fresh processes") {
  using namespace venture::platform;
  auto normal = run_runtime_child(VENTURE_RESTART_CHILD, {"exit"}, {}, [] { return false; });
  CHECK(normal.status == 85);
  CHECK_FALSE(normal.restart);
  auto restart = run_runtime_child(VENTURE_RESTART_CHILD, {"restart"}, {}, [] { return false; });
  CHECK(restart.status == 0);
  CHECK(restart.restart);
  auto next = run_runtime_child(VENTURE_RESTART_CHILD, {"arguments", "a space, a quote \" and \\ path"}, {}, [] { return false; });
  CHECK(next.status == 0);
  CHECK_FALSE(next.restart);
}
TEST_CASE("Concurrent worker exit cannot overtake a claimed supervisor relaunch") {
  using namespace venture::platform;
  // Control reproduces the original claim-to-publication race deterministically.
  const auto lost = run_runtime_child(VENTURE_RESTART_CHILD,
      {"restart-race-control"}, {}, [] { return false; });
  REQUIRE(lost.status == 0);
  REQUIRE_FALSE(lost.restart);
  const auto first = run_runtime_child(VENTURE_RESTART_CHILD,
      {"restart-race"}, {}, [] { return false; });
  REQUIRE(first.status == 0);
  REQUIRE_FALSE(first.completion_status(false).has_value());
  REQUIRE(first.launch_payload == "claimed-cold-launch");
  // Drive the supervisor continuation into a fresh worker with the saved intent.
  const auto fresh = run_runtime_child(VENTURE_RESTART_CHILD,
      {"received-cold-launch"}, {}, [] { return false; }, first.launch_payload);
  CHECK(fresh.status == 0);
  CHECK_FALSE(fresh.restart);
}
TEST_CASE("Supervisor never relaunches a worker with a fatal restart outcome") {
  const auto failed = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD,
      {"failed-restart"}, {}, [] { return false; });
  CHECK(failed.status == 139);
  CHECK(failed.completion_status(false) == 139);
  CHECK(failed.completion_status(true) == 139);
}
TEST_CASE("Kernel worker termination bypasses blocked Windows DLL detach") {
#ifdef _WIN32
  const auto control_began = std::chrono::steady_clock::now();
  const auto control = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD,
      {"detach-control", VENTURE_EXIT_DETACH_FIXTURE}, {}, [&] {
        return std::chrono::steady_clock::now() - control_began >= std::chrono::milliseconds(100);
      });
  CHECK(control.status == 124);
  const auto began = std::chrono::steady_clock::now();
  const auto child = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD,
      {"detach-hang", VENTURE_EXIT_DETACH_FIXTURE}, {}, [&] {
        return std::chrono::steady_clock::now() - began >= std::chrono::seconds(2);
      });
  CHECK(child.status == 0);
  CHECK_FALSE(child.restart);
  CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(2));
#endif
}
TEST_CASE("Post-exit log flush has an independent bound and identifies its waiter") {
  const auto began = std::chrono::steady_clock::now();
  const auto child = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD,
      {"post-exit-wait"}, {}, [] { return false; }, {}, true);
  CHECK(child.status == 124);
  CHECK(child.exit_wait == venture::platform::RuntimeExitWait::log_flush);
  CHECK(child.exit_wait_tid != 0);
  CHECK(child.supervisor_stopped);
  CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(16));
}
TEST_CASE("Exit preparation cannot consume terminal exit grace and stream fragments retain metadata") {
  using namespace venture::platform;
  RuntimeTeardownGrace grace(true);
  const auto began = RuntimeTeardownGrace::Clock::now();
  char prepare[10]{2, char(RuntimeExitWait::profiler_flush), 42};
  grace.consume(std::span(prepare, 3), began, [](auto, auto, auto) {});
  grace.consume(std::span(prepare+3, 7), began, [](auto, auto, auto) {});
  CHECK_FALSE(grace.expired(began + std::chrono::seconds(90)));
  char flush[10]{2, char(RuntimeExitWait::log_flush), 43};
  grace.consume(std::span(flush), began + std::chrono::seconds(90), [](auto, auto, auto) {});
  CHECK(grace.exit_wait_tid == 43);
  CHECK_FALSE(grace.expired(began + std::chrono::seconds(99)));
  CHECK(grace.expired(began + std::chrono::seconds(100)));
}
TEST_CASE("A blocked dump helper cannot prevent supervisor termination") {
#ifdef _WIN32
  const auto* local = venture::environment::getenv("LOCALAPPDATA");
  REQUIRE(local != nullptr);
  const auto began = std::chrono::steady_clock::now();
  const auto run = venture::paths::from_utf8(local) / "Venture/dev-cache/runs/diagnostic-tests" /
      ("dump-helper-bound-" + std::to_string(began.time_since_epoch().count()));
  REQUIRE(SetEnvironmentVariableA("VENTURE_TEST_DUMP_HELPER_HANG", "1"));
  struct Clear { ~Clear() { SetEnvironmentVariableA("VENTURE_TEST_DUMP_HELPER_HANG", nullptr); } } clear;
  const auto child = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD,
      {"wait"}, {}, [&] { return std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(100); }, {}, false, run);
  CHECK(child.status == 124);
  CHECK(child.dump_error == ERROR_TIMEOUT);
  CHECK(child.timeout_dump.empty());
  CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(22));
  REQUIRE(venture::paths::within(std::filesystem::weakly_canonical(venture::paths::from_utf8(local) / "Venture/dev-cache/runs/diagnostic-tests"), std::filesystem::weakly_canonical(run)));
  std::filesystem::remove_all(run);
#endif
}
TEST_CASE("App worker cancellation remains bounded") {
  using namespace std::chrono;
  const auto began = steady_clock::now();
  auto run_directory = std::filesystem::path{};
#ifdef _WIN32
  const auto* local = venture::environment::getenv("LOCALAPPDATA");
  REQUIRE(local != nullptr);
  const auto root = venture::paths::from_utf8(local) / "Venture/dev-cache/runs";
  run_directory = root / "diagnostic-tests" /
      ("worker-timeout-" + std::to_string(began.time_since_epoch().count()));
  std::filesystem::create_directories(run_directory);
#endif
  auto child = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD, {"wait"}, {}, [&] {
    return steady_clock::now() - began >= milliseconds(100);
  }, {}, false, run_directory);
  CHECK(child.status == 124);
  CHECK(child.completion_status(true) == 124);
  CHECK(child.supervisor_stopped);
  CHECK_FALSE(child.restart);
  CHECK(steady_clock::now() - began >= seconds(10));
  CHECK(steady_clock::now() - began < seconds(15));
#ifdef _WIN32
  REQUIRE(child.dump_error == 0);
  REQUIRE(std::filesystem::is_regular_file(child.timeout_dump));
  CHECK(venture::paths::within(run_directory, child.timeout_dump));
  CHECK(venture::paths::within(root, child.timeout_dump));
  std::ifstream dump(child.timeout_dump, std::ios::binary);
  MINIDUMP_HEADER header{};
  dump.read(reinterpret_cast<char*>(&header), sizeof(header));
  REQUIRE(header.Signature == MINIDUMP_SIGNATURE);
  dump.seekg(header.StreamDirectoryRva);
  bool thread_stacks = false, thread_info = false;
  for (unsigned i = 0; i < header.NumberOfStreams; ++i) {
    MINIDUMP_DIRECTORY stream{};
    dump.read(reinterpret_cast<char*>(&stream), sizeof(stream));
    if (stream.StreamType == ThreadInfoListStream) thread_info = true;
    if (stream.StreamType != ThreadListStream) continue;
    const auto next = dump.tellg();
    dump.seekg(stream.Location.Rva);
    unsigned count = 0;
    dump.read(reinterpret_cast<char*>(&count), sizeof(count));
    for (unsigned j = 0; j < count; ++j) {
      MINIDUMP_THREAD thread{};
      dump.read(reinterpret_cast<char*>(&thread), sizeof(thread));
      thread_stacks |= thread.Stack.Memory.DataSize > 0 && thread.ThreadContext.DataSize > 0;
    }
    dump.seekg(next);
  }
  CHECK(thread_stacks);
  CHECK(thread_info);
  dump.close();
#endif
  if (!run_directory.empty()) std::filesystem::remove_all(run_directory);
}
TEST_CASE("App worker stop request runs normal teardown before the timeout") {
  using namespace std::chrono;
  const auto began = steady_clock::now();
  auto child = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD, {"graceful"}, {}, [&] {
    return steady_clock::now() - began >= milliseconds(200);
  });
  CHECK(child.status == 0);
  CHECK_FALSE(child.restart);
  CHECK(steady_clock::now() - began < seconds(5));
}
TEST_CASE("App worker grace starts after delayed lifecycle delivery") {
  using namespace std::chrono;
  const auto began = steady_clock::now();
  auto child = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD,
      {"slow-diagnostics"}, {}, [&] { return steady_clock::now() - began >= milliseconds(100); }, {}, true);
  CHECK(child.status == 37);
  CHECK_FALSE(child.restart);
  CHECK(steady_clock::now() - began >= seconds(11));
  CHECK(steady_clock::now() - began < seconds(16));
}
TEST_CASE("App worker stop request survives delayed initialization and a one-shot request") {
  using namespace std::chrono;
  const auto began = steady_clock::now();
  bool requested = false;
  auto child = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD, {"graceful-delayed"}, {}, [&] {
    if (requested || steady_clock::now() - began < milliseconds(100)) return false;
    requested = true;
    return true;
  });
  CHECK(requested);
  CHECK(child.status == 0);
  CHECK_FALSE(child.restart);
  CHECK(steady_clock::now() - began < seconds(5));
}
TEST_CASE("Cold launch payload crosses fresh workers privately and without truncation") {
  using namespace venture::platform;
  std::string payload(runtime_launch_payload_limit, '\0');
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(i % 256);
  for (int restart = 0; restart < 2; ++restart) {
    const auto began = std::chrono::steady_clock::now();
    auto child = run_runtime_child(VENTURE_RESTART_CHILD, {"intent"}, {}, [&] {
      return std::chrono::steady_clock::now() - began >= std::chrono::seconds(5);
    }, payload);
    REQUIRE(child.status == 0);
    CHECK(child.restart);
    CHECK(child.launch_payload == payload);
    payload = std::move(child.launch_payload);
  }
  CHECK_THROWS_AS(run_runtime_child(VENTURE_RESTART_CHILD, {"intent"}, {}, [] { return false; },
      std::string(runtime_launch_payload_limit + 1, 'x')), std::length_error);
}
TEST_CASE("Credential broker handles survive an app process restart") {
  auto channels = venture::PipeChannel::pair();
  auto handles = channels[1]->handles();
  std::stop_source stop;
  std::jthread server([&] {
    unsigned requests = 0;
    channels[0]->serve([&](const venture::Json& request) {
      if (++requests == 2) stop.request_stop();
      return request == venture::Json{{"op", "fixture"}} ? venture::Json{{"reply", "retained"}} : venture::Json{};
    }, stop.get_token());
  });
  for (const auto mode : {"broker-restart", "broker"}) {
    const auto began = std::chrono::steady_clock::now();
    const auto result = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD,
        {mode, std::to_string(handles[0]), std::to_string(handles[1])},
        {handles[0], handles[1]}, [&] {
          return std::chrono::steady_clock::now() - began >= std::chrono::seconds(5);
        });
    CHECK(result.status == 0);
    CHECK(result.restart == (std::string_view(mode) == "broker-restart"));
    if (result.status != 0) break;
  }
  // A broken inheritance implementation must fail, not leave serve() waiting
  // indefinitely on the parent's duplicate of the child's write endpoint.
  channels[1]->close();
  server.join();
}

TEST_CASE("Guest exit preserves an unclosed stdio save and host evidence buffers") {
  const auto root = std::filesystem::temp_directory_path() /
      ("venture-exit-proof-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto child = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD,
      {"guest-file-exit", venture::paths::utf8(root)}, {}, [] { return false; }, {}, true);
  REQUIRE(child.status == 0);
  auto read = [](const auto& path) { std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {}); };
  CHECK(read(root / "files/save.bin") == "guest-save-without-fclose");
  const auto events = read(root / "events.jsonl");
  CHECK(events.find("\"sequence\":1023") != std::string::npos);
  unsigned proofs = 0;
  std::istringstream lines(events); std::string line;
  while (std::getline(lines, line)) {
    const auto event = venture::Json::parse(line);
    if (event.value("kind", "") == "exit-proof") {
      CHECK(event.at("sequence") == proofs); ++proofs;
    }
  }
  CHECK(proofs == 1024);
  CHECK(read(root / "runtime.log").find("buffered-final-line") != std::string::npos);
  CHECK(read(root / "gpu.jsonl").find("exit-proof") != std::string::npos);
  const auto wav = read(root / "mix.wav");
  REQUIRE(wav.size() > 44);
  CHECK(wav.substr(0, 4) == "RIFF");
  auto u32 = [&](unsigned p) { unsigned n=0; for (unsigned i=0;i!=4;++i) n |= unsigned(static_cast<unsigned char>(wav[p+i])) << (8*i); return n; };
  CHECK(u32(4) + 8 == wav.size());
  CHECK(u32(40) + 44 == wav.size());
  CHECK(u32(40) > 0);
  // Completed fixture owns this unique directory; no developer-run dumps here.
  std::filesystem::remove_all(root);
}

TEST_CASE("Full and broken diagnostic pipes cannot obstruct reliable worker restart") {
  const auto began = std::chrono::steady_clock::now();
  const auto child = venture::platform::run_runtime_child(VENTURE_RESTART_CHILD,
      {"telemetry-overload"}, {}, [&] { return std::chrono::steady_clock::now() - began >= std::chrono::seconds(5); });
  CHECK(child.status == 0);
  CHECK(child.restart);
  CHECK(child.launch_payload == "telemetry-survived");
}
