// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>
#include "core/elf.h"
#include "core/a32_abi.h"
#include "core/linker_runtime.h"
#include "core/process_runtime.h"
#include "core/hle_runtime.h"
#include "core/thread_runtime.h"
#include "core/address_space.h"
#include "hle/registry.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
using namespace venture;

namespace {
GuestAddr lifecycle_thunk(std::string_view library, std::string_view name,
                          hle::Handler handler, void* user) {
  if (process_config().isa() == GuestIsa::a32) {
    // Dynamic test callbacks have the same void(void) ABI as NDK tzset. Use
    // that declaration and bind only these private handler pointers; no libc
    // export or production adapter is replaced by this synthetic fixture.
    hle::a32::bind_handler(handler, handler, [](const hle::a32::Descriptor& d) {
      return hle::a32::Check{d.arguments.empty() && !d.variadic &&
          d.result.kind == hle::a32::Kind::Void, "fixture void(void) callback"};
    });
    return hle::make_thunk("libc.so", "tzset", handler, user);
  }
  return hle::make_thunk(library, name, handler, user);
}
// Entirely synthetic ELF fixture. Relocations connect guest init/fini arrays
// to HLE callbacks so the lock interleaving does not depend on JIT scheduling.
std::filesystem::path lifecycle_image(const std::filesystem::path& directory,
                                      std::string_view suffix, bool invalid = false) {
  std::array<std::byte, 4096> file{};
  auto put = [&]<class T>(std::size_t offset, const T& value) {
    std::memcpy(file.data() + offset, &value, sizeof(value));
  };
  const bool a32 = process_config().isa() == GuestIsa::a32;
  elf::Header header{};
  std::memcpy(header.ident, "\177ELF\2\1\1", 7);
  header.type = 3; header.machine = 183; header.version = 1;
  header.ehsize = sizeof(header); header.phoff = 64;
  header.phentsize = sizeof(elf::Phdr); header.phnum = 2;
  put(0, header);
  put(64, elf::Phdr{1, 7, 0, 0, 0, 4096, 4096, 4096});
  std::string strings(1, '\0');
  auto name = [&](std::string_view value) {
    auto offset = static_cast<std::uint32_t>(strings.size());
    strings += value; strings += '\0'; return offset;
  };
  const auto needed = name("libloader-lifecycle-hle.so");
  const auto init = name("lifecycle_init_" + std::string(suffix));
  const auto fini = name("lifecycle_fini_" + std::string(suffix));
  std::memcpy(file.data() + 0x400, strings.data(), strings.size());
  put(0x500, std::array<elf::Sym, 3>{{{}, {init, 0x12, 0, 0, 0, 0}, {fini, 0x12, 0, 0, 0, 0}}});
  put(0x700, std::array<std::uint32_t, 6>{1, 3, 1, 0, 2, 0});
  const std::array<elf::Rela, 2> relas{{{0x900, (1ull << 32) | 257, 0}, {0x908, (2ull << 32) | 257, 0}}};
  put(0xa00, relas);
  const std::array<elf::Dyn, 14> dynamic{{
      {1, needed}, {5, 0x400}, {10, strings.size()}, {6, 0x500}, {11, sizeof(elf::Sym)}, {4, 0x700},
      {7, 0xa00}, {8, sizeof(relas)}, {9, sizeof(elf::Rela)},
      {25, 0x900}, {27, invalid ? 7u : 8u}, {26, 0x908}, {28, 8}, {0, 0}}};
  if (a32) {
    elf::Header32 h{};
    std::memcpy(h.ident, "\177ELF\1\1\1", 7);
    h.type = 3; h.machine = 40; h.version = 1; h.ehsize = sizeof(h);
    h.phoff = 52; h.phentsize = sizeof(elf::Phdr32); h.phnum = 2;
    put(0, h);
    put(52, elf::Phdr32{1, 0, 0, 0, 4096, 4096, 7, 4096});
    put(0x500, std::array<elf::Sym32, 3>{{{}, {init, 0, 0, 0x12, 0, 0}, {fini, 0, 0, 0x12, 0, 0}}});
    const std::array<elf::Rel32, 2> rels{{{0x900, (1u << 8) | 2}, {0x908, (2u << 8) | 2}}};
    put(0xa00, rels);
    const std::array<elf::Dyn32, 14> dyn32{{
        {1, needed}, {5, 0x400}, {10, static_cast<std::uint32_t>(strings.size())},
        {6, 0x500}, {11, sizeof(elf::Sym32)}, {4, 0x700},
        {17, 0xa00}, {18, sizeof(rels)}, {19, sizeof(elf::Rel32)},
        {25, 0x900}, {27, invalid ? 3u : 4u}, {26, 0x908}, {28, 4}, {0, 0}}};
    put(84, elf::Phdr32{2, 0x200, 0x200, 0, sizeof(dyn32), sizeof(dyn32), 6, 4});
    put(0x200, dyn32);
  } else {
    put(120, elf::Phdr{2, 6, 0x200, 0x200, 0, sizeof(dynamic), sizeof(dynamic), 8});
    put(0x200, dynamic);
  }
  std::filesystem::create_directories(directory);
  auto path = directory / ("libloader-lifecycle-" + std::string(suffix) + (invalid ? "-invalid" : "") + ".so");
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(file.data()), file.size()); output.close();
  if (!output) throw std::runtime_error("cannot write synthetic lifecycle fixture");
  return path;
}
}
TEST_CASE("loader lifecycle concurrent constructor reentry and close") {
#ifdef VENTURE_TEST_LOADER_A32
  select_process_isa(GuestIsa::a32);
  AddressSpace::get().initialize(GuestIsa::a32);
#endif
  struct State {
    std::atomic<bool> constructor_entered{false}, close_started{false};
    std::atomic<bool> diagnostic_finished{false}, diagnostic_nonblocking{false};
    std::atomic<int> init_a{0}, fini_a{0}, init_b{0}, fini_b{0}, init_c{0}, fini_c{0};
    std::filesystem::path nested, independent;
    Library* reopened = nullptr;
    bool reopened_once = false;
    Library* provider = nullptr;
    bool reentered = false, nested_opened = false, active_retained = false;
  } state;
  // A bounded failure is essential: the pre-fix interleaving deadlocks two
  // native threads, so neither thread can perform normal test cleanup.
  std::jthread watchdog([](std::stop_token stop) {
    for (unsigned i = 0; i < 800 && !stop.stop_requested(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!stop.stop_requested()) {
      std::fputs("P1 synthetic loader lifecycle deadlock: constructor reentry versus concurrent dlclose\n", stderr);
      std::fflush(stderr); std::_Exit(86);
    }
  });
  auto provider = std::make_shared<HleLibrary>("libloader-lifecycle-hle.so");
  state.provider = provider.get();
  provider->add_function("lifecycle_init_a", lifecycle_thunk(provider->soname(), "init a", [](hle::CallContext& ctx) {
    auto& state = *static_cast<State*>(ctx.user()); ++state.init_a;
    state.constructor_entered.store(true);
    while (!state.close_started.load() || !state.diagnostic_finished.load()) std::this_thread::yield();
    // Before the fix, close takes state then waits for our initializer lock.
    // Observe its lock acquisition rather than relying on a fixed sleep. With
    // one recursive loader lock, close stays outside and this probe times out.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (Linker::get().try_for_each_image([](const Library&) {}) &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    state.reentered = Linker::get().symbol(state.provider, "lifecycle_init_b") != 0;
    auto* nested = Linker::get().open(state.nested.string(), kRtldNow, nullptr);
    state.nested_opened = nested != nullptr;
    if (nested) Linker::get().close(nested);
  }, &state));
  provider->add_function("lifecycle_fini_a", lifecycle_thunk(provider->soname(), "fini a", [](hle::CallContext& ctx) {
    ++static_cast<State*>(ctx.user())->fini_a;
  }, &state));
  provider->add_function("lifecycle_init_b", lifecycle_thunk(provider->soname(), "init b", [](hle::CallContext& ctx) {
    ++static_cast<State*>(ctx.user())->init_b;
  }, &state));
  provider->add_function("lifecycle_fini_b", lifecycle_thunk(provider->soname(), "fini b", [](hle::CallContext& ctx) {
    auto& s = *static_cast<State*>(ctx.user()); ++s.fini_b;
    if (!s.reopened_once) {
      s.reopened_once = true;
      s.reopened = Linker::get().open(s.independent.string(), kRtldNow, nullptr);
    }
  }, &state));
  provider->add_function("lifecycle_init_c", lifecycle_thunk(provider->soname(), "init c", [](hle::CallContext& ctx) {
    auto& s = *static_cast<State*>(ctx.user()); ++s.init_c;
    s.reentered &= Linker::get().symbol(s.provider, "lifecycle_init_b") != 0;
    Library* self = nullptr;
    Linker::get().for_each_image([&](const Library& image) {
      if (image.path().ends_with(s.nested.filename().string())) self = const_cast<Library*>(&image);
    });
    if (self) {
      Linker::get().close(self);
      s.active_retained = s.fini_c == 0 && Linker::get().library_at(self->map_start) == self;
    }
  }, &state));
  provider->add_function("lifecycle_fini_c", lifecycle_thunk(provider->soname(), "fini c", [](hle::CallContext& ctx) {
    auto& s = *static_cast<State*>(ctx.user()); ++s.fini_c;
    s.reentered &= Linker::get().symbol(s.provider, "lifecycle_init_b") != 0;
  }, &state));
  auto& linker = Linker::get(); linker.register_hle_library(provider);
  auto directory = std::filesystem::current_path() / "build/loader-lifecycle-fixtures" /
      (process_config().isa() == GuestIsa::a32 ? "a32" : "a64");
  auto a = lifecycle_image(directory, "a"), b = lifecycle_image(directory, "b");
  state.independent = b;
  state.nested = lifecycle_image(directory, "c");
  auto invalid = lifecycle_image(directory, "c", true);
  GuestThread thread(7191); thread.attach_to_current_host_thread();
  linker_set_inspection(false);
  auto* independent = linker.open(b.string(), kRtldNow, nullptr);
  REQUIRE(independent != nullptr);
  int close_result = -1;
  std::jthread closer([&] {
    GuestThread other(7192); other.attach_to_current_host_thread();
    while (!state.constructor_entered.load()) std::this_thread::yield();
    state.close_started.store(true);
    close_result = linker.close(independent);
  });
  std::jthread diagnostic([&] {
    while (!state.constructor_entered.load()) std::this_thread::yield();
    DlInfo info; std::int32_t count = -1;
    const bool symbol = linker.try_address_info(0, &info);
    const auto exidx = linker.try_find_exidx(0, &count);
    const bool images = linker.try_for_each_image([](const Library&) {});
    state.diagnostic_nonblocking.store(!symbol && !exidx && count == 0 && !images);
    state.diagnostic_finished.store(true);
  });
  auto* root = linker.open(a.string(), kRtldNow, nullptr);
  diagnostic.join();
  closer.join();
  REQUIRE(root != nullptr);
  CHECK(close_result == 0);
  CHECK(state.diagnostic_nonblocking.load());
  CHECK(state.reentered); CHECK(state.nested_opened); CHECK(state.active_retained);
  CHECK(state.init_a == 1); CHECK(state.init_b == 2); CHECK(state.init_c == 1);
  CHECK(state.fini_a == 0); CHECK(state.fini_b == 1); CHECK(state.fini_c == 1);
  bool revived_visible = false;
  linker.for_each_image([&](const Library& image) { revived_visible |= &image == state.reopened; });
  REQUIRE(state.reopened != nullptr); REQUIRE(revived_visible);
  CHECK(linker.close(state.reopened) == 0); CHECK(state.fini_b == 2);
  CHECK(linker.close(root) == 0); CHECK(state.fini_a == 1);
  CHECK(linker.open(invalid.string(), kRtldNow, nullptr) == nullptr);
  CHECK_FALSE(linker.take_error().empty());
  CHECK(linker.open(invalid.string(), kRtldNow | kRtldNoLoad, nullptr) == nullptr);
  for (auto path : {a, b, state.nested, invalid}) std::filesystem::remove(path);
  watchdog.request_stop();
}

TEST_CASE("loader lifecycle interrupted final snapshot destruction cannot terminate or deadlock") {
#ifdef VENTURE_TEST_LOADER_A32
  select_process_isa(GuestIsa::a32);
  AddressSpace::get().initialize(GuestIsa::a32);
  REQUIRE(process_config().isa() == GuestIsa::a32);
#endif
  GuestThread owner(7194); owner.attach_to_current_host_thread();
  auto provider = std::make_shared<HleLibrary>("libloader-lifecycle-hle.so");
  provider->add_function("lifecycle_init_cleanup", lifecycle_thunk(provider->soname(), "init cleanup", [](hle::CallContext&) {}, nullptr));
  provider->add_function("lifecycle_fini_cleanup", lifecycle_thunk(provider->soname(), "fini cleanup", [](hle::CallContext&) {}, nullptr));
  auto& linker = Linker::get(); linker.register_hle_library(provider);
  linker_set_inspection(false);
  const auto dir = std::filesystem::current_path() / "build/loader-lifecycle-fixtures" /
      (process_config().isa() == GuestIsa::a32 ? "a32" : "a64");
  const auto file = lifecycle_image(dir, "cleanup");
  auto* image = linker.open(file.string(), kRtldNow, nullptr);
  REQUIRE(image != nullptr);
  std::promise<GuestThread*> snapshot;
  auto held = snapshot.get_future();
  std::promise<void> drop;
  auto release = drop.get_future();
  std::promise<void> completed;
  auto done = completed.get_future();
  std::thread worker([&] {
    GuestThread thread(7195); thread.attach_to_current_host_thread();
    linker.for_each_image([&](const Library& entry) {
      if (&entry == image) { snapshot.set_value(&thread); release.wait(); }
    }); // Drops the last image snapshot after dlclose below.
    completed.set_value();
  });
  auto* thread = held.get();
  CHECK(linker.close(image) == 0);
  std::unique_lock lock(linker_lifecycle_mutex());
  thread->exit_requested = true; thread->interrupt();
  drop.set_value();
  const bool destroyed = done.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  CHECK(destroyed); // Owner still holds metadata while joining the exiting worker.
  lock.unlock(); worker.join();
  CHECK(std::filesystem::remove(file));
}
