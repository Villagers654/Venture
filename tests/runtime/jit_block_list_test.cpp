#include <doctest/doctest.h>
#include "cpu/block_list.h"
#include "cpu/cpu.h"
#include "core/elf.h"
#include "core/linker_runtime.h"
#include "core/process.h"
#include "core/thread_runtime.h"
#include "hle/registry.h"
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
std::atomic<bool> pause_next_snapshot{false}; // Taken by the background worker.
void snapshot_observer() {
  if (!pause_next_snapshot.exchange(false)) return;
  snapshot_gate->entered.set_value();
  snapshot_gate->release.wait();
}
struct Row { std::uint32_t module; std::uint64_t offset, mode; };
std::string hint_file(const std::vector<std::string>& modules, const std::vector<Row>& rows) {
  std::string out("VHL1", 4);
  const auto put = [&](const auto& v) { out.append(reinterpret_cast<const char*>(&v), sizeof(v)); };
  put(std::uint32_t(modules.size()));
  for (const auto& m : modules) { put(std::uint32_t(m.size())); out += m; }
  put(std::uint32_t(rows.size()));
  for (const auto& r : rows) { put(r.module); put(r.offset); put(r.mode); }
  return out;
}
std::pair<std::vector<std::string>, std::vector<Row>> parse(const std::filesystem::path& file) {
  std::ifstream stream(file, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(stream)), {});
  std::size_t at = 4;
  const auto take = [&]<class T>(T& v) { std::memcpy(&v, bytes.data() + at, sizeof(v)); at += sizeof(v); };
  std::uint32_t count = 0;
  take(count);
  std::vector<std::string> modules(count);
  for (auto& m : modules) { std::uint32_t n = 0; take(n); m.assign(bytes.data() + at, n); at += n; }
  take(count);
  std::vector<Row> rows(count);
  for (auto& r : rows) { take(r.module); take(r.offset); take(r.mode); }
  return {modules, rows};
}
std::vector<std::uint64_t> drain(venture::JitBlockList& list) {
  venture::JitBlockList::flush();
  std::vector<std::uint64_t> out;
  std::uint64_t d;
  while (list.next_hint(d)) out.push_back(d);
  return out;
}
}

using namespace venture;
TEST_CASE("JIT block list persists validated module-relative hints per thread name and rejects corrupt input") {
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
    if (!e.is_symlink() && e.is_regular_file()) std::filesystem::remove(e.path());
  register_all_hle_libraries();
  GuestThread thread(97); thread.attach_to_current_host_thread();
  linker_set_inspection(false);
  auto* lib = Linker::get().open(source.string(), kRtldNow, nullptr);
  INFO(Linker::get().take_error()); REQUIRE(lib != nullptr);
  const auto entry = lib->load_bias + 0x180;
  constexpr std::uint64_t mode = 0x0800000000000000ull;
  const auto cache = cache_directory / "Unit_thread-64.vhl";

  JitBlockList cold;
  cold.name("Unit thread", false);
  CHECK(drain(cold).empty());
  cold.record(entry | mode); cold.record(entry | mode);
  cold.record(0x00ff000000000000ull); // Anonymous address must not be persisted.
  cold.save();
  REQUIRE(std::filesystem::exists(cache));
  auto [modules, rows] = parse(cache);
  REQUIRE(modules.size() == 1);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].offset == 0x180);
  CHECK(rows[0].mode == mode);
  JitBlockList warm; warm.name("Unit thread", false);
  CHECK(drain(warm) == std::vector<std::uint64_t>{entry | mode});
  JitBlockList other; other.name("Other", false);
  CHECK(drain(other).empty());
  {
    // A hint compiled again counts from its latest compilation: A, B, A, C saves as B, A, C.
    JitBlockList order; order.name("Order", false);
    for (const auto m : {1ull, 2ull, 1ull, 3ull}) order.record(entry | (m << 56));
    order.save();
    const auto [m, saved] = parse(cache_directory / "Order-64.vhl");
    REQUIRE(saved.size() == 3);
    CHECK(saved[0].mode == 2ull << 56);
    CHECK(saved[1].mode == 1ull << 56);
    CHECK(saved[2].mode == 3ull << 56);
  }
  CHECK(thread.call(entry, {}) == 42);
  {
    using namespace std::chrono_literals;
    // Naming never waits for the load: the worker's linker snapshot is paused here,
    // and the file mutex is free while it waits.
    std::promise<void> release;
    SnapshotGate gate{{}, release.get_future().share()}; snapshot_gate = &gate;
    auto entered = gate.entered.get_future();
    JitBlockListTestAccess::observe(snapshot_observer);
    pause_next_snapshot = true;
    auto naming = std::async(std::launch::async, [&] { JitBlockList list; list.name("Unit thread", false); return list.hints_pending(); });
    CHECK(naming.wait_for(5s) == std::future_status::ready);
    const bool paused = entered.wait_for(5s) == std::future_status::ready;
    CHECK(paused);
    if (paused) CHECK(JitBlockListTestAccess::files_unlocked());
    else pause_next_snapshot = false;
    CHECK_FALSE(naming.get()); // Not loaded while the snapshot waits.
    release.set_value(); JitBlockList::flush();
    JitBlockListTestAccess::observe(nullptr); snapshot_gate = nullptr;
  }
  {
    using namespace std::chrono_literals;
    // A retiring thread only queues its hints: persistence, including a slow
    // linker snapshot, runs on the background worker. Detaching must return
    // while that snapshot is paused, and other threads must still retire.
    std::promise<void> release;
    SnapshotGate gate{{}, release.get_future().share()}; snapshot_gate = &gate;
    auto entered = gate.entered.get_future();
    JitBlockListTestAccess::observe(snapshot_observer);
    auto cleanup = std::async(std::launch::async, [&] {
      GuestThread worker(30997); worker.attach_to_current_host_thread();
      worker.cpu().set_thread_name("Unit worker");
      JitBlockList::flush();
      worker.cpu().set_fpcr(0); // A different descriptor than the stored hint.
      CHECK(worker.call(entry, {}) == 42);
      pause_next_snapshot = true; // The next snapshot is the worker's save.
      worker.detach_from_current_host_thread();
    });
    const bool detached = cleanup.wait_for(5s) == std::future_status::ready;
    CHECK(detached);
    const bool paused = entered.wait_for(5s) == std::future_status::ready;
    CHECK(paused);
    if (paused) {
      CHECK(JitBlockListTestAccess::files_unlocked());
      auto independent = std::async(std::launch::async, [&] {
        GuestThread other_thread(30998); other_thread.attach_to_current_host_thread();
        other_thread.detach_from_current_host_thread();
      });
      CHECK(independent.wait_for(2s) == std::future_status::ready);
      release.set_value(); independent.get();
    } else { pause_next_snapshot = false; release.set_value(); }
    cleanup.get(); JitBlockList::flush();
    JitBlockListTestAccess::observe(nullptr); snapshot_gate = nullptr;
    CHECK(std::filesystem::exists(cache_directory / "Unit_worker-64.vhl"));
  }
  {
    // A full file: a hint seen again moves to the end, and a new one drops the oldest.
    constexpr std::size_t limit = std::size_t(1) << 20;
    std::vector<Row> full;
    for (std::uint64_t i = 0; i < limit; ++i) full.push_back({0, i * 4, 0}); // Saving does not validate old rows.
    paths::atomic_write(cache, hint_file(modules, full));
    JitBlockList replacement;
    replacement.name("Unit thread", false);
    replacement.record(lib->load_bias);
    replacement.record(entry | 0xfe00000000000000ull);
    replacement.save();
    const auto [m, result] = parse(cache);
    REQUIRE(result.size() == limit);
    CHECK(result[0].offset == full[2].offset);
    CHECK(result[0].mode == full[2].mode);
    CHECK(result[limit - 2].offset == 0);
    CHECK(result[limit - 2].mode == 0);
    CHECK(result[limit - 1].offset == 0x180);
    CHECK(result[limit - 1].mode == 0xfe00000000000000ull);
  }
  {
    using namespace std::chrono_literals;
    // Another process's writer holds the directory lock: save() waits for it
    // (merge, budget and replace are one critical section), then writes through
    // a unique temporary file and leaves none behind.
    std::optional<FileLock> lock(std::in_place, cache_directory / "persist.lock");
    JitBlockList blocked;
    blocked.name("Unit thread", false);
    blocked.record(entry | 0xfd00000000000000ull);
    auto writing = std::async(std::launch::async, [&] { blocked.save(); });
    CHECK(writing.wait_for(300ms) == std::future_status::timeout);
    lock.reset();
    REQUIRE(writing.wait_for(10s) == std::future_status::ready);
    const auto [m, result] = parse(cache);
    CHECK(result.back().offset == 0x180);
    CHECK(result.back().mode == 0xfd00000000000000ull);
    for (const auto& e : std::filesystem::directory_iterator(cache_directory))
      CHECK(e.path().extension() != ".tmp");
  }
  const auto invalid = [&](const std::string& bytes) {
    paths::atomic_write(cache, bytes);
    JitBlockList list; list.name("Unit thread", false);
    CHECK(drain(list).empty());
  };
  invalid(hint_file({std::string(40, '0')}, {{0, 0x180, mode}}));  // Another module.
  invalid(hint_file(modules, {{1, 0x180, mode}}));                 // No such module index.
  invalid(hint_file(modules, {{0, 0x5000, mode}}));                // Outside the executable segment.
  invalid(hint_file(modules, {{0, 0x180, 1}}));                    // Mode overlapping the address.
  invalid(hint_file(modules, {{0, 0x180, mode}}).substr(0, 30));   // Truncated.
  invalid("{broken");
  CHECK(Linker::get().close(lib) == 0);
}
