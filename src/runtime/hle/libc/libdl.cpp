#include "vfs.h"
#include "core/thread_lifetime.h"
#include "abi32.h"
#include "core/token32.h"
#include "core/linker_runtime.h"
#include "core/address_space.h"
#include "core/hle_runtime.h"
#include "core/signals.h"
#include "platform/memory.h"
#include "core/guest_storage.h"
#include "core/process.h"
#include "core/process_image.h"
#include "common/log.h"
#include "common/paths.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
namespace venture::hle::libc {
namespace {
std::string& local_error() { return thread_lifetime::local<std::string, struct DlLocalError>(); }
std::string& error_text() { return thread_lifetime::local<std::string, struct DlErrorText>(); }
std::string& info_path() { return thread_lifetime::local<std::string, struct DlInfoPath>(); }
std::string& info_symbol() { return thread_lifetime::local<std::string, struct DlInfoSymbol>(); }
Library* caller(CallContext& c) {
  auto lock = guest_loader_lock();
  return Linker::get().library_at(c.cpu().lr());
}
Library* dlopen_fn(CallContext& c, const char* name, int flags) {
  if (!name) return Linker::get().open_main(flags);
  return Linker::get().open(name, flags, caller(c));
}
int dlclose_fn(Library* l) { return Linker::get().close(l); }
GuestAddr dlsym_fn(CallContext& c, Library* l, const char* name) {
  if (!name) {
    local_error() = "dlsym: null symbol";
    return 0;
  }
  if (!l) return Linker::get().symbol_default(name, caller(c));
  if (h2g(l) == UINT64_MAX) return Linker::get().symbol_next(name, caller(c));
  return Linker::get().symbol(l, name);
}
GuestAddr dlvsym_fn(CallContext& c, Library* l, const char* name, const char* version) {
  auto lock = guest_loader_lock();
  if (!version || !*version) return dlsym_fn(c, l, name);
  if (!name) {
    local_error() = "dlvsym: null symbol";
    return 0;
  }
  if (!l || h2g(l) == UINT64_MAX) {
    local_error() = "dlvsym: versioned global/next lookup requires linker support";
    return 0;
  }
  auto p = l->find_symbol(name, version);
  if (!p) local_error() = "dlvsym: versioned symbol not found";
  return p;
}
const char* dlerror_fn() {
  error_text() = local_error().empty() ? Linker::get().take_error() : std::exchange(local_error(), {});
  return error_text().empty() ? nullptr : error_text().c_str();
}
const char* dlerror32() {
  auto* error = dlerror_fn();
  auto& storage = thread_lifetime::local<GuestStorage, struct DlErrorStorage32>();
  return error ? storage.string(error) : nullptr;
}
struct GuestDlInfo {
  GuestAddr path, base, symbol, address;
};
int dladdr_fn(GuestAddr address, GuestDlInfo* out) {
  if (!out) return 0;
  DlInfo i;
  if (!Linker::get().address_info(address, &i)) return 0;
  info_path() = std::move(i.path);
  info_symbol() = std::move(i.symbol);
  *out = {h2g(info_path().c_str()), i.base, info_symbol().empty() ? 0 : h2g(info_symbol().c_str()),
          i.symbol_address};
  return 1;
}
struct PhdrInfo {
  GuestAddr addr, name, phdr;
  std::uint16_t phnum;
  std::uint8_t pad[6];
  std::uint64_t adds, subs, tls_modid;
  GuestAddr tls_data;
};
static_assert(sizeof(PhdrInfo) == 64 && offsetof(PhdrInfo, adds) == 32);
// bionic/libc/include/link.h (NDK armeabi-v7a): 64-bit counters are 8-aligned,
// while Elf32_Addr, pointers, size_t and the TLS module ID are four bytes.
struct PhdrInfo32 {
  std::uint32_t addr, name, phdr;
  std::uint16_t phnum, pad;
  std::uint64_t adds, subs;
  std::uint32_t tls_modid, tls_data;
};
static_assert(sizeof(PhdrInfo32) == 40 && offsetof(PhdrInfo32, adds) == 16);
int iterate32_fn(CallContext& c, GuestAddr callback, GuestAddr data) {
  std::size_t count = 0;
  Linker::get().for_each_image([&](const Library&) { ++count; });
  int result = 0;
  // for_each_image pins its shared_ptr snapshot through every callback. Do not
  // retain bare Library pointers after it returns: dlclose may run concurrently.
  Linker::get().for_each_image([&](const Library& library) {
    if (result) return;
    const auto* l = &library;
    const auto bytes = sizeof(PhdrInfo32) + l->path().size() + 1;
    const auto memory = AddressSpace::get().allocate(bytes, kProtRead | kProtWrite, "[dl_phdr_info32]");
    if (!memory || memory > UINT32_MAX || bytes > (1ull << 32) - memory) {
      AddressSpace::get().free(memory); c.thread().set_errno(err::enomem); result = -1; return;
    }
    auto* p = g2h<PhdrInfo32>(memory);
    const auto tls = l->tls_module_id ? c.thread().thread_pointer + l->tls_offset : 0;
    if (l->load_bias > UINT32_MAX || l->phdr > UINT32_MAX || l->tls_module_id > UINT32_MAX || tls > UINT32_MAX) {
      AddressSpace::get().free(memory); c.thread().set_errno(err::eoverflow); result = -1; return;
    }
    *p = {static_cast<std::uint32_t>(l->load_bias), static_cast<std::uint32_t>(memory + sizeof(*p)),
          static_cast<std::uint32_t>(l->phdr), l->phnum, 0, count, 0,
          static_cast<std::uint32_t>(l->tls_module_id), static_cast<std::uint32_t>(tls)};
    std::memcpy(g2h(p->name), l->path().c_str(), l->path().size() + 1);
    try { result = static_cast<std::int32_t>(c.thread().call(callback, {memory, sizeof(*p), data})); }
    catch (...) { AddressSpace::get().free(memory); throw; }
    AddressSpace::get().free(memory);
  });
  return result;
}
int iterate_fn(CallContext& c, GuestAddr callback, GuestAddr data) {
  struct Image {
    GuestAddr addr, phdr;
    std::uint16_t phnum;
    std::uint64_t modid;
    std::int64_t offset;
    std::string name;
  };
  std::vector<Image> images;
    for (const auto& image : process_image::synthetic_modules())
      images.push_back({image.load_bias, image.phdr, image.phnum, 0, 0, image.path});
  Linker::get().for_each_image([&](const Library& l) {
    images.push_back({l.load_bias, l.phdr, l.phnum, l.tls_module_id, l.tls_offset, l.path()});
  });
  if (images.empty()) return 0;
  // Bionic keeps the record on the stack and the name in the linker.
  const auto memory = AddressSpace::get().allocate(sizeof(PhdrInfo), kProtRead | kProtWrite, "[dl_phdr_info]");
  if (!memory) {
    c.thread().set_errno(err::enomem);
    log::error("libdl", "dl_iterate_phdr scratch allocation failed");
    return -1;
  }
  int result = 0;
  try {
    for (const auto& l : images) {
      auto* p = g2h<PhdrInfo>(memory);
      *p = {l.addr,
            process_image::module_name(l.name),
            l.phdr,
            l.phnum,
            {},
            images.size(),
            0,
            l.modid,
            l.modid ? c.thread().thread_pointer + l.offset : 0};
      result = static_cast<int>(c.thread().call(callback, {memory, sizeof(*p), data}));
      if (result) break;
    }
  } catch (...) {
    AddressSpace::get().free(memory);
    throw;
  }
  AddressSpace::get().free(memory);
  return result;
}
// Linker namespaces (bionic linker_namespaces.h). Venture's linker is flat:
// every library is visible to every caller, which is what a namespace linked
// to all others sees. A namespace keeps what can still mean something without
// isolation: its search paths, tried first for a bare name dlopened into it.
// Links always succeed, since everything is already shared.
struct Namespace {
  std::string name;
  std::vector<std::string> paths;  // ld_library_path, then default_library_path (guest)
};
std::mutex namespaces_mutex;
std::map<GuestAddr, Namespace> namespaces;
void split_paths(const char* text, std::vector<std::string>& out) {
  std::string_view s = text ? text : "";
  while (!s.empty()) {
    auto end = std::min(s.find(':'), s.size());
    if (end) out.emplace_back(s.substr(0, end));
    s.remove_prefix(std::min(end + 1, s.size()));
  }
}
GuestAddr create_namespace(const char* name, const char* ld_library_path, const char* default_library_path,
                           std::uint64_t type, const char*, GuestAddr) {
  Namespace ns{name ? name : ""};
  split_paths(ld_library_path, ns.paths);
  split_paths(default_library_path, ns.paths);
  // The handle is an opaque guest pointer, unique for the process's lifetime.
  const auto handle = AddressSpace::get().allocate(kGuestPageSize, kProtRead, "[anon:linker_namespace]");
  if (!handle) { local_error() = "android_create_namespace: out of memory"; return 0; }
  log::info("libdl", "Linker namespace \"{}\" (type {:#x}, {} search paths) in Venture's flat linker", ns.name, type,
            ns.paths.size());
  std::lock_guard lock(namespaces_mutex);
  namespaces[handle] = std::move(ns);
  return handle;
}
GuestAddr loader_create_namespace(const char* name, const char* ld_library_path, const char* default_library_path,
                                  std::uint64_t type, const char* permitted, GuestAddr parent, GuestAddr) {
  return create_namespace(name, ld_library_path, default_library_path, type, permitted, parent);
}
bool link_namespaces(GuestAddr from, GuestAddr, const char*) {
  if (!from) { local_error() = "android_link_namespaces: namespace_from is null"; return false; }
  return true;
}
bool link_namespaces_all_libs(GuestAddr from, GuestAddr to) {
  if (!from || !to) { local_error() = "android_link_namespaces_all_libs: a namespace is null"; return false; }
  return true;
}
bool init_anonymous_namespace(const char*, const char*) { return true; }
void* exported_namespace(const char*) {
  // No namespace is exported by this flat linker. Bionic returns null for an
  // absent/hidden namespace, including a null name, without changing dlerror.
  return nullptr;
}
// android_dlextinfo (android/dlext.h): flags, reserved_addr, reserved_size,
// relro_fd, library_fd, library_fd_offset, library_namespace.
struct DlextInfo {
  std::uint64_t flags, reserved_addr, reserved_size;
  std::int32_t relro_fd, library_fd;
  std::int64_t library_fd_offset;
  GuestAddr library_namespace;
};
static_assert(sizeof(DlextInfo) == 48 && offsetof(DlextInfo, library_namespace) == 40);
constexpr std::uint64_t kDlextReservedAddress = 0x1, kDlextReservedAddressHint = 0x2, kDlextUseLibraryFd = 0x10,
                        kDlextUseLibraryFdOffset = 0x20, kDlextForceLoad = 0x40, kDlextUseNamespace = 0x200;
constexpr std::uint64_t kDlextSupported = kDlextReservedAddress | kDlextReservedAddressHint | kDlextUseLibraryFd |
                                          kDlextUseLibraryFdOffset | kDlextForceLoad | kDlextUseNamespace;
bool placement(const DlextInfo* ext, elf::Placement& out) {
  if (!ext || !ext->flags) return true;
  if (ext->flags & ~kDlextSupported) {
    log::warn("libdl", "unsupported android_dlopen_ext flags {:#x}", ext->flags & ~kDlextSupported);
    local_error() = "android_dlopen_ext: extended flags are not supported";
    return false;
  }
  if (ext->flags & (kDlextReservedAddress | kDlextReservedAddressHint))
    out = {ext->reserved_addr, ext->reserved_size, (ext->flags & kDlextReservedAddress) != 0};
  return true;
}
// ANDROID_DLEXT_USE_LIBRARY_FD: the library is the descriptor's file, `name`
// only names it. A descriptor on a guest path opens that path. One whose bytes
// copy a system library Venture provides (liblinkernsbypass loads a copy of
// libvulkan.so with a rewritten soname from a memfd, to get a second instance
// in its own namespace) is that library, as the copy would run the same code.
// Any other file in memory is written to the cache and loaded from there.
std::optional<std::string> library_from_fd(const DlextInfo& ext, const char* name) {
  const std::int64_t offset = ext.flags & kDlextUseLibraryFdOffset ? ext.library_fd_offset : 0;
  auto file = FdTable::get().file(ext.library_fd);
  if (!file) { local_error() = "android_dlopen_ext: library_fd is not an open descriptor"; return std::nullopt; }
  if (!offset && file->path.starts_with('/')) {
    MappedPath mapped;
    if (!resolve_path(file->path, at_cwd, &mapped) && !mapped.device && std::filesystem::is_regular_file(mapped.host))
      return file->path;
  }
  Stat st{};
  if (int r = file->stat(&st); r || st.size <= offset) {
    local_error() = "android_dlopen_ext: cannot read library_fd";
    return std::nullopt;
  }
  std::string bytes(static_cast<std::size_t>(st.size - offset), '\0');
  for (std::size_t done = 0; done < bytes.size();) {
    auto n = file->pread(bytes.data() + done, bytes.size() - done, offset + static_cast<std::int64_t>(done));
    if (n <= 0) { local_error() = "android_dlopen_ext: cannot read library_fd"; return std::nullopt; }
    done += static_cast<std::size_t>(n);
  }
  if (auto soname = process_image::copied_library(bytes)) {
    log::info("libdl", "android_dlopen_ext: {} (descriptor {}) copies the system {}; serving that library",
              name ? name : "", ext.library_fd, *soname);
    return *soname;
  }
  static std::atomic<unsigned> serial{0};
  const auto base = paths::utf8(paths::from_utf8(name ? name : "library.so").filename());
  const auto number = serial++;
  const auto directory = process_config().cache_dir / "dlopen-fd";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  const auto host = directory / paths::from_utf8(std::to_string(number) + "-" + base);
  {
    std::ofstream out(host, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) { local_error() = "android_dlopen_ext: cannot stage library_fd"; return std::nullopt; }
  }
  auto guest = (file->path.starts_with('/') ? file->path : "/memfd:" + base) + "#" + std::to_string(number);
  Linker::get().register_file(guest, host);
  return guest;
}
// A bare name dlopened into a namespace: its search paths first.
std::string namespace_candidate(GuestAddr handle, const char* name) {
  if (!handle || !name || std::strchr(name, '/')) return name ? name : "";
  std::vector<std::string> paths;
  {
    std::lock_guard lock(namespaces_mutex);
    if (auto it = namespaces.find(handle); it != namespaces.end()) paths = it->second.paths;
  }
  for (const auto& directory : paths) {
    auto candidate = directory + "/" + name;
    MappedPath mapped;
    if (!resolve_path(candidate, at_cwd, &mapped) && !mapped.device && std::filesystem::is_regular_file(mapped.host))
      return candidate;
  }
  return name;
}
Library* open_ext(const char* name, int flags, const DlextInfo* ext, Library* from) {
  elf::Placement where;
  if (!placement(ext, where)) return nullptr;
  if (!name && !(ext && ext->flags & kDlextUseLibraryFd)) return Linker::get().open_main(flags);
  std::string target = name ? name : "";
  if (ext && ext->flags & kDlextUseLibraryFd) {
    auto from_fd = library_from_fd(*ext, name);
    if (!from_fd) return nullptr;
    target = std::move(*from_fd);
  } else if (ext && ext->flags & kDlextUseNamespace) {
    target = namespace_candidate(ext->library_namespace, name);
  }
  return Linker::get().open(target, flags, from, where.address ? &where : nullptr);
}
Library* android_open(CallContext& c, const char* name, int flags, const DlextInfo* ext) {
  return open_ext(name, flags, ext, caller(c));
}
void ld_path(char* out, std::uint64_t n) {
  auto path = process_config().guest_native_lib_dir() + ":" + process_config().guest_system_lib_dir();
  if (n) {
    auto count = std::min<std::uint64_t>(path.size(), n - 1);
    std::memcpy(out, path.data(), count);
    out[count] = 0;
  }
}
void update_path(const char* text) {
  auto lock = guest_loader_lock();
  std::vector<std::string> paths;
  std::string s = text ? text : "";
  for (std::size_t i = 0; i < s.size();) {
    auto end = s.find(':', i);
    if (end == s.npos) end = s.size();
    MappedPath p;
    if (!resolve_path(s.substr(i, end - i), at_cwd, &p) && !p.device) {
      paths.push_back(venture::paths::utf8(p.host));
    }
    i = end + 1;
  }
  Linker::get().set_search_paths(std::move(paths));
}
void cfi(CallContext& c, std::uint64_t type, GuestAddr target) {
  auto lock = guest_loader_lock();
  Library* owner = nullptr;
  Linker::get().for_each_image([&](const Library& l) {
    if (target >= l.map_start && target - l.map_start < l.map_size)
      owner = const_cast<Library*>(&l);
  });
  if (owner) {
    if (auto check = owner->find_symbol("__cfi_check")) c.thread().call(check, {type, target, 0});
    return;
  }
  log::error("libdl", "CFI target outside loaded ELF images");
  signals::deliver_synchronous(c.thread(), {.signo = 6});
}
GuestAddr unwind_find_exidx(GuestAddr pc, std::int32_t* count) {
  auto lock = guest_loader_lock();
  std::int32_t entries = 0;
  auto address = Linker::get().find_exidx(pc, &entries);
  if (count && !platform::write_memory(h2g(count), &entries, sizeof(entries)))
    throw hle::MemoryFault{h2g(count), true};
  return address;
}
struct DlHandle32 { Library* library; explicit DlHandle32(Library* p) : library(p) {} };
Token32Registry dl_tokens;
std::uint32_t dlopen32(CallContext& c, const char* name, int flags) {
  auto lock = guest_loader_lock();
  // bionic dlfcn.h: ARM RTLD_NOW=0 and RTLD_GLOBAL=2 (A64 uses 2/256).
  auto normalized = (flags & ~2) | ((flags & 2) ? kRtldGlobal : 0) | kRtldNow;
  auto* l = dlopen_fn(c, name, normalized);
  return l ? dl_tokens.insert(std::make_shared<DlHandle32>(l)).value : 0;
}
int dlclose32(std::uint32_t token) {
  auto lock = guest_loader_lock();
  auto h = dl_tokens.resolve(Token32<DlHandle32>{token});
  if (!h) { local_error() = "dlclose: invalid A32 handle"; return -1; }
  if (!h->library) { local_error() = "dlclose: closed A32 handle"; return -1; }
  auto r = dlclose_fn(h->library);
  if (!r) { h->library = nullptr; dl_tokens.erase(Token32<DlHandle32>{token}); }
  return r;
}
GuestAddr dlsym32(CallContext& c, std::uint32_t token, const char* name) {
  auto lock = guest_loader_lock();
  // bionic's historical LP32 pseudo-handles differ from LP64.
  if (token == UINT32_MAX || !token) return dlsym_fn(c, nullptr, name);
  if (token == UINT32_MAX - 1) return dlsym_fn(c, g2h<Library>(UINT64_MAX), name);
  auto h = dl_tokens.resolve(Token32<DlHandle32>{token});
  if (!h) { local_error() = "dlsym: invalid A32 handle"; return 0; }
  if (!h->library) { local_error() = "dlsym: closed A32 handle"; return 0; }
  return dlsym_fn(c, h->library, name);
}
GuestAddr dlvsym32(CallContext& c, std::uint32_t token, const char* name, const char* version) {
  auto lock = guest_loader_lock();
  if (!version || !*version) return dlsym32(c, token, name);
  auto h = dl_tokens.resolve(Token32<DlHandle32>{token});
  if (!h) { local_error() = "dlvsym: invalid A32 handle or unsupported versioned global lookup"; return 0; }
  if (!h->library) { local_error() = "dlvsym: closed A32 handle"; return 0; }
  return dlvsym_fn(c, h->library, name, version);
}
// bionic/linker/dlfcn.cpp: the linker's __loader_* entry points, which
// libdl.so forwards to with the caller's return address. In-process tools call
// them directly with an explicit (trusted) caller.
Library* loader_caller(GuestAddr address) {
  auto lock = guest_loader_lock();
  return address ? Linker::get().library_at(address) : nullptr;
}
Library* loader_dlopen(const char* name, int flags, GuestAddr) {
  if (!name) return Linker::get().open_main(flags);
  return Linker::get().open(name, flags, nullptr);
}
Library* loader_dlopen_ext(const char* name, int flags, const DlextInfo* ext, GuestAddr) {
  return open_ext(name, flags, ext, nullptr);
}
GuestAddr loader_dlvsym(Library* l, const char* name, const char* version, GuestAddr caller_address) {
  if (!name) {
    local_error() = "dlsym: null symbol";
    return 0;
  }
  auto lock = guest_loader_lock();
  auto* caller = loader_caller(caller_address);
  if (!l) return Linker::get().symbol_default(name, caller);
  if (h2g(l) == UINT64_MAX) return Linker::get().symbol_next(name, caller);
  if (!version || !*version) return Linker::get().symbol(l, name);
  auto p = l->find_symbol(name, version);
  if (!p) local_error() = "dlvsym: versioned symbol not found";
  return p;
}
GuestAddr loader_dlsym(Library* l, const char* name, GuestAddr caller) { return loader_dlvsym(l, name, nullptr, caller); }
struct DlInfo32 { std::uint32_t path, base, symbol, address; };
int dladdr32(GuestAddr address, DlInfo32* out) {
  if (!out) return 0;
  DlInfo i; if (!Linker::get().address_info(address & ~1ull, &i)) return 0;
  *out = {h2g32(low_string(i.path)), std::uint32_t(i.base), i.symbol.empty() ? 0 : h2g32(low_string(i.symbol)), std::uint32_t(i.symbol_address)};
  return 1;
}
std::uint32_t android_open32(CallContext& c, const char* name, int flags, const std::uint64_t* ext) {
  if (ext && *ext) { local_error() = "android_dlopen_ext: extended flags are not supported"; log::warn("libdl", "unsupported android_dlopen_ext flags {:#x}", *ext); return 0; }
  return dlopen32(c, name, flags);
}
// libdl_android.so: the platform-private namespace API libnativeloader and
// in-process tools use. bionic's dlsym on its handle also reaches the linker's
// __loader_* forms, which liblinkernsbypass looks up there.
void register_namespace_api(HleLibrary& lib) {
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("android_create_namespace", create_namespace);
  B("android_link_namespaces", link_namespaces);
  B("android_link_namespaces_all_libs", link_namespaces_all_libs);
  B("android_init_anonymous_namespace", init_anonymous_namespace);
  B("android_get_exported_namespace", exported_namespace);
  B("__loader_android_create_namespace", loader_create_namespace);
  B("__loader_android_link_namespaces", link_namespaces);
  B("__loader_android_link_namespaces_all_libs", link_namespaces_all_libs);
  B("__loader_android_init_anonymous_namespace", init_anonymous_namespace);
  B("__loader_android_get_exported_namespace", exported_namespace);
#undef B
}
}  // namespace
void register_libdl(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
    abi32::adapt<&dlopen32>("dlopen", "libdl.so"); abi32::adapt<&dlclose32>("dlclose", "libdl.so");
    abi32::adapt<&dlsym32>("dlsym", "libdl.so"); abi32::adapt<&dlvsym32>("dlvsym", "libdl.so");
    abi32::adapt<&dladdr32>("dladdr", "libdl.so"); abi32::adapt<&iterate32_fn>("dl_iterate_phdr", "libdl.so");
    abi32::adapt<&android_open32>("android_dlopen_ext", "libdl.so");
  }
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("dlopen", dlopen_fn);
  B("dlclose", dlclose_fn);
  B("dlsym", dlsym_fn);
  B("dlvsym", dlvsym_fn);
  if (process_config().isa() == GuestIsa::a32)
    B("dlerror", dlerror32);
  else
    B("dlerror", dlerror_fn);
  B("dladdr", dladdr_fn);
  if (process_config().isa() == GuestIsa::a32) { B("dl_iterate_phdr", iterate32_fn); }
  else { B("dl_iterate_phdr", iterate_fn); }
  B("android_dlopen_ext", android_open);
  B("android_get_exported_namespace", exported_namespace);
  B("android_get_LD_LIBRARY_PATH", ld_path);
  B("android_update_LD_LIBRARY_PATH", update_path);
  B("__cfi_slowpath", cfi);
  if (process_config().isa() == GuestIsa::a32) {
    B("__gnu_Unwind_Find_exidx", unwind_find_exidx);
    B("dl_unwind_find_exidx", unwind_find_exidx);
  }
#undef B
}
namespace {
// A32 __loader_* entry points: libdl.so's 32-bit forms with the explicit
// caller address last (handles are the same A32 tokens dlopen returns).
std::uint32_t loader_dlopen32(CallContext& c, const char* name, int flags, GuestAddr) { return dlopen32(c, name, flags); }
std::uint32_t loader_dlopen_ext32(CallContext& c, const char* name, int flags, const std::uint64_t* ext, GuestAddr) {
  return android_open32(c, name, flags, ext);
}
GuestAddr loader_dlsym32(CallContext& c, std::uint32_t token, const char* name, GuestAddr) { return dlsym32(c, token, name); }
GuestAddr loader_dlvsym32(CallContext& c, std::uint32_t token, const char* name, const char* version, GuestAddr) {
  return dlvsym32(c, token, name, version);
}
}  // namespace
void register_loader(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
    abi32::adapt<&loader_dlopen32>("__loader_dlopen", "ld-android.so");
    abi32::adapt<&loader_dlopen_ext32>("__loader_android_dlopen_ext", "ld-android.so");
    abi32::adapt<&loader_dlsym32>("__loader_dlsym", "ld-android.so");
    abi32::adapt<&loader_dlvsym32>("__loader_dlvsym", "ld-android.so");
    abi32::adapt<&dlclose32>("__loader_dlclose", "ld-android.so");
    abi32::adapt<&dladdr32>("__loader_dladdr", "ld-android.so");
    abi32::adapt<&iterate32_fn>("__loader_dl_iterate_phdr", "ld-android.so");
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
    B("__loader_dlopen", loader_dlopen);
    B("__loader_android_dlopen_ext", loader_dlopen_ext);
    B("__loader_dlsym", loader_dlsym);
    B("__loader_dlvsym", loader_dlvsym);
    B("__loader_dlclose", dlclose_fn);
    B("__loader_dlerror", dlerror32);
    B("__loader_dladdr", dladdr_fn);
    B("__loader_dl_iterate_phdr", iterate32_fn);
#undef B
    return;
  }
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("__loader_dlopen", loader_dlopen);
  B("__loader_android_dlopen_ext", loader_dlopen_ext);
  B("__loader_dlsym", loader_dlsym);
  B("__loader_dlvsym", loader_dlvsym);
  B("__loader_dlclose", dlclose_fn);
  B("__loader_dlerror", dlerror_fn);
  B("__loader_dladdr", dladdr_fn);
  B("__loader_dl_iterate_phdr", iterate_fn);
  B("__loader_android_get_exported_namespace", exported_namespace);
  B("__loader_android_get_LD_LIBRARY_PATH", ld_path);
  B("__loader_android_update_LD_LIBRARY_PATH", update_path);
  B("__loader_android_create_namespace", loader_create_namespace);
  B("__loader_android_link_namespaces", link_namespaces);
  B("__loader_android_link_namespaces_all_libs", link_namespaces_all_libs);
  B("__loader_android_init_anonymous_namespace", init_anonymous_namespace);
#undef B
}
void register_libdl_android(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a64) register_namespace_api(lib);
}
// bionic's libdl dlopen hands its return address to the linker's
// __loader_dlopen through a BL. Tools such as liblinkernsbypass follow the
// first BL from &dlopen to reach the linker, so A64 dlopen has that shape.
void shape_dlopen(HleLibrary& libdl, const HleLibrary& loader) {
  const auto target = loader.hle_symbol("__loader_dlopen");
  libdl.add_function("dlopen", code_stub(std::array<std::uint32_t, 10>{
      0xa9bf7bfdu,  // stp x29, x30, [sp, #-16]!
      0x910003fdu,  // mov x29, sp
      0xaa1e03e2u,  // mov x2, x30
      0x94000003u,  // bl 1f
      0xa8c17bfdu,  // ldp x29, x30, [sp], #16
      0xd65f03c0u,  // ret
      0x58000050u,  // 1: ldr x16, 2f
      0xd61f0200u,  // br x16
      std::uint32_t(target), std::uint32_t(target >> 32)}, "dlopen"));  // 2: __loader_dlopen
}
}  // namespace venture::hle::libc
