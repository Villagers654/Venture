#include "common/environment.h"
#include <doctest/doctest.h>
#include "common/paths.h"
#include "core/address_space.h"
#include "core/elf.h"
#include "core/linker_runtime.h"
#include "core/process_runtime.h"
#include "core/hle_runtime.h"
#include "cpu/stubs.h"
#include "core/thread_runtime.h"
#include "core/tls.h"
#include "platform/memory.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <vector>
#include <memory>
#include <miniz.h>
#include "common/file_ops.h"
#include "common/saves_migration.h"
#include "common/library/library.h"
#ifdef _WIN32
#include "common/platform/win32/native_path.h"
#endif
#include "hle/libc/vfs.h"
#include "hle/registry.h"
#include "hle/vrapi/vrapi.h"
using namespace venture;
#ifdef _WIN32
#endif

TEST_CASE("core relocation decoders APS2 groups and RELR bitmaps") {
  const std::array<unsigned char, 15> packed{'A','P','S','2', 2, 0, 2, 15, 8, 0x83, 8, 0x10};
  std::vector<elf::Rela> relocations;
  elf::decode_aps2({reinterpret_cast<const std::byte*>(packed.data()), 12}, [&](auto r) { relocations.push_back(r); });
  REQUIRE(relocations.size() == 2);
  CHECK(relocations[0].offset == 8);
  CHECK(relocations[1].offset == 16);
  CHECK(relocations[0].info == 1027);
  CHECK(relocations[1].addend == 16);
  std::array<std::uint64_t, 3> words{0x1000, 1 | (1ull << 1) | (1ull << 3), 0x3000};
  std::vector<std::uint64_t> offsets;
  elf::decode_relr(words, [&](auto p) { offsets.push_back(p); });
  CHECK(offsets == std::vector<std::uint64_t>{0x1000, 0x1008, 0x1018, 0x3000});
  CHECK_THROWS(elf::decode_aps2({}, [](auto) {}));
  const std::array<std::uint64_t, 1> bad{3};
  CHECK_THROWS(elf::decode_relr(bad, [](auto) {}));
  // ELF32 RELR: 4-byte words, 31 bitmap slots of 4 bytes each.
  std::array<std::uint32_t, 3> words32{0x1000, 1u | (1u << 1) | (1u << 31), 0x3000};
  offsets.clear();
  elf::decode_relr32(words32, [&](auto p) { offsets.push_back(p); });
  CHECK(offsets == std::vector<std::uint64_t>{0x1000, 0x1004, 0x1004 + 30 * 4, 0x3000});
  const std::array<std::uint32_t, 1> bad32{3};
  CHECK_THROWS(elf::decode_relr32(bad32, [](auto) {}));
}
// A one-page shared object exporting `symbol` and binding "bound" to it.
static std::filesystem::path scope_fixture(const std::filesystem::path& directory, std::string_view filename,
                                           std::string_view symbol, bool global_group) {
  std::array<std::byte, 4096> file{};
  auto put = [&]<class T>(std::size_t offset, const T& value) {
    std::memcpy(file.data() + offset, &value, sizeof(value));
  };
  elf::Header header{};
  std::memcpy(header.ident, "\177ELF\2\1\1", 7);
  header.type = 3; header.machine = 183; header.version = 1; header.ehsize = sizeof(header);
  header.phoff = 64; header.phentsize = sizeof(elf::Phdr); header.phnum = 2;
  put(0, header); put(64, elf::Phdr{1, 6, 0, 0, 0, 4096, 4096, 4096});
  std::string strings(1, '\0');
  auto shared = std::uint32_t(strings.size()); strings += symbol; strings += '\0';
  auto bound = std::uint32_t(strings.size()); strings += "bound"; strings += '\0';
  std::memcpy(file.data() + 0x400, strings.data(), strings.size());
  put(0x500, std::array<elf::Sym, 3>{{{}, {shared, 0x11, 0, 1, 0x900, 8}, {bound, 0x11, 0, 1, 0x908, 8}}});
  put(0x700, std::array<std::uint32_t, 6>{1, 3, 1, 0, 2, 0});
  put(0x800, elf::Rela{0x908, (std::uint64_t(1) << 32) | 257, 0});
  std::array<elf::Dyn, 10> dynamic{{{5, 0x400}, {10, strings.size()}, {6, 0x500},
    {11, sizeof(elf::Sym)}, {4, 0x700}, {7, 0x800}, {8, sizeof(elf::Rela)},
    {9, sizeof(elf::Rela)}, {0x6ffffffb, global_group ? 2u : 0u}, {0, 0}}};
  put(120, elf::Phdr{2, 6, 0x200, 0x200, 0, sizeof(dynamic), sizeof(dynamic), 8});
  put(0x200, dynamic);
  auto path = directory / filename;
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(file.data()), file.size());
  return path;
}
TEST_CASE("core ELF relocation global group differs from RTLD_DEFAULT scope") {
  auto directory = std::filesystem::current_path() / "build/core-fixtures";
  std::filesystem::create_directories(directory);
  auto fixture = [&](std::string_view filename, std::string_view symbol, bool global_group) {
    return scope_fixture(directory, filename, symbol, global_group);
  };
  struct RestoreInspection { ~RestoreInspection() { linker_set_inspection(false); } } restore;
  linker_set_inspection(true);
  auto& linker = Linker::get();
  for (bool global_group : {false, true}) {
    const auto suffix = global_group ? "flag" : "dlopen";
    const auto symbol = std::string("scope_symbol_") + suffix;
    auto* provider = linker.open(fixture(std::string("libscope-provider-") + suffix + ".so", symbol, global_group).string(),
                                 kRtldNow | kRtldGlobal, nullptr);
    INFO(linker.take_error()); REQUIRE(provider);
    auto* consumer = linker.open(fixture(std::string("libscope-consumer-") + suffix + ".so", symbol, false).string(),
                                 kRtldNow | kRtldLocal, nullptr);
    INFO(linker.take_error()); REQUIRE(consumer);
    const auto own = consumer->find_symbol(symbol), shared = provider->find_symbol(symbol);
    REQUIRE(own != 0); REQUIRE(shared != 0); CHECK(own != shared);
    CHECK(*g2h<GuestAddr>(consumer->find_symbol("bound")) == (global_group ? shared : own));
    CHECK(linker.symbol_default(symbol, consumer) == shared);
    CHECK(linker.close(consumer) == 0); CHECK(linker.close(provider) == 0);
  }
}
TEST_CASE("core android_dlopen_ext loads a library into the range its caller reserved") {
  auto directory = std::filesystem::current_path() / "build/core-fixtures";
  std::filesystem::create_directories(directory);
  struct RestoreInspection { ~RestoreInspection() { linker_set_inspection(false); } } restore;
  linker_set_inspection(true);
  auto& linker = Linker::get();
  // A game reserves a window first, then loads its library into it.
  constexpr std::uint64_t size = 0x100000;
  const auto window = AddressSpace::get().map(0x58000000, size, 0, kMapPrivate | kMapAnonymous | kMapFixedNoReplace,
                                               nullptr, 0, "[game window]");
  REQUIRE(window == 0x58000000);
  elf::Placement inside{GuestAddr(window), size, true};
  auto* placed = linker.open(scope_fixture(directory, "libplaced.so", "placed_symbol", false).string(), kRtldNow,
                             nullptr, &inside);
  INFO(linker.take_error()); REQUIRE(placed);
  CHECK(placed->find_symbol("placed_symbol") == GuestAddr(window) + 0x900);
  CHECK(*g2h<GuestAddr>(placed->find_symbol("bound")) == GuestAddr(window) + 0x900);
  // Too small: a required range fails, a hint loads elsewhere.
  elf::Placement small{GuestAddr(window) + size - 0x1000, 0x800, true};
  CHECK_FALSE(linker.open(scope_fixture(directory, "libsmall.so", "small_symbol", false).string(), kRtldNow, nullptr,
                          &small));
  CHECK(linker.take_error().find("too small") != std::string::npos);
  small.required = false;
  auto* hinted = linker.open(scope_fixture(directory, "libhinted.so", "hinted_symbol", false).string(), kRtldNow,
                             nullptr, &small);
  INFO(linker.take_error()); REQUIRE(hinted);
  const auto hinted_at = hinted->find_symbol("hinted_symbol");
  CHECK((hinted_at < GuestAddr(window) || hinted_at >= GuestAddr(window) + size));
  CHECK(linker.close(hinted) == 0); CHECK(linker.close(placed) == 0);
  AddressSpace::get().unmap(GuestAddr(window), size);
}
TEST_CASE("core synthetic HLE backing keeps precedence, guest execution and lifetime") {
  GuestThread thread(95); thread.attach_to_current_host_thread();
  int initialized = 0, finalized = 0, resolved = 0;
  auto provider = std::make_shared<HleLibrary>("libcore-fixture-hle.so");
  // A local root must precede its HLE dependency for an unversioned lookup.
  provider->add_function("core_fixture_entry", hle::make_thunk(provider->soname(), "shadowed entry", [](hle::CallContext& ctx) { ctx.return_x(99); }));
  provider->add_function("core_fixture_init", hle::make_thunk(provider->soname(), "init", [](hle::CallContext& ctx) {
    ++*static_cast<int*>(ctx.user());
  }, &initialized));
  provider->add_function("core_fixture_fini", hle::make_thunk(provider->soname(), "fini", [](hle::CallContext& ctx) {
    ++*static_cast<int*>(ctx.user());
  }, &finalized));
  Linker::get().register_hle_library(provider);
  auto resolver_id = hle::register_function("test", "ifunc ABI", [](hle::CallContext& ctx) {
    auto* argument = g2h<const std::uint64_t>(ctx.x(1));
    CHECK(argument[0] == 24);
    CHECK(argument[1] == cpu_backend().info().hwcap);
    CHECK(argument[2] == cpu_backend().info().hwcap2);
    CHECK(ctx.x(0) == (argument[1] | (1ull << 62)));
    ++*static_cast<int*>(ctx.user());
    // The trap resumes at the final RET in the emitted thunk. Keep this
    // relocation independent of the instrumentation-safe entry's byte size.
    const auto bytes = guest_stub(GuestIsa::a64, GuestStub::hle_call).instructions().size_bytes();
    ctx.return_x(ctx.cpu().pc() - (0x140 + bytes - 4) + 0x180);
  }, &resolved);
  std::array<std::byte, 4096> file{};
  auto put = [&]<class T>(std::size_t offset, const T& value) { std::memcpy(file.data() + offset, &value, sizeof(value)); };
  elf::Header header{};
  std::memcpy(header.ident, "\177ELF\2\1\1", 7);
  header.type = 3; header.machine = 183; header.version = 1; header.ehsize = sizeof(header);
  header.phoff = 64; header.phentsize = sizeof(elf::Phdr); header.phnum = 3;
  put(0, header);
  put(64, elf::Phdr{1, 7, 0, 0, 0, 4096, 4096, 4096});
  put(120, elf::Phdr{2, 6, 0x200, 0x200, 0, 0x100, 0x100, 8});
  put(176, elf::Phdr{7, 4, 0xa00, 0xa00, 0, 4, 32, 16});
  const auto resolver = guest_stub(GuestIsa::a64, GuestStub::hle_call, resolver_id);
  REQUIRE(resolver.instructions().size_bytes() <= 0x40);
  std::memcpy(file.data() + 0x140, resolver.words.data(), resolver.instructions().size_bytes());
  put(0x180, std::array<std::uint32_t, 2>{0xd2800540, 0xd65f03c0});
  put(0x188, std::array<std::uint32_t, 2>{0xd2800560, 0xd65f03c0});
  put(0xa00, std::uint32_t{0x12345678});
  std::string strings(1, '\0');
  auto name = [&](std::string_view value) { auto offset = static_cast<std::uint32_t>(strings.size()); strings += value; strings += '\0'; return offset; };
  auto entry = name("core_fixture_entry"), indirect = name("core_fixture_ifunc"), tls_symbol = name("core_fixture_tls");
  auto data_symbol = name("core_fixture_data");
  auto versioned_entry = name("core_fixture_versioned");
  auto init = name("core_fixture_init"), fini = name("core_fixture_fini"), weak = name("core_fixture_absent_weak");
  auto weak_tls = name("core_fixture_weak_tls");
  auto v1 = name("CORE_1"), v2 = name("CORE_2"), needed = name(provider->soname());
  std::memcpy(file.data() + 0x400, strings.data(), strings.size());
  const std::array<elf::Sym, 10> symbols{{{}, {entry, 0x12, 0, 1, 0x180, 8}, {indirect, 0x1a, 0, 1, 0x140, 16},
      {tls_symbol, 0x16, 0, 1, 0, 4}, {init, 0x12, 0, 0, 0, 0}, {fini, 0x12, 0, 0, 0, 0},
      {weak, 0x22, 0, 0, 0, 0}, {versioned_entry, 0x12, 0, 1, 0x188, 8}, {weak_tls, 0x26, 0, 0, 0, 0},
      {data_symbol, 0x11, 0, 1, 0xa04, 4}}};
  put(0x500, symbols);
  put(0x700, std::array<std::uint32_t, 13>{1, 10, 1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 0});
  put(0xb00, std::array<std::uint16_t, 10>{0, 2, 1, 1, 1, 1, 1, 0x8003, 1, 1});
  put(0xb40, elf::Verdef{1, 0, 2, 1, 0, 20, 28}); put(0xb54, elf::Verdaux{v1, 0});
  put(0xb5c, elf::Verdef{1, 0, 3, 1, 0, 20, 0}); put(0xb70, elf::Verdaux{v2, 0});
  auto ri = [](std::uint32_t sym, std::uint32_t type) { return (std::uint64_t{sym} << 32) | type; };
  const std::array<elf::Rela, 11> relas{{
      {0x900, ri(0, 1027), 0x180}, {0x908, ri(1, 257), 0}, {0x910, ri(2, 257), 0},
      {0x918, ri(0, 1032), 0x140}, {0x920, ri(6, 1026), 0}, {0x928, ri(3, 1028), 0},
      {0x930, ri(3, 1029), 0}, {0x938, ri(3, 1030), 0}, {0x940, ri(3, 1031), 0},
      {0x980, ri(4, 257), 0}, {0x950, ri(8, 1031), 0}}};
  put(0xd00, relas);
  // Separate PLT RELA table also resolves a fini-array entry.
  put(0xc00, elf::Rela{0x988, ri(5, 1026), 0});
  const std::array<elf::Dyn, 23> dynamic{{
      {1, needed}, {5, 0x400}, {10, strings.size()}, {6, 0x500}, {11, sizeof(elf::Sym)}, {4, 0x700},
      {7, 0xd00}, {8, sizeof(relas)}, {9, sizeof(elf::Rela)}, {23, 0xc00}, {2, sizeof(elf::Rela)}, {20, 7},
      {25, 0x980}, {27, 8}, {26, 0x988}, {28, 8}, {0x6ffffff0, 0xb00}, {0x6ffffffc, 0xb40},
      {0x6ffffffd, 2}, {0, 0}}};
  // 23 dynamic entries occupy 0x170 bytes and fit before the string table.
  put(120, elf::Phdr{2, 6, 0x200, 0x200, 0, sizeof(dynamic), sizeof(dynamic), 8});
  put(0x200, dynamic);
  auto directory = std::filesystem::current_path() / "build/core-fixtures";
  std::filesystem::create_directories(directory);
  auto path = directory / "libcore-elf-fixture.so";
  { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(file.data()), file.size()); }
  auto slot = std::make_shared<HleLibrary>("libcore-elf-fixture.so");
  auto override = hle::make_thunk(slot->soname(), "entry override", [](hle::CallContext& ctx) { ctx.return_x(88); });
  slot->add_function("core_fixture_entry", override);
  std::uint32_t override_data = 0x76543210;
  slot->add_data("core_fixture_data", h2g(&override_data));
  Linker::get().register_hle_library(slot);
  CHECK(slot->find_symbol("core_fixture_ifunc") == 0); // Attaching is lazy.
  linker_set_inspection(false);
  auto* lib = Linker::get().open(path.string(), kRtldNow, nullptr);
  INFO(Linker::get().take_error()); REQUIRE(lib != nullptr);
  REQUIRE(lib == slot.get());
  CHECK(lib->map_size == 0);
  CHECK(initialized == 0);
  CHECK(resolved == 0);
  CHECK(Linker::get().symbol(lib, "core_fixture_data") == h2g(&override_data));
  CHECK(lib->map_size == 0);
  CHECK(Linker::get().symbol(lib, "core_fixture_absent") == 0);
  CHECK_FALSE(Linker::get().take_error().empty());
  CHECK(lib->map_size == 0);
  REQUIRE(Linker::get().symbol(lib, "core_fixture_ifunc") != 0);
  REQUIRE(lib->map_size != 0);
  CHECK(lib->find_symbol("core_fixture_data") == h2g(&override_data));
  CHECK(Linker::get().symbol(lib, "core_fixture_data") == h2g(&override_data));
  DlInfo mapped_info;
  CHECK(Linker::get().address_info(lib->load_bias + 0x180, &mapped_info));
  CHECK(mapped_info.symbol == "core_fixture_entry"); CHECK(mapped_info.symbol_address == lib->load_bias + 0x180);
  // dladdr's address index answers like a table scan: the highest defined
  // non-TLS symbol at or below the address, the later entry on a tie.
  const auto* backing = lib->mapped_image();
  for (GuestAddr p = lib->map_start; p < lib->map_start + lib->map_size; p += 4) {
    GuestAddr want_address = 0; std::string want_name;
    for (const auto& sym : backing->symbols()) {
      if (!sym.shndx || !sym.name || (sym.info & 15) == 6) continue;
      const auto addr = sym.shndx == 0xfff1 ? sym.value : backing->load_bias + sym.value;
      if (addr <= p && addr >= want_address) { want_address = addr; want_name = backing->symbol_name(sym); }
    }
    DlInfo info;
    REQUIRE(Linker::get().address_info(p, &info));
    CHECK(info.symbol == want_name); CHECK(info.symbol_address == want_address);
  }
  bool in_phdr_list = false;
  Linker::get().for_each_image([&](const Library& image) { in_phdr_list |= image.map_start == lib->map_start; });
  CHECK(in_phdr_list);
  CHECK(initialized == 1);
  CHECK(resolved == 1); // IRELATIVE and GNU_IFUNC share the same cached resolver.
  CHECK(thread.call(Linker::get().symbol(lib, "core_fixture_ifunc"), {}) == 42);
  CHECK(thread.call(lib->find_symbol("core_fixture_entry"), {}) == 88);
  CHECK(thread.call(lib->find_symbol("core_fixture_entry", "CORE_2"), {}) == 88);
  CHECK(lib->find_symbol("core_fixture_versioned") == 0); // Non-default version.
  CHECK(lib->find_symbol("core_fixture_versioned", "CORE_1") == 0);
  CHECK(thread.call(lib->find_symbol("core_fixture_versioned", "CORE_2"), {}) == 43);
  CHECK(thread.call(Linker::get().symbol_default("core_fixture_entry", lib), {}) == 88);
  CHECK(thread.call(Linker::get().symbol_next("core_fixture_entry", lib), {}) == 99);
  auto* main = Linker::get().open_main(kRtldNow);
  REQUIRE(main != nullptr);
  CHECK(Linker::get().symbol(main, "core_fixture_ifunc") == 0); // RTLD_LOCAL is private.
  CHECK_FALSE(Linker::get().take_error().empty());
  REQUIRE(Linker::get().open(path.string(), kRtldNow | kRtldGlobal, nullptr) == lib);
  CHECK(thread.call(Linker::get().symbol(main, "core_fixture_ifunc"), {}) == 42);
  CHECK(Linker::get().symbol(main, "core_fixture_tls") == lib->find_symbol("core_fixture_tls"));
  CHECK(Linker::get().close(main) == 0);
  CHECK(Linker::get().close(lib) == 0); // Retain the original dlopen reference.
  CHECK(finalized == 0);
  auto base = lib->load_bias;
  CHECK(*g2h<GuestAddr>(base + 0x900) == base + 0x180);
  CHECK(*g2h<GuestAddr>(base + 0x908) == override);
  CHECK(*g2h<GuestAddr>(base + 0x910) == base + 0x180);
  CHECK(*g2h<GuestAddr>(base + 0x918) == base + 0x180);
  CHECK(*g2h<GuestAddr>(base + 0x920) == 0);
  CHECK(*g2h<GuestAddr>(base + 0x928) == lib->tls_module_id);
  CHECK(*g2h<GuestAddr>(base + 0x930) == 0);
  CHECK(*g2h<GuestAddr>(base + 0x938) == static_cast<GuestAddr>(lib->tls_offset));
  auto* descriptor = g2h<const GuestAddr>(base + 0x940);
  CHECK(thread.call(descriptor[0], {base + 0x940}) == static_cast<GuestAddr>(lib->tls_offset));
  auto* weak_descriptor = g2h<const GuestAddr>(base + 0x950);
  CHECK(static_cast<GuestAddr>(thread.call(weak_descriptor[0], {base + 0x950})) + thread.thread_pointer == 0);
  CHECK(*g2h<std::uint32_t>(lib->find_symbol("core_fixture_tls")) == 0x12345678);
  CHECK(Linker::get().close(lib) == 0);
  CHECK(finalized == 1);
  CHECK(slot->map_size == 0);
  CHECK(Linker::get().symbol(main, "core_fixture_ifunc") == 0);
  CHECK_FALSE(Linker::get().take_error().empty());
  CHECK(Linker::get().symbol(lib, "core_fixture_entry") == override);
  CHECK(slot->find_symbol("core_fixture_ifunc") == 0);
  CHECK(Linker::get().take_error().empty());
  REQUIRE(Linker::get().open(path.string(), kRtldNow, nullptr) == slot.get());
  CHECK(initialized == 1);
  CHECK(thread.call(Linker::get().symbol(slot.get(), "core_fixture_ifunc"), {}) == 42);
  CHECK(initialized == 2);
  CHECK(Linker::get().close(slot.get()) == 0);
  CHECK(finalized == 2);
  slot->add_function("core_fixture_ifunc", override);
  auto global_dynamic = dynamic;
  global_dynamic[19] = {0x6ffffffb, 2}; // DF_1_GLOBAL must promote the stable slot.
  put(0x200, global_dynamic);
  { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(file.data()), file.size()); }
  REQUIRE(Linker::get().open(path.string(), kRtldNow | kRtldGlobal, nullptr) == slot.get());
  CHECK(Linker::get().symbol_default("core_fixture_ifunc", nullptr) == override);
  CHECK(Linker::get().symbol_default("core_fixture_data", nullptr) == h2g(&override_data));
  CHECK(slot->map_size == 0);
  REQUIRE(slot->find_symbol("core_fixture_versioned", "CORE_2") != 0);
  CHECK(*g2h<GuestAddr>(slot->load_bias + 0x908) == override);
  CHECK(Linker::get().close(slot.get()) == 0);
  CHECK(finalized == 3);
  CHECK(Linker::get().symbol_default("core_fixture_entry", nullptr) == 0);
  CHECK_FALSE(Linker::get().take_error().empty());
  CHECK(Linker::get().symbol_default("core_fixture_ifunc", nullptr) == 0);
  CHECK_FALSE(Linker::get().take_error().empty());
  // A dependency failure after attachment rolls back the backing, not the API slot.
  const char absent[] = "libcore-missing-hle.so";
  static_assert(sizeof(absent) == sizeof("libcore-fixture-hle.so"));
  std::memcpy(file.data() + 0x400 + needed, absent, sizeof(absent));
  { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(file.data()), file.size()); }
  REQUIRE(Linker::get().open(path.string(), kRtldNow, nullptr) == slot.get());
  CHECK(slot->map_size == 0);
  CHECK(slot->find_symbol("core_fixture_versioned", "CORE_2") == 0);
  CHECK(Linker::get().take_error().find("library not found") != std::string::npos);
  CHECK(Linker::get().close(slot.get()) == 0);
  CHECK(slot->map_size == 0);
  CHECK(slot->find_symbol("core_fixture_entry") == override);
  CHECK(slot->find_symbol("core_fixture_ifunc") == override);
  std::filesystem::remove(path);
}

TEST_CASE("core APK extraction supports long native cache and source paths") {
  const auto root = fs::temp_directory_path() / ("venture-apk-path-" + random_account_key());
  struct Restore {
    ProcessConfig saved = process_config(); fs::path root;
    ~Restore() { process_config() = std::move(saved); remove_tree(root.parent_path(), root); }
  } restore{process_config(), root};
  const auto deep = root / std::string(100, 'a') / std::string(100, 'b') / std::string(80, 'c');
  fs::create_directories(paths::io_path(deep));
  for (const bool arm32 : {false, true}) for (const bool long_source : {false, true}) {
    auto& config = process_config(); config = {};
    config.abi = arm32 ? "armeabi-v7a" : "arm64-v8a";
    config.apk = (long_source ? deep : root) / (arm32 ? "arm32.apk" : "arm64.apk");
    config.native_lib_dir = deep / (arm32 ? "native32" : "native64");
    fs::create_directories(paths::io_path(config.native_lib_dir));
    REQUIRE((config.apk.native().size() > 260) == long_source);
    std::array<std::byte, 20> header{};
    std::memcpy(header.data(), "\177ELF", 4);
    header[4] = arm32 ? std::byte{1} : std::byte{2};
    header[5] = std::byte{1}; header[6] = std::byte{1}; header[18] = arm32 ? std::byte{40} : std::byte{183};
    const auto member = "lib/" + config.abi + "/libfixture.so";
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    REQUIRE(mz_zip_writer_add_mem(&zip, member.c_str(), header.data(), header.size(), MZ_BEST_COMPRESSION));
    // A non-ELF member (a loader's config) is copied as data, as Android does.
    const auto config_member = "lib/" + config.abi + "/libfixture.config.so";
    REQUIRE(mz_zip_writer_add_mem(&zip, config_member.c_str(), "{}", 2, MZ_BEST_COMPRESSION));
    void* archive = nullptr; std::size_t bytes = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &archive, &bytes));
    std::unique_ptr<void, decltype(&mz_free)> storage(archive, mz_free);
    mz_zip_writer_end(&zip);
    paths::atomic_write(paths::io_path(config.apk), std::string_view(static_cast<const char*>(archive), bytes));
    auto all = extract_native_libraries(); REQUIRE(all.size() == 2);
    CHECK(paths::read(all[0]) == "{}");
    std::vector files{all[1]};
    CHECK(paths::read(files[0]) == std::string(reinterpret_cast<const char*>(header.data()), header.size()));
    // A valid-size but corrupted cached file must still be repaired at length.
    auto corrupted = header; corrupted[10] = std::byte{42};
    paths::atomic_write(files[0], std::string_view(reinterpret_cast<const char*>(corrupted.data()), corrupted.size()));
    CHECK(extract_native_libraries() == all);
    CHECK(paths::read(files[0]) == std::string(reinterpret_cast<const char*>(header.data()), header.size()));
    CHECK(extract_native_libraries() == all);
  }
}
