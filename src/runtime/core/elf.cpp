#include "core/shutdown_trace.h"
#include "core/mutex_watch.h"
#include "core/elf.h"
#include "core/linker_runtime.h"
#include "core/profile.h"
#include "core/address_space.h"
#include "core/memory_metadata.h"
#include "core/thread.h"
#include "core/tls.h"
#include "cpu/block_list.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>

namespace venture::elf {
namespace {
constexpr std::uint32_t pt_load = 1, pt_dynamic = 2, pt_phdr = 6, pt_tls = 7, pt_relro = 0x6474e552;
constexpr std::uint32_t pt_arm_exidx = 0x70000001;
struct CallbackOwnership {
  Library& library;
  explicit CallbackOwnership(Image& image) : library(*image.owner) { ++library.lifecycle_references; }
  ~CallbackOwnership() { --library.lifecycle_references; }
};
std::vector<Image*> initialization_order;
std::uint32_t protection(std::uint32_t flags) {
  return ((flags & 4) ? kProtRead : 0) | ((flags & 2) ? kProtWrite : 0) | ((flags & 1) ? kProtExec : 0);
}
template<class T> T read_file(std::ifstream& input, std::uint64_t offset) {
  T value{}; input.seekg(static_cast<std::streamoff>(offset));
  if (!input.read(reinterpret_cast<char*>(&value), sizeof(value))) throw std::runtime_error("truncated ELF metadata");
  return value;
}
}
GuestIsa detect_isa(std::span<const std::byte> bytes) {
  if (bytes.size() < 20 || std::memcmp(bytes.data(), "\177ELF", 4) || bytes[5] != std::byte{1} || bytes[6] != std::byte{1})
    throw std::runtime_error("expected little-endian ARM ELF header");
  std::uint16_t machine;
  std::memcpy(&machine, bytes.data() + 18, sizeof(machine));
  if (bytes[4] == std::byte{2} && machine == 183) return GuestIsa::a64;
  if (bytes[4] == std::byte{1} && machine == 40) return GuestIsa::a32;
  throw std::runtime_error("ELF class/e_machine does not identify ARM32 or AArch64");
}
GuestIsa detect_isa(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  std::array<std::byte, 20> bytes{};
  if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
    throw std::runtime_error("cannot read ELF header: " + file.string());
  return detect_isa(bytes);
}
std::set<std::string> exported_names(const std::filesystem::path& file, std::string_view version) {
  std::ifstream input(file, std::ios::binary);
  const auto size = std::filesystem::file_size(file);
  const bool a32 = detect_isa(file) == GuestIsa::a32;
  std::vector<Phdr> segments;
  std::uint64_t phoff; unsigned count, stride;
  if (a32) { auto h = read_file<Header32>(input, 0); phoff = h.phoff; count = h.phnum; stride = h.phentsize; }
  else { auto h = read_file<Header>(input, 0); phoff = h.phoff; count = h.phnum; stride = h.phentsize; }
  if (!count || count > 4096 || stride != (a32 ? sizeof(Phdr32) : sizeof(Phdr)) ||
      phoff > size || std::uint64_t(count) * stride > size - phoff) throw std::runtime_error("invalid export census headers");
  for (unsigned i = 0; i < count; ++i) {
    if (a32) { auto p = read_file<Phdr32>(input, phoff + i * stride); segments.push_back({p.type, p.flags, p.offset, p.vaddr, p.paddr, p.filesz, p.memsz, p.align}); }
    else segments.push_back(read_file<Phdr>(input, phoff + i * stride));
  }
  auto offset = [&](std::uint64_t address, std::uint64_t length) {
    for (const auto& p : segments) if (p.type == pt_load && address >= p.vaddr &&
        address - p.vaddr <= p.filesz && length <= p.filesz - (address - p.vaddr) &&
        p.offset <= size && address - p.vaddr <= size - p.offset && length <= size - p.offset - (address - p.vaddr))
      return p.offset + address - p.vaddr;
    throw std::runtime_error("export census metadata outside file");
  };
  std::map<std::int64_t, std::uint64_t> tags;
  for (const auto& p : segments) if (p.type == pt_dynamic) {
    const unsigned entry = a32 ? sizeof(Dyn32) : sizeof(Dyn);
    for (std::uint64_t at = 0; at + entry <= p.filesz; at += entry) {
      Dyn d;
      if (a32) { auto v = read_file<Dyn32>(input, offset(p.vaddr + at, entry)); d = {v.tag, v.value}; }
      else d = read_file<Dyn>(input, offset(p.vaddr + at, entry));
      if (!d.tag) break;
      tags[d.tag] = d.value;
    }
  }
  auto word = [&](std::uint64_t address) { return read_file<std::uint32_t>(input, offset(address, 4)); };
  std::uint64_t n = 0;
  if (tags[4]) n = word(tags[4] + 4);
  else if (auto hash = tags[0x6ffffef5]) {
    const auto buckets = word(hash), first = word(hash + 4), bloom = word(hash + 8);
    if (!buckets || !bloom || buckets > 10000000 || bloom > 10000000) throw std::runtime_error("invalid export census GNU hash");
    const auto start = hash + 16 + (a32 ? 4ull : 8ull) * bloom;
    std::uint32_t last = 0;
    for (unsigned i = 0; i < buckets; ++i) last = std::max(last, word(start + 4ull * i));
    n = first;
    if (last && last >= first) {
      auto chain = start + 4ull * buckets + 4ull * (last - first);
      for (;;) {
        if (last >= 10000000) throw std::runtime_error("export census hash exceeds limit");
        const auto value = word(chain); ++last; chain += 4;
        if (value & 1) break;
      }
      n = last;
    }
  } else throw std::runtime_error("export census missing dynamic hash");
  const auto symsize = a32 ? sizeof(Sym32) : sizeof(Sym);
  if (n > 10000000 || tags[11] != symsize || tags[10] > size) throw std::runtime_error("invalid export census symbol table");
  std::string strings(tags[10], '\0');
  input.seekg(offset(tags[5], strings.size()));
  if (!input.read(strings.data(), strings.size())) throw std::runtime_error("truncated export census strings");
  auto string_at = [&](std::uint32_t at) {
    if (at >= strings.size()) throw std::runtime_error("export census string offset outside table");
    const auto end = strings.find('\0', at);
    if (end == std::string::npos) throw std::runtime_error("unterminated export census string");
    return strings.substr(at, end - at);
  };
  std::map<unsigned, std::string> versions;
  auto verdef = tags[0x6ffffffc];
  if (tags[0x6ffffffd] > 10000) throw std::runtime_error("export census version count exceeds limit");
  for (unsigned i = 0; i < tags[0x6ffffffd]; ++i) {
    auto def = read_file<Verdef>(input, offset(verdef, sizeof(Verdef)));
    auto aux = read_file<Verdaux>(input, offset(verdef + def.aux, sizeof(Verdaux)));
    versions[def.ndx] = string_at(aux.name);
    if (!def.next) break;
    verdef += def.next;
  }
  std::set<std::string> result;
  for (std::uint64_t i = 0; i < n; ++i) {
    Sym s;
    if (a32) { auto v = read_file<Sym32>(input, offset(tags[6] + i * symsize, symsize)); s = {v.name, v.info, v.other, v.shndx, v.value, v.size}; }
    else s = read_file<Sym>(input, offset(tags[6] + i * symsize, symsize));
    if (!s.shndx || !(s.info >> 4) || ((s.other & 3) != 0 && (s.other & 3) != 3)) continue;
    if (tags[0x6ffffff0]) {
      const auto v = read_file<std::uint16_t>(input, offset(tags[0x6ffffff0] + 2 * i, 2));
      if ((version.empty() && (v & 0x8000)) || (!version.empty() && versions[v & 0x7fff] != version)) continue;
    } else if (!version.empty()) continue;
    if (s.name >= strings.size()) throw std::runtime_error("export census string offset outside table");
    auto end = strings.find('\0', s.name);
    if (end == std::string::npos) throw std::runtime_error("unterminated export census symbol");
    result.emplace(strings.substr(s.name, end - s.name));
  }
  return result;
}
Image::Image(const std::filesystem::path& file, std::string guest_path, const Placement* placement) {
  // Host source path only for the features that read it (hint hashing, compile census).
  if (JitBlockList::enabled() || profile::compile_timing_enabled) source_file_ = file;
  path_ = std::move(guest_path); soname_ = file.filename().string();
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open ELF: " + file.string());
  const auto file_size = std::filesystem::file_size(file);
  auto ident = read_file<std::array<std::byte, 20>>(input, 0);
  isa_ = detect_isa(ident);
  if (isa_ == GuestIsa::a32) arm_apply_table = arm_relocation_table();
  Header header{};
  if (isa_ == GuestIsa::a32) {
    auto h = read_file<Header32>(input, 0);
    std::memcpy(header.ident, h.ident, 16);
    header.type = h.type; header.machine = h.machine; header.version = h.version;
    header.entry = h.entry; header.phoff = h.phoff; header.shoff = h.shoff; header.flags = h.flags;
    header.ehsize = h.ehsize; header.phentsize = h.phentsize; header.phnum = h.phnum;
    header.shentsize = h.shentsize; header.shnum = h.shnum; header.shstrndx = h.shstrndx;
  } else header = read_file<Header>(input, 0);
  const auto ehsize = isa_ == GuestIsa::a32 ? sizeof(Header32) : sizeof(Header);
  const auto phsize = isa_ == GuestIsa::a32 ? sizeof(Phdr32) : sizeof(Phdr);
  if (header.type != 3 || header.version != 1 || header.ehsize != ehsize || header.phentsize != phsize ||
      !header.phnum || header.phnum > 4096)
    throw std::runtime_error("invalid ARM ELF ET_DYN header: " + file.string());
  if (header.phoff > file_size || header.phnum * phsize > file_size - header.phoff)
    throw std::runtime_error("ELF program headers outside file");
  std::uint64_t low = std::numeric_limits<std::uint64_t>::max(), high = 0, alignment = 4096;
  for (unsigned i = 0; i < header.phnum; ++i) {
    Phdr ph;
    if (isa_ == GuestIsa::a32) {
      auto p = read_file<Phdr32>(input, header.phoff + i * phsize);
      ph = {p.type, p.flags, p.offset, p.vaddr, p.paddr, p.filesz, p.memsz, p.align};
    } else ph = read_file<Phdr>(input, header.phoff + i * phsize);
    const auto limit = isa_ == GuestIsa::a32 ? 1ull << 32 : 1ull << 46;
    if (ph.offset > file_size || ph.filesz > file_size - ph.offset ||
        ((ph.type == pt_load || ph.type == pt_tls) && ph.filesz > ph.memsz) ||
        ph.vaddr > limit || ph.memsz > limit - ph.vaddr ||
        (ph.align && (ph.align & (ph.align - 1)))) throw std::runtime_error("invalid ELF program segment");
    if (ph.type == pt_load && ph.memsz) {
      if ((ph.vaddr & 4095) != (ph.offset & 4095)) throw std::runtime_error("unaligned ELF PT_LOAD");
      low = std::min(low, align_down(ph.vaddr, 4096)); high = std::max(high, align_up(ph.vaddr + ph.memsz, 4096));
      alignment = std::max(alignment, ph.align);
    }
    headers.push_back(ph);
  }
  if (high <= low || high - low > (1ull << 40)) throw std::runtime_error("invalid ELF load span");
  map_size = high - low + (low % alignment);
  if (placement && placement->address && map_size <= placement->size && !(placement->address & 4095))
    map_start = AddressSpace::get().allocate_at(placement->address, map_size, kProtNone, path_);
  else if (placement && placement->required)
    throw std::runtime_error("android_dlopen_ext: the reserved address space is too small for " + soname_);
  else
    map_start = AddressSpace::get().allocate(map_size, kProtNone, path_, alignment);
  if (!map_start) throw std::bad_alloc();
  load_bias = map_start + (low % alignment) - low;
  try {
    for (const auto& ph : headers) if (ph.type == pt_load && ph.memsz) {
      const auto start = align_down(load_bias + ph.vaddr, 4096);
      const auto size = align_up(load_bias + ph.vaddr + ph.memsz, 4096) - start;
      // Preserve guest execute permission for ifuncs while relocation writes remain possible.
      if (AddressSpace::get().protect(start, size, kProtRead | kProtWrite | ((ph.flags & 1) ? kProtExec : 0))) throw std::bad_alloc();
      label_memory(start, size, path_, align_down(ph.offset, 4096));
      input.seekg(static_cast<std::streamoff>(ph.offset));
      if (ph.filesz && !input.read(g2h<char>(load_bias + ph.vaddr), static_cast<std::streamsize>(ph.filesz))) throw std::runtime_error("truncated ELF PT_LOAD");
      if (ph.memsz > ph.filesz) std::memset(g2h(load_bias + ph.vaddr + ph.filesz), 0, ph.memsz - ph.filesz);
    }
    phnum = header.phnum;
    for (const auto& ph : headers) {
      if (ph.type == pt_phdr) phdr = address(ph.vaddr, phnum * phsize);
      if (!phdr && ph.type == pt_load && ph.offset <= header.phoff &&
          header.phoff - ph.offset + phnum * phsize <= ph.filesz)
        phdr = load_bias + ph.vaddr + header.phoff - ph.offset;
      if (isa_ == GuestIsa::a32 && ph.type == pt_arm_exidx) {
        if (exidx || ph.memsz % 8 || ph.memsz / 8 > INT32_MAX)
          throw std::runtime_error("invalid PT_ARM_EXIDX");
        exidx = address(ph.vaddr, ph.memsz); exidx_count = static_cast<std::uint32_t>(ph.memsz / 8);
      }
    }
    if (!phdr) {
      phdr_allocation_ = AddressSpace::get().allocate(phnum * phsize, kProtRead | kProtWrite, "[ELF phdr]");
      if (!phdr_allocation_) throw std::bad_alloc();
      phdr = phdr_allocation_;
      // dl_iterate_phdr sees original guest wire bytes, not normalized host Phdrs.
      input.seekg(static_cast<std::streamoff>(header.phoff));
      if (!input.read(g2h<char>(phdr), phnum * phsize)) throw std::runtime_error("truncated ELF phdr");
      AddressSpace::get().protect(phdr, align_up(phnum * phsize, 4096), kProtRead);
    }
    parse_dynamic();
    for (const auto& ph : headers) if (ph.type == pt_tls && ph.memsz) {
      if (isa_ == GuestIsa::a32) throw std::runtime_error("ARM32 native ELF TLS is not implemented");
      auto module = tls::add_module({g2h<const std::byte>(address(ph.vaddr, ph.filesz)), static_cast<std::size_t>(ph.filesz)}, ph.memsz, ph.align, ph.align ? ph.vaddr % ph.align : 0);
      tls_module_id = module.id; tls_offset = module.offset;
    }
    report.library = soname_; report.path = path_; report.size = map_size;
  } catch (...) { AddressSpace::get().free(map_start); AddressSpace::get().free(phdr_allocation_); map_start = 0; throw; }
}
Image::~Image() {
  // A final snapshot can die on an interrupted worker while another thread
  // owns the loader lock and joins it. Destruction must neither throw nor wait
  // forever; dlclose already removed finalized images from initialization_order.
  auto& mutex = linker_lifecycle_mutex();
  if (mutex.lock_cleanup()) {
    std::unique_lock lock(mutex, std::adopt_lock);
    std::erase(initialization_order, this);
  }
  AddressSpace::get().free(map_start); AddressSpace::get().free(phdr_allocation_);
}
GuestAddr Image::address(std::uint64_t vaddr, std::uint64_t size) const {
  for (const auto& ph : headers) if (ph.type == pt_load && vaddr >= ph.vaddr &&
      vaddr - ph.vaddr <= ph.memsz && size <= ph.memsz - (vaddr - ph.vaddr)) return load_bias + vaddr;
  throw std::runtime_error("ELF address outside PT_LOAD: " + soname_);
}
std::uint64_t Image::dynamic(std::int64_t tag) const { auto it = dynamic_.find(tag); return it == dynamic_.end() ? 0 : it->second; }
std::string Image::string_at(std::uint64_t offset) const {
  if (offset >= strsize_) throw std::runtime_error("ELF string offset outside DT_STRTAB");
  auto* p = g2h<const char>(address(strtab_ + offset, strsize_ - offset));
  auto* end = static_cast<const char*>(std::memchr(p, 0, strsize_ - offset));
  if (!end) throw std::runtime_error("unterminated ELF dynamic string");
  return {p, end};
}
void Image::parse_dynamic() {
  bool terminated = false;
  for (const auto& ph : headers) if (ph.type == pt_dynamic) {
    const auto entry_size = isa_ == GuestIsa::a32 ? sizeof(Dyn32) : sizeof(Dyn);
    auto p = address(ph.vaddr, ph.memsz);
    for (std::uint64_t i = 0; i < ph.memsz / entry_size; ++i) {
      Dyn entry;
      if (isa_ == GuestIsa::a32) {
        Dyn32 d; std::memcpy(&d, g2h(p + i * entry_size), sizeof(d)); entry = {d.tag, d.value};
      } else std::memcpy(&entry, g2h(p + i * entry_size), sizeof(entry));
      if (!entry.tag) { terminated = true; break; }
      if (entry.tag == 1) needed_.push_back(entry.value);
      else dynamic_[entry.tag] = entry.value;
    }
  }
  if (!terminated) throw std::runtime_error("missing terminated ELF PT_DYNAMIC");
  strtab_ = dynamic(5); strsize_ = dynamic(10);
  address(strtab_, strsize_);
  if (dynamic(14)) soname_ = string_at(dynamic(14));
  for (auto offset : needed_) report.needed.push_back(string_at(offset));
  symbolic = dynamic_.contains(16) || (dynamic(30) & 2);
  global = dynamic(0x6ffffffb) & 2;
  nodelete = dynamic(0x6ffffffb) & 8;
  const auto symsize = isa_ == GuestIsa::a32 ? sizeof(Sym32) : sizeof(Sym);
  if (dynamic(11) != symsize) throw std::runtime_error("invalid ELF DT_SYMENT");
  if (auto hash = dynamic(4)) {
    auto* h = g2h<const std::uint32_t>(address(hash, 8));
    symbol_count_ = h[1];
    address(hash, (2ull + h[0] + h[1]) * 4);
  } else if (auto hash = dynamic(0x6ffffef5)) {
    auto* h = g2h<const std::uint32_t>(address(hash, 16));
    const auto nbuckets = h[0], first = h[1], bloom = h[2];
    if (!nbuckets || !bloom || bloom > (1u << 26)) throw std::runtime_error("invalid ELF GNU hash header");
    auto buckets_vaddr = hash + 16 + (isa_ == GuestIsa::a32 ? 4ull : 8ull) * bloom;
    auto* buckets = g2h<const std::uint32_t>(address(buckets_vaddr, 4ull * nbuckets));
    std::uint32_t last = 0;
    for (std::uint32_t i = 0; i < nbuckets; ++i) last = std::max(last, buckets[i]);
    symbol_count_ = first;
    if (last >= first && last) {
      auto chain = buckets_vaddr + 4ull * nbuckets + 4ull * (last - first);
      for (;;) {
        if (last >= 10'000'000) throw std::runtime_error("ELF GNU hash chain exceeds limit");
        auto value = *g2h<const std::uint32_t>(address(chain, 4));
        ++last; chain += 4;
        if (value & 1) break;
      }
      symbol_count_ = last;
    }
  } else throw std::runtime_error("ELF has neither DT_HASH nor DT_GNU_HASH");
  if (symbol_count_ > 10'000'000) throw std::runtime_error("excessive ELF symbol count");
  auto symaddr = address(dynamic(6), symbol_count_ * symsize);
  if (isa_ == GuestIsa::a32) {
    symbols32_.reserve(symbol_count_);
    for (std::size_t i = 0; i < symbol_count_; ++i) {
      Sym32 s; std::memcpy(&s, g2h(symaddr + i * symsize), sizeof(s));
      symbols32_.push_back({s.name, s.info, s.other, s.shndx, s.value, s.size});
    }
    symbols_ = symbols32_.data();
  } else symbols_ = g2h<const Sym>(symaddr);
  versym_ = dynamic(0x6ffffff0);
  if (versym_) address(versym_, 2 * symbol_count_);
  auto verdef = dynamic(0x6ffffffc);
  for (std::uint64_t i = 0; i < dynamic(0x6ffffffd); ++i) {
    auto& def = *g2h<const Verdef>(address(verdef, sizeof(Verdef)));
    auto& aux = *g2h<const Verdaux>(address(verdef + def.aux, sizeof(Verdaux)));
    versions_[def.ndx] = {string_at(aux.name), {}, false};
    if (!def.next) break;
    verdef += def.next;
  }
  auto verneed = dynamic(0x6ffffffe);
  for (std::uint64_t i = 0; i < dynamic(0x6fffffff); ++i) {
    auto& need = *g2h<const Verneed>(address(verneed, sizeof(Verneed)));
    auto aux_addr = verneed + need.aux;
    for (unsigned j = 0; j < need.count; ++j) {
      auto& aux = *g2h<const Vernaux>(address(aux_addr, sizeof(Vernaux)));
      versions_[aux.other & 0x7fff] = {string_at(aux.name), string_at(need.file), false};
      if (!aux.next) break;
      aux_addr += aux.next;
    }
    if (!need.next) break;
    verneed += need.next;
  }
  for (std::size_t i = 0; i < symbol_count_; ++i) {
    auto& sym = symbols_[i];
    if (sym.shndx && (sym.info >> 4) && ((sym.other & 3) == 0 || (sym.other & 3) == 3))
      exports_[symbol_name(sym)].push_back(i);
  }
}
std::string Image::symbol_name(const Sym& sym) const { return string_at(sym.name); }
const Sym& Image::symbol_at(std::size_t index) const {
  if (index >= symbol_count_) throw std::runtime_error("ELF relocation symbol index outside dynsym");
  return symbols_[index];
}
SymbolVersion Image::symbol_version(std::size_t index) const {
  if (!versym_) return {};
  auto v = g2h<const std::uint16_t>(address(versym_, symbol_count_ * 2))[index];
  auto it = versions_.find(v & 0x7fff);
  SymbolVersion result = it == versions_.end() ? SymbolVersion{} : it->second;
  result.hidden = v & 0x8000; return result;
}
const Sym* Image::lookup(std::string_view name, std::string_view version) const {
  auto it = exports_.find(std::string(name));
  if (it == exports_.end()) return nullptr;
  for (auto i : it->second) {
    auto v = symbol_version(i);
    if ((version.empty() && !v.hidden) || (!version.empty() && v.name == version)) return &symbols_[i];
  }
  return nullptr;
}
GuestAddr Image::find_symbol(std::string_view name, std::string_view version) const {
  auto* sym = lookup(name, version);
  if (!sym) return 0;
  if ((sym->info & 15) == 6) {
    auto* thread = GuestThread::current(); return thread ? tls::get_addr(*thread, tls_module_id, sym->value) : 0;
  }
  if ((sym->info & 15) == 10) {
    if (auto it = ifunc_results.find(sym->value); it != ifunc_results.end()) return it->second;
  }
  return sym->shndx == 0xfff1 ? sym->value : load_bias + sym->value;
}
void Image::relocations(const std::function<void(const Rela&)>& visit) {
  if (isa_ == GuestIsa::a32) {
    if (dynamic(8) || dynamic(0x60000012))
      throw std::runtime_error("ARM32 RELA relocations are not implemented");
    if (dynamic(19) && dynamic(19) != sizeof(Rel32)) throw std::runtime_error("invalid DT_RELENT");
    auto rel = [&](std::uint64_t offset, std::uint32_t info) {
      const auto type = info & 0xff;
      std::uint32_t addend = 0;
      // bionic/linker/linker_relocate.cpp: REL GOT/PLT slots
      // have no implicit addend. TLS offsets are included for future support.
      if (type == 23 || type == 2 || type == 18 || type == 19)
        std::memcpy(&addend, g2h(address(offset, 4)), 4);
      visit({offset, (std::uint64_t{info >> 8} << 32) | type, addend});
    };
    auto table = [&](std::uint64_t p, std::uint64_t bytes) {
      if (!bytes) return;
      if (bytes % sizeof(Rel32)) throw std::runtime_error("mis-sized ELF REL table");
      auto entries = address(p, bytes);
      for (std::size_t i = 0; i < bytes / sizeof(Rel32); ++i) {
        Rel32 r; std::memcpy(&r, g2h(entries + i * sizeof(r)), sizeof(r));
        rel(r.offset, r.info);
      }
    };
    table(dynamic(17), dynamic(18));
    // DT_ANDROID_REL: the APS2 stream with REL entries (no addend groups).
    if (auto size = dynamic(0x60000010)) {
      auto p = address(dynamic(0x6000000f), size);
      decode_aps2({g2h<const std::byte>(p), static_cast<std::size_t>(size)}, [&](const Rela& r) {
        if (r.addend) throw std::runtime_error("ARM32 Android REL group has an addend");
        ++report.aps2_count; rel(static_cast<std::uint32_t>(r.offset), static_cast<std::uint32_t>(r.info));
      });
    }
    if (dynamic(2) && dynamic(20) != 17) throw std::runtime_error("ARM32 PLT must use REL");
    table(dynamic(23), dynamic(2));
    return;
  }
  auto table = [&](std::uint64_t p, std::uint64_t bytes) {
    if (!bytes) return;
    if (bytes % sizeof(Rela)) throw std::runtime_error("mis-sized ELF RELA table");
    auto* entries = g2h<const Rela>(address(p, bytes));
    for (std::size_t i = 0; i < bytes / sizeof(Rela); ++i) visit(entries[i]);
  };
  if (dynamic(9) && dynamic(9) != sizeof(Rela)) throw std::runtime_error("invalid DT_RELAENT");
  table(dynamic(7), dynamic(8));
  if (dynamic(2) && dynamic(20) != 7) throw std::runtime_error("AArch64 PLT must use RELA");
  table(dynamic(23), dynamic(2));
  if (auto size = dynamic(0x60000012)) {
    auto p = address(dynamic(0x60000011), size);
    decode_aps2({g2h<const std::byte>(p), static_cast<std::size_t>(size)}, [&](const Rela& r) { ++report.aps2_count; visit(r); });
  }
}
void Image::relative_relocations(const std::function<void(std::uint64_t)>& visit) {
  auto p = dynamic(36), size = dynamic(35), entry = dynamic(37);
  if (!p) { p = dynamic(0x6fffe000); size = dynamic(0x6fffe001); entry = dynamic(0x6fffe003); }
  if (!size) return;
  if (isa_ == GuestIsa::a32) {
    if ((entry && entry != 4) || size % 4) throw std::runtime_error("invalid RELR entry size");
    decode_relr32({g2h<const std::uint32_t>(address(p, size)), static_cast<std::size_t>(size / 4)},
                  [&](auto offset) { ++report.relr_count; visit(offset); });
    return;
  }
  if ((entry && entry != 8) || size % 8) throw std::runtime_error("invalid RELR entry size");
  decode_relr({g2h<const std::uint64_t>(address(p, size)), static_cast<std::size_t>(size / 8)},
              [&](auto offset) { ++report.relr_count; visit(offset); });
}
void Image::protect_segments() {
  // Merge permissions on pages shared by adjacent PT_LOADs.
  std::map<GuestAddr, std::uint32_t> pages;
  for (const auto& ph : headers) if (ph.type == pt_load && ph.memsz)
    for (auto p = align_down(load_bias + ph.vaddr, 4096); p < align_up(load_bias + ph.vaddr + ph.memsz, 4096); p += 4096)
      pages[p] |= protection(ph.flags);
  for (const auto& ph : headers) if (ph.type == pt_relro && ph.memsz)
    // bionic/linker/linker_phdr.cpp protects every page touched by PT_GNU_RELRO.
    for (auto p = align_down(load_bias + ph.vaddr, 4096); p < align_up(load_bias + ph.vaddr + ph.memsz, 4096); p += 4096)
      pages[p] = kProtRead;
  for (auto it = pages.begin(); it != pages.end();) {
    auto start = it->first, end = start + 4096; auto prot = it->second; ++it;
    while (it != pages.end() && it->first == end && it->second == prot) { end += 4096; ++it; }
    if (AddressSpace::get().protect(start, end - start, prot)) throw std::runtime_error("ELF final memory protection failed");
  }
}
void Image::initialize() {
  std::lock_guard lock(linker_lifecycle_mutex());
  if (initialized) return;
  CallbackOwnership ownership(*this);
  if (shutdown_trace::mutex_watch_enabled) shutdown_trace::mutex_watch_image(soname(), load_bias, map_start, map_size);
  initialized = true; // Protect dependency cycles and reentrant dlopen from constructors.
  for (auto* dep : dependencies) if (auto* image = dep->mapped_image()) image->initialize();
  auto* thread = GuestThread::current();
  if (!thread) throw std::runtime_error("ELF initializers require an attached guest thread");
  initialization_order.push_back(this);
  if (auto init = dynamic(12)) {
    if (isa_ == GuestIsa::a32) { address(init & ~1ull, 4); thread->call(load_bias + init, {}); }
    else thread->call(address(init, 4), {});
  }
  if (auto size = dynamic(27)) {
    if (isa_ == GuestIsa::a32) {
      if (size % 4) throw std::runtime_error("invalid ARM32 DT_INIT_ARRAYSZ");
      auto* array = g2h<const std::uint32_t>(address(dynamic(25), size));
      for (std::size_t i = 0; i < size / 4; ++i) if (array[i] && array[i] != UINT32_MAX) thread->call(array[i], {});
      return;
    }
    if (size % 8) throw std::runtime_error("invalid DT_INIT_ARRAYSZ");
    auto* array = g2h<const GuestAddr>(address(dynamic(25), size));
    for (std::size_t i = 0; i < size / 8; ++i) if (array[i] && array[i] != ~GuestAddr{0}) thread->call(array[i], {});
  }
}
void Image::finalize() {
  std::lock_guard lock(linker_lifecycle_mutex());
  if (!initialized) return;
  CallbackOwnership ownership(*this);
  if (shutdown_trace::enabled) shutdown_trace::sample_watch("ELF finalizers enter " + soname());
  initialized = false;
  std::erase(initialization_order, this);
  auto* thread = GuestThread::current();
  if (!thread) throw std::runtime_error("ELF finalizers require an attached guest thread");
  if (auto size = dynamic(28)) {
    if (isa_ == GuestIsa::a32) {
      if (size % 4) throw std::runtime_error("invalid ARM32 DT_FINI_ARRAYSZ");
      auto* array = g2h<const std::uint32_t>(address(dynamic(26), size));
      for (std::size_t i = size / 4; i > 0; --i) if (array[i - 1] && array[i - 1] != UINT32_MAX) thread->call(array[i - 1], {});
    } else {
    auto* array = g2h<const GuestAddr>(address(dynamic(26), size));
    for (std::size_t i = size / 8; i > 0; --i) if (array[i - 1] && array[i - 1] != ~GuestAddr{0}) thread->call(array[i - 1], {});
    }
  }
  if (auto fini = dynamic(13)) {
    if (isa_ == GuestIsa::a32) { address(fini & ~1ull, 4); thread->call(load_bias + fini, {}); }
    else thread->call(address(fini, 4), {});
  }
  if (shutdown_trace::enabled) shutdown_trace::sample_watch("ELF finalizers leave " + soname());
}
void Image::build_address_index() const {
  auto& index = address_index_;
  for (std::size_t i = 0; i < symbol_count_; ++i) {
    const auto& sym = symbols_[i];
    if (!sym.shndx || !sym.name || (sym.info & 15) == 6) continue;
    auto addr = sym.shndx == 0xfff1 ? sym.value : load_bias + sym.value;
    if (isa_ == GuestIsa::a32) {
      if ((sym.info & 15) != 2 || !sym.size) continue;
      index.zero_start |= !addr;
      index.max_size = std::max<std::uint64_t>(index.max_size, sym.size);
      addr &= ~GuestAddr{1};
    }
    index.entries.emplace_back(addr, static_cast<std::uint32_t>(i));
  }
  std::sort(index.entries.begin(), index.entries.end());
}
void Image::nearest_symbol(GuestAddr p, DlInfo& out) const {
  if (isa_ == GuestIsa::a32) p &= ~GuestAddr{1};
  // dladdr runs per frame in some titles; a table scan costs tens of
  // microseconds in large libraries. The index gives the scan's answer: the
  // highest qualifying address, the later table entry on a tie.
  std::call_once(address_index_once_, [this] { build_address_index(); });
  const auto& index = address_index_;
  if (!out.symbol_address && !index.zero_start) {
    auto it = std::upper_bound(index.entries.begin(), index.entries.end(), p,
                               [](GuestAddr value, const auto& entry) { return value < entry.first; });
    if (isa_ != GuestIsa::a32) {
      if (it != index.entries.begin()) {
        --it;
        out.symbol_address = it->first; out.symbol = symbol_name(symbols_[it->second]);
      }
      return;
    }
    // A32 symbols must contain p; no FUNC starts further back than max_size.
    while (it != index.entries.begin() && p - (--it)->first < index.max_size) {
      const auto& sym = symbols_[it->second];
      if (p - it->first < sym.size) {
        out.symbol_address = sym.shndx == 0xfff1 ? sym.value : load_bias + sym.value;
        out.symbol = symbol_name(sym);
        return;
      }
    }
    return;
  }
  for (std::size_t i = 0; i < symbol_count_; ++i) {
    auto& sym = symbols_[i];
    if (!sym.shndx || !sym.name || (sym.info & 15) == 6) continue;
    auto addr = sym.shndx == 0xfff1 ? sym.value : load_bias + sym.value;
    if (isa_ == GuestIsa::a32) {
      auto start = addr & ~GuestAddr{1};
      // AAELF32 FUNC values retain bit 0 for interworking. Range comparisons
      // use the instruction address and size, returned symbol pointers do not.
      if ((sym.info & 15) != 2 || !sym.size || p < start || p - start >= sym.size) continue;
      if (!out.symbol_address || start >= (out.symbol_address & ~GuestAddr{1})) {
        out.symbol_address = addr; out.symbol = symbol_name(sym);
      }
      continue;
    }
    if (addr <= p && addr >= out.symbol_address) { out.symbol_address = addr; out.symbol = symbol_name(sym); }
  }
}
void finalize_images(std::span<const std::shared_ptr<Library>> libraries) {
  std::lock_guard lock(linker_lifecycle_mutex());
  std::set<Image*> selected;
  std::vector<std::shared_ptr<Library>> snapshot;
  for (const auto& library : libraries) {
    // Pin the Image itself: retaining its stable HLE slot alone does not keep
    // a backing alive when another finalizer recursively closes that slot.
    std::shared_ptr<Library> image = library;
    if (auto* hle = dynamic_cast<HleLibrary*>(library.get())) image = hle->backing();
    if (image && image->mapped_image()) {
      selected.insert(image->mapped_image()); snapshot.push_back(std::move(image));
    }
  }
  const auto order = initialization_order;
  for (auto it = order.rbegin(); it != order.rend(); ++it)
    if (selected.contains(*it) && (*it)->initialized) (*it)->finalize();
}
}
