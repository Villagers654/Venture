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
constexpr std::string_view kProgramPath = "/system/bin/app_process64";
constexpr std::string_view kLinkerPath = "/apex/com.android.runtime/bin/linker64";
constexpr std::string_view kLoaderSoname = "ld-android.so";
constexpr std::uint32_t kRet = 0xd65f03c0u, kNop = 0xd503201fu;

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
// Bytes of an image whose file offsets equal its virtual addresses. Symbol
// values are relative to `base`; HLE thunks may lie outside the image, which
// st_value expresses by unsigned wrap-around like any load-bias arithmetic.
std::string build_elf(const ElfSpec& spec, GuestAddr base, Layout* layout) {
  enum Section { kNull, kText, kData, kDynsym, kDynstr, kHash, kDynamic, kSymtab, kStrtab, kShstrtab, kInterp, kCount };
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
    std::vector<Dyn> dynamic{{DT_HASH, 0}, {DT_STRTAB, 0}, {DT_SYMTAB, 0}, {DT_STRSZ, dynstr.text().size()}, {DT_SYMENT, sizeof(Sym)}};
    if (spec.program) { dynamic.push_back({DT_DEBUG, 0}); dynamic.push_back({DT_FLAGS_1, 0x08000000}); }  // DF_1_PIE
    else dynamic.push_back({DT_SONAME, soname});
    dynamic.push_back({DT_NULL, 0});
    const std::uint32_t names[kCount]{0, shstr.add(".text"), shstr.add(".data"), shstr.add(".dynsym"), shstr.add(".dynstr"),
        shstr.add(".hash"), shstr.add(".dynamic"), shstr.add(".symtab"), shstr.add(".strtab"), shstr.add(".shstrtab"),
        shstr.add(".interp")};

    const auto phnum = 4 + (spec.interp.empty() ? 0 : 1) + (spec.data_size ? 1 : 0);
    std::uint64_t at = sizeof(Ehdr);
    const auto phoff = at; at += phnum * sizeof(Phdr);
    const auto interp = at; at += spec.interp.empty() ? 0 : spec.interp.size() + 1;
    at = align(at, 8); const auto dynsym_at = at; at += dynsym.size() * sizeof(Sym);
    const auto dynstr_at = at; at += dynstr.text().size();
    at = align(at, 8); const auto hash_at = at; at += hash.size() * 4;
    at = align(at, 8); const auto dynamic_at = at; at += dynamic.size() * sizeof(Dyn);
    const auto symtab_at = at; at += symtab.size() * sizeof(Sym);
    const auto strtab_at = at; at += strtab.text().size();
    const auto shstr_at = at; at += shstr.text().size();
    at = align(at, 8); const auto shoff = at; at += kCount * sizeof(Shdr);
    const auto ro_size = align(at, kGuestPageSize);
    const auto data_at = spec.data_size ? ro_size : 0;
    const auto size = ro_size + align(spec.data_size, kGuestPageSize);
    *layout = {phoff, std::uint64_t(phnum), dynamic_at, 0, data_at, size, ro_size};
    for (std::size_t i = 0; i < dynamic.size(); ++i) if (dynamic[i].tag == DT_DEBUG) layout->debug_slot = dynamic_at + i * sizeof(Dyn) + 8;
    if (pass == 0) continue;

    // Like a mapped file: the RW segment's contents are runtime state, not file bytes.
    out.assign(ro_size, '\0');
    const auto put = [&](std::uint64_t offset, const void* p, std::size_t n) { std::memcpy(out.data() + offset, p, n); };
    for (auto& d : dynamic) {
      if (d.tag == DT_HASH) d.value = hash_at;
      if (d.tag == DT_STRTAB) d.value = dynstr_at;
      if (d.tag == DT_SYMTAB) d.value = dynsym_at;
    }
    Ehdr e{{0x7f, 'E', 'L', 'F', 2, 1, 1}, 3, 183, 1, 0, phoff, shoff, 0, sizeof(Ehdr), sizeof(Phdr), std::uint16_t(phnum),
           sizeof(Shdr), kCount, kShstrtab};
    put(0, &e, sizeof(e));
    std::vector<Phdr> ph;
    ph.push_back({PT_PHDR, 4, phoff, phoff, phoff, phnum * sizeof(Phdr), phnum * sizeof(Phdr), 8});
    if (!spec.interp.empty()) ph.push_back({PT_INTERP, 4, interp, interp, interp, spec.interp.size() + 1, spec.interp.size() + 1, 1});
    ph.push_back({PT_LOAD, 4, 0, 0, 0, ro_size, ro_size, kGuestPageSize});
    if (spec.data_size) ph.push_back({PT_LOAD, 6, data_at, data_at, data_at, 0, size - data_at, kGuestPageSize});
    ph.push_back({PT_DYNAMIC, 4, dynamic_at, dynamic_at, dynamic_at, dynamic.size() * sizeof(Dyn), dynamic.size() * sizeof(Dyn), 8});
    ph.push_back({PT_GNU_STACK, 6, 0, 0, 0, 0, 0, 16});
    put(phoff, ph.data(), ph.size() * sizeof(Phdr));
    if (!spec.interp.empty()) put(interp, spec.interp.c_str(), spec.interp.size() + 1);
    put(dynsym_at, dynsym.data(), dynsym.size() * sizeof(Sym));
    put(dynstr_at, dynstr.text().data(), dynstr.text().size());
    put(hash_at, hash.data(), hash.size() * 4);
    put(dynamic_at, dynamic.data(), dynamic.size() * sizeof(Dyn));
    put(symtab_at, symtab.data(), symtab.size() * sizeof(Sym));
    put(strtab_at, strtab.text().data(), strtab.text().size());
    put(shstr_at, shstr.text().data(), shstr.text().size());
    std::array<Shdr, kCount> sh{};
    // Function symbols are HLE thunks in executable guest memory. The section
    // only gives them a defining index; it spans no file bytes of its own.
    sh[kText] = {names[kText], 1, 6, 0, 0, 0, 0, 0, 16, 0};
    if (spec.data_size) sh[kData] = {names[kData], 8, 3, data_at, data_at, size - data_at, 0, 0, 16, 0};
    else sh[kData] = {names[kData], 8, 3, 0, 0, 0, 0, 0, 16, 0};
    sh[kDynsym] = {names[kDynsym], 11, 2, dynsym_at, dynsym_at, dynsym.size() * sizeof(Sym), kDynstr, 1, 8, sizeof(Sym)};
    sh[kDynstr] = {names[kDynstr], 3, 2, dynstr_at, dynstr_at, dynstr.text().size(), 0, 0, 1, 0};
    sh[kHash] = {names[kHash], 5, 2, hash_at, hash_at, hash.size() * 4, kDynsym, 0, 8, 4};
    sh[kDynamic] = {names[kDynamic], 6, 3, dynamic_at, dynamic_at, dynamic.size() * sizeof(Dyn), kDynstr, 0, 8, sizeof(Dyn)};
    sh[kSymtab] = {names[kSymtab], 2, 0, 0, symtab_at, symtab.size() * sizeof(Sym), kStrtab, 1, 8, sizeof(Sym)};
    sh[kStrtab] = {names[kStrtab], 3, 0, 0, strtab_at, strtab.text().size(), 0, 0, 1, 0};
    sh[kShstrtab] = {names[kShstrtab], 3, 0, 0, shstr_at, shstr.text().size(), 0, 0, 1, 0};
    if (!spec.interp.empty()) sh[kInterp] = {names[kInterp], 1, 2, interp, interp, spec.interp.size() + 1, 0, 0, 1, 0};
    else sh[kInterp] = {names[kInterp], 1, 2, 0, 0, 0, 0, 0, 1, 0};
    put(shoff, sh.data(), sizeof(sh));
  }
  return out;
}

// ---- bionic structures (LP64) -------------------------------------------------------------
struct LinkMap { GuestAddr addr, name, ld, next, prev; };
struct RDebug { std::int32_t version; std::uint32_t pad; GuestAddr map, brk; std::int32_t state; std::uint32_t pad2; GuestAddr ldbase; };
static_assert(sizeof(RDebug) == 40);
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
constexpr std::uint32_t FLAG_LINKED = 1, FLAG_EXE = 4, FLAG_NEW_FORMAT = 0x40000000;
constexpr std::uint64_t kSoinfoSize = 1024;  // bionic's soinfo is larger than the fields above

struct LinkerData {  // the linker's RW segment
  std::uint64_t dl_mutex[5];  // pthread_mutex_t, PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
  RDebug r_debug;
  GuestAddr solist, sonext, somain;
};

// bionic/libc/bionic/pthread_internal.cpp: g_thread_list and its rwlock, in
// libc's data segment. The lock word layout is Venture's pthread_rwlock_t
// (hle/libc/threads.cpp Rwlock: state, sequence, writers, owner).
struct LibcData {
  std::uint32_t thread_list_lock[14];  // pthread_rwlock_t
  GuestAddr thread_list;
};
static_assert(sizeof(LibcData) == 64);
// pthread_internal_t's leading fields; __pthread_start reads the start routine
// and its argument at kStartRoutineOffset.
struct BionicThread { GuestAddr next, prev; std::int32_t tid, cached_pid; };
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
  GuestAddr data = 0;  // LinkerData
  GuestAddr linker_soinfo = 0, program_soinfo = 0, r_brk = 0;
  std::unordered_map<std::string, Image> images;              // by soname, HLE libraries
  std::map<std::string, std::string, std::less<>> files;     // guest path -> bytes
  std::map<GuestAddr, std::pair<const Image*, std::string>> entries;  // HLE entry -> (image, name)
  std::unordered_map<const Library*, Node> nodes;
  std::vector<SyntheticModule> synthetic_modules;
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
LinkerData& data(State& s) { return *g2h<LinkerData>(s.data); }

std::string library_path(std::string_view soname) {
  if (soname == "libc.so" || soname == "libm.so" || soname == "libdl.so")
    return "/apex/com.android.runtime/lib64/bionic/" + std::string(soname);
  if (soname == "libart.so") return "/apex/com.android.art/lib64/libart.so";
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
  auto p = pool_alloc(s, text.size() + 1);
  std::memcpy(g2h(p), text.data(), text.size());
  return p;
}
void fill_soinfo(State& s, GuestAddr address, const std::string& path, GuestAddr base, std::uint64_t size, GuestAddr bias,
                 GuestAddr phdr, std::uint64_t phnum, GuestAddr dynamic, std::uint32_t flags, std::uint32_t rtld_flags) {
  auto& si = *g2h<Soinfo>(address);
  const bool fresh = !si.version;
  si.phdr = phdr; si.phnum = phnum; si.base = base; si.size = size; si.dynamic = dynamic;
  si.flags = FLAG_LINKED | FLAG_NEW_FORMAT | flags; si.load_bias = bias; si.version = 6; si.rtld_flags = rtld_flags;
  si.constructors_called = 1; si.ref_count = 1; si.target_sdk_version = process_config().sdk_level;
  si.link_map_head.addr = bias; si.link_map_head.ld = dynamic;
  if (fresh || !si.realpath[2]) {
    const auto text = guest_string(s, path);
    si.realpath[0] = align(path.size() + 1, 16) | 1; si.realpath[1] = path.size(); si.realpath[2] = text;
    si.link_map_head.name = text;
    const auto slash = path.rfind('/');
    si.soname = guest_string(s, slash == path.npos ? path : path.substr(slash + 1));
  }
}
GuestAddr dynamic_of(const Library& l) {
  for (std::uint64_t i = 0; i < l.phnum; ++i) {
    Phdr p; std::memcpy(&p, g2h(l.phdr + i * sizeof(Phdr)), sizeof(p));
    if (p.type == PT_DYNAMIC) return l.load_bias + p.vaddr;
  }
  return 0;
}

// ---- linker internals called by guest code ------------------------------------------------
GuestAddr solist_get_head() { auto& s = state(); return s.data ? data(s).solist : 0; }
GuestAddr solist_get_somain() { auto& s = state(); return s.data ? data(s).somain : 0; }
GuestAddr soinfo_get_realpath(GuestAddr si) { return si ? g2h<Soinfo>(si)->realpath[2] : 0; }
GuestAddr soinfo_get_soname(GuestAddr si) { return si ? g2h<Soinfo>(si)->soname : 0; }

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
  if (libc) spec.data_size = sizeof(LibcData);
  auto& image = s.images[lib.soname()] = map_image(s, spec, library_path(lib.soname()), [&](GuestAddr base, const Layout& layout) {
    ElfSpec full = spec;
    if (!libc) return full;
    const auto d = base + layout.data;
    full.internal.push_back({"_ZL13g_thread_list", d + offsetof(LibcData, thread_list), 8, false});
    full.internal.push_back({"_ZL18g_thread_list_lock", d + offsetof(LibcData, thread_list_lock), 56, false});
    full.internal.push_back({"_ZL15__pthread_startPv", s.pthread_start, 40, true});
    return full;
  });
  for (auto& e : spec.exports) s.entries.try_emplace(e.address, &image, e.name);
  if (libc) {
    s.entries.try_emplace(s.pthread_start, &image, "__pthread_start");
    const auto d = image.base + image.layout.data;
    std::memset(g2h(d), 0, sizeof(LibcData));
    thread_list().data.store(d, std::memory_order_release);
  }
}
void notify(State& s, int transition) {
  // bionic calls rtld_db_dlactivity on every change; it is an empty function
  // unless a debugger or in-process tool has hooked it.
  auto* thread = GuestThread::current();
  std::uint32_t first;
  std::memcpy(&first, g2h(s.r_brk), 4);
  auto& r = data(s).r_debug;
  r.state = transition;
  if (thread && first != kNop) thread->call(s.r_brk, {});
}
}  // namespace

void publish() {
  if (process_config().isa() != GuestIsa::a64) return;
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
    // The linker: loader exports in .dynsym, bionic's internal names in .symtab.
    s.r_brk = hle::code_stub(std::array<std::uint32_t, 5>{kNop, kNop, kNop, kNop, kRet}, "rtld_db_dlactivity");
    if (pthread_exit) {
      // bionic: start_routine(arg) on the new thread, then pthread_exit(result).
      // Tools locate the start routine's offset from this LDP and BLR.
      static_assert(kStartRoutineOffset == 0x20);
      s.pthread_start = hle::code_stub(std::array<std::uint32_t, 10>{
          0xa9bf7bfdu,  // stp x29, x30, [sp, #-16]!
          0x910003fdu,  // mov x29, sp
          kNop, kNop,
          0xa9420010u,  // ldp x16, x0, [x0, #0x20]
          0xd63f0200u,  // blr x16
          0x58000050u,  // ldr x16, 1f
          0xd61f0200u,  // br x16
          std::uint32_t(pthread_exit), std::uint32_t(pthread_exit >> 32)}, "__pthread_start");  // 1: pthread_exit
    }
    std::vector<Symbol> functions{
        {"__dl__Z15solist_get_headv", hle::make_thunk(kLoaderSoname, "solist_get_head", hle::wrap<&solist_get_head>()), 16, true},
        {"__dl__Z17solist_get_somainv", hle::make_thunk(kLoaderSoname, "solist_get_somain", hle::wrap<&solist_get_somain>()), 16, true},
        {"__dl__ZNK6soinfo12get_realpathEv", hle::make_thunk(kLoaderSoname, "soinfo::get_realpath", hle::wrap<&soinfo_get_realpath>()), 16, true},
        {"__dl__ZNK6soinfo10get_sonameEv", hle::make_thunk(kLoaderSoname, "soinfo::get_soname", hle::wrap<&soinfo_get_soname>()), 16, true},
        {"__dl_rtld_db_dlactivity", s.r_brk, 20, true}};
    ElfSpec spec; spec.soname = std::string(kLoaderSoname); spec.data_size = sizeof(LinkerData);
    for (auto& [name, address] : loader->functions()) {
      spec.exports.push_back({name, address, 16, true});
      spec.internal.push_back({"__dl_" + name, address, 16, true});
    }
    spec.internal.insert(spec.internal.end(), functions.begin(), functions.end());
    s.linker = map_image(s, spec, std::string(kLinkerPath), [&](GuestAddr base, const Layout& layout) {
      const auto d = base + layout.data;
      ElfSpec full = spec;
      full.exports.push_back({"_r_debug", d + offsetof(LinkerData, r_debug), sizeof(RDebug), false});
      full.internal.push_back({"__dl__r_debug", d + offsetof(LinkerData, r_debug), sizeof(RDebug), false});
      full.internal.push_back({"__dl__ZL10g_dl_mutex", d + offsetof(LinkerData, dl_mutex), 40, false});
      full.internal.push_back({"__dl__ZL6solist", d + offsetof(LinkerData, solist), 8, false});
      full.internal.push_back({"__dl__ZL6sonext", d + offsetof(LinkerData, sonext), 8, false});
      full.internal.push_back({"__dl__ZL6somain", d + offsetof(LinkerData, somain), 8, false});
      return full;
    });
    for (auto& e : spec.exports) s.entries.try_emplace(e.address, &s.linker, e.name);
    for (auto& f : functions) s.entries.try_emplace(f.address, &s.linker, f.name.substr(5));
    s.data = s.linker.base + s.linker.layout.data;
    auto& d = data(s);
    std::memset(&d, 0, sizeof(d));
    *reinterpret_cast<std::uint32_t*>(d.dl_mutex) = 0x4000;  // recursive
    d.r_debug.version = 1; d.r_debug.brk = s.r_brk; d.r_debug.state = RT_CONSISTENT; d.r_debug.ldbase = s.linker.base;

    // The program: an app_process64-shaped PIE whose DT_DEBUG points at r_debug.
    ElfSpec program; program.program = true; program.interp = "/system/bin/linker64";
    s.program = map_image(s, program, std::string(kProgramPath));
    AddressSpace::get().protect(s.program.base, s.program.layout.ro_size, kProtRead | kProtWrite);
    *g2h<GuestAddr>(s.program.base + s.program.layout.debug_slot) = s.linker.base + s.linker.layout.data + offsetof(LinkerData, r_debug);
    AddressSpace::get().protect(s.program.base, s.program.layout.ro_size, kProtRead);

    s.linker_soinfo = new_soinfo(s);
    fill_soinfo(s, s.linker_soinfo, std::string(kLinkerPath), s.linker.base, s.linker.layout.size, s.linker.base,
                s.linker.base + s.linker.layout.phdr, s.linker.layout.phnum, s.linker.base + s.linker.layout.dynamic, 0, 0);
    s.program_soinfo = new_soinfo(s);
    fill_soinfo(s, s.program_soinfo, std::string(kProgramPath), s.program.base, s.program.layout.size, s.program.base,
                s.program.base + s.program.layout.phdr, s.program.layout.phnum, s.program.base + s.program.layout.dynamic, FLAG_EXE, 0);
    d.somain = s.program_soinfo;

    // Linux fs/binfmt_elf.c create_elf_tables order.
    const auto execfn = guest_string(s, kProgramPath), platform = guest_string(s, "aarch64");
    const auto random_at = pool_alloc(s, 16);
    std::random_device device;
    for (int i = 0; i < 16; ++i) *g2h<std::uint8_t>(random_at + i) = std::uint8_t(device());
    const auto& cpu = cpu_backend().info();
    // No AT_SYSINFO_EHDR: Venture has no vDSO.
    s.auxv = {{16, cpu.hwcap}, {6, kGuestPageSize}, {17, 100},
              {3, s.program.base + s.program.layout.phdr}, {4, sizeof(Phdr)}, {5, s.program.layout.phnum},
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
// Writer side of the guest rwlock (hle/libc/threads.cpp rw_acquire/rw_unlock).
// Readers hold it only while they walk the list. A reader that died holding
// it (a thread killed at exit) must not stall thread start or exit: after a
// bounded wait the list is updated without the lock.
bool lock_list(LibcData& d) {
  std::atomic_ref state(d.thread_list_lock[0]), writers(d.thread_list_lock[2]);
  writers.fetch_add(1);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool locked = false;
  for (;;) {
    std::uint32_t expected = 0;
    if (state.compare_exchange_weak(expected, UINT_MAX, std::memory_order_acquire)) { locked = true; break; }
    if (std::chrono::steady_clock::now() > deadline) break;
    std::this_thread::yield();
  }
  writers.fetch_sub(1);
  if (!locked) log::warn("process-image", "g_thread_list_lock held for 2 s; updating the thread list without it");
  return locked;
}
void unlock_list(LibcData& d, GuestAddr lock) {
  std::atomic_ref(d.thread_list_lock[0]).store(0, std::memory_order_release);
  std::atomic_ref(d.thread_list_lock[1]).fetch_add(1, std::memory_order_release);
  futex_wake(lock + 4, INT_MAX);
}
}  // namespace

void thread_attached(GuestThread& thread) {
  auto& list = thread_list();
  const auto address = list.data.load(std::memory_order_acquire);
  const auto handle = thread.pthread_self;
  if (!address || !handle) return;
  std::lock_guard lock(list.mutex);
  if (!list.members.emplace(handle, thread.tid()).second) return;
  auto& d = *g2h<LibcData>(address);
  auto& node = *g2h<BionicThread>(handle);
  const bool locked = lock_list(d);
  // __pthread_internal_add: push at the head.
  node.tid = thread.tid(); node.cached_pid = kGuestPid;
  node.prev = 0; node.next = d.thread_list;
  if (node.next) g2h<BionicThread>(node.next)->prev = handle;
  d.thread_list = handle;
  if (locked) unlock_list(d, address + offsetof(LibcData, thread_list_lock));
}
void thread_detached(GuestThread& thread) {
  auto& list = thread_list();
  const auto address = list.data.load(std::memory_order_acquire);
  const auto handle = thread.pthread_self;
  if (!address || !handle) return;
  std::lock_guard lock(list.mutex);
  if (!list.members.erase(handle)) return;
  auto& d = *g2h<LibcData>(address);
  auto& node = *g2h<BionicThread>(handle);
  const bool locked = lock_list(d);
  if (node.next) g2h<BionicThread>(node.next)->prev = node.prev;
  if (node.prev) g2h<BionicThread>(node.prev)->next = node.next;
  else d.thread_list = node.next;
  node.next = node.prev = 0;
  if (locked) unlock_list(d, address + offsetof(LibcData, thread_list_lock));
}
std::pair<GuestAddr, GuestAddr> thread_entry(GuestThread& thread, GuestAddr start, GuestAddr argument) {
  auto& s = state();
  if (!s.pthread_start || !thread.pthread_self || !thread_list().data.load(std::memory_order_acquire)) return {start, argument};
  auto* fields = g2h<GuestAddr>(thread.pthread_self + kStartRoutineOffset);
  fields[0] = start; fields[1] = argument;
  return {s.pthread_start, thread.pthread_self};
}
bool published() { auto& s = state(); std::lock_guard lock(s.mutex); return s.published; }

std::vector<SyntheticModule> synthetic_modules() {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  return s.synthetic_modules;
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
    if (node.path != path) { g2h<Soinfo>(node.soinfo)->realpath[2] = 0; node.path = path; }
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
  for (std::size_t i = 0; i < order.size(); ++i) {
    auto& si = *g2h<Soinfo>(order[i]);
    si.next = i + 1 < order.size() ? order[i + 1] : 0;
    si.link_map_head.next = i + 1 < order.size() ? order[i + 1] + offsetof(Soinfo, link_map_head) : 0;
    si.link_map_head.prev = i ? order[i - 1] + offsetof(Soinfo, link_map_head) : 0;
  }
  // gdb's order: the executable heads r_map, then the linker, then libraries.
  auto map_of = [](GuestAddr si) { return si + offsetof(Soinfo, link_map_head); };
  auto& program = *g2h<Soinfo>(s.program_soinfo); auto& linker = *g2h<Soinfo>(s.linker_soinfo);
  program.link_map_head.prev = 0; program.link_map_head.next = map_of(s.linker_soinfo);
  linker.link_map_head.prev = map_of(s.program_soinfo);
  linker.link_map_head.next = order.size() > 2 ? map_of(order[2]) : 0;
  if (order.size() > 2) g2h<Soinfo>(order[2])->link_map_head.prev = map_of(s.linker_soinfo);
  auto& d = data(s);
  d.solist = order.front(); d.sonext = order.back(); d.r_debug.map = map_of(s.program_soinfo);
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
  if (path == "/system/bin/linker64") return std::string(kLinkerPath);
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
std::string auxv() {
  auto& s = state(); std::lock_guard lock(s.mutex);
  std::string out;
  for (auto& [t, v] : s.auxv) { out.append(reinterpret_cast<const char*>(&t), 8); out.append(reinterpret_cast<const char*>(&v), 8); }
  const std::uint64_t end[2]{0, 0};
  out.append(reinterpret_cast<const char*>(end), sizeof(end));
  return out;
}
std::string system_path(std::string_view soname) {
  if (soname == kLoaderSoname) return std::string(kLinkerPath);
  return library_path(soname);
}
std::string program_path() { return std::string(kProgramPath); }
}  // namespace venture::process_image
