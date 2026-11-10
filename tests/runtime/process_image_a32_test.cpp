// The A32 process image as in-process instrumentation reads it on a 32-bit
// Android process: ELF32 objects, Elf32_auxv_t, bionic's 32-bit soinfo and
// r_debug, found through the linker's own symbol table.
#include <doctest/doctest.h>
#include "core/address_space.h"
#include "core/hle_runtime.h"
#include "core/linker_runtime.h"
#include "core/process.h"
#include "core/process_image.h"
#include "hle/registry.h"
#include "java/jvm.h"
#include <cstring>
#include <optional>
#include <set>
#include <string>

using namespace venture;
namespace {
template <class T> T read(const std::string& f, std::size_t at) {
  T v{};
  REQUIRE(at + sizeof(T) <= f.size());
  std::memcpy(&v, f.data() + at, sizeof(T));
  return v;
}
// st_value of a .symtab or .dynsym symbol in an ELF32 file.
std::optional<std::uint32_t> symbol(const std::string& f, std::string_view name) {
  const auto shoff = read<std::uint32_t>(f, 0x20);
  const auto shentsize = read<std::uint16_t>(f, 0x2e), shnum = read<std::uint16_t>(f, 0x30);
  for (unsigned i = 0; i < shnum; ++i) {
    const auto sh = shoff + i * shentsize;
    const auto type = read<std::uint32_t>(f, sh + 4);
    if (type != 2 && type != 11) continue;  // SHT_SYMTAB, SHT_DYNSYM
    const auto offset = read<std::uint32_t>(f, sh + 16), size = read<std::uint32_t>(f, sh + 20);
    const auto strtab = shoff + read<std::uint32_t>(f, sh + 24) * shentsize;
    const auto strings = read<std::uint32_t>(f, strtab + 16);
    for (std::uint32_t s = offset; s + 16 <= offset + size; s += 16)
      if (std::string_view(f.data() + strings + read<std::uint32_t>(f, s)) == name) return read<std::uint32_t>(f, s + 4);
  }
  return std::nullopt;
}
std::uint32_t word(GuestAddr a) { return *g2h<std::uint32_t>(a); }
}  // namespace

TEST_CASE("A32 process image: ELF32 linker, 32-bit auxv, soinfo list and r_debug") {
  REQUIRE(process_config().isa() == GuestIsa::a32);
  register_all_hle_libraries();
  java::register_jni_library();
  auto& linker = Linker::get();
  REQUIRE(linker.open("libart.so", kRtldNow | kRtldGlobal, nullptr));
  process_image::publish();
  REQUIRE(process_image::published());

  CHECK(process_image::program_path() == "/system/bin/app_process32");
  const auto linker_path = process_image::system_path("ld-android.so");
  CHECK(linker_path == "/apex/com.android.runtime/bin/linker");
  CHECK(process_image::has_file("/system/bin/linker"));

  // Elf32_auxv_t: 4-byte type and value, AT_NULL last.
  const auto auxv = process_image::auxv();
  REQUIRE(auxv.size() % 8 == 0);
  CHECK(read<std::uint64_t>(auxv, auxv.size() - 8) == 0);
  std::uint64_t base = 0, phdr = 0;
  REQUIRE(process_image::auxval(7, &base));  // AT_BASE
  REQUIRE(process_image::auxval(3, &phdr));  // AT_PHDR
  bool saw_base = false;
  for (std::size_t i = 0; i + 8 <= auxv.size(); i += 8)
    saw_base |= read<std::uint32_t>(auxv, i) == 7 && read<std::uint32_t>(auxv, i + 4) == base;
  CHECK(saw_base);

  // The linker in memory and on disk is an ARM ELF32 object.
  const auto* ident = g2h<unsigned char>(base);
  CHECK(std::memcmp(ident, "\x7f" "ELF", 4) == 0);
  CHECK(ident[4] == 1);                                // ELFCLASS32
  CHECK(*g2h<std::uint16_t>(base + 0x12) == 40);       // EM_ARM
  const auto file = process_image::file(linker_path);
  REQUIRE(file);
  CHECK(read<std::uint8_t>(*file, 4) == 1);
  CHECK(read<std::uint16_t>(*file, 0x12) == 40);
  CHECK(read<std::uint16_t>(*file, 0x2e) == 40);       // sizeof(Elf32_Shdr)
  const auto program = process_image::file(process_image::program_path());
  REQUIRE(program);
  CHECK(read<std::uint8_t>(*program, 4) == 1);

  // solist through the linker's symbol table, then soinfo::next (offset 164)
  // and soinfo::soname (offset 376) of bionic's 32-bit soinfo.
  const auto solist = symbol(*file, "__dl__ZL6solist");
  REQUIRE(solist);
  REQUIRE(symbol(*file, "__dl__Z15solist_get_headv"));
  std::set<std::string> sonames;
  unsigned count = 0;
  for (GuestAddr si = word(base + *solist); si && count < 512; si = word(si + 164), ++count)
    if (const auto name = word(si + 376)) sonames.insert(g2h<char>(name));
  CHECK(count >= 3);
  CHECK(sonames.contains("libc.so"));
  CHECK(sonames.contains("libart.so"));

  // r_debug { r_version, r_map, r_brk, r_state, r_ldbase }; r_map heads with
  // the executable, then the linker (link_map: l_addr, l_name, l_ld, l_next, l_prev).
  const auto r_debug = symbol(*file, "_r_debug");
  REQUIRE(r_debug);
  const GuestAddr rd = base + *r_debug;
  CHECK(word(rd) == 1);
  const GuestAddr map = word(rd + 4);
  REQUIRE(map);
  CHECK(std::string(g2h<char>(word(map + 4))) == "/system/bin/app_process32");
  const GuestAddr next = word(map + 12);
  REQUIRE(next);
  CHECK(word(next + 16) == map);
  CHECK(std::string(g2h<char>(word(next + 4))) == linker_path);
  CHECK(word(rd + 8) != 0);  // r_brk
}
