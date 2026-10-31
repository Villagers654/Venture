#include "common/first_frame_progress.h"
#include "hle/vulkan/sync_trace.h"
#include <doctest/doctest.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#include "platform/file_activity.h"
#include <fstream>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

#include "core/hle_runtime.h"
#include "core/thread_runtime.h"
#include "core/tls.h"
#include "core/error.h"
#include "core/lifecycle_grace.h"
#include "core/address_space.h"
#include "common/quit_bound.h"
#include "core/shutdown_trace.h"
#include "core/profile.h"
#include "core/mutex_watch.h"
#include "core/os_quit.h"
#include <vector>
#include "core/wait.h"
#include "core/diagnostic_buffer.h"
#include "core/diagnostic_trace.h"
#include "common/log.h"
#include "common/environment.h"
#include "common/process.h"
#include <string>
#include <filesystem>
#include <fstream>
#include "platform/thread_priority.h"
#include "platform/runtime_stop.h"
#include "platform/profile.h"
#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include "platform/memory.h"
#include <array>
#include <atomic>
#include <cstring>
#include <thread>
#include <future>
#include "../async_wait.h"

using namespace venture;
TEST_CASE("OS data activity sees writes outside guest descriptor wrappers") {
  namespace fs = std::filesystem;
  using Watch = venture::platform::FileActivity;
  auto root = fs::temp_directory_path() / ("venture-data-watch-" + std::to_string(venture::monotonic_ns()));
  fs::create_directories(root / "nested");
  struct Cleanup {
    fs::path root;
    ~Cleanup() { std::error_code error; fs::remove_all(root, error); }
  } cleanup{root};
  // No HLE descriptor is involved: this is also how JDK and SQLite writes
  // reach the OS. Start watching before any application save is attempted.
  Watch watch(root);
  REQUIRE(watch.poll() == Watch::Poll::quiet);
  auto changed = [&] {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    do {
      const auto result = watch.poll();
      REQUIRE(result != Watch::Poll::failed);
      if (result == Watch::Poll::changed) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < end);
    FAIL("OS did not report the data write");
  };
  { std::ofstream file(root / "nested/save.tmp"); file << "checkpoint"; }
  changed();
  fs::rename(root / "nested/save.tmp", root / "nested/save.dat");
  changed();
  fs::create_directories(root / "created");
  changed(); // Installs recursive Linux watches for the new directory.
  { std::ofstream file(root / "created/prefs.xml"); file << "preferences"; }
  changed();
  // Save code replaces whole directories; only losing the root is a coverage gap.
  fs::remove_all(root / "created");
  changed();
  fs::create_directories(root / "created");
  changed();
  { std::ofstream file(root / "created/prefs.xml"); file << "recreated"; }
  changed();
  fs::rename(root / "created", root / "moved");
  changed();
  { std::ofstream file(root / "moved/prefs.xml"); file << "after move"; }
  changed();
  Watch unavailable(root / "absent");
  CHECK(unavailable.poll() == Watch::Poll::failed);
}
TEST_CASE("host quit backstop is finite configurable and starts at posting") {
  using namespace std::chrono_literals;
  CHECK(quit_backstop(nullptr) == 5s);
  CHECK(menu_quit_backstop(nullptr, "15000") == 15s);
  CHECK(menu_quit_backstop("0", "15000") == 5s);
  CHECK(menu_quit_backstop("true", "15000") == 15s);
  CHECK(menu_quit_backstop("1", "15000") == 15s);
  for (const auto* bad : {"", "0", "-1", "99", "300001", "100x", "999999999999999999"})
    CHECK(quit_backstop(bad) == 5s);
  CHECK(quit_backstop("100") == 100ms);
  CHECK(quit_backstop("15000") == 15s);
  LifecycleGrace grace(quit_backstop("15000"));
  const auto posted = LifecycleGrace::Clock::time_point{} + 1h;
  CHECK_FALSE(grace.expired(posted));
  grace.posted(posted);
  CHECK_FALSE(grace.expired(posted + 15s));
  CHECK(grace.expired(posted + 15001ms));
}
TEST_CASE("a file save that is the only remaining quit work extends the window to 30 s") {
  using namespace std::chrono_literals;
  LifecycleGrace grace;
  const auto posted = LifecycleGrace::Clock::time_point{} + 1h;
  grace.posted(posted);
  CHECK(grace.expired(posted + 6s));
  CHECK_FALSE(grace.expired(posted + 6s, true));
  CHECK_FALSE(grace.expired(posted + 30s, true));
  CHECK(grace.expired(posted + 30001ms, true));
  CHECK(grace.within_save_bound(posted + 30s));
  CHECK_FALSE(grace.within_save_bound(posted + 31s));
  // A longer configured backstop is never shortened by a save.
  LifecycleGrace longer(quit_backstop("60000"));
  longer.posted(posted);
  CHECK_FALSE(longer.expired(posted + 45s, true));
}
TEST_CASE("core thread creation, join, exit unwind and TLS surplus") {
  GuestThread parent(90); parent.attach_to_current_host_thread();
  std::array<std::byte, 4> initial{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  auto module = tls::add_module(initial, 32, 16);
  auto parent_address = tls::get_addr(parent, module.id, 0);
  const auto allocation = parent.thread_pointer - 1024 * 1024;
  if (auto bytes = platform::committed_bytes(allocation, 17 * 1024 * 1024))
    CHECK(*bytes <= 2 * 1024 * 1024);
  REQUIRE(parent_address != 0);
  CHECK(*g2h<std::uint32_t>(parent_address) == 0x04030201);
  CHECK(*g2h<std::uint32_t>(parent_address + 4) == 0);
  *g2h<std::uint32_t>(parent_address) = 99;
  bool unwound = false;
  auto exiting = hle::make_thunk("test", "thread exit", [](hle::CallContext& ctx) {
    struct Unwind { bool* done; ~Unwind() { *done = true; } } unwind{static_cast<bool*>(ctx.user())};
    CHECK(*g2h<std::uint32_t>(tls::get_addr(ctx.thread(), ctx.x(0), 0)) == 0x04030201);
    CHECK(ctx.thread().thread_pointer != 0);
    CHECK(g2h<std::uint64_t>(ctx.thread().thread_pointer)[1] == ctx.thread().pthread_self);
    CHECK(g2h<std::uint64_t>(ctx.thread().thread_pointer)[2] == 0);
    CHECK(g2h<std::uint64_t>(ctx.thread().thread_pointer)[5] != 0);
    exit_guest_thread(42);
  }, &unwound);
  auto child = create_guest_thread(exiting, module.id);
  REQUIRE(child != nullptr);
  std::uint64_t value = 0;
  CHECK(join_guest_thread(child->pthread_self, &value) == 0);
  CHECK(value == 42); CHECK(unwound);
  CHECK(*g2h<std::uint32_t>(parent_address) == 99);
  auto later = tls::add_module(initial, 64, 64);
  CHECK((tls::get_addr(parent, later.id, 0) & 63) == 0);
  CHECK(*g2h<std::uint32_t>(tls::get_addr(parent, later.id, 0)) == 0x04030201);
  CHECK(*g2h<std::uint32_t>(tls::get_addr(*child, later.id, 0)) == 0x04030201);
  // A late, aligned module crosses committed pages in both existing threads.
  const auto grown = tls::add_module(initial, 128 * 1024 + 13, 64 * 1024, 16);
  for (auto* thread : {&parent, child.get()}) {
    CHECK((tls::get_addr(*thread, grown.id, 0) & 65535) == 16);
    CHECK(*g2h<std::uint32_t>(tls::get_addr(*thread, grown.id, 0)) == 0x04030201);
    CHECK(*g2h<std::byte>(tls::get_addr(*thread, grown.id, 128 * 1024 + 12)) == std::byte{0});
  }
  CHECK(*g2h<std::uint32_t>(parent_address) == 99);
  const std::array code{0xd28000e0u, 0xd65f03c0u};
  auto quick = hle::code_stub(code, "detached");
  GuestThreadOptions options; options.detached = true; options.guard_size = 8192;
  auto detached = create_guest_thread(quick, 0, options);
  REQUIRE(detached != nullptr);
  CHECK(detach_guest_thread(detached->pthread_self) < 0);
}
#ifdef __linux__
#endif

TEST_CASE("a guest call made while the guest runs on its own coroutine stack uses that stack") {
  // RE4 decompresses on a UE4 ctx::continuation stack; zlib calls back into the guest there.
  GuestThread thread(93); thread.attach_to_current_host_thread();
  const auto fiber = platform::reserve(0, 65536, false);
  REQUIRE(fiber != 0);
  REQUIRE(platform::commit(fiber, 65536, kProtRead | kProtWrite));
  const std::array code{0xf94003e0u, 0xd65f03c0u};  // ldr x0, [sp]; ret
  auto ninth = hle::code_stub(code, "ninth argument");
  auto& cpu = thread.cpu();
  const auto original_sp = cpu.sp();
  cpu.set_sp(fiber + 32768);
  CHECK(thread.call(ninth, {1, 2, 3, 4, 5, 6, 7, 8, 99}) == 99);
  CHECK(cpu.sp() == fiber + 32768);
  cpu.set_sp(original_sp);
  platform::release(fiber, 65536);
}
TEST_CASE("OS data writer census excludes non-file objects and catches buffered handles") {
  const auto root = std::filesystem::temp_directory_path() /
      ("venture-writer-census-" + std::to_string(monotonic_ns()));
  std::filesystem::create_directories(root);
  struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code error; std::filesystem::remove_all(p, error); } } cleanup{root};
#ifdef _WIN32
  // Completion-port access bits overlap FILE_WRITE_DATA, but the handle is
  // not a file and cannot be duplicated through the file-census path.
  const auto port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
  REQUIRE(port);
  struct ClosePort { HANDLE h; ~ClosePort() { CloseHandle(h); } } close_port{port};
  const auto subdirectory = root / "writable-directory";
  std::filesystem::create_directory(subdirectory);
  const auto writable_directory = CreateFileW(subdirectory.c_str(),
      FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  REQUIRE(writable_directory != INVALID_HANDLE_VALUE);
  ClosePort close_writable_directory{writable_directory};
#endif
  CHECK(platform::data_writers(root) == platform::DataWriters::closed);
  std::unique_ptr<FILE, decltype(&std::fclose)> writer(std::fopen((root/"save.dat").string().c_str(), "wb"), &std::fclose);
  REQUIRE(writer);
  REQUIRE(std::fwrite("buffered-save", 1, 13, writer.get()) == 13);
  CHECK(std::filesystem::file_size(root/"save.dat") == 0);
  CHECK(platform::data_writers(root) == platform::DataWriters::open);
  writer.reset();
  std::ifstream reader(root/"save.dat");
  CHECK(platform::data_writers(root) == platform::DataWriters::closed);
  std::string bytes((std::istreambuf_iterator<char>(reader)), {});
  CHECK(bytes == "buffered-save");
  reader.close();
  CHECK(platform::data_writers(root/"absent") == platform::DataWriters::unknown);
}

#ifndef _WIN32
#endif
TEST_CASE("frame stall trigger fires once per stall when focused frames stop advancing") {
  startup_diagnostics::FrameStallTrigger trigger;
  CHECK_FALSE(trigger.poll(0, 0, false));  // nothing submitted yet
  CHECK_FALSE(trigger.poll(1, 10, false));
  CHECK_FALSE(trigger.poll(4, 20, false));
  CHECK_FALSE(trigger.poll(8, 20, false));  // 4 s without a frame
  CHECK_FALSE(trigger.poll(9, 20, true));   // unfocused restarts the wait
  CHECK_FALSE(trigger.poll(13, 20, false));
  CHECK(trigger.poll(14, 20, false));
  CHECK_FALSE(trigger.poll(30, 20, false));  // once per stall
  // Frames resuming re-arm it: a freeze after a loading stall is captured too.
  CHECK_FALSE(trigger.poll(31, 25, false));
  CHECK(trigger.poll(36, 25, false));
  // At most max_captures per run.
  for (std::uint64_t f = 30; f < 60; f += 5) {
    CHECK_FALSE(trigger.poll(std::int64_t(f * 10), f, false));
    trigger.poll(std::int64_t(f * 10 + 6), f, false);
  }
  CHECK_FALSE(trigger.poll(1000, 99, false));
  CHECK_FALSE(trigger.poll(1006, 99, false));
}
