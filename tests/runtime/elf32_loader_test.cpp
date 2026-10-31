#include <doctest/doctest.h>
#include "common/paths.h"
#include "core/elf.h"
#include "core/a32_coverage.h"
#include "core/linker_runtime.h"
#include "core/process_runtime.h"
#include "core/address_space.h"
#include "java/elf_exports.h"
#include "hle/registry.h"
#include "platform/memory.h"
#include <miniz.h>
#include <array>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

using namespace venture;
namespace {
#ifdef _WIN32
// This constructor must observe the reservation already made by the PE entry,
// before doctest registration (the startup allocation race that failed under -j8).
const bool reserved_before_constructors = [] {
  constexpr GuestAddr p = 0x40000000;
  auto out = platform::reserve(p, 65536, true);
  if (out) platform::release(out, 65536);
  return out == 0;
}();
#endif
struct Fixture {
  std::array<std::byte, 4096> data{};
  std::string strings = std::string(1, '\0');
  std::vector<elf::Dyn32> dynamic;
  std::filesystem::path path;
  template<class T> void put(std::size_t offset, const T& value) { std::memcpy(data.data() + offset, &value, sizeof(value)); }
  std::uint32_t name(std::string_view text) {
    auto offset = static_cast<std::uint32_t>(strings.size()); strings += text; strings += '\0'; return offset;
  }
  explicit Fixture(std::string_view filename, bool gnu = false, bool external_phdr = false) {
    elf::Header32 h{};
    std::memcpy(h.ident, "\177ELF\1\1\1", 7);
    h.type = 3; h.machine = 40; h.version = 1; h.ehsize = sizeof(h);
    h.phoff = external_phdr ? 0xe00 : 52; h.phentsize = sizeof(elf::Phdr32); h.phnum = 3;
    put(0, h);
    put(h.phoff, elf::Phdr32{1, 0, 0, 0, external_phdr ? 0xd00u : 4096u, 4096, 7, 4096});
    put(h.phoff + 32, elf::Phdr32{2, 0x200, 0x200, 0, 0x200, 0x200, 6, 4});
    put(h.phoff + 64, elf::Phdr32{0x70000001, 0xa00, 0xa00, 0, 16, 16, 4, 4});
    put(0xa00, std::array<std::uint32_t, 4>{0x7fffff00, 1, 0x7fffff08, 1});
    auto thumb = name("Java_fixture_thumb"), weak = name("fixture_weak"), object = name("fixture_object");
    auto v1 = name("ARM_1"), v2 = name("ARM_2"), lib = name("liba32-fixture-hle.so"), imported = name("fixture_import");
    put(0x500, std::array<elf::Sym32, 6>{{{}, {thumb, 0x181, 8, 0x12, 0, 1},
        {weak, 0, 0, 0x22, 0, 0}, {object, 0, 4, 0x11, 0, 0}, {thumb, 0x191, 8, 0x12, 0, 1},
        {imported, 0, 0, 0x12, 0, 0}}});
    if (gnu) {
      // A single ELF32 bloom word; an 8-byte reader picks up a chain as a bucket.
      put(0x700, std::array<std::uint32_t, 11>{1, 1, 1, 5, 0xffffffff, 1, 2, 2, 2, 2, 3});
    } else put(0x700, std::array<std::uint32_t, 9>{1, 6, 1, 0, 2, 3, 4, 5, 0});
    put(0xb00, std::array<std::uint16_t, 6>{0, 2, 1, 1, 0x8003, 4});
    put(0xb40, elf::Verdef{1, 0, 2, 1, 0, 20, 28}); put(0xb54, elf::Verdaux{v1, 0});
    put(0xb5c, elf::Verdef{1, 0, 3, 1, 0, 20, 0}); put(0xb70, elf::Verdaux{v2, 0});
    put(0xb80, elf::Verneed{1, 1, lib, 16, 0}); put(0xb90, elf::Vernaux{0, 0, 4, v1, 0});
    const std::array<elf::Rel32, 6> rels{{{0x900, 23}, {0x908, (1 << 8) | 2},
        {0x910, (1 << 8) | 21}, {0x918, (2 << 8) | 21}, {0x920, (3 << 8) | 21}, {0x928, (5 << 8) | 21}}};
    put(0xc00, rels); put(0xc40, elf::Rel32{0x930, (1 << 8) | 22});
    for (std::size_t off = 0x900; off <= 0x930; off += 8) put(off, std::array<std::uint32_t, 2>{0x44, 0xfeedface});
    put(0x900, std::uint32_t{0x180});
    dynamic = {{1, lib}, {5, 0x400}, {6, 0x500}, {11, sizeof(elf::Sym32)}, {gnu ? 0x6ffffef5 : 4, 0x700},
        {17, 0xc00}, {18, sizeof(rels)}, {19, sizeof(elf::Rel32)}, {23, 0xc40}, {2, sizeof(elf::Rel32)}, {20, 17},
        {16, 0}, {25, 0x900}, {27, 4}, {26, 0x908}, {28, 4}, {0x6ffffff0, 0xb00}, {0x6ffffffc, 0xb40},
        {0x6ffffffd, 2}, {0x6ffffffe, 0xb80}, {0x6fffffff, 1}};
    path = std::filesystem::temp_directory_path() / std::string(filename);
  }
  void write() {
    std::memcpy(data.data() + 0x400, strings.data(), strings.size());
    auto ds = dynamic; ds.push_back({10, static_cast<std::uint32_t>(strings.size())}); ds.push_back({0, 0});
    std::memcpy(data.data() + 0x200, ds.data(), ds.size() * sizeof(ds[0]));
    std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(data.data()), data.size());
  }
  ~Fixture() { std::filesystem::remove(path); }
};
struct Inspection {
  ProcessConfig previous = process_config();
  // This binary is A32-only: the real low arena must exist before any guest allocation.
  Inspection() {
    select_process_isa(GuestIsa::a32);
    AddressSpace::get().initialize(GuestIsa::a32);
    linker_set_inspection(true);
  }
  ~Inspection() { linker_set_import_coverage_hook({}); linker_set_inspection(false); process_config() = previous; }
};
}
#ifdef _WIN32
TEST_CASE("A32 low reservation precedes C++ constructors") {
  CHECK(reserved_before_constructors);
}
#endif
TEST_CASE("ELF32 ARM relocation addends, four-byte writes, versions and exidx") {
  Inspection inspect;
  auto provider = std::make_shared<HleLibrary>("liba32-fixture-hle.so");
  // Invalid OBJECT provider must not be used as data, even though it has a low address.
  provider->add_function("fixture_object", 0x12340);
  provider->add_function("fixture_import", 0x12380);
  Linker::get().register_hle_library(provider);
  CHECK(provider->path() == "/system/lib/liba32-fixture-hle.so");
  for (bool gnu : {false, true}) for (bool copied : {false, true}) {
    Fixture f(gnu ? "liba32-gnu.so" : "liba32-sysv.so", gnu, copied); f.write();
    std::size_t coverage = 0;
    linker_set_import_coverage_hook([&](const elf::Image&, const elf::Import&) { ++coverage; return "fixture"; });
    auto* lib = Linker::get().open(f.path.string(), kRtldNow, nullptr);
    INFO(Linker::get().take_error()); REQUIRE(lib != nullptr);
    auto* image = dynamic_cast<elf::Image*>(lib); REQUIRE(image != nullptr);
    CHECK(image->isa() == GuestIsa::a32); CHECK(image->symbolic);
    CHECK(image->find_symbol("Java_fixture_thumb") == image->load_bias + 0x181);
    CHECK(image->find_symbol("Java_fixture_thumb", "ARM_2") == image->load_bias + 0x191);
    CHECK(image->symbol_version(5).provider == provider->soname());
    CHECK(image->symbol_version(5).name == "ARM_1");
    CHECK(*g2h<std::uint32_t>(image->load_bias + 0x900) == image->load_bias + 0x180);
    CHECK(*g2h<std::uint32_t>(image->load_bias + 0x908) == image->load_bias + 0x181 + 0x44);
    CHECK(*g2h<std::uint32_t>(image->load_bias + 0x910) == image->load_bias + 0x181);
    CHECK(*g2h<std::uint32_t>(image->load_bias + 0x918) == 0); // weak
    CHECK(*g2h<std::uint32_t>(image->load_bias + 0x920) == 0); // missing object
    CHECK(*g2h<std::uint32_t>(image->load_bias + 0x928) == 0x12380);
    CHECK(*g2h<std::uint32_t>(image->load_bias + 0x930) == image->load_bias + 0x181);
    for (auto off = 0x904; off <= 0x934; off += 8) CHECK(*g2h<std::uint32_t>(image->load_bias + off) == 0xfeedface);
    CHECK(image->report.relocations == std::map<std::string, std::uint64_t>{{"RELATIVE", 1}, {"ABS32", 1}, {"GLOB_DAT", 4}, {"JUMP_SLOT", 1}});
    CHECK(coverage == 3); CHECK(image->report.imports.size() == 3);
    CHECK(image->report.unresolved.size() == 2);
    CHECK(image->report.unresolved[0].weak); CHECK(image->report.unresolved[0].address == 0);
    std::int32_t count = -1;
    CHECK(Linker::get().find_exidx(image->load_bias + 0x181, &count) == image->load_bias + 0xa00);
    CHECK(count == 2);
    CHECK(Linker::get().find_exidx(image->map_start + image->map_size, &count) == 0); CHECK(count == 0);
    CHECK(g2h<elf::Phdr32>(lib->phdr)[1].type == 2); CHECK(g2h<elf::Phdr32>(lib->phdr)[2].type == 0x70000001);
    if (copied) CHECK((lib->phdr < lib->map_start || lib->phdr >= lib->map_start + lib->map_size));
    DlInfo info; REQUIRE(Linker::get().address_info(image->load_bias + 0x184, &info));
    CHECK(info.symbol == "Java_fixture_thumb"); CHECK(info.symbol_address == image->load_bias + 0x181);
    CHECK(image->dynamic(27) == 4); CHECK(image->dynamic(28) == 4);
    std::size_t exported = 0;
    java::exported_java_symbols(*lib, [&](auto name, auto address) { CHECK(name == "Java_fixture_thumb"); CHECK(address == lib->load_bias + 0x181); ++exported; });
    CHECK(exported == 2); // hidden version is enumerated, binding still uses default lookup
    auto pc = image->load_bias + 0x181;
    CHECK(Linker::get().close(lib) == 0); CHECK(Linker::get().find_exidx(pc, &count) == 0); CHECK(count == 0);
  }
}
TEST_CASE("ELF32 rejects unsupported dynamic relocations and mixed ISA") {
  Inspection inspect;
  Linker::get().register_hle_library(std::make_shared<HleLibrary>("liba32-fixture-hle.so"));
  for (auto type : {3u, 42u, 17u, 160u}) {
    Fixture f("liba32-rejected.so"); f.put(0xc00, elf::Rel32{0x900, type}); f.write();
    CHECK(Linker::get().open(f.path.string(), kRtldNow, nullptr) == nullptr);
    CHECK(Linker::get().take_error().find(elf::relocation_name32(type)) != std::string::npos);
  }
  Fixture f("liba32-mixed.so"); f.write();
  select_process_isa(GuestIsa::a64);
  CHECK(Linker::get().open(f.path.string(), kRtldNow, nullptr) == nullptr);
  CHECK(Linker::get().take_error().find("ISA") != std::string::npos);
  CHECK(elf::detect_isa(f.path) == GuestIsa::a32);
  f.data[4] = std::byte{2};
  CHECK_THROWS(elf::detect_isa(std::span(f.data).first(20)));
}
TEST_CASE("ELF32 HLE backing preserves Thumb exports, precedence and unload") {
  Inspection inspect;
  auto provider = std::make_shared<HleLibrary>("liba32-fixture-hle.so");
  provider->add_function("fixture_import", 0x12380);
  provider->add_data("fixture_object", 0x12400);
  Linker::get().register_hle_library(provider);
  Fixture f("liba32-backed-slot.so");
  const auto guest_only = f.name("guest_only");
  elf::Sym32 data_export{};
  std::memcpy(&data_export, f.data.data() + 0x500 + 3 * sizeof(elf::Sym32), sizeof(data_export));
  data_export.value = 0x1b0; data_export.shndx = 1;
  f.put(0x500 + 3 * sizeof(elf::Sym32), data_export);
  f.put(0x500 + 2 * sizeof(elf::Sym32), elf::Sym32{guest_only, 0x1a1, 4, 0x12, 0, 1});
  f.put(0x500 + 4 * sizeof(elf::Sym32), elf::Sym32{f.name("guest_versioned"), 0x191, 8, 0x12, 0, 1});
  f.put(0x1a0, std::array<std::uint16_t, 2>{0x202a, 0x4770});
  f.write();
  auto slot = std::make_shared<HleLibrary>("liba32-backed-slot.so");
  slot->add_function("Java_fixture_thumb", 0x123a1);
  slot->add_data("fixture_object", 0x12500);
  Linker::get().register_hle_library(slot);
  CHECK(slot->find_symbol("guest_only") == 0);
  auto* lib = Linker::get().open(f.path.string(), kRtldNow, nullptr);
  REQUIRE(lib == slot.get());
  CHECK(lib->map_size == 0);
  CHECK(lib->find_symbol("Java_fixture_thumb") == 0x123a1);
  CHECK(lib->map_size == 0);
  CHECK(Linker::get().symbol(lib, "absent_guest_export") == 0);
  CHECK_FALSE(Linker::get().take_error().empty());
  CHECK(lib->map_size == 0);
  REQUIRE(Linker::get().symbol(lib, "guest_only") != 0);
  REQUIRE(lib->map_size != 0);
  CHECK(lib->isa() == GuestIsa::a32);
  CHECK(lib->find_symbol("Java_fixture_thumb", "ARM_2") == 0x123a1);
  CHECK(lib->find_symbol("guest_only") == lib->load_bias + 0x1a1);
  CHECK(lib->find_symbol("guest_versioned") == 0);
  CHECK(lib->find_symbol("guest_versioned", "ARM_1") == 0);
  CHECK(lib->find_symbol("guest_versioned", "ARM_2") == lib->load_bias + 0x191);
  CHECK(*g2h<std::uint32_t>(lib->load_bias + 0x910) == 0x123a1);
  CHECK(*g2h<std::uint32_t>(lib->load_bias + 0x918) == lib->load_bias + 0x1a1);
  CHECK(*g2h<std::uint32_t>(lib->load_bias + 0x920) == 0x12500);
  std::int32_t count{};
  const auto pc = lib->load_bias + 0x1a1;
  CHECK(Linker::get().library_at(pc) == slot.get());
  CHECK(Linker::get().find_exidx(pc, &count) == lib->load_bias + 0xa00);
  CHECK(count == 2);
  DlInfo info;
  REQUIRE(Linker::get().address_info(pc, &info));
  CHECK(info.symbol == "guest_only");
  CHECK(info.path.ends_with("/liba32-backed-slot.so"));
  CHECK(Linker::get().close(lib) == 0);
  CHECK(slot->map_size == 0);
  CHECK(slot->find_symbol("guest_only") == 0);
  CHECK(slot->find_symbol("Java_fixture_thumb") == 0x123a1);
  CHECK(Linker::get().find_exidx(pc, &count) == 0);
  CHECK(Linker::get().library_at(pc) == nullptr);
  REQUIRE(Linker::get().open(f.path.string(), kRtldNow, nullptr) == slot.get());
  CHECK(slot->map_size == 0);
  REQUIRE(Linker::get().symbol(slot.get(), "guest_only") != 0);
  CHECK(slot->map_size != 0);
  CHECK(Linker::get().close(slot.get()) == 0);
  f.dynamic.push_back({0x6ffffffb, 2}); f.write();
  REQUIRE(Linker::get().open(f.path.string(), kRtldNow | kRtldGlobal, nullptr) == slot.get());
  CHECK(Linker::get().symbol_default("Java_fixture_thumb", nullptr) == 0x123a1);
  CHECK(Linker::get().symbol_default("fixture_object", nullptr) == 0x12500);
  CHECK(slot->map_size == 0);
  REQUIRE(Linker::get().symbol(slot.get(), "guest_only") != 0);
  CHECK(*g2h<std::uint32_t>(slot->load_bias + 0x910) == 0x123a1);
  CHECK(Linker::get().close(slot.get()) == 0);
  CHECK(Linker::get().symbol_default("guest_only", nullptr) == 0);
  CHECK_FALSE(Linker::get().take_error().empty());
  // A real relocation miss in the consumer activates only its DT_NEEDED slot.
  Fixture consumer("liba32-slot-consumer.so");
  consumer.dynamic[0].value = consumer.name(slot->soname());
  consumer.put(0x500 + 2 * sizeof(elf::Sym32), elf::Sym32{consumer.name("guest_only"), 0, 0, 0x12, 0, 0});
  consumer.write();
  CHECK(slot->map_size == 0);
  auto* dependent = Linker::get().open(consumer.path.string(), kRtldNow, nullptr);
  INFO(Linker::get().take_error()); REQUIRE(dependent);
  REQUIRE(slot->map_size != 0);
  CHECK(*g2h<std::uint32_t>(dependent->load_bias + 0x918) == slot->load_bias + 0x1a1);
  // The consumer, rather than a synthetic reference, keeps the backing alive.
  CHECK(slot->references == 0);
  CHECK(Linker::get().close(dependent) == 0);
  CHECK(slot->map_size == 0);
  f.dynamic[0].value = f.name("liba32-slot-missing.so"); f.write();
  REQUIRE(Linker::get().open(f.path.string(), kRtldNow, nullptr) == slot.get());
  CHECK(slot->find_symbol("guest_only") == 0);
  CHECK(Linker::get().take_error().find("library not found") != std::string::npos);
  CHECK(Linker::get().close(slot.get()) == 0);
  CHECK(slot->map_size == 0);
  CHECK(slot->find_symbol("guest_only") == 0);
  CHECK(slot->find_symbol("fixture_object") == 0x12500);
}
TEST_CASE("APK slice selection prefers arm64 and confirms extracted ELF e_machine") {
  const auto previous = process_config();
  struct Restore { ProcessConfig c; ~Restore() { process_config() = c; } } restore{previous};
  // Unique per run, and removed whole: extraction may leave files beside the APK.
  auto install = std::filesystem::temp_directory_path() /
                 ("venture-a32-loader-slice-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::remove_all(install);
  std::filesystem::create_directories(install);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{install};
  auto apk = install / "slice.apk";
  elf::Header a64{}; std::memcpy(a64.ident, "\177ELF\2\1\1", 7); a64.machine = 183;
  elf::Header32 a32{}; std::memcpy(a32.ident, "\177ELF\1\1\1", 7); a32.machine = 40;
  for (int scenario : {0, 1, 2}) {
    mz_zip_archive zip{}; REQUIRE(mz_zip_writer_init_file(&zip, apk.string().c_str(), 0));
    const std::array<std::uint32_t, 2> manifest{0x00080003, 8};
    REQUIRE(mz_zip_writer_add_mem(&zip, "AndroidManifest.xml", manifest.data(), sizeof(manifest), 0));
    if (scenario == 0) REQUIRE(mz_zip_writer_add_mem(&zip, "lib/arm64-v8a/libslice.so", &a64, sizeof(a64), 0));
    if (scenario == 2) REQUIRE(mz_zip_writer_add_mem(&zip, "lib/armeabi-v7a/libslice.so", &a64, sizeof(a64), 0));
    else REQUIRE(mz_zip_writer_add_mem(&zip, "lib/armeabi-v7a/libslice.so", &a32, sizeof(a32), 0));
    REQUIRE(mz_zip_writer_finalize_archive(&zip)); REQUIRE(mz_zip_writer_end(&zip));
    { std::ofstream plan(install / "plan.json");
      plan << "{\"package\":\"org.venture.nativeslices\",\"versionCode\":1,\"files\":[{\"kind\":\"apk\",\"name\":\"slice.apk\"}]}"; }
    configure_game(install);
    const auto expected = scenario == 0 ? GuestIsa::a64 : GuestIsa::a32;
    CHECK(process_config().isa() == expected);
    CHECK(process_config().guest_native_lib_dir() == (scenario == 0 ? "/data/app/org.venture.nativeslices/lib/arm64" : "/data/app/org.venture.nativeslices/lib/arm"));
    CHECK(process_config().properties.at("ro.product.cpu.abi") == process_config().abi);
    // A library for another machine is kept, as Android's installer does;
    // only a dlopen of it would fail.
    auto files = extract_native_libraries(); REQUIRE(files.size() == 1);
    CHECK(elf::detect_isa(files[0]) == (scenario == 2 ? GuestIsa::a64 : expected));
    CHECK(files[0].parent_path().filename() == process_config().abi);
  }
}
