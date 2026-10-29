#include "core/address_space.h"
#include "core/elf.h"
#include "core/hle_runtime.h"
#include "core/guest_storage.h"
#include "hle/libc/libc_parts.h"
#include "core/linker_runtime.h"
#include "core/process_runtime.h"
#include "core/thread.h"
#include "hle/registry.h"
#include <array>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <semaphore>
#include <thread>
using namespace venture;
namespace {
// Original synthetic ELF data only; no SDK or game binaries.
struct ReplacementFixture {
  std::array<std::byte, 4096> bytes{};
  std::string strings = std::string(1, '\0');
  std::vector<elf::Sym> symbols{{}};
  std::vector<elf::Dyn> dynamic;
  std::vector<elf::Rela> relocations;
  unsigned objects = 0;
  template <class T> void put(std::size_t at, const T &value) {
    std::memcpy(bytes.data() + at, &value, sizeof(value));
  }
  std::uint32_t text(std::string_view s) {
    auto n = std::uint32_t(strings.size());
    strings += s;
    strings += '\0';
    return n;
  }
  void function(std::string_view name, unsigned value) {
    auto at = 0x100 + 8 * (symbols.size() - 1);
    symbols.push_back({text(name), 0x12, 0, 1, at, 8});
    put(at, process_config().isa() == GuestIsa::a32
                ? std::array<std::uint32_t, 2>{0xe3a00000u | value, 0xe12fff1e}
                : std::array<std::uint32_t, 2>{0xd2800000u | (value << 5),
                                               0xd65f03c0});
  }
  void object(std::string_view name, unsigned value) {
    auto at = 0xa00 + 4 * objects++;
    symbols.push_back({text(name), 0x11, 0, 1, at, 4});
    put(at, value);
  }
  void import(std::string_view name) {
    auto index = symbols.size();
    symbols.push_back({text(name), 0x12, 0, 0, 0, 0});
    relocations.push_back(
        {0x900 + 8 * (index - 1), (std::uint64_t(index) << 32) | 257, 0});
  }
  void needed(std::string_view name) { dynamic.push_back({1, text(name)}); }
  void initialize_data(unsigned value) {
    dynamic.push_back({12, 0x180});
    if (process_config().isa() == GuestIsa::a32)
      put(0x180, std::array<std::uint32_t, 7>{0xe59f0010, 0xe3a01000u | value,
                                              0xe08f0000, 0xe5801000,
                                              0xe12fff1e, 0xe1a00000, 0x870});
    else
      put(0x180,
          std::array<std::uint32_t, 4>{0x10004400, 0x52800001u | (value << 5),
                                       0xb9000001, 0xd65f03c0});
  }
  void initialize_from_import(std::uint32_t slot) {
    dynamic.push_back({12, 0x180});
    if (process_config().isa() == GuestIsa::a32)
      put(0x180, std::array<std::uint32_t, 10>{0xe59f0018, 0xe08f0000,
          0xe5900000, 0xe5901000, 0xe59f000c, 0xe08f0000, 0xe5801000,
          0xe12fff1e, slot - 0x18c, 0xa00 - 0x19c});
    else {
      auto adr = [](unsigned delta) { return 0x10000000u | ((delta & 3) << 29) | ((delta >> 2) << 5); };
      put(0x180, std::array<std::uint32_t, 6>{adr(slot - 0x180),
          0xf9400000, 0xb9400001, adr(0xa00 - 0x18c), 0xb9000001, 0xd65f03c0});
    }
  }
  void initialize_call_import(std::uint32_t slot) {
    dynamic.push_back({12, 0x180});
    if (process_config().isa() == GuestIsa::a32)
      put(0x180, std::array<std::uint32_t, 7>{0xe92d4010, 0xe59f400c,
          0xe08f4004, 0xe5944000, 0xe12fff34, 0xe8bd8010, slot - 0x190});
    else {
      const auto delta = slot - 0x184;
      put(0x180, std::array<std::uint32_t, 6>{0xa9bf7bfd,
          0x10000010u | ((delta & 3) << 29) | ((delta >> 2) << 5),
          0xf9400210, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0});
    }
  }
  void write(const std::filesystem::path &path) {
    const bool a32 = process_config().isa() == GuestIsa::a32;
    elf::Header h{};
    std::memcpy(h.ident, "\177ELF\2\1\1", 7);
    h.type = 3;
    h.machine = 183;
    h.version = 1;
    h.ehsize = sizeof(h);
    h.phoff = 64;
    h.phentsize = 56;
    h.phnum = 2;
    put(0, h);
    put(64, elf::Phdr{1, 7, 0, 0, 0, 4096, 4096, 4096});
    dynamic.insert(dynamic.end(), {{5, 0x400},
                                   {10, strings.size()},
                                   {6, 0x500},
                                   {11, 24},
                                   {4, 0x700},
                                   {7, 0xc00},
                                   {8, relocations.size() * 24},
                                   {9, 24},
                                   {0, 0}});
    put(120, elf::Phdr{2, 6, 0x200, 0x200, 0, dynamic.size() * 16,
                       dynamic.size() * 16, 8});
    std::memcpy(bytes.data() + 0x200, dynamic.data(), dynamic.size() * 16);
    std::memcpy(bytes.data() + 0x400, strings.data(), strings.size());
    std::memcpy(bytes.data() + 0x500, symbols.data(), symbols.size() * 24);
    std::vector<std::uint32_t> hash{1, std::uint32_t(symbols.size()), 1, 0};
    for (std::uint32_t i = 1; i < symbols.size(); ++i)
      hash.push_back(i + 1 < symbols.size() ? i + 1 : 0);
    std::memcpy(bytes.data() + 0x700, hash.data(), hash.size() * 4);
    std::memcpy(bytes.data() + 0xc00, relocations.data(),
                relocations.size() * 24);
    if (a32) {
      elf::Header32 h32{};
      std::memcpy(h32.ident, "\177ELF\1\1\1", 7);
      h32.type = 3;
      h32.machine = 40;
      h32.version = 1;
      h32.ehsize = 52;
      h32.phoff = 52;
      h32.phentsize = 32;
      h32.phnum = 2;
      put(0, h32);
      put(52, elf::Phdr32{1, 0, 0, 0, 4096, 4096, 7, 4096});
      std::vector<elf::Dyn32> ds;
      for (auto d : dynamic) {
        if (d.tag == 7)
          d = {17, 0xc00};
        else if (d.tag == 8)
          d = {18, relocations.size() * 8};
        else if (d.tag == 9)
          d = {19, 8};
        else if (d.tag == 11)
          d.value = 16;
        ds.push_back({std::int32_t(d.tag), std::uint32_t(d.value)});
      }
      put(84, elf::Phdr32{2, 0x200, 0x200, 0, std::uint32_t(ds.size() * 8),
                          std::uint32_t(ds.size() * 8), 6, 4});
      std::memcpy(bytes.data() + 0x200, ds.data(), ds.size() * 8);
      std::vector<elf::Sym32> ss;
      for (auto s : symbols)
        ss.push_back({s.name, std::uint32_t(s.value), std::uint32_t(s.size),
                      s.info, s.other, s.shndx});
      std::memcpy(bytes.data() + 0x500, ss.data(), ss.size() * 16);
      std::vector<elf::Rel32> rs;
      for (auto r : relocations)
        rs.push_back(
            {std::uint32_t(r.offset), std::uint32_t((r.info >> 32) << 8) | 2});
      std::memcpy(bytes.data() + 0xc00, rs.data(), rs.size() * 8);
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }
};
} // namespace
TEST_CASE("replacement exports preserve originals without unrelated C++ fallback") {
#ifdef VENTURE_REPLACEMENT_FIXTURE_A32
  select_process_isa(GuestIsa::a32);
#endif
  AddressSpace::get().initialize(process_config().isa());
  auto dir = std::filesystem::current_path() / "build/replacement-fixtures" /
             (process_config().isa() == GuestIsa::a32 ? "a32" : "a64");
  std::filesystem::create_directories(dir);
  constexpr std::array helpers{"__dynamic_cast", "__cxa_throw",
                               "_Unwind_Backtrace", "_ZStfixture_helper"};
  ReplacementFixture cpp;
  for (unsigned i = 0; i < helpers.size(); ++i)
    cpp.function(helpers[i], 201 + i);
  cpp.object("__cxa_fixture_data", 207);
  cpp.initialize_data(217);
  cpp.object("_end", 239);
  cpp.object("_ZTIreplacement_type", 209);
  cpp.write(dir / "libc++_shared.so");
  ReplacementFixture original;
  for (unsigned i = 0; i < helpers.size(); ++i)
    original.function(helpers[i], 101 + i);
  original.function("replacement_native_api", 105);
  original.function("vrapi_fixture_missing", 106);
  original.function("__cxa_fixture_original", 118);
  original.object("replacement_data", 107);
  original.initialize_data(117);
  original.object("_end", 139);
  original.object("_ZTIreplacement_type", 109);
  original.object("__cxa_fixture_data", 110);
  original.write(dir / "libreplacement-fixture.so");
  ReplacementFixture caller;
  caller.needed("libreplacement-fixture.so");
  for (auto name : helpers)
    caller.import(name);
  caller.import("replacement_native_api");
  caller.import("vrapi_fixture_missing");
  caller.import("_ZTIreplacement_type");
  caller.import("__cxa_fixture_data");
  caller.initialize_from_import(0x938);
  caller.write(dir / "libreplacement-caller.so");
  auto hle = std::make_shared<HleLibrary>("libreplacement-fixture.so");
  const auto native =
      hle::make_thunk(hle->soname(), "replacement_native_api",
                      [](hle::CallContext &c) { c.return_x(305); });
  hle->add_function("replacement_native_api", native);
  auto &linker = Linker::get();
  linker.register_hle_library(hle);
  linker.set_search_paths({dir.string()});
  linker_set_inspection(false);
  GuestThread thread(99);
  thread.attach_to_current_host_thread();
  auto images = [&] { unsigned count = 0; linker.for_each_image([&](const Library&) { ++count; }); return count; };
  const auto initial_images = images();
  ReplacementFixture no_miss;
  no_miss.needed("libreplacement-fixture.so");
  no_miss.import("replacement_native_api");
  // An unused undefined dynsym entry is not a symbol lookup miss.
  no_miss.symbols.push_back({no_miss.text("__cxa_fixture_original"), 0x12, 0, 0, 0, 0});
  no_miss.write(dir / "libreplacement-no-miss.so");
  auto* unchanged = linker.open((dir / "libreplacement-no-miss.so").string(), kRtldNow, nullptr);
  REQUIRE(unchanged != nullptr);
  CHECK(images() == initial_images + 1); // Only the dependent, exactly as main.
  CHECK(linker.close(unchanged) == 0);
  CHECK(images() == initial_images);
  auto *root = linker.open((dir / "libreplacement-caller.so").string(),
                           kRtldNow, nullptr);
  INFO(linker.take_error());
  REQUIRE(root != nullptr);
  auto *replacement =
      linker.open("libreplacement-fixture.so", kRtldNow | kRtldNoLoad, nullptr);
  REQUIRE(replacement != nullptr);
  auto *cpp_library =
      linker.open("libc++_shared.so", kRtldNow | kRtldNoLoad, nullptr);
  CHECK(cpp_library == nullptr);
  linker.take_error();
  Library *backing = nullptr;
  linker.for_each_image([&](const Library &image) {
    if (image.path().ends_with("/libreplacement-fixture.so"))
      backing = const_cast<Library *>(&image);
  });
  REQUIRE(backing == replacement->mapped_image());
  Library* diagnostic_backing = nullptr;
  CHECK(linker.try_for_each_image([&](const Library& image) {
    if (image.path().ends_with("/libreplacement-fixture.so"))
      diagnostic_backing = const_cast<Library*>(&image);
  }));
  CHECK(diagnostic_backing == backing);
  CHECK(images() == initial_images + 2);
  for (unsigned i = 0; i < helpers.size(); ++i) {
    const auto value = *g2h<GuestAddr>(root->load_bias + 0x900 + 8 * i);
    REQUIRE(value == linker.symbol(replacement, helpers[i]));
    CHECK(thread.call(value, {}) == 101 + i);
    CHECK(linker.symbol(replacement, helpers[i]) == value);

    CHECK(linker.symbol(nullptr, helpers[i]) ==
          0); // Never leak RTLD_LOCAL helpers.
    linker.take_error();
  }
  CHECK(*g2h<GuestAddr>(root->load_bias + 0x920) == native);
  CHECK(thread.call(linker.symbol(replacement, "vrapi_fixture_missing"), {}) == 106);
  linker.take_error();
  CHECK(*g2h<unsigned>(*g2h<GuestAddr>(root->load_bias + 0x930)) == 109);
  CHECK(*g2h<unsigned>(linker.symbol(replacement, "__cxa_fixture_data")) == 110);
  CHECK(*g2h<unsigned>(root->load_bias + 0xa00) == 110); // Provider initialized before dependent ctor.
  CHECK(linker.close(replacement) == 0);
  CHECK(linker.close(root) == 0);
  auto* stable = linker.open("libreplacement-fixture.so", kRtldNow | kRtldNoLoad, nullptr);
  CHECK(stable == hle.get());
  CHECK(linker.close(stable) == 0);
  ReplacementFixture fallback;
  fallback.needed("libreplacement-fixture.so");
  fallback.import("__cxa_fixture_original");
  fallback.import("vrapi_fixture_missing");
  fallback.write(dir / "libreplacement-fallback.so");
  auto* retained = linker.open((dir / "libreplacement-fallback.so").string(), kRtldNow, nullptr);
  REQUIRE(retained != nullptr);
  CHECK(images() == initial_images + 2);
  CHECK(thread.call(*g2h<GuestAddr>(retained->load_bias + 0x900), {}) == 118);
  auto* handle = linker.open("libreplacement-fixture.so", kRtldNow | kRtldNoLoad, nullptr);
  CHECK(thread.call(linker.symbol(handle, "vrapi_fixture_missing"), {}) == 106);
  linker.take_error();
  CHECK(*g2h<unsigned>(linker.symbol(handle, "replacement_data")) == 117);
  linker.take_error();
  Library* original_image = nullptr;
  linker.for_each_image([&](const Library& lib) {
    if (lib.path().ends_with("/libreplacement-fixture.so")) original_image = const_cast<Library*>(&lib);
  });
  REQUIRE(original_image != nullptr);
  CHECK(*g2h<unsigned>(original_image->load_bias + 0xa00) == 117);
  CHECK(linker.close(handle) == 0);
  CHECK(linker.close(retained) == 0);
  CHECK(images() == initial_images);
}

namespace {
std::filesystem::path review_directory(std::string_view scenario) {
#ifdef VENTURE_REPLACEMENT_FIXTURE_A32
  select_process_isa(GuestIsa::a32);
#endif
  AddressSpace::get().initialize(process_config().isa());
  auto directory = std::filesystem::current_path() / "build/replacement-review" /
      (process_config().isa() == GuestIsa::a32 ? "a32" : "a64") / scenario;
  std::filesystem::create_directories(directory);
  Linker::get().set_search_paths({directory.string()});
  linker_set_inspection(false);
  return directory;
}
}
TEST_CASE("replacement exports demand lookup and reentrant constructor do not deadlock") {
  const auto dir = review_directory("lock-order");
  GuestThread thread(98); thread.attach_to_current_host_thread();
  auto& linker = Linker::get();
  auto slot = std::make_shared<HleLibrary>("liblock-backing.so"); linker.register_hle_library(slot);
  ReplacementFixture original; original.function("guest_helper", 42);
  original.write(dir / slot->soname());
  REQUIRE(linker.open((dir / slot->soname()).string(), kRtldNow, nullptr) == slot.get());
  struct GateState {
    std::binary_semaphore entered{0}, attempting{0};
    std::atomic<bool> reentered{false};
    HleLibrary* hooks{};
  } gate;
  std::atomic<bool> activated{false};
  auto hooks = std::make_shared<HleLibrary>("libc.so");
  hooks->add_data("available", 0x12340);
  gate.hooks = hooks.get();
  const auto gate_handler = +[](hle::CallContext& c) {
    auto& gate = *static_cast<GateState*>(c.user());
    auto& linker = Linker::get();
    gate.entered.release(); gate.attempting.acquire();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
      if (!linker.try_for_each_image([](const Library&) {})) break;
      std::this_thread::yield();
    }
    gate.reentered = linker.symbol(gate.hooks, "available") == 0x12340;
    c.return_x(0);
  };
  hooks->add_function("sched_yield", hle::make_thunk(hooks->soname(), "sched_yield",
      hle::a32::bind_dynamic<int()>(gate_handler, gate_handler), &gate));
  linker.register_hle_library(hooks);
  ReplacementFixture constructor; constructor.needed(hooks->soname()); constructor.import("sched_yield");
  constructor.initialize_call_import(0x900); constructor.write(dir / "libreview-constructor.so");
  std::mutex mutex; std::condition_variable cv; bool done = false;
  std::thread watchdog([&] {
    std::unique_lock lock(mutex);
    if (!cv.wait_for(lock, std::chrono::seconds(8), [&] { return done; })) std::_Exit(92);
  });
  Library* initialized = nullptr;
  std::thread b([&] { GuestThread guest(100); guest.attach_to_current_host_thread();
    initialized = linker.open((dir / "libreview-constructor.so").string(), kRtldNow, nullptr); });
  gate.entered.acquire();
  std::thread a([&] { GuestThread guest(101); guest.attach_to_current_host_thread();
    gate.attempting.release(); activated = linker.symbol(slot.get(), "guest_helper") != 0; });
  a.join(); b.join();
  { std::lock_guard lock(mutex); done = true; } cv.notify_one(); watchdog.join();
  CHECK(initialized); CHECK(gate.reentered); CHECK(activated);
  CHECK(linker.close(initialized) == 0); CHECK(linker.close(slot.get()) == 0);
}

TEST_CASE("replacement exports global reopen promotes only the requested dependency closure") {
  const auto dir = review_directory("promotion");
  GuestThread thread(98); thread.attach_to_current_host_thread();
  ReplacementFixture leaf; leaf.function("promotion_leaf", 11); leaf.write(dir / "libpromotion-leaf.so");
  ReplacementFixture sibling; sibling.function("promotion_sibling", 12); sibling.write(dir / "libpromotion-sibling.so");
  ReplacementFixture root; root.function("promotion_consumer", 13);
  root.needed("libpromotion-leaf.so"); root.needed("libpromotion-sibling.so"); root.write(dir / "libpromotion-root.so");
  auto& linker = Linker::get();
  auto* a = linker.open((dir / "libpromotion-root.so").string(), kRtldNow, nullptr); REQUIRE(a);
  auto* b = linker.open("libpromotion-leaf.so", kRtldNow | kRtldGlobal, nullptr); REQUIRE(b);
  CHECK(linker.symbol_default("promotion_leaf", nullptr) != 0);
  CHECK(linker.symbol_default("promotion_consumer", nullptr) == 0);
  CHECK(linker.symbol_default("promotion_sibling", nullptr) == 0);
  CHECK_FALSE(a->global);
  CHECK(linker.close(b) == 0); CHECK(linker.close(a) == 0);
}
TEST_CASE("replacement exports finalization and adopted lookup preserve one lock order") {
  const auto dir = review_directory("finalization-order");
  GuestThread thread(98); thread.attach_to_current_host_thread();
  auto& linker = Linker::get();
  auto slot = std::make_shared<HleLibrary>("libfinalize-backing.so"); linker.register_hle_library(slot);
  ReplacementFixture original; original.function("guest_helper", 42);
  original.write(dir / slot->soname());
  REQUIRE(linker.open((dir / slot->soname()).string(), kRtldNow, nullptr) == slot.get());
  struct GateState {
    std::binary_semaphore entered{0}, attempting{0};
    std::atomic<bool> reentered{false};
    HleLibrary* hooks{};
  } gate;
  std::atomic<bool> activated{false};
  auto hooks = std::make_shared<HleLibrary>("libc.so");
  hooks->add_data("available", 0x12340);
  gate.hooks = hooks.get();
  const auto gate_handler = +[](hle::CallContext& c) {
    auto& gate = *static_cast<GateState*>(c.user());
    auto& linker = Linker::get();
    gate.entered.release(); gate.attempting.acquire();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
      if (!linker.try_for_each_image([](const Library&) {})) break;
      std::this_thread::yield();
    }
    gate.reentered = linker.symbol(gate.hooks, "available") == 0x12340;
    c.return_x(0);
  };
  hooks->add_function("sched_yield", hle::make_thunk(hooks->soname(), "sched_yield",
      hle::a32::bind_dynamic<int()>(gate_handler, gate_handler), &gate));
  linker.register_hle_library(hooks);
  ReplacementFixture constructor; constructor.needed(hooks->soname()); constructor.import("sched_yield");
  constructor.initialize_call_import(0x900); constructor.write(dir / "libfinalize-constructor.so");
  std::mutex mutex; std::condition_variable cv; bool done = false;
  std::thread watchdog([&] {
    std::unique_lock lock(mutex);
    if (!cv.wait_for(lock, std::chrono::seconds(8), [&] { return done; })) std::_Exit(92);
  });
  Library* initialized = nullptr;
  std::thread b([&] { GuestThread::adopt_current();
    initialized = linker.open((dir / "libfinalize-constructor.so").string(), kRtldNow, nullptr); });
  gate.entered.acquire();
  std::thread a([&] { GuestThread guest(101); guest.attach_to_current_host_thread();
    gate.attempting.release(); linker_finalize(); activated = true; });
  a.join(); b.join();
  { std::lock_guard lock(mutex); done = true; } cv.notify_one(); watchdog.join();
  CHECK(initialized); CHECK(gate.reentered); CHECK(activated);
  CHECK(linker.close(initialized) == 0); CHECK(linker.close(slot.get()) == 0);
}

TEST_CASE("linker never resolves a bare library name against the host working directory") {
  // Linux packages keep host libEGL.so/libGLESv2.so beside the binary, which
  // is the working directory of Steam shortcuts.
  const auto dir = std::filesystem::temp_directory_path() / "venture-linker-cwd-probe";
  std::filesystem::create_directories(dir / "empty");
  { std::ofstream(dir / "libventure-cwd-probe.so") << "not an ARM ELF"; }
  const auto previous = std::filesystem::current_path();
  std::filesystem::current_path(dir);
  auto& linker = Linker::get();
  linker.set_search_paths({(dir / "empty").string()});
  auto* lib = linker.open("libventure-cwd-probe.so", kRtldNow, nullptr);
  const auto error = linker.take_error();
  std::filesystem::current_path(previous);
  std::filesystem::remove_all(dir);
  CHECK(lib == nullptr);
  CHECK(error.find("library not found") != std::string::npos);
}
