// SPDX-License-Identifier: GPL-3.0-or-later
// The stall watch's bookkeeping and the stall finder's capture of a synthetic
// freeze: a render thread stuck in xrEndFrame while the main thread polls a
// fence that never signals (GitHub #154's shape).
#include "core/stall_watch.h"
#include "core/stall_finder.h"
#include "core/thread.h"
#include "core/thread_api.h"
#include "common/log.h"
#include "common/stall_record.h"
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

using namespace venture;
namespace sw = venture::stall_watch;

namespace {
struct StallLogCapture {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      std::format("venture-stall-log-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  StallLogCapture() { log::set_file(path.string()); }
  ~StallLogCapture() { log::set_file(""); std::error_code ignored; std::filesystem::remove(path, ignored); }
  std::string text() {
    log::set_file("");
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
};
const sw::ThreadView* find_thread(const std::vector<sw::ThreadView>& threads, std::uint32_t os_tid) {
  for (const auto& t : threads)
    if (t.os_tid == os_tid) return &t;
  return nullptr;
}
std::uint32_t this_os_tid() {
  // The watch names threads by OS id; a call in progress shows which one is ours.
  sw::Op probe(sw::Kind::idle, "probe");
  const auto longest = sw::longest();
  return longest.os_tid;
}
}  // namespace

TEST_CASE("stall watch: calls in progress nest, end, and the longest is found") {
  sw::reset_for_testing();
  const auto tid = this_os_tid();
  {
    sw::Op outer(sw::Kind::xr, "guest.xrEndFrame");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    sw::Op inner(sw::Kind::submit, "vkQueueSubmit", 0x51);
    const auto threads = sw::threads();
    const auto* mine = find_thread(threads, tid);
    REQUIRE(mine);
    REQUIRE(mine->ops.size() == 2);
    CHECK(std::strcmp(mine->ops[0].name, "guest.xrEndFrame") == 0);
    CHECK(mine->ops[1].kind == sw::Kind::submit);
    CHECK(mine->ops[1].object == 0x51);
    const auto longest = sw::longest();
    CHECK(longest.os_tid == tid);
    CHECK(std::strcmp(longest.name, "guest.xrEndFrame") == 0);  // the outermost call
    CHECK(longest.since_ns == outer.started());
    outer.finish(-8);  // an XR call ends with its result, before the inner scope unwinds
  }
  CHECK(find_thread(sw::threads(), tid) == nullptr);
  CHECK(sw::longest().since_ns == 0);
  const auto xr = sw::xr_calls();
  REQUIRE(!xr.empty());
  CHECK(std::strcmp(xr.back().name, "guest.xrEndFrame") == 0);
  CHECK(xr.back().result == -8);
  CHECK(xr.back().ns >= 2'000'000);
}

TEST_CASE("stall watch: fences keep their submission, polls count until signalled") {
  sw::reset_for_testing();
  const auto tid = this_os_tid();
  const auto direct = sw::guest_submit(0x51, 0xF00D, false);
  CHECK(direct > 0);
  auto f = sw::fence(0xF00D);
  CHECK(f.known);
  CHECK(f.submit == direct);
  CHECK(f.queue == 0x51);
  CHECK_FALSE(f.deferred);
  CHECK(f.issued_ns == f.submitted_ns);
  CHECK_FALSE(sw::fence(0xBEEF).known);

  // Held by the queue dispatcher, then given to the driver: the same submission id.
  const auto held = sw::guest_submit(0x52, 0xBEEF, true);
  CHECK(held == direct + 1);
  CHECK(sw::fence(0xBEEF).issued_ns == 0);
  CHECK(sw::native_submit(0x99, 0xBEEF) == held);
  CHECK(sw::fence(0xBEEF).issued_ns != 0);
  CHECK(sw::native_submit(0x99, 0xBEEF) == 0);  // only once
  CHECK(sw::native_submit(0x99, 0xF00D) == 0);  // a direct submission is not the dispatcher's

  for (int i = 0; i < 100; ++i) sw::fence_checked(0xF00D, false);
  auto threads = sw::threads();
  auto* mine = find_thread(threads, tid);
  REQUIRE(mine);
  REQUIRE(mine->polls.size() == 1);
  CHECK(mine->polls[0].fence == 0xF00D);
  CHECK(mine->polls[0].checks == 100);
  // More fences than entries: the least recently checked one goes.
  for (std::uint64_t fence = 1; fence <= 4; ++fence) sw::fence_checked(fence, false);
  threads = sw::threads();
  mine = find_thread(threads, tid);
  REQUIRE(mine);
  CHECK(mine->polls.size() == 4);
  for (const auto& p : mine->polls) CHECK(p.fence != 0xF00D);
  for (std::uint64_t fence = 1; fence <= 4; ++fence) sw::fence_checked(fence, true);
  CHECK(find_thread(sw::threads(), tid) == nullptr);
}

TEST_CASE("stall watch: event rings keep the newest entries in order") {
  sw::reset_for_testing();
  for (std::uint64_t i = 1; i <= 1000; ++i)
    sw::event(sw::Event::submit, "vkQueueSubmit", 0x51, 0, i, std::int64_t(i), 10, 0);
  const auto events = sw::vulkan_events();
  REQUIRE(events.size() == 256);
  CHECK(events.front().submit == 745);
  CHECK(events.back().submit == 1000);
  for (std::size_t i = 1; i < events.size(); ++i) CHECK(events[i].submit == events[i - 1].submit + 1);
  const auto before = sw::totals();
  sw::pipelines(3, 5'000'000);
  sw::texture_upload();
  sw::gc_began();
  CHECK(sw::totals().gc_since_ns != 0);
  sw::gc_ended();
  const auto after = sw::totals();
  CHECK(after.pipelines == before.pipelines + 3);
  CHECK(after.pipeline_ns == before.pipeline_ns + 5'000'000);
  CHECK(after.uploads == before.uploads + 1);
  CHECK(after.gcs == before.gcs + 1);
  CHECK(after.gc_since_ns == 0);
}

TEST_CASE("stall watch: a call is cheap enough to bracket every submit and wait") {
  sw::reset_for_testing();
  constexpr int kCalls = 200'000;
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < kCalls; ++i) {
    sw::Op op(sw::Kind::wait, "vkWaitForFences", 0xF00D);
    sw::fence_checked(0xF00D, false);
  }
  const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kCalls;
  MESSAGE(std::format("stall watch call + fence check: {:.0f} ns", ns));
  CHECK(ns < 2000);  // generous: well under a microsecond on any test machine
}

TEST_CASE("stall finder: long calls while frames keep coming get one rate-limited line each") {
  sw::reset_for_testing();
  StallLogCapture capture;
  stall_finder::Finder finder(stall_finder::Sources{.host_stacks = false});
  std::uint64_t frames = 1;
  const auto start = std::chrono::steady_clock::now();
  const auto now_ms = [&] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() +
           100'000;
  };
  std::atomic<int> phase{0};
  std::thread worker([&] {
    for (int call = 0; call < 2; ++call) {
      while (phase.load() != call * 2 + 1) std::this_thread::yield();
      sw::Op op(sw::Kind::idle, "vkDeviceWaitIdle", 0x77);
      while (phase.load() == call * 2 + 1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  const auto run_for = [&](std::chrono::milliseconds length) {
    const auto until = std::chrono::steady_clock::now() + length;
    while (std::chrono::steady_clock::now() < until) {
      finder.poll(now_ms(), ++frames, false);  // frames keep coming: no stall
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  };
  phase = 1;
  run_for(std::chrono::milliseconds(700));
  phase = 2;
  run_for(std::chrono::milliseconds(200));
  // A second long call right after: within the 5 s spacing, so no line.
  phase = 3;
  run_for(std::chrono::milliseconds(700));
  phase = 4;
  run_for(std::chrono::milliseconds(200));
  worker.join();
  const auto calls = stall::parse_calls(capture.text());
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].call == "vkDeviceWaitIdle (0x77)");
  CHECK(calls[0].ms >= 600);
  CHECK(calls[0].ms < 2000);
}

TEST_CASE("stall finder: a synthetic freeze names the render thread's xrEndFrame and the fence the main thread polls") {
  sw::reset_for_testing();
  StallLogCapture capture;
  std::atomic<bool> stop{false};
  std::atomic<int> ready{0};
  std::uint64_t submission = 0;
  std::thread render([&] {
    auto& thread = GuestThread::adopt_current();
    threads::set_name(thread.pthread_self, "UnityGfxDeviceW");
    sw::Op end_frame(sw::Kind::xr, "guest.xrEndFrame");
    ++ready;
    while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  });
  std::thread main([&] {
    auto& thread = GuestThread::adopt_current();
    threads::set_name(thread.pthread_self, "UnityMain");
    submission = sw::guest_submit(0x51, 0xF00D, false);
    ++ready;
    while (!stop.load()) {
      sw::fence_checked(0xF00D, false);  // vkGetFenceStatus: VK_NOT_READY
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  while (ready.load() != 2) std::this_thread::yield();

  stall_finder::Finder finder(stall_finder::Sources{.host_stacks = true});
  const auto start = std::chrono::steady_clock::now();
  const auto now_ms = [&] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() +
           100'000;
  };
  finder.poll(now_ms(), 1, false);  // the last frame
  while (now_ms() < 101'300) {
    finder.poll(now_ms(), 1, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  finder.poll(now_ms(), 2, false);  // frames resume: the record
  stop = true;
  render.join();
  main.join();

  const auto text = capture.text();
  const auto records = stall::parse_log(text);
  REQUIRE(records.size() == 1);
  const auto& r = records[0];
  CHECK(r.ms >= 1200);
  CHECK(r.clock.size() == 8);  // HH:MM:SS
  CHECK(r.summary.find("UnityGfxDeviceW was in guest.xrEndFrame from") != std::string::npos);
  CHECK(r.summary.find("UnityMain polled fence 0xf00d") != std::string::npos);
  CHECK(r.summary.find(std::format("guest submission #{} on queue 0x51", submission)) != std::string::npos);
  bool op = false, fence = false, xr = false, host = false;
  for (const auto& e : r.evidence) {
    if (e.kind == "op" && e.text.starts_with("UnityGfxDeviceW (render)") &&
        e.text.find("guest.xrEndFrame") != std::string::npos && e.text.find("still in it") != std::string::npos)
      op = true;
    if (e.kind == "fence" && e.text.starts_with("UnityMain checked fence 0xf00d") &&
        e.text.find("still when the stall was recorded") != std::string::npos)
      fence = true;
    if (e.kind == "xr" && e.text.starts_with("session ")) xr = true;
    if (e.kind == "host" && e.text.starts_with("UnityGfxDeviceW (tid ")) host = true;
  }
  CHECK(op);
  CHECK(fence);
  CHECK(xr);
  CHECK(host);
  CHECK(text.find("[stall-host] #1 threads=") != std::string::npos);
  const auto section = stall::report_section(records);
  CHECK(section.find("1 of them was a freeze of 1 s or more.") != std::string::npos);
  CHECK(section.find("  Summary: UnityGfxDeviceW was in guest.xrEndFrame") != std::string::npos);
  MESSAGE(section);
}
