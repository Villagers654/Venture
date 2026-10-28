#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <span>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/linker.h"

namespace venture::elf {
// ELF gABI and AAELF64 fixed-width structures; host <elf.h> is unavailable on Windows.
struct Header {
  unsigned char ident[16];
  std::uint16_t type, machine;
  std::uint32_t version;
  std::uint64_t entry, phoff, shoff;
  std::uint32_t flags;
  std::uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr { std::uint32_t type, flags; std::uint64_t offset, vaddr, paddr, filesz, memsz, align; };
struct Dyn { std::int64_t tag; std::uint64_t value; };
struct Sym { std::uint32_t name; std::uint8_t info, other; std::uint16_t shndx; std::uint64_t value, size; };
struct Rela { std::uint64_t offset, info; std::int64_t addend; };
// System V gABI / AAELF32 wire layouts. Never alias these as their ELF64 counterparts.
struct Header32 {
  unsigned char ident[16];
  std::uint16_t type, machine;
  std::uint32_t version, entry, phoff, shoff, flags;
  std::uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr32 { std::uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };
struct Shdr32 { std::uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize; };
struct Dyn32 { std::int32_t tag; std::uint32_t value; };
struct Sym32 { std::uint32_t name, value, size; std::uint8_t info, other; std::uint16_t shndx; };
struct Rel32 { std::uint32_t offset, info; };
static_assert(sizeof(Header32) == 52 && sizeof(Phdr32) == 32 && sizeof(Shdr32) == 40 &&
              sizeof(Dyn32) == 8 && sizeof(Sym32) == 16 && sizeof(Rel32) == 8);
struct Verdef { std::uint16_t version, flags, ndx, count; std::uint32_t hash, aux, next; };
struct Verdaux { std::uint32_t name, next; };
struct Verneed { std::uint16_t version, count; std::uint32_t file, aux, next; };
struct Vernaux { std::uint32_t hash; std::uint16_t flags, other; std::uint32_t name, next; };
static_assert(sizeof(Header) == 64 && sizeof(Phdr) == 56 && sizeof(Sym) == 24 && sizeof(Rela) == 24);
struct SymbolVersion { std::string name, provider; bool hidden = false; };
struct ArmRelocation {
  std::uint32_t type;
  const char* name;
  enum Operation { none, absolute, relative } operation;
};
std::span<const ArmRelocation> arm_relocation_table();
struct Import {
  std::string name, version, provider;
  bool weak = false;
  std::uint8_t type = 0;
  std::size_t symbol_index = 0;
  GuestAddr address = 0;
  std::string resolution, coverage;
};
struct Report {
  std::string library, path;
  std::uint64_t size{};
  std::vector<std::string> needed;
  std::map<std::string, std::uint64_t> relocations;
  std::vector<Import> unresolved;
  std::vector<Import> imports; // A32: every undefined dynsym entry, including unused/weak imports.
  std::uint64_t aps2_count{}, relr_count{}, deferred_resolvers{};
};

class Image final : public Library {
 public:
  explicit Image(const std::filesystem::path& file, std::string guest_path);
  ~Image() override;
  GuestIsa isa() const override { return isa_; }
  const std::filesystem::path& source_file() const { return source_file_; }
  Image* mapped_image() const override { return const_cast<Image*>(this); }
  GuestAddr find_symbol(std::string_view name, std::string_view version = {}) const override;
  const Sym* lookup(std::string_view name, std::string_view version = {}) const;
  std::string symbol_name(const Sym& symbol) const;
  SymbolVersion symbol_version(std::size_t index) const;
  const Sym& symbol_at(std::size_t index) const;
  std::span<const Sym> symbols() const { return {symbols_, symbol_count_}; }
  std::string string_at(std::uint64_t offset) const;
  GuestAddr address(std::uint64_t vaddr, std::uint64_t size = 1) const;
  std::uint64_t dynamic(std::int64_t tag) const;
  void relocations(const std::function<void(const Rela&)>& visit);
  void relative_relocations(const std::function<void(std::uint64_t)>& visit);
  void protect_segments();
  void initialize();
  void finalize();
  void nearest_symbol(GuestAddr address, DlInfo& info) const;
  std::vector<Phdr> headers;
  GuestAddr exidx = 0;
  std::uint32_t exidx_count = 0; // EHABI entries are pairs of 32-bit words.
  std::span<const ArmRelocation> arm_apply_table; // selected once for an ELF32 image
  std::vector<Library*> dependencies;
  Library* owner = this;
  Report report;
  bool symbolic = false, relocated = false, initialized = false;
  mutable std::map<std::uint64_t, GuestAddr> ifunc_results;
 private:
  std::filesystem::path source_file_;
  void parse_dynamic();
  GuestIsa isa_ = GuestIsa::a64;
  std::vector<Sym> symbols32_;
  std::unordered_map<std::int64_t, std::uint64_t> dynamic_;
  std::vector<std::uint64_t> needed_;
  std::unordered_map<std::string, std::vector<std::size_t>> exports_;
  std::map<std::uint16_t, SymbolVersion> versions_;
  const Sym* symbols_ = nullptr;
  std::size_t symbol_count_ = 0;
  std::uint64_t strtab_ = 0, strsize_ = 0, versym_ = 0;
  GuestAddr phdr_allocation_ = 0;
  // nearest_symbol candidates by (address, table index), built on first use.
  struct AddressIndex {
    std::vector<std::pair<GuestAddr, std::uint32_t>> entries;
    std::uint64_t max_size = 0;  // A32: largest FUNC size, bounds the backward scan
    bool zero_start = false;     // A32: a FUNC at address 0 keeps the table-order scan
  };
  void build_address_index() const;
  mutable std::once_flag address_index_once_;
  mutable AddressIndex address_index_;
};
std::string relocation_name(std::uint32_t type);
std::string relocation_name32(std::uint32_t type);
GuestIsa detect_isa(std::span<const std::byte> header);
GuestIsa detect_isa(const std::filesystem::path& file);
// Host-only export metadata; never maps or executes guest code.
std::set<std::string> exported_names(const std::filesystem::path& file, std::string_view version = {});
// Separately testable Android APS2 and RELR decoders; callback receives virtual offsets.
void decode_aps2(std::span<const std::byte> bytes, const std::function<void(const Rela&)>& visit);
void decode_relr(std::span<const std::uint64_t> words, const std::function<void(std::uint64_t)>& visit);
void decode_relr32(std::span<const std::uint32_t> words, const std::function<void(std::uint64_t)>& visit);
void finalize_images(std::span<const std::shared_ptr<Library>> libraries);
}
