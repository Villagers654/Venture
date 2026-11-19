// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/stall_record.h"
#include "common/process.h"
#include "launcher/debug_report.h"
#include <doctest/doctest.h>
#include <chrono>
#include <format>
#include <fstream>

using namespace venture;
using Action = stall::Detector::Action;

TEST_CASE("stall detector: one snapshot at 250 ms, a recheck at 1 s, a record when frames resume") {
  stall::Detector d;
  CHECK(d.poll(0, 0, false) == Action::none);  // nothing drawn yet: startup is not a stall
  CHECK(d.poll(5000, 0, false) == Action::none);
  CHECK(d.poll(10'000, 1, false) == Action::none);
  CHECK(d.poll(10'200, 1, false) == Action::none);  // under the threshold
  CHECK(d.poll(10'250, 1, false) == Action::snapshot);
  CHECK(d.poll(10'300, 1, false) == Action::none);  // one snapshot per stall
  CHECK(d.poll(11'000, 1, false) == Action::recheck);
  CHECK(d.poll(12'000, 1, false) == Action::none);
  CHECK(d.poll(12'100, 2, false) == Action::record);
  CHECK(d.duration_ms() == 2100);
  // Frames flowing: nothing, and no per-poll work beyond the comparison.
  for (std::int64_t t = 12'150, f = 3; t < 30'000; t += 50, ++f) CHECK(d.poll(t, std::uint64_t(f), false) == Action::none);

  SUBCASE("a short stall ends before the recheck and is still recorded") {
    stall::Detector s;
    s.poll(0, 1, false);
    CHECK(s.poll(300, 1, false) == Action::snapshot);
    CHECK(s.poll(450, 2, false) == Action::record);
    CHECK(s.duration_ms() == 450);
  }
}

TEST_CASE("stall detector: paused or quitting games are excluded and restart the wait") {
  stall::Detector d;
  d.poll(0, 5, false);
  CHECK(d.poll(200, 5, true) == Action::none);    // unfocused before the threshold
  CHECK(d.poll(5000, 5, true) == Action::none);   // however long it stays unfocused
  CHECK(d.poll(5200, 5, false) == Action::none);  // the wait restarted when focus came back
  CHECK(d.poll(5450, 5, false) == Action::snapshot);
  // A stall that ends in a pause or quit is dropped, not recorded.
  CHECK(d.poll(5600, 5, true) == Action::discard);
  CHECK(d.poll(5650, 6, false) == Action::none);
}

TEST_CASE("stall detector: at most one capture per 10 s and 30 per run") {
  stall::Detector d;
  std::int64_t t = 0;
  std::uint64_t frames = 1;
  d.poll(t, frames, false);
  int captures = 0;
  // A stall every 2 s for 20 minutes: only one in each 10 s is captured.
  for (int i = 0; i < 600; ++i) {
    t += 2000;
    if (d.poll(t - 1000, frames, false) == Action::snapshot) ++captures;
    if (d.poll(t, ++frames, false) == Action::record) CHECK(d.duration_ms() == 2000);
  }
  CHECK(captures == stall::Detector::max_captures);
  stall::Detector spaced;
  spaced.poll(0, 1, false);
  CHECK(spaced.poll(300, 1, false) == Action::snapshot);
  CHECK(spaced.poll(400, 2, false) == Action::record);
  CHECK(spaced.poll(5000, 2, false) == Action::none);  // a second stall 5 s later
  CHECK(spaced.poll(5100, 3, false) == Action::none);
  CHECK(spaced.poll(10'500, 3, false) == Action::snapshot);
}

TEST_CASE("stall causes: host calls, roles and the stall's verdict") {
  CHECK(stall::host_call_cause("libvulkan.so", "vkCreateGraphicsPipelines") == stall::Cause::pipeline);
  CHECK(stall::host_call_cause("libGLESv3.so", "glLinkProgram") == stall::Cause::pipeline);
  CHECK(stall::host_call_cause("libvulkan.so", "vkWaitForFences") == stall::Cause::gpu);
  CHECK(stall::host_call_cause("libopenxr_loader.so", "xrWaitFrame") == stall::Cause::xr);
  CHECK(stall::host_call_cause("libc.so", "pthread_cond_wait") == stall::Cause::wait);
  CHECK(stall::host_call_cause("libc.so", "usleep") == stall::Cause::wait);
  CHECK(stall::host_call_cause("libc.so", "read") == stall::Cause::io);
  CHECK(stall::host_call_cause("libandroid.so", "AAsset_read") == stall::Cause::io);
  CHECK(stall::host_call_cause("JNIEnv", "CallStaticVoidMethodV") == stall::Cause::java);
  CHECK(stall::host_call_cause("libc.so", "malloc") == stall::Cause::host);
  CHECK(stall::thread_role("UnityMain") == "main");
  CHECK(stall::thread_role("UnityGfxDeviceW") == "render");
  CHECK(stall::thread_role("RHIThread") == "rhi");
  CHECK(stall::thread_role("Job.Worker 3").empty());

  stall::Record r;
  r.ms = 1000;
  r.shown = {{.tid = 1, .name = "UnityMain", .role = "main", .cause = stall::Cause::wait},
             {.tid = 2, .name = "UnityGfxDeviceW", .role = "render", .cause = stall::Cause::gpu}};
  CHECK(stall::classify(r) == stall::Cause::gpu);  // main waits on render, render on the GPU
  r.shown[0].cause = stall::Cause::jit;
  CHECK(stall::classify(r) == stall::Cause::jit);
  r.shown[0].cause = stall::Cause::wait;
  r.gc_ms = 600;
  CHECK(stall::classify(r) == stall::Cause::gc);
  r.gc_ms = 0;
  r.hard_faults = 900;
  CHECK(stall::classify(r) == stall::Cause::paging);
  r.hard_faults = 10;
  r.shown = {{.tid = 9, .name = "Worker", .cause = stall::Cause::jit}};
  CHECK(stall::classify(r) == stall::Cause::jit);  // no named render thread: a compiling one
  r.shown.clear();
  CHECK(stall::classify(r) == stall::Cause::unknown);
}

namespace {
// A run with three stalls: two in JIT compiles on the main thread, one on the GPU.
std::string sample_log() {
  std::string log;
  const auto add = [&](const stall::Record& r) {
    for (const auto& line : stall::format(r)) log += std::format("{:9.3f} 7 I [stall] {}\n", r.at_s + r.ms / 1000.0, line);
  };
  stall::Record jit{.id = 1, .at_s = 61.5, .ms = 2170, .snapshots = 2, .cause = stall::Cause::jit, .cpu_percent = 103,
                    .faults = 4200, .hard_faults = 3, .read_kib = 12, .gc_ms = 0, .private_mib = 5120, .threads = 87};
  jit.shown = {{.tid = 120, .name = "UnityMain", .role = "main", .where = "JIT compile of libil2cpp.so+0x1a2b3c",
                .cause = stall::Cause::jit, .moved = 1, .stack = "libil2cpp.so+0x1a0000 < libunity.so+0x55aa00"},
               {.tid = 121, .name = "UnityGfxDeviceW", .role = "render", .where = "libc.so!pthread_cond_wait",
                .cause = stall::Cause::wait, .moved = 0}};
  add(jit);
  log += "   70.000 3 I [game] something \"quoted\" in between\n";
  auto second = jit;
  second.id = 2;
  second.at_s = 300.25;
  second.ms = 640;
  second.snapshots = 1;
  for (auto& t : second.shown) t.moved = -1;
  add(second);
  stall::Record gpu{.id = 3, .at_s = 900, .ms = 410, .snapshots = 1, .cause = stall::Cause::gpu, .threads = 80};
  gpu.shown = {{.tid = 120, .name = "UnityMain", .role = "main", .where = "libvulkan.so!vkQueueSubmit",
                .cause = stall::Cause::gpu, .stack = "libunity.so+0x123"}};
  add(gpu);
  return log;
}
}  // namespace

TEST_CASE("stall records: logged lines parse back and the report aggregates them") {
  const auto log = sample_log();
  CHECK(log.find("[stall] #1 at=61.5s ms=2170 cause=jit snapshots=2 threads=87 cpu=103% faults=4200 hard_faults=3 "
                 "read_kib=12 gc_ms=0 private_mib=5120\n") != std::string::npos);
  CHECK(log.find("[stall] #1 tid=120 name=\"UnityMain\" role=main cause=jit moved=yes where=\"JIT compile of "
                 "libil2cpp.so+0x1a2b3c\" stack=\"libil2cpp.so+0x1a0000 < libunity.so+0x55aa00\"\n") != std::string::npos);
  const auto records = stall::parse_log(log);
  REQUIRE(records.size() == 3);
  CHECK(records[0].id == 1);
  CHECK(records[0].ms == 2170);
  CHECK(records[0].at_s == doctest::Approx(61.5));
  CHECK(records[0].cpu_percent == doctest::Approx(103));
  CHECK(records[0].hard_faults == 3);
  CHECK(records[0].private_mib == 5120);
  REQUIRE(records[0].shown.size() == 2);
  CHECK(records[0].shown[0].where == "JIT compile of libil2cpp.so+0x1a2b3c");
  CHECK(records[0].shown[0].moved == 1);
  CHECK(records[0].shown[1].moved == 0);
  CHECK(records[1].shown[0].moved == -1);
  CHECK(records[2].gc_ms == -1);  // unknown stays unknown
  CHECK(records[2].cause == stall::Cause::gpu);

  const auto section = stall::report_section(records);
  CHECK(section.starts_with("\n=== Stalls (3) ===\n"));
  CHECK(section.find("Frames stopped for 250 ms or more 3 times while the game had focus: 3.22 s in all, worst 2.17 s.")
        != std::string::npos);
  CHECK(section.find("Causes: jit (Venture) 2, gpu 1\n") != std::string::npos);
  CHECK(section.find("Main/render thread most often at:\n  2 of 3 stalls: UnityMain (main): JIT compile of "
                     "libil2cpp.so+0x1a2b3c\n  1 of 3 stalls: UnityMain (main): libvulkan.so!vkQueueSubmit\n")
        != std::string::npos);
  // Worst first.
  CHECK(section.find("#1 at 61.5 s: 2170 ms, cause jit; CPU 103%, 4200 page faults (3 from disk), read 12 KiB, "
                     "Java GC 0 ms, private 5120 MiB, 87 threads\n  UnityMain (main) tid 120: JIT compile of "
                     "libil2cpp.so+0x1a2b3c [jit, moving]\n    < libil2cpp.so+0x1a0000 < libunity.so+0x55aa00\n")
        != std::string::npos);
  CHECK(section.find("#1 at") < section.find("#2 at"));
  CHECK(section.find("#2 at") < section.find("#3 at"));
  CHECK(stall::report_section({}).empty());

  const auto verdict = stall::venture_cause(records);
  REQUIRE(verdict);
  CHECK(verdict->cause == "The game stopped drawing 3 times for up to 2.17 s while it had focus; 2 of them were in "
                          "Venture's JIT translating the game's code (see Stalls).");
  CHECK_FALSE(stall::venture_cause({records[2]}));
  CHECK_FALSE(stall::venture_cause({records[0], records[2]}));  // half is not most
}

TEST_CASE("debug report: a run's stalls get their own section and name a Venture-side cause") {
  using namespace venture::launcher;
  const auto data = fs::temp_directory_path() / ("venture-report-stalls-" + std::to_string(current_process_id()));
  fs::remove_all(data);
  fs::create_directories(data / "logs");
  const auto start = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) - std::chrono::minutes(2);
  std::ofstream(data / "logs" / "runtime-com.example.game.log", std::ios::binary)
      << std::format("  1.000 1 I [launch] Process started at {:%FT%TZ}\n", start) << sample_log()
      << "  999.000 1 I [launch] Exit code: 0 (stop requested)\n";
  const auto file = write_debug_report({.data = data, .version = "9.9.9", .package = "com.example.game"});
  std::ifstream in(file, std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(in), {}};
  CHECK(text.find("=== Likely cause ===\nThe game stopped drawing 3 times for up to 2.17 s") != std::string::npos);
  CHECK(text.find("=== Next steps ===\n- Send this report with an issue: the stalls point at Venture.") != std::string::npos);
  CHECK(text.find("\n=== Stalls (3) ===\n") != std::string::npos);
  in.close();
  fs::remove_all(data);
}
