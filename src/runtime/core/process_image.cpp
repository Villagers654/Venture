// Guest-visible Android process structures. Layouts follow AOSP android12L:
// bionic/linker/linker_soinfo.h (soinfo, SOINFO_VERSION 6), bionic/libc/include/link.h
// (r_debug, link_map), bionic/linker/linker_gdb_support.cpp (rtld_db_dlactivity),
// and the ELF gABI. Thunks stay in Venture's thunk pages; each system library
// image carries the ELF header, program headers, dynamic section and symbol
// tables that describe them, at the path that library has on a Quest.
#include "core/process_image.h"

#include "core/address_space.h"
#include "core/hle.h"
#include "core/hle_runtime.h"
#include "core/linker.h"
#include "core/linker_runtime.h"
#include "core/memory_metadata.h"
#include "core/process.h"
#include "core/thread.h"
#include "core/thread_runtime.h"
#include "core/wait.h"
#include "cpu/cpu.h"
#include "common/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace venture::process_image {
namespace {
// An A32 process is a 32-bit Android process on the same device: app_process32,
// the 32-bit linker and /system/lib, with ELF32 objects and ILP32 structures.
bool wide() { return process_config().isa() == GuestIsa::a64; }
std::string_view program_path_view() { return wide() ? "/system/bin/app_process64" : "/system/bin/app_process32"; }
std::string_view linker_path() { return wide() ? "/apex/com.android.runtime/bin/linker64" : "/apex/com.android.runtime/bin/linker"; }
std::string_view linker_link() { return wide() ? "/system/bin/linker64" : "/system/bin/linker"; }
constexpr std::string_view kLoaderSoname = "ld-android.so";
constexpr std::uint32_t kRet = 0xd65f03c0u, kNop = 0xd503201fu;
constexpr std::uint32_t kArmBxLr = 0xe12fff1eu, kArmNop = 0xe320f000u;  // ARM ARM A8.8.27, A8.8.120 (ARM mode)
std::uint32_t nop() { return wide() ? kNop : kArmNop; }

// ---- ELF64 writer -------------------------------------------------------------------------
struct Ehdr { std::uint8_t ident[16]; std::uint16_t type, machine; std::uint32_t version; std::uint64_t entry, phoff, shoff;
              std::uint32_t flags; std::uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { std::uint32_t type, flags; std::uint64_t offset, vaddr, paddr, filesz, memsz, align; };
struct Shdr { std::uint32_t name, type; std::uint64_t flags, addr, offset, size; std::uint32_t link, info; std::uint64_t addralign, entsize; };
struct Sym { std::uint32_t name; std::uint8_t info, other; std::uint16_t shndx; std::uint64_t value, size; };
struct Dyn { std::int64_t tag; std::uint64_t value; };
static_assert(sizeof(Ehdr) == 64 && sizeof(Phdr) == 56 && sizeof(Shdr) == 64 && sizeof(Sym) == 24 && sizeof(Dyn) == 16);
enum : std::uint32_t { PT_LOAD = 1, PT_DYNAMIC = 2, PT_INTERP = 3, PT_PHDR = 6, PT_GNU_STACK = 0x6474e551 };
enum : std::int64_t { DT_NULL = 0, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_STRSZ = 10, DT_SYMENT = 11, DT_SONAME = 14,
                      DT_DEBUG = 21, DT_FLAGS_1 = 0x6ffffffb };

struct Symbol { std::string name; GuestAddr address; std::uint64_t size; bool function; };
struct ElfSpec {
  std::string soname, interp;
  std::vector<Symbol> exports;   // .dynsym and .symtab
  std::vector<Symbol> internal;  // .symtab only (bionic's __dl_ linker internals)
  std::uint64_t data_size = 0;   // trailing RW segment (linker globals)
  bool program = false;
};
struct Layout { std::uint64_t phdr = 0, phnum = 0, dynamic = 0, debug_slot = 0, data = 0, size = 0, ro_size = 0; };

class Strings {
 public:
  Strings() { text_.push_back('\0'); }
  std::uint32_t add(std::string_view s) {
    if (s.empty()) return 0;
    if (auto it = offsets_.find(std::string(s)); it != offsets_.end()) return it->second;
    auto offset = static_cast<std::uint32_t>(text_.size());
    text_.append(s); text_.push_back('\0'); offsets_.emplace(std::string(s), offset); return offset;
  }
  const std::string& text() const { return text_; }
 private:
  std::string text_;
  std::unordered_map<std::string, std::uint32_t> offsets_;
};
std::uint64_t align(std::uint64_t value, std::uint64_t to) { return (value + to - 1) & ~(to - 1); }
std::uint32_t elf_hash(std::string_view name) {
  std::uint32_t h = 0;
  for (unsigned char c : name) { h = (h << 4) + c; auto g = h & 0xf0000000u; if (g) h ^= g >> 24; h &= ~g; }
  return h;
}
// ELF32 forms (gABI): the same fields in 32-bit words, with Phdr and Sym reordered.
struct Ehdr32 { std::uint8_t ident[16]; std::uint16_t type, machine; std::uint32_t version, entry, phoff, shoff, flags;
                std::uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr32 { std::uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };
struct Shdr32 { std::uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize; };
struct Sym32 { std::uint32_t name, value, size; std::uint8_t info, other; std::uint16_t shndx; };
struct Dyn32 { std::int32_t tag; std::uint32_t value; };
static_assert(sizeof(Ehdr32) == 52 && sizeof(Phdr32) == 32 && sizeof(Shdr32) == 40 && sizeof(Sym32) == 16 && sizeof(Dyn32) == 8);
struct ElfSizes { std::uint64_t ehdr, phdr, shdr, sym, dyn, word; };
ElfSizes elf_sizes(bool is_wide) {
  return is_wide ? ElfSizes{sizeof(Ehdr), sizeof(Phdr), sizeof(Shdr), sizeof(Sym), sizeof(Dyn), 8}
                 : ElfSizes{sizeof(Ehdr32), sizeof(Phdr32), sizeof(Shdr32), sizeof(Sym32), sizeof(Dyn32), 4};
}
// Records are built in the 64-bit forms; a 32-bit image stores each value
// modulo 2^32, as its own address arithmetic does.
template <class T> void append_as(std::string& out, const T& v) { out.append(reinterpret_cast<const char*>(&v), sizeof(v)); }
void encode(std::string& out, const Ehdr& e, bool is_wide) {
  if (is_wide) return append_as(out, e);
  Ehdr32 n{};
  std::memcpy(n.ident, e.ident, 16);
  n.ident[4] = 1;  // ELFCLASS32
  n.type = e.type; n.machine = 40;  // EM_ARM
  n.version = e.version; n.entry = std::uint32_t(e.entry); n.phoff = std::uint32_t(e.phoff); n.shoff = std::uint32_t(e.shoff);
  n.flags = 0x05000200;  // EF_ARM_EABI_VER5 | EF_ARM_ABI_FLOAT_SOFT, as Android armeabi-v7a objects carry
  n.ehsize = sizeof(Ehdr32); n.phentsize = sizeof(Phdr32); n.phnum = e.phnum; n.shentsize = sizeof(Shdr32);
  n.shnum = e.shnum; n.shstrndx = e.shstrndx;
  append_as(out, n);
}
void encode(std::string& out, const Phdr& p, bool is_wide) {
  if (is_wide) return append_as(out, p);
  append_as(out, Phdr32{p.type, std::uint32_t(p.offset), std::uint32_t(p.vaddr), std::uint32_t(p.paddr), std::uint32_t(p.filesz),
                        std::uint32_t(p.memsz), p.flags, std::uint32_t(p.align)});
}
void encode(std::string& out, const Shdr& h, bool is_wide) {
  if (is_wide) return append_as(out, h);
  append_as(out, Shdr32{h.name, h.type, std::uint32_t(h.flags), std::uint32_t(h.addr), std::uint32_t(h.offset), std::uint32_t(h.size),
                        h.link, h.info, std::uint32_t(h.addralign), std::uint32_t(h.entsize)});
}
void encode(std::string& out, const Sym& y, bool is_wide) {
  if (is_wide) return append_as(out, y);
  append_as(out, Sym32{y.name, std::uint32_t(y.value), std::uint32_t(y.size), y.info, y.other, y.shndx});
}
void encode(std::string& out, const Dyn& d, bool is_wide) {
  if (is_wide) return append_as(out, d);
  append_as(out, Dyn32{std::int32_t(d.tag), std::uint32_t(d.value)});
}
template <class T> std::string encode_all(const std::vector<T>& items, bool is_wide) {
  std::string out;
  for (const auto& item : items) encode(out, item, is_wide);
  return out;
}

// Bytes of an image whose file offsets equal its virtual addresses. Symbol
// values are relative to `base`; HLE thunks may lie outside the image, which
// st_value expresses by unsigned wrap-around like any load-bias arithmetic.
std::string build_elf(const ElfSpec& spec, GuestAddr base, Layout* layout) {
  enum Section { kNull, kText, kData, kDynsym, kDynstr, kHash, kDynamic, kSymtab, kStrtab, kShstrtab, kInterp, kCount };
  const bool is_wide = wide();
  const auto z = elf_sizes(is_wide);
  Strings dynstr, strtab, shstr;
  std::vector<Sym> dynsym(1), symtab(1);
  const auto symbol = [&](const Symbol& s, Strings& strings) {
    const bool data = !s.function;
    const bool inside_data = spec.data_size && layout->data && s.address >= base + layout->data;
    return Sym{strings.add(s.name), std::uint8_t((1 << 4) | (data ? 1 : 2)), 0,
               std::uint16_t(inside_data ? kData : kText), s.address - base, s.size};
  };
  // Pass 1 sizes the layout, pass 2 writes with final symbol values.
  std::string out;
  for (int pass = 0; pass < 2; ++pass) {
    dynstr = {}; strtab = {}; shstr = {}; dynsym.assign(1, {}); symtab.assign(1, {});
    const auto soname = spec.program ? 0 : dynstr.add(spec.soname);
    for (const auto& s : spec.exports) { dynsym.push_back(symbol(s, dynstr)); symtab.push_back(symbol(s, strtab)); }
    for (const auto& s : spec.internal) symtab.push_back(symbol(s, strtab));
    const std::uint32_t nbucket = std::max<std::uint32_t>(1, std::uint32_t(dynsym.size() / 2 + 1));
    std::vector<std::uint32_t> hash(2 + nbucket + dynsym.size());
    hash[0] = nbucket; hash[1] = std::uint32_t(dynsym.size());
    for (std::uint32_t i = 1; i < dynsym.size(); ++i) {
      auto& bucket = hash[2 + elf_hash(dynstr.text().c_str() + dynsym[i].name) % nbucket];
      hash[2 + nbucket + i] = bucket; bucket = i;
    }
    std::vector<Dyn> dynamic{{DT_HASH, 0}, {DT_STRTAB, 0}, {DT_SYMTAB, 0}, {DT_STRSZ, dynstr.text().size()}, {DT_SYMENT, z.sym}};
    if (spec.program) { dynamic.push_back({DT_DEBUG, 0}); dynamic.push_back({DT_FLAGS_1, 0x08000000}); }  // DF_1_PIE
    else dynamic.push_back({DT_SONAME, soname});
    dynamic.push_back({DT_NULL, 0});
    const std::uint32_t names[kCount]{0, shstr.add(".text"), shstr.add(".data"), shstr.add(".dynsym"), shstr.add(".dynstr"),
        shstr.add(".hash"), shstr.add(".dynamic"), shstr.add(".symtab"), shstr.add(".strtab"), shstr.add(".shstrtab"),
        shstr.add(".interp")};

    const auto phnum = 4 + (spec.interp.empty() ? 0 : 1) + (spec.data_size ? 1 : 0);
    std::uint64_t at = z.ehdr;
    const auto phoff = at; at += phnum * z.phdr;
    const auto interp = at; at += spec.interp.empty() ? 0 : spec.interp.size() + 1;
    at = align(at, 8); const auto dynsym_at = at; at += dynsym.size() * z.sym;
    const auto dynstr_at = at; at += dynstr.text().size();
    at = align(at, 8); const auto hash_at = at; at += hash.size() * 4;
    at = align(at, 8); const auto dynamic_at = at; at += dynamic.size() * z.dyn;
    const auto symtab_at = at; at += symtab.size() * z.sym;
    const auto strtab_at = at; at += strtab.text().size();
    const auto shstr_at = at; at += shstr.text().size();
    at = align(at, 8); const auto shoff = at; at += kCount * z.shdr;
    const auto ro_size = align(at, kGuestPageSize);
    const auto data_at = spec.data_size ? ro_size : 0;
    const auto size = ro_size + align(spec.data_size, kGuestPageSize);
    *layout = {phoff, std::uint64_t(phnum), dynamic_at, 0, data_at, size, ro_size};
    for (std::size_t i = 0; i < dynamic.size(); ++i) if (dynamic[i].tag == DT_DEBUG) layout->debug_slot = dynamic_at + i * z.dyn + z.word;
    if (pass == 0) continue;

    // Like a mapped file: the RW segment's contents are runtime state, not file bytes.
    out.assign(ro_size, '\0');
    const auto put = [&](std::uint64_t offset, const void* p, std::size_t n) { std::memcpy(out.data() + offset, p, n); };
    const auto put_bytes = [&](std::uint64_t offset, const std::string& bytes) { put(offset, bytes.data(), bytes.size()); };
    for (auto& d : dynamic) {
      if (d.tag == DT_HASH) d.value = hash_at;
      if (d.tag == DT_STRTAB) d.value = dynstr_at;
      if (d.tag == DT_SYMTAB) d.value = dynsym_at;
    }
    std::string header;
    encode(header, Ehdr{{0x7f, 'E', 'L', 'F', 2, 1, 1}, 3, 183, 1, 0, phoff, shoff, 0, sizeof(Ehdr), sizeof(Phdr),
                        std::uint16_t(phnum), sizeof(Shdr), kCount, kShstrtab}, is_wide);
    put_bytes(0, header);
    const auto ph_bytes = phnum * z.phdr;
    std::vector<Phdr> ph;
    ph.push_back({PT_PHDR, 4, phoff, phoff, phoff, ph_bytes, ph_bytes, z.word});
    if (!spec.interp.empty()) ph.push_back({PT_INTERP, 4, interp, interp, interp, spec.interp.size() + 1, spec.interp.size() + 1, 1});
    ph.push_back({PT_LOAD, 4, 0, 0, 0, ro_size, ro_size, kGuestPageSize});
    if (spec.data_size) ph.push_back({PT_LOAD, 6, data_at, data_at, data_at, 0, size - data_at, kGuestPageSize});
    ph.push_back({PT_DYNAMIC, 4, dynamic_at, dynamic_at, dynamic_at, dynamic.size() * z.dyn, dynamic.size() * z.dyn, z.word});
    ph.push_back({PT_GNU_STACK, 6, 0, 0, 0, 0, 0, 16});
    put_bytes(phoff, encode_all(ph, is_wide));
    if (!spec.interp.empty()) put(interp, spec.interp.c_str(), spec.interp.size() + 1);
    put_bytes(dynsym_at, encode_all(dynsym, is_wide));
    put(dynstr_at, dynstr.text().data(), dynstr.text().size());
    put(hash_at, hash.data(), hash.size() * 4);
    put_bytes(dynamic_at, encode_all(dynamic, is_wide));
    put_bytes(symtab_at, encode_all(symtab, is_wide));
    put(strtab_at, strtab.text().data(), strtab.text().size());
    put(shstr_at, shstr.text().data(), shstr.text().size());
    std::vector<Shdr> sh(kCount);
    // Function symbols are HLE thunks in executable guest memory. The section
    // only gives them a defining index; it spans no file bytes of its own.
    sh[kText] = {names[kText], 1, 6, 0, 0, 0, 0, 0, 16, 0};
    if (spec.data_size) sh[kData] = {names[kData], 8, 3, data_at, data_at, size - data_at, 0, 0, 16, 0};
    else sh[kData] = {names[kData], 8, 3, 0, 0, 0, 0, 0, 16, 0};
    sh[kDynsym] = {names[kDynsym], 11, 2, dynsym_at, dynsym_at, dynsym.size() * z.sym, kDynstr, 1, z.word, z.sym};
    sh[kDynstr] = {names[kDynstr], 3, 2, dynstr_at, dynstr_at, dynstr.text().size(), 0, 0, 1, 0};
    sh[kHash] = {names[kHash], 5, 2, hash_at, hash_at, hash.size() * 4, kDynsym, 0, z.word, 4};
    sh[kDynamic] = {names[kDynamic], 6, 3, dynamic_at, dynamic_at, dynamic.size() * z.dyn, kDynstr, 0, z.word, z.dyn};
    sh[kSymtab] = {names[kSymtab], 2, 0, 0, symtab_at, symtab.size() * z.sym, kStrtab, 1, z.word, z.sym};
    sh[kStrtab] = {names[kStrtab], 3, 0, 0, strtab_at, strtab.text().size(), 0, 0, 1, 0};
    sh[kShstrtab] = {names[kShstrtab], 3, 0, 0, shstr_at, shstr.text().size(), 0, 0, 1, 0};
    if (!spec.interp.empty()) sh[kInterp] = {names[kInterp], 1, 2, interp, interp, spec.interp.size() + 1, 0, 0, 1, 0};
    else sh[kInterp] = {names[kInterp], 1, 2, 0, 0, 0, 0, 0, 1, 0};
    put_bytes(shoff, encode_all(sh, is_wide));
  }
  return out;
}

// ---- bionic structures --------------------------------------------------------------------
// Guest words are 8 bytes in an A64 process and 4 in an A32 one.
std::uint64_t word() { return wide() ? 8 : 4; }
GuestAddr load_word(GuestAddr p) {
  if (wide()) { std::uint64_t v; std::memcpy(&v, g2h(p), 8); return v; }
  std::uint32_t v; std::memcpy(&v, g2h(p), 4); return v;
}
void store_word(GuestAddr p, std::uint64_t v) {
  if (wide()) { std::memcpy(g2h(p), &v, 8); return; }
  const auto n = std::uint32_t(v); std::memcpy(g2h(p), &n, 4);
}
template <class T> void store(GuestAddr p, T v) { std::memcpy(g2h(p), &v, sizeof(v)); }
template <class T> T load(GuestAddr p) { T v; std::memcpy(&v, g2h(p), sizeof(v)); return v; }

// bionic/libc/include/link.h. link_map is five words in either class.
struct LinkMap { GuestAddr addr, name, ld, next, prev; };
struct RDebug { std::int32_t version; std::uint32_t pad; GuestAddr map, brk; std::int32_t state; std::uint32_t pad2; GuestAddr ldbase; };
struct RDebug32 { std::int32_t version; std::uint32_t map, brk; std::int32_t state; std::uint32_t ldbase; };
static_assert(sizeof(RDebug) == 40 && sizeof(RDebug32) == 20);
enum { RT_CONSISTENT = 0, RT_ADD = 1, RT_DELETE = 2 };
// android12L soinfo, 64-bit: head (phdr, phnum) then the body. Only fields
// with stable meaning are filled; the rest stay zero as for a fresh object.
struct Soinfo {
  GuestAddr phdr; std::uint64_t phnum;
  GuestAddr base; std::uint64_t size; GuestAddr dynamic; GuestAddr next;
  std::uint32_t flags; std::uint32_t pad0;
  GuestAddr strtab, symtab; std::uint64_t nbucket, nchain; GuestAddr bucket, chain;
  GuestAddr plt_rela; std::uint64_t plt_rela_count; GuestAddr rela; std::uint64_t rela_count;
  GuestAddr preinit_array; std::uint64_t preinit_array_count; GuestAddr init_array; std::uint64_t init_array_count;
  GuestAddr fini_array; std::uint64_t fini_array_count; GuestAddr init_func, fini_func;
  std::uint64_t ref_count;
  LinkMap link_map_head;
  std::uint8_t constructors_called; std::uint8_t pad1[7];
  GuestAddr load_bias;
  std::uint8_t has_dt_symbolic; std::uint8_t pad2[3];
  std::uint32_t version;
  std::uint64_t st_dev, st_ino;
  GuestAddr children_head, children_tail, parents_head, parents_tail;
  std::int64_t file_offset; std::uint32_t rtld_flags, dt_flags_1; std::uint64_t strtab_size;
  std::uint64_t gnu_nbucket; GuestAddr gnu_bucket, gnu_chain; std::uint32_t gnu_maskwords, gnu_shift2; GuestAddr gnu_bloom_filter;
  GuestAddr local_group_root; GuestAddr android_relocs; std::uint64_t android_relocs_size;
  GuestAddr soname;
  std::uint64_t realpath[3];  // libc++ std::string, long form
  GuestAddr versym, verdef_ptr; std::uint64_t verdef_cnt; GuestAddr verneed_ptr; std::uint64_t verneed_cnt;
  std::int32_t target_sdk_version;
};
static_assert(offsetof(Soinfo, base) == 16 && offsetof(Soinfo, next) == 40 && offsetof(Soinfo, link_map_head) == 208 &&
              offsetof(Soinfo, load_bias) == 256 && offsetof(Soinfo, version) == 268 && offsetof(Soinfo, parents_head) == 304 &&
              offsetof(Soinfo, rtld_flags) == 328 && offsetof(Soinfo, realpath) == 416);
// The 32-bit ARM soinfo: bionic's linker builds with __work_around_b_24465209__
// there (old_name_ and the unused words), plus plt_got_, ARM_exidx and
// has_text_relocations. dev_t and ino_t are 32-bit; off64_t is 8-aligned.
struct LinkMap32 { std::uint32_t addr, name, ld, next, prev; };
struct Soinfo32 {
  char old_name[128];
  std::uint32_t phdr, phnum, unused0, base, size, unused1, dynamic, unused2, unused3, next, flags;
  std::uint32_t strtab, symtab, nbucket, nchain, bucket, chain, plt_got, plt_rel, plt_rel_count, rel, rel_count;
  std::uint32_t preinit_array, preinit_array_count, init_array, init_array_count, fini_array, fini_array_count, init_func, fini_func;
  std::uint32_t arm_exidx, arm_exidx_count, ref_count;
  LinkMap32 link_map_head;
  std::uint8_t constructors_called, pad1[3];
  std::uint32_t load_bias;
  std::uint8_t has_text_relocations, has_dt_symbolic, pad2[2];
  std::uint32_t version, st_dev, st_ino;
  std::uint32_t children_head, children_tail, parents_head, parents_tail;
  std::int64_t file_offset; std::uint32_t rtld_flags, dt_flags_1, strtab_size;
  std::uint32_t gnu_nbucket, gnu_bucket, gnu_chain, gnu_maskwords, gnu_shift2, gnu_bloom_filter;
  std::uint32_t local_group_root, android_relocs, android_relocs_size, soname;
  std::uint32_t realpath[3];  // libc++ std::string, long form
  std::uint32_t versym, verdef_ptr, verdef_cnt, verneed_ptr, verneed_cnt;
  std::int32_t target_sdk_version;
};
static_assert(offsetof(Soinfo32, phdr) == 128 && offsetof(Soinfo32, base) == 140 && offsetof(Soinfo32, next) == 164 &&
              offsetof(Soinfo32, ref_count) == 256 && offsetof(Soinfo32, link_map_head) == 260 &&
              offsetof(Soinfo32, load_bias) == 284 && offsetof(Soinfo32, version) == 292 &&
              offsetof(Soinfo32, parents_head) == 312 && offsetof(Soinfo32, file_offset) == 320 &&
              offsetof(Soinfo32, rtld_flags) == 328 && offsetof(Soinfo32, soname) == 376 && offsetof(Soinfo32, realpath) == 380 &&
              offsetof(Soinfo32, target_sdk_version) == 412);
constexpr std::uint32_t FLAG_LINKED = 1, FLAG_EXE = 4, FLAG_NEW_FORMAT = 0x40000000;
constexpr std::uint64_t kSoinfoSize = 1024;  // bionic's soinfo is larger than the fields above
constexpr std::uint64_t kNone = ~0ull;
struct SoinfoLayout {
  std::uint64_t phdr, phnum, base, size, dynamic, next, flags, ref_count, link_map, constructors_called, load_bias,
      version, rtld_flags, soname, realpath, target_sdk_version, old_name;
};
#define VENTURE_SOINFO_LAYOUT(T, old) SoinfoLayout{offsetof(T, phdr), offsetof(T, phnum), offsetof(T, base), offsetof(T, size), \
    offsetof(T, dynamic), offsetof(T, next), offsetof(T, flags), offsetof(T, ref_count), offsetof(T, link_map_head),           \
    offsetof(T, constructors_called), offsetof(T, load_bias), offsetof(T, version), offsetof(T, rtld_flags),                   \
    offsetof(T, soname), offsetof(T, realpath), offsetof(T, target_sdk_version), old}
const SoinfoLayout& soinfo_layout() {
  static const SoinfoLayout l64 = VENTURE_SOINFO_LAYOUT(Soinfo, kNone), l32 = VENTURE_SOINFO_LAYOUT(Soinfo32, 0);
  return wide() ? l64 : l32;
}
#undef VENTURE_SOINFO_LAYOUT

struct LinkerData {  // the linker's RW segment
  std::uint64_t dl_mutex[5];  // pthread_mutex_t, PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
  RDebug r_debug;
  GuestAddr solist, sonext, somain;
};
struct LinkerData32 {
  std::uint32_t dl_mutex;  // 32-bit pthread_mutex_t is one word
  RDebug32 r_debug;
  std::uint32_t solist, sonext, somain;
};
struct LinkerLayout {
  std::uint64_t size, dl_mutex, dl_mutex_size, r_debug, r_debug_size, r_map, r_brk, r_state, r_ldbase, solist, sonext, somain;
};
#define VENTURE_LINKER_LAYOUT(T, R) LinkerLayout{sizeof(T), offsetof(T, dl_mutex), sizeof(T::dl_mutex), offsetof(T, r_debug), \
    sizeof(R), offsetof(T, r_debug) + offsetof(R, map), offsetof(T, r_debug) + offsetof(R, brk),                               \
    offsetof(T, r_debug) + offsetof(R, state), offsetof(T, r_debug) + offsetof(R, ldbase),                                     \
    offsetof(T, solist), offsetof(T, sonext), offsetof(T, somain)}
const LinkerLayout& linker_layout() {
  static const LinkerLayout l64 = VENTURE_LINKER_LAYOUT(LinkerData, RDebug), l32 = VENTURE_LINKER_LAYOUT(LinkerData32, RDebug32);
  return wide() ? l64 : l32;
}
#undef VENTURE_LINKER_LAYOUT

// bionic/libc/bionic/pthread_internal.cpp: g_thread_list and its rwlock, in
// libc's data segment. The lock words are Venture's pthread_rwlock_t:
// hle/libc/threads.cpp Rwlock (state, sequence, writers, owner) on A64 and
// threads32.cpp Rwlock (state, sequence, owner, waiters) on A32.
struct LibcData {
  std::uint32_t thread_list_lock[14];  // pthread_rwlock_t
  GuestAddr thread_list;
};
struct LibcData32 {
  std::uint32_t thread_list_lock[10];  // 32-bit pthread_rwlock_t is 40 bytes
  std::uint32_t thread_list;
};
static_assert(sizeof(LibcData) == 64 && sizeof(LibcData32) == 44);
std::uint64_t libc_data_size() { return wide() ? sizeof(LibcData) : sizeof(LibcData32); }
std::uint64_t thread_list_offset() { return wide() ? offsetof(LibcData, thread_list) : offsetof(LibcData32, thread_list); }
std::uint64_t thread_list_lock_size() { return wide() ? sizeof(LibcData::thread_list_lock) : sizeof(LibcData32::thread_list_lock); }
// pthread_internal_t's leading fields (next, prev, tid, cached_pid); __pthread_start
// reads the start routine and its argument at kStartRoutineOffset.
constexpr std::uint64_t kStartRoutineOffset = 0x20;

struct Image {
  std::string path, bytes;  // bytes: the file as stored on the device
  GuestAddr base = 0;
  Layout layout;
};
struct Node {
  std::weak_ptr<Library> library;
  GuestAddr soinfo = 0;
  std::string path;
};
struct State {
  std::mutex mutex;
  bool published = false;
  Image program, linker;
  GuestAddr data = 0;  // LinkerData or LinkerData32
  GuestAddr linker_soinfo = 0, program_soinfo = 0, r_brk = 0;
  std::unordered_map<std::string, Image> images;              // by soname, HLE libraries
  std::map<std::string, std::string, std::less<>> files;     // guest path -> bytes
  std::map<GuestAddr, std::pair<const Image*, std::string>> entries;  // HLE entry -> (image, name)
  std::unordered_map<const Library*, Node> nodes;
  std::vector<SyntheticModule> synthetic_modules;
  std::map<std::string, GuestAddr, std::less<>> strings;
  GuestAddr pool = 0; std::uint64_t pool_used = 0;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> auxv;
  GuestAddr pthread_start = 0;
};
// Separate from State::mutex: thread start and exit take only this one.
struct ThreadList {
  std::mutex mutex;
  std::atomic<GuestAddr> data{0};  // LibcData, once libc's image exists
  std::unordered_map<GuestAddr, std::int32_t> members;  // handle -> tid
};
ThreadList& thread_list() { static auto* t = new ThreadList; return *t; }
State& state() { static auto* s = new State; return *s; }
GuestAddr linker_field(const State& s, std::uint64_t offset) { return s.data + offset; }

std::string library_path(std::string_view soname) {
  const std::string lib = wide() ? "lib64" : "lib";
  if (soname == "libc.so" || soname == "libm.so" || soname == "libdl.so")
    return "/apex/com.android.runtime/" + lib + "/bionic/" + std::string(soname);
  if (soname == "libart.so") return "/apex/com.android.art/" + lib + "/libart.so";
  return process_config().guest_system_lib_dir() + "/" + std::string(soname);
}
// Maps `spec` at a fresh base: read-only image, RW segment, file bytes recorded.
Image map_image(State& s, const ElfSpec& spec, std::string path, const std::function<ElfSpec(GuestAddr, const Layout&)>& finalize = {}) {
  Image image; image.path = std::move(path);
  // Size with finalize's symbols too: they only change values, never the count,
  // so a placeholder base sizes the same as the real one.
  build_elf(spec, 0, &image.layout);
  if (finalize) build_elf(finalize(0, image.layout), 0, &image.layout);
  const auto sized = image.layout;
  image.base = AddressSpace::get().allocate(image.layout.size, kProtRead | kProtWrite, image.path);
  if (!image.base) throw std::bad_alloc();
  const auto final_spec = finalize ? finalize(image.base, image.layout) : spec;
  image.bytes = build_elf(final_spec, image.base, &image.layout);
  if (image.layout.size != sized.size || image.layout.data != sized.data)
    throw std::logic_error("process image: " + image.path + " layout changed after sizing");
  std::memcpy(g2h(image.base), image.bytes.data(), image.bytes.size());
  AddressSpace::get().protect(image.base, image.layout.ro_size, kProtRead);
  label_memory(image.base, image.layout.ro_size, image.path, 0);
  if (image.layout.data) label_memory(image.base + image.layout.data, image.layout.size - image.layout.data, image.path, image.layout.data);
  s.files[image.path] = image.bytes;
  return image;
}
// bionic's linker allocator: soinfo and its strings are never returned, so
// tools walking the list concurrently with dlclose never touch freed memory.
constexpr std::uint64_t kPoolSize = kGuestPageSize * 4;
GuestAddr pool_alloc(State& s, std::uint64_t n) {
  n = align(n, 16);
  if (n > kPoolSize) throw std::length_error("process image: linker allocation too large");
  if (!s.pool || s.pool_used + n > kPoolSize) {
    s.pool = AddressSpace::get().allocate(kPoolSize, kProtRead | kProtWrite, "[anon:linker_alloc]");
    if (!s.pool) throw std::bad_alloc();
    s.pool_used = 0;
  }
  auto p = s.pool + s.pool_used; s.pool_used += n;
  std::memset(g2h(p), 0, n);
  return p;
}
GuestAddr new_soinfo(State& s) { return pool_alloc(s, kSoinfoSize); }
GuestAddr guest_string(State& s, std::string_view text) {
  if (const auto it = s.strings.find(text); it != s.strings.end()) return it->second;
  auto p = pool_alloc(s, text.size() + 1);
  std::memcpy(g2h(p), text.data(), text.size());
  s.strings.emplace(text, p);
  return p;
}
GuestAddr map_of(GuestAddr si) { return si + soinfo_layout().link_map; }
GuestAddr realpath_data(GuestAddr si) { return si + soinfo_layout().realpath + 2 * word(); }
void fill_soinfo(State& s, GuestAddr address, const std::string& path, GuestAddr base, std::uint64_t size, GuestAddr bias,
                 GuestAddr phdr, std::uint64_t phnum, GuestAddr dynamic, std::uint32_t flags, std::uint32_t rtld_flags) {
  const auto& o = soinfo_layout();
  const auto w = word();
  const bool fresh = !load<std::uint32_t>(address + o.version);
  store_word(address + o.phdr, phdr); store_word(address + o.phnum, phnum); store_word(address + o.base, base);
  store_word(address + o.size, size); store_word(address + o.dynamic, dynamic);
  store<std::uint32_t>(address + o.flags, FLAG_LINKED | FLAG_NEW_FORMAT | flags);
  store_word(address + o.load_bias, bias);
  store<std::uint32_t>(address + o.version, 6);
  store<std::uint32_t>(address + o.rtld_flags, rtld_flags);
  store<std::uint8_t>(address + o.constructors_called, 1);
  store_word(address + o.ref_count, 1);
  store<std::int32_t>(address + o.target_sdk_version, process_config().sdk_level);
  const auto map = address + o.link_map;
  store_word(map, bias); store_word(map + 2 * w, dynamic);  // l_addr, l_ld
  if (fresh || !load_word(realpath_data(address))) {
    const auto text = guest_string(s, path);
    const auto realpath = address + o.realpath;
    store_word(realpath, align(path.size() + 1, 16) | 1); store_word(realpath + w, path.size()); store_word(realpath + 2 * w, text);
    store_word(map + w, text);  // l_name
    const auto slash = path.rfind('/');
    const auto name = slash == path.npos ? path : path.substr(slash + 1);
    store_word(address + o.soname, guest_string(s, name));
    if (o.old_name != kNone) {  // soinfo::set_soname copies it into old_name_ on 32-bit
      auto* old = g2h<char>(address + o.old_name);
      const auto n = std::min<std::size_t>(name.size(), 127);
      std::memcpy(old, name.data(), n); old[n] = '\0';
    }
  }
}
GuestAddr dynamic_of(const Library& l) {
  const auto entry = wide() ? sizeof(Phdr) : sizeof(Phdr32);
  for (std::uint64_t i = 0; i < l.phnum; ++i) {
    const auto at = l.phdr + i * entry;
    if (wide()) { const auto p = load<Phdr>(at); if (p.type == PT_DYNAMIC) return l.load_bias + p.vaddr; }
    else { const auto p = load<Phdr32>(at); if (p.type == PT_DYNAMIC) return std::uint32_t(l.load_bias + p.vaddr); }
  }
  return 0;
}

// ---- linker internals called by guest code ------------------------------------------------
GuestAddr solist_get_head() { auto& s = state(); return s.data ? load_word(linker_field(s, linker_layout().solist)) : 0; }
GuestAddr solist_get_somain() { auto& s = state(); return s.data ? load_word(linker_field(s, linker_layout().somain)) : 0; }
GuestAddr soinfo_get_realpath(GuestAddr si) { return si ? load_word(realpath_data(si)) : 0; }
GuestAddr soinfo_get_soname(GuestAddr si) { return si ? load_word(si + soinfo_layout().soname) : 0; }

const Image* image_for(const State& s, const Library& l) {
  if (auto it = s.images.find(l.soname()); it != s.images.end() && l.is_hle() && !l.mapped_image()) return &it->second;
  return nullptr;
}
void build_library_image(State& s, const HleLibrary& lib) {
  if (s.images.contains(lib.soname()) || lib.soname() == kLoaderSoname) return;
  ElfSpec spec; spec.soname = lib.soname();
  for (auto& [name, address] : lib.functions()) if (!name.starts_with("<")) spec.exports.push_back({name, address, 16, true});
  std::sort(spec.exports.begin(), spec.exports.end(), [](auto& a, auto& b) { return a.name < b.name; });
  const bool libc = lib.soname() == "libc.so" && s.pthread_start;
  if (libc) spec.data_size = libc_data_size();
  auto& image = s.images[lib.soname()] = map_image(s, spec, library_path(lib.soname()), [&](GuestAddr base, const Layout& layout) {
    ElfSpec full = spec;
    if (!libc) return full;
    const auto d = base + layout.data;
    full.internal.push_back({"_ZL13g_thread_list", d + thread_list_offset(), word(), false});
    full.internal.push_back({"_ZL18g_thread_list_lock", d, thread_list_lock_size(), false});
    full.internal.push_back({"_ZL15__pthread_startPv", s.pthread_start, wide() ? 40u : 36u, true});
    return full;
  });
  for (auto& e : spec.exports) s.entries.try_emplace(e.address, &image, e.name);
  if (libc) {
    s.entries.try_emplace(s.pthread_start, &image, "__pthread_start");
    const auto d = image.base + image.layout.data;
    std::memset(g2h(d), 0, libc_data_size());
    thread_list().data.store(d, std::memory_order_release);
  }
}
void notify(State& s, int transition) {
  // bionic calls rtld_db_dlactivity on every change; it is an empty function
  // unless a debugger or in-process tool has hooked it.
  auto* thread = GuestThread::current();
  const auto first = load<std::uint32_t>(s.r_brk);
  store<std::int32_t>(linker_field(s, linker_layout().r_state), transition);
  if (thread && first != nop()) thread->call(s.r_brk, {});
}
// bionic: start_routine(arg) on the new thread, then pthread_exit(result).
// Tools locate the start routine's offset from the load pair and the
// register call after it (Frida: LDP/BLR on arm64, LDRD/BLX on ARM).
GuestAddr pthread_start_stub(GuestAddr pthread_exit) {
  static_assert(kStartRoutineOffset == 0x20);
  if (wide())
    return hle::code_stub(std::array<std::uint32_t, 10>{
        0xa9bf7bfdu,  // stp x29, x30, [sp, #-16]!
        0x910003fdu,  // mov x29, sp
        kNop, kNop,
        0xa9420010u,  // ldp x16, x0, [x0, #0x20]
        0xd63f0200u,  // blr x16
        0x58000050u,  // ldr x16, 1f
        0xd61f0200u,  // br x16
        std::uint32_t(pthread_exit), std::uint32_t(pthread_exit >> 32)}, "__pthread_start");  // 1: pthread_exit
  return hle::code_stub(std::array<std::uint32_t, 9>{
      0xe92d4010u,  // push {r4, lr}
      kArmNop, kArmNop, kArmNop,
      0xe1c022d0u,  // ldrd r2, r3, [r0, #0x20]
      0xe1a00003u,  // mov r0, r3
      0xe12fff32u,  // blx r2
      0xe51ff004u,  // ldr pc, [pc, #-4]
      std::uint32_t(pthread_exit)}, "__pthread_start");
}
}  // namespace

void publish() {
  auto& s = state();
  std::shared_ptr<const HleLibrary> loader;
  GuestAddr pthread_exit = 0;
  for (auto& l : Linker::get().hle_libraries()) {
    if (l->soname() == kLoaderSoname) loader = l;
    if (l->soname() == "libc.so")
      for (auto& [name, address] : l->functions()) if (name == "pthread_exit") pthread_exit = address;
  }
  if (!loader) throw std::runtime_error("process image: ld-android.so is not registered");
  {
    std::lock_guard lock(s.mutex);
    if (s.published) return;
    const auto w = word();
    const auto& ll = linker_layout();
    const std::string linker_file(linker_path()), program_file(program_path_view());
    // The linker: loader exports in .dynsym, bionic's internal names in .symtab.
    s.r_brk = wide() ? hle::code_stub(std::array<std::uint32_t, 5>{kNop, kNop, kNop, kNop, kRet}, "rtld_db_dlactivity")
                     : hle::code_stub(std::array<std::uint32_t, 5>{kArmNop, kArmNop, kArmNop, kArmNop, kArmBxLr}, "rtld_db_dlactivity");
    if (pthread_exit) s.pthread_start = pthread_start_stub(pthread_exit);
    std::vector<Symbol> functions{
        {"__dl__Z15solist_get_headv", hle::make_thunk(kLoaderSoname, "solist_get_head", hle::wrap<&solist_get_head>()), 16, true},
        {"__dl__Z17solist_get_somainv", hle::make_thunk(kLoaderSoname, "solist_get_somain", hle::wrap<&solist_get_somain>()), 16, true},
        {"__dl__ZNK6soinfo12get_realpathEv", hle::make_thunk(kLoaderSoname, "soinfo::get_realpath", hle::wrap<&soinfo_get_realpath>()), 16, true},
        {"__dl__ZNK6soinfo10get_sonameEv", hle::make_thunk(kLoaderSoname, "soinfo::get_soname", hle::wrap<&soinfo_get_soname>()), 16, true},
        {"__dl_rtld_db_dlactivity", s.r_brk, 20, true}};
    ElfSpec spec; spec.soname = std::string(kLoaderSoname); spec.data_size = ll.size;
    for (auto& [name, address] : loader->functions()) {
      spec.exports.push_back({name, address, 16, true});
      spec.internal.push_back({"__dl_" + name, address, 16, true});
    }
    spec.internal.insert(spec.internal.end(), functions.begin(), functions.end());
    s.linker = map_image(s, spec, linker_file, [&](GuestAddr base, const Layout& layout) {
      const auto d = base + layout.data;
      ElfSpec full = spec;
      full.exports.push_back({"_r_debug", d + ll.r_debug, ll.r_debug_size, false});
      full.internal.push_back({"__dl__r_debug", d + ll.r_debug, ll.r_debug_size, false});
      full.internal.push_back({"__dl__ZL10g_dl_mutex", d + ll.dl_mutex, ll.dl_mutex_size, false});
      full.internal.push_back({"__dl__ZL6solist", d + ll.solist, w, false});
      full.internal.push_back({"__dl__ZL6sonext", d + ll.sonext, w, false});
      full.internal.push_back({"__dl__ZL6somain", d + ll.somain, w, false});
      return full;
    });
    for (auto& e : spec.exports) s.entries.try_emplace(e.address, &s.linker, e.name);
    for (auto& f : functions) s.entries.try_emplace(f.address, &s.linker, f.name.substr(5));
    s.data = s.linker.base + s.linker.layout.data;
    std::memset(g2h(s.data), 0, ll.size);
    store<std::uint32_t>(linker_field(s, ll.dl_mutex), 0x4000);  // PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
    store<std::int32_t>(linker_field(s, ll.r_debug), 1);  // r_version
    store_word(linker_field(s, ll.r_brk), s.r_brk);
    store<std::int32_t>(linker_field(s, ll.r_state), RT_CONSISTENT);
    store_word(linker_field(s, ll.r_ldbase), s.linker.base);

    // The program: an app_process-shaped PIE whose DT_DEBUG points at r_debug.
    ElfSpec program; program.program = true; program.interp = std::string(linker_link());
    s.program = map_image(s, program, program_file);
    AddressSpace::get().protect(s.program.base, s.program.layout.ro_size, kProtRead | kProtWrite);
    store_word(s.program.base + s.program.layout.debug_slot, linker_field(s, ll.r_debug));
    AddressSpace::get().protect(s.program.base, s.program.layout.ro_size, kProtRead);

    s.linker_soinfo = new_soinfo(s);
    fill_soinfo(s, s.linker_soinfo, linker_file, s.linker.base, s.linker.layout.size, s.linker.base,
                s.linker.base + s.linker.layout.phdr, s.linker.layout.phnum, s.linker.base + s.linker.layout.dynamic, 0, 0);
    s.program_soinfo = new_soinfo(s);
    fill_soinfo(s, s.program_soinfo, program_file, s.program.base, s.program.layout.size, s.program.base,
                s.program.base + s.program.layout.phdr, s.program.layout.phnum, s.program.base + s.program.layout.dynamic, FLAG_EXE, 0);
    store_word(linker_field(s, ll.somain), s.program_soinfo);

    // Linux fs/binfmt_elf.c create_elf_tables order. A 32-bit process on an
    // arm64 kernel reports COMPAT_ELF_PLATFORM "v8l" and the compat hwcaps.
    const auto execfn = guest_string(s, program_file), platform = guest_string(s, wide() ? "aarch64" : "v8l");
    const auto random_at = pool_alloc(s, 16);
    std::random_device device;
    for (int i = 0; i < 16; ++i) *g2h<std::uint8_t>(random_at + i) = std::uint8_t(device());
    const auto& cpu = cpu_backend().info();
    // No AT_SYSINFO_EHDR: Venture has no vDSO.
    s.auxv = {{16, cpu.hwcap}, {6, kGuestPageSize}, {17, 100},
              {3, s.program.base + s.program.layout.phdr}, {4, elf_sizes(wide()).phdr}, {5, s.program.layout.phnum},
              {7, s.linker.base}, {8, 0}, {9, s.program.base}, {11, 10125}, {12, 10125}, {13, 10125}, {14, 10125},
              {23, 0}, {25, random_at}, {26, cpu.hwcap2}, {31, execfn}, {15, platform}, {51, 5120}};
    s.published = true;
  }
  Linker::get().publish_process_view();
  // Threads that started before libc's image existed. The list lock nests
  // inside the registry lock only here, before any guest reader exists.
  for_each_guest_thread([](GuestThread& t) { thread_attached(t); });
}

namespace {
// Writer side of the guest rwlock: hle/libc/threads.cpp rw_acquire/rw_unlock
// on A64 (state, sequence, writers), threads32.cpp on A32 (state, sequence;
// no writer count). Readers hold it only while they walk the list. A reader
// that died holding it (a thread killed at exit) must not stall thread start
// or exit: after a bounded wait the list is updated without the lock.
bool lock_list(GuestAddr lock) {
  auto* words = g2h<std::uint32_t>(lock);
  std::atomic_ref state(words[0]);
  const bool counts_writers = wide();
  if (counts_writers) std::atomic_ref(words[2]).fetch_add(1);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool locked = false;
  for (;;) {
    std::uint32_t expected = 0;
    if (state.compare_exchange_weak(expected, UINT_MAX, std::memory_order_acquire)) { locked = true; break; }
    if (std::chrono::steady_clock::now() > deadline) break;
    std::this_thread::yield();
  }
  if (counts_writers) std::atomic_ref(words[2]).fetch_sub(1);
  if (!locked) log::warn("process-image", "g_thread_list_lock held for 2 s; updating the thread list without it");
  return locked;
}
void unlock_list(GuestAddr lock) {
  auto* words = g2h<std::uint32_t>(lock);
  std::atomic_ref(words[0]).store(0, std::memory_order_release);
  std::atomic_ref(words[1]).fetch_add(1, std::memory_order_release);
  futex_wake(lock + 4, INT_MAX);
}
// pthread_internal_t: next, prev, tid, cached_pid.
GuestAddr thread_next(GuestAddr t) { return t + 0; }
GuestAddr thread_prev(GuestAddr t) { return t + word(); }
}  // namespace

void thread_attached(GuestThread& thread) {
  auto& list = thread_list();
  const auto address = list.data.load(std::memory_order_acquire);
  const auto handle = thread.pthread_self;
  if (!address || !handle) return;
  std::lock_guard lock(list.mutex);
  if (!list.members.emplace(handle, thread.tid()).second) return;
  const auto head = address + thread_list_offset();
  const bool locked = lock_list(address);
  // __pthread_internal_add: push at the head.
  store<std::int32_t>(handle + 2 * word(), thread.tid());
  store<std::int32_t>(handle + 2 * word() + 4, kGuestPid);
  const auto next = load_word(head);
  store_word(thread_prev(handle), 0); store_word(thread_next(handle), next);
  if (next) store_word(thread_prev(next), handle);
  store_word(head, handle);
  if (locked) unlock_list(address);
}
void thread_detached(GuestThread& thread) {
  auto& list = thread_list();
  const auto address = list.data.load(std::memory_order_acquire);
  const auto handle = thread.pthread_self;
  if (!address || !handle) return;
  std::lock_guard lock(list.mutex);
  if (!list.members.erase(handle)) return;
  const auto head = address + thread_list_offset();
  const bool locked = lock_list(address);
  const auto next = load_word(thread_next(handle)), prev = load_word(thread_prev(handle));
  if (next) store_word(thread_prev(next), prev);
  if (prev) store_word(thread_next(prev), next);
  else store_word(head, next);
  store_word(thread_next(handle), 0); store_word(thread_prev(handle), 0);
  if (locked) unlock_list(address);
}
std::pair<GuestAddr, GuestAddr> thread_entry(GuestThread& thread, GuestAddr start, GuestAddr argument) {
  auto& s = state();
  if (!s.pthread_start || !thread.pthread_self || !thread_list().data.load(std::memory_order_acquire)) return {start, argument};
  store_word(thread.pthread_self + kStartRoutineOffset, start);
  store_word(thread.pthread_self + kStartRoutineOffset + word(), argument);
  return {s.pthread_start, thread.pthread_self};
}
bool published() { auto& s = state(); std::lock_guard lock(s.mutex); return s.published; }

std::vector<SyntheticModule> synthetic_modules() {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  return s.synthetic_modules;
}

GuestAddr module_name(std::string_view path) {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  return guest_string(s, path);
}

void sync(const std::vector<std::shared_ptr<Library>>& libraries) {
  auto& s = state();
  std::unique_lock lock(s.mutex);
  if (!s.published) return;
  for (auto& l : libraries) if (auto* hle = dynamic_cast<const HleLibrary*>(l.get())) build_library_image(s, *hle);
  // Desired order: linker, program, then libraries as loaded.
  std::vector<GuestAddr> order{s.linker_soinfo, s.program_soinfo};
  std::vector<SyntheticModule> synthetic;
  auto append_image = [&](const Image& image) {
    synthetic.push_back({image.path, image.base, image.base + image.layout.phdr,
                         static_cast<std::uint16_t>(image.layout.phnum)});
  };
  append_image(s.program);
  append_image(s.linker);
  std::unordered_map<const Library*, Node> live;
  bool added = false;
  for (auto& l : libraries) {
    if (l->soname() == kLoaderSoname) continue;
    const Image* image = image_for(s, *l);
    if (!image && !l->mapped_image()) continue;  // virtual placeholder without definitions
    if (image) append_image(*image);
    auto it = s.nodes.find(l.get());
    Node node;
    if (it != s.nodes.end() && !it->second.library.expired() && it->second.library.lock() == l) node = it->second;
    else { node.library = l; node.soinfo = new_soinfo(s); added = true; }
    const auto path = image ? image->path : l->path();
    if (node.path != path) { store_word(realpath_data(node.soinfo), 0); node.path = path; }
    const auto rtld = l->global ? 0x100u : 0u;
    if (image) fill_soinfo(s, node.soinfo, path, image->base, image->layout.size, image->base, image->base + image->layout.phdr,
                           image->layout.phnum, image->base + image->layout.dynamic, 0, rtld);
    else fill_soinfo(s, node.soinfo, path, l->map_start, l->map_size, l->load_bias, l->phdr, l->phnum, dynamic_of(*l), 0, rtld);
    order.push_back(node.soinfo);
    live.emplace(l.get(), std::move(node));
  }
  const bool removed = std::any_of(s.nodes.begin(), s.nodes.end(), [&](auto& p) { return !live.contains(p.first); });
  s.nodes = std::move(live);
  s.synthetic_modules = std::move(synthetic);
  if (!added && !removed) {
    // Paths and ranges may still change in place (a backing mapped later).
    return;
  }
  if (added || removed) { lock.unlock(); notify(s, added ? RT_ADD : RT_DELETE); lock.lock(); }
  // Retired soinfo memory is never reused: concurrent walkers stay safe.
  const auto& o = soinfo_layout();
  const auto w = word();
  const auto set_map = [&](GuestAddr si, std::optional<GuestAddr> next, std::optional<GuestAddr> prev) {
    if (next) store_word(map_of(si) + 3 * w, *next);  // l_next
    if (prev) store_word(map_of(si) + 4 * w, *prev);  // l_prev
  };
  for (std::size_t i = 0; i < order.size(); ++i) {
    store_word(order[i] + o.next, i + 1 < order.size() ? order[i + 1] : 0);
    set_map(order[i], i + 1 < order.size() ? map_of(order[i + 1]) : 0, i ? map_of(order[i - 1]) : 0);
  }
  // gdb's order: the executable heads r_map, then the linker, then libraries.
  set_map(s.program_soinfo, map_of(s.linker_soinfo), GuestAddr(0));
  set_map(s.linker_soinfo, order.size() > 2 ? map_of(order[2]) : 0, map_of(s.program_soinfo));
  if (order.size() > 2) set_map(order[2], std::nullopt, map_of(s.linker_soinfo));
  const auto& ll = linker_layout();
  store_word(linker_field(s, ll.solist), order.front());
  store_word(linker_field(s, ll.sonext), order.back());
  store_word(linker_field(s, ll.r_map), map_of(s.program_soinfo));
  lock.unlock();
  notify(s, RT_CONSISTENT);
}

bool address_info(GuestAddr address, DlInfo* out) {
  auto& s = state(); std::lock_guard lock(s.mutex);
  if (!s.published) return false;
  if (auto it = s.entries.find(address); it != s.entries.end()) {
    if (out) *out = {it->second.first->path, it->second.first->base, it->second.second, address};
    return true;
  }
  for (const auto* image : {&s.program, &s.linker})
    if (address >= image->base && address - image->base < image->layout.size) {
      if (out) *out = {image->path, image->base, {}, 0};
      return true;
    }
  for (auto& [soname, image] : s.images)
    if (address >= image.base && address - image.base < image.layout.size) {
      if (out) *out = {image.path, image.base, {}, 0};
      return true;
    }
  return false;
}
// The device's compatibility symlinks into the runtime APEX.
std::string canonical(std::string_view path) {
  if (path == linker_link()) return std::string(linker_path());
  for (std::string_view name : {"libc.so", "libm.so", "libdl.so"})
    if (path == process_config().guest_system_lib_dir() + "/" + std::string(name)) return library_path(name);
  return std::string(path);
}
std::optional<std::string> file(std::string_view path) {
  auto& s = state(); std::lock_guard lock(s.mutex);
  if (auto it = s.files.find(canonical(path)); it != s.files.end()) return it->second;
  return std::nullopt;
}
bool has_file(std::string_view path) {
  auto& s = state(); std::lock_guard lock(s.mutex);
  return s.files.find(canonical(path)) != s.files.end();
}
bool auxval(std::uint64_t type, std::uint64_t* value) {
  auto& s = state(); std::lock_guard lock(s.mutex);
  for (auto& [t, v] : s.auxv) if (t == type) { *value = v; return true; }
  return false;
}
// Elf64_auxv_t or Elf32_auxv_t pairs, ending with AT_NULL.
std::string auxv() {
  auto& s = state(); std::lock_guard lock(s.mutex);
  std::string out;
  const auto put = [&](std::uint64_t v) {
    if (wide()) out.append(reinterpret_cast<const char*>(&v), 8);
    else { const auto n = std::uint32_t(v); out.append(reinterpret_cast<const char*>(&n), 4); }
  };
  for (auto& [t, v] : s.auxv) { put(t); put(v); }
  put(0); put(0);
  return out;
}
std::string system_path(std::string_view soname) {
  if (soname == kLoaderSoname) return std::string(linker_path());
  return library_path(soname);
}
std::string program_path() { return std::string(program_path_view()); }
}  // namespace venture::process_image
