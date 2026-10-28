#include <doctest/doctest.h>
#include "cpu/block_list.h"
#include "cpu/cpu.h"
#include "core/elf.h"
#include "core/linker_runtime.h"
#include "core/process.h"
#include "core/thread_runtime.h"
#include "hle/registry.h"
#include "common/json.h"
#include "common/paths.h"
#include "common/file_ops.h"
#include <array>
#include <cstring>
#include <fstream>
#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <thread>

namespace venture {
struct JitBlockListTestAccess {
  static void observe(void (*callback)()) { JitBlockList::snapshot_observer_for_test(callback); }
  static bool files_unlocked() {
    auto& mutex = JitBlockList::persistence_mutex_for_test();
    if (!mutex.try_lock()) return false;
    mutex.unlock(); return true;
  }
};
}
namespace {
struct SnapshotGate {
  std::promise<void> entered;
  std::shared_future<void> release;
};
SnapshotGate* snapshot_gate = nullptr;
thread_local bool pause_snapshot = false;
std::atomic<bool> pause_next_snapshot{false}; // Any thread, e.g. the background writer.
void snapshot_observer() {
  if (!pause_snapshot && !pause_next_snapshot.exchange(false)) return;
  pause_snapshot = false;
  snapshot_gate->entered.set_value();
  snapshot_gate->release.wait();
}
}

using namespace venture;
TEST_CASE("JIT block list persists validated module-relative hints and rejects corrupt input") {
  if (!JitBlockList::enabled()) return; // Dedicated CTest enables the feature.
  const auto directory = std::filesystem::current_path() / "build/jit-block-list-fixture";
  std::filesystem::create_directories(directory);
  const auto source = directory / "libblock-list-fixture.so";
  std::array<std::byte, 4096> file{};
  auto put = [&]<class T>(std::size_t offset, const T& value) { std::memcpy(file.data() + offset, &value, sizeof(value)); };
  elf::Header header{};
  std::memcpy(header.ident, "\177ELF\2\1\1", 7);
  header.type = 3; header.machine = 183; header.version = 1; header.ehsize = sizeof(header);
  header.phoff = 64; header.phentsize = sizeof(elf::Phdr); header.phnum = 2;
  put(0, header);
  put(64, elf::Phdr{1, 7, 0, 0, 0, 4096, 4096, 4096});
  const std::array<elf::Dyn, 7> dynamic{{{5, 0x400}, {10, 7}, {6, 0x500}, {11, sizeof(elf::Sym)}, {4, 0x600}, {0, 0}}};
  put(120, elf::Phdr{2, 6, 0x200, 0x200, 0, sizeof(dynamic), sizeof(dynamic), 8});
  put(0x200, dynamic);
  std::memcpy(file.data() + 0x400, "\0entry", 7);
  put(0x500, std::array<elf::Sym, 2>{{{}, {1, 0x12, 0, 1, 0x180, 8}}});
  put(0x600, std::array<std::uint32_t, 5>{1, 2, 1, 0, 0});
  put(0x180, std::array<std::uint32_t, 2>{0xd2800540, 0xd65f03c0}); // mov x0,42; ret
  { std::ofstream stream(source, std::ios::binary); stream.write(reinterpret_cast<const char*>(file.data()), file.size()); }
  auto saved_cache = process_config().cache_dir;
  struct Restore { std::filesystem::path saved; ~Restore() { process_config().cache_dir = std::move(saved); } } restore{saved_cache};
  process_config().cache_dir = directory;
  const auto cache_directory = directory / "jit-block-lists";
  if (std::filesystem::exists(cache_directory)) for (const auto& e : std::filesystem::directory_iterator(cache_directory))
    if (!e.is_symlink() && e.is_regular_file() && e.path().extension() == ".vbl") std::filesystem::remove(e.path());
  register_all_hle_libraries();
  GuestThread thread(97); thread.attach_to_current_host_thread();
  linker_set_inspection(false);
  auto* lib = Linker::get().open(source.string(), kRtldNow, nullptr);
  INFO(Linker::get().take_error()); REQUIRE(lib != nullptr);
  const auto entry = lib->load_bias + 0x180;
  constexpr std::uint64_t mode = 0x0800000000000000ull;
  std::vector<std::uint64_t> warmed;
  const auto compile = [&](std::uint64_t d) { warmed.push_back(d); return true; };
  JitBlockList cold;
  cold.prepare(entry, false, compile);
  CHECK(warmed.empty());
  cold.record(entry | mode); cold.record(entry | mode);
  cold.record(0x00ff000000000000ull); // Anonymous address must not be persisted.
  cold.save();
  // Directory order is unspecified; the cache shares it with persist.lock.
  std::filesystem::path cache;
  for (const auto& e : std::filesystem::directory_iterator(cache_directory))
    if (e.path().extension() == ".vbl") cache = e.path();
  REQUIRE_FALSE(cache.empty());
  std::ifstream stream(cache); Json document; stream >> document;
  stream.close();
  REQUIRE(document["blocks"].size() == 1);
  CHECK(document["blocks"][0][1] == 0x180);
  CHECK(document["blocks"][0][2] == mode);
  JitBlockList warm; warm.prepare(entry, false, compile);
  REQUIRE(warmed == std::vector<std::uint64_t>{entry | mode});
  thread.cpu().set_fpcr(0x400000); // Match the persisted descriptor mode.
  CHECK(thread.call(entry, {}) == 42); // Real runtime warms and executes the ELF.
  {
    using namespace std::chrono_literals;
    // Pause the snapshot before it can enter the linker. Another thread must
    // still be able to acquire the file mutex (including from an IFUNC owner).
    std::promise<void> release;
    SnapshotGate gate{{}, release.get_future().share()}; snapshot_gate = &gate;
    auto entered = gate.entered.get_future();
    JitBlockListTestAccess::observe(snapshot_observer);
    auto preparation = std::async(std::launch::async, [&] {
      pause_snapshot = true;
      JitBlockList list; list.prepare(entry, false, [](auto) { return true; });
    });
    const bool paused = entered.wait_for(5s) == std::future_status::ready;
    CHECK(paused);
    if (paused) CHECK(JitBlockListTestAccess::files_unlocked());
    release.set_value(); preparation.get();
    JitBlockListTestAccess::observe(nullptr); snapshot_gate = nullptr;
  }
  {
    using namespace std::chrono_literals;
    // A retiring thread only queues its hints: persistence, including a slow
    // linker snapshot, runs on the background writer. Detaching must return
    // while that snapshot is paused, and other threads must still retire.
    // Drain every gate even on regression so this test fails rather than hangs.
    std::promise<void> release;
    SnapshotGate gate{{}, release.get_future().share()}; snapshot_gate = &gate;
    auto entered = gate.entered.get_future();
    JitBlockListTestAccess::observe(snapshot_observer);
    auto cleanup = std::async(std::launch::async, [&] {
      GuestThread worker(30997); worker.attach_to_current_host_thread();
      worker.cpu().set_fpcr(0); // A different descriptor than the warmed hint.
      CHECK(worker.call(entry, {}) == 42);
      pause_next_snapshot = true; // The next snapshot after the call is the writer's.
      worker.detach_from_current_host_thread();
    });
    const bool detached = cleanup.wait_for(5s) == std::future_status::ready;
    CHECK(detached);
    const bool paused = entered.wait_for(5s) == std::future_status::ready;
    CHECK(paused); // The writer, not the retiring thread, entered the snapshot.
    if (paused) {
      CHECK(JitBlockListTestAccess::files_unlocked());
      auto independent = std::async(std::launch::async, [&] {
        GuestThread other(30998); other.attach_to_current_host_thread();
        other.detach_from_current_host_thread();
      });
      const bool progressed = independent.wait_for(2s) == std::future_status::ready;
      CHECK(progressed);
      release.set_value(); independent.get();
    } else { pause_next_snapshot = false; release.set_value(); }
    cleanup.get(); JitBlockList::flush();
    JitBlockListTestAccess::observe(nullptr); snapshot_gate = nullptr;
  }
  {
    using namespace std::chrono_literals;
    // A thread's first call snapshots the linker outside its invalidation
    // gate: another thread's code invalidation does not wait for that snapshot.
    std::promise<void> release;
    SnapshotGate gate{{}, release.get_future().share()}; snapshot_gate = &gate;
    auto entered = gate.entered.get_future();
    JitBlockListTestAccess::observe(snapshot_observer);
    auto first_call = std::async(std::launch::async, [&] {
      GuestThread worker(30996); worker.attach_to_current_host_thread();
      pause_snapshot = true;
      CHECK(worker.call(entry, {}) == 42);
      worker.detach_from_current_host_thread();
    });
    const bool paused = entered.wait_for(5s) == std::future_status::ready;
    CHECK(paused);
    if (paused) {
      auto invalidation = std::async(std::launch::async, [&] { cpu_backend().invalidate(entry, 8); });
      CHECK(invalidation.wait_for(2s) == std::future_status::ready);
      release.set_value(); invalidation.get();
    } else { pause_snapshot = false; release.set_value(); }
    first_call.get(); JitBlockList::flush();
    JitBlockListTestAccess::observe(nullptr); snapshot_gate = nullptr;
  }
  {
    using namespace std::chrono_literals;
    // A constructor holds the loader while it waits for a thread it started:
    // that thread's first call skips the hints instead of waiting for the loader.
    std::promise<void> called;
    auto called_future = called.get_future();
    std::thread started;
    {
      auto loader = guest_loader_lock();
      started = std::thread([&] {
        GuestThread worker(30995); worker.attach_to_current_host_thread();
        CHECK(worker.call(entry, {}) == 42);
        worker.detach_from_current_host_thread();
        called.set_value();
      });
      CHECK(called_future.wait_for(5s) == std::future_status::ready);
    }
    started.join(); JitBlockList::flush();
  }
  {
    // Fill the bounded file with descriptors without executing synthetic code.
    // Refresh the oldest hint, then demand a new one: the next oldest must go.
    auto full = document; full["blocks"] = Json::array();
    for (std::uint64_t i = 0; i < 32768; ++i)
      full["blocks"].push_back(Json::array({document["blocks"][0][0],
          (i % 256) * 4, (i / 256) << 56}));
    paths::atomic_write(cache, full.dump());
    JitBlockList replacement;
    replacement.prepare(entry, false, [](auto) { return false; });
    replacement.record(lib->load_bias);
    replacement.record(entry | 0xfe00000000000000ull);
    replacement.save();
    std::ifstream saved(cache); Json result; saved >> result;
    REQUIRE(result["blocks"].size() == 32768);
    CHECK(result["blocks"][0] == full["blocks"][2]);
    CHECK(result["blocks"][32766] == full["blocks"][0]);
    CHECK(result["blocks"][32767] == Json::array({document["blocks"][0][0],
        0x180ull, 0xfe00000000000000ull}));
  }
  {
    using namespace std::chrono_literals;
    // Another process's writer holds the directory lock: save() waits for it
    // (merge, budget and replace are one critical section), then writes through
    // a unique temporary file and leaves none behind.
    std::optional<FileLock> other(std::in_place, cache_directory / "persist.lock");
    JitBlockList blocked;
    blocked.prepare(entry, false, [](auto) { return false; });
    blocked.record(entry | 0xfd00000000000000ull);
    auto writing = std::async(std::launch::async, [&] { blocked.save(); });
    CHECK(writing.wait_for(300ms) == std::future_status::timeout);
    other.reset();
    REQUIRE(writing.wait_for(10s) == std::future_status::ready);
    std::ifstream saved(cache); Json result; saved >> result;
    CHECK(result["blocks"].back() == Json::array({document["blocks"][0][0], 0x180ull, 0xfd00000000000000ull}));
    for (const auto& e : std::filesystem::directory_iterator(cache_directory))
      CHECK(e.path().extension() != ".tmp");
  }
  const auto invalid = [&](Json changed) {
    paths::atomic_write(cache, changed.dump()); warmed.clear();
    JitBlockList list; list.prepare(entry, false, compile); CHECK(warmed.empty());
  };
  auto changed = document; changed["blocks"][0][0] = std::string(64, '0'); invalid(changed);
  changed = document; changed["blocks"][0][1] = 0x5000ull; invalid(changed);
  changed = document; changed["blocks"][0][2] = 1ull; invalid(changed);
  changed = document; changed["version"] = 2; invalid(changed);
  paths::atomic_write(cache, "{broken"); warmed.clear();
  JitBlockList broken; CHECK_NOTHROW(broken.prepare(entry, false, compile)); CHECK(warmed.empty());
  CHECK(Linker::get().close(lib) == 0);
}
