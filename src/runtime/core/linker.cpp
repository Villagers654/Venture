#include "common/paths.h"
#include "core/linker.h"
#include "core/hle_unwind.h"
#include "core/linker_runtime.h"
#include "core/elf.h"
#include "core/hle_runtime.h"
#include "core/process_image.h"
#include "core/process_runtime.h"
#include "core/thread.h"
#include "core/thread_runtime.h"
#include "core/tls.h"
#include "core/address_space.h"
#include "common/log.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace venture {
struct HleLibrary::Impl {
  mutable std::mutex mutex;
  std::unordered_map<std::string, GuestAddr> symbols;
  std::set<std::string, std::less<>> functions;
  std::shared_ptr<elf::Image> backing;
};
HleLibrary::HleLibrary(std::string soname, std::vector<std::string> aliases) : impl_(std::make_shared<Impl>()), aliases_(std::move(aliases)) {
  soname_ = std::move(soname); path_ = process_config().guest_system_lib_dir() + "/" + soname_; hle_ = true;
}
void HleLibrary::add_function(std::string_view name, GuestAddr thunk) {
  std::lock_guard lock(impl_->mutex);
  impl_->symbols[std::string(name)] = thunk; impl_->functions.emplace(name);
}
void HleLibrary::add_data(std::string_view name, GuestAddr address) {
  if (process_config().isa() == GuestIsa::a32 && address >= (1ull << 32)) {
    log::error("linker", "A32 high-address data export {}:{}", soname_, name);
    throw std::overflow_error("A32 HLE data export is not in guest memory");
  }
  std::lock_guard lock(impl_->mutex); impl_->symbols[std::string(name)] = address;
  impl_->functions.erase(std::string(name));
}
bool HleLibrary::is_function(std::string_view name) const {
  std::lock_guard lock(impl_->mutex); return impl_->functions.contains(name);
}
std::vector<std::pair<std::string, GuestAddr>> HleLibrary::functions() const {
  std::lock_guard lock(impl_->mutex);
  std::vector<std::pair<std::string, GuestAddr>> result;
  for (const auto& name : impl_->functions) result.emplace_back(name, impl_->symbols.at(name));
  return result;
}
GuestAddr HleLibrary::hle_symbol(std::string_view name) const {
  std::lock_guard lock(impl_->mutex);
  auto it = impl_->symbols.find(std::string(name));
  return it != impl_->symbols.end() ? it->second : 0;
}
GuestAddr HleLibrary::find_symbol(std::string_view name, std::string_view version) const {
  std::lock_guard lock(linker_lifecycle_mutex());
  if (auto address = hle_symbol(name)) return address;
  if (!mapped_image()) Linker::get().activate_backing(const_cast<HleLibrary*>(this), name, version);
  auto image = backing(); return image ? image->find_symbol(name, version) : 0;
}
std::shared_ptr<elf::Image> HleLibrary::backing() const {
  std::lock_guard lock(impl_->mutex); return impl_->backing;
}
elf::Image* HleLibrary::mapped_image() const { return backing().get(); }
GuestIsa HleLibrary::isa() const {
  auto image = backing(); return image ? image->isa() : process_config().isa();
}
void HleLibrary::set_backing(std::shared_ptr<elf::Image> image) {
  std::lock_guard lock(impl_->mutex);
  if (image) { image->owner = this; image->group_root = group_root; global |= image->global; nodelete |= image->nodelete; }
  load_bias = image ? image->load_bias : 0;
  phdr = image ? image->phdr : 0; phnum = image ? image->phnum : 0;
  map_start = image ? image->map_start : 0; map_size = image ? image->map_size : 0;
  tls_module_id = image ? image->tls_module_id : 0; tls_offset = image ? image->tls_offset : 0;
  path_ = image ? image->path() : process_config().guest_system_lib_dir() + "/" + soname_;
  impl_->backing = std::move(image);
}
namespace {
class MainLibrary final : public Library {
 public:
  MainLibrary() { soname_ = path_ = "[main]"; hle_ = true; }
  GuestAddr find_symbol(std::string_view name, std::string_view version = {}) const override;
};
struct State {
  LoaderMutex mutex;
  MainLibrary main;
  std::vector<std::shared_ptr<Library>> libraries;
  std::unordered_map<std::string, std::shared_ptr<Library>> names;
  std::vector<std::string> search_paths;
  std::set<std::string> unresolved;
  std::vector<Library*> globals;
  bool inspect = false;
  ImportCoverageHook import_coverage;
  std::map<HleLibrary*, std::filesystem::path> backing_files;
  std::map<std::pair<std::filesystem::path, std::string>, std::set<std::string>> exports;
  std::vector<elf::Image*> exidx_images; // sorted by map_start, under mutex
};
State& state() { static auto* s = new State; return *s; }
void promote(State& s, Library* lib) {
  if (std::find(s.globals.begin(), s.globals.end(), lib) == s.globals.end()) s.globals.push_back(lib);
  lib->global = true;
}
thread_local std::string last_error;
// Venture replaces app_process and its basic startup providers. These HLE
// definitions exist before any app dlopen, independently of APK DT_NEEDED lists.
// app_process lists libdl/liblog, then the default libc/libm dependencies.
constexpr std::string_view startup_libraries[]{"libdl.so", "liblog.so", "libc.so", "libm.so"};
bool system_library(std::string_view name) {
  static const std::set<std::string_view> names{"libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so", "libandroid.so",
      "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libvulkan.so", "libOpenSLES.so", "libaaudio.so",
      "libmediandk.so", "libjnigraphics.so", "libstdc++.so", "libnativewindow.so", "libsync.so", "libbinder_ndk.so",
      "libcamera2ndk.so", "libneuralnetworks.so", "libGLESv3_adreno.so", "libvrapi.so", "libopenxr_loader.so"};
  return names.contains(name);
}
std::filesystem::path library_file(State& s, std::string_view name, std::string_view base) {
  // Like bionic's dlopen, a name without a slash is only searched for, never
  // taken as a path (which would find a host file in the working directory).
  if (name.find_first_of("/\\") != std::string_view::npos) {
    auto path = resolve_guest_path(name);
    if (!path.empty() && std::filesystem::is_regular_file(path)) return path;
  }
  for (const auto& directory : s.search_paths) {
    auto candidate = paths::from_utf8(directory) / paths::from_utf8(base);
    if (std::filesystem::is_regular_file(candidate)) return candidate;
  }
  return {};
}
bool backing_exports(State& s, HleLibrary* slot, std::string_view name, std::string_view version) {
  if (slot->mapped_image() || slot->hle_symbol(name)) return false;
  auto [file, added_file] = s.backing_files.try_emplace(slot);
  if (added_file) file->second = library_file(s, slot->soname(), slot->soname());
  const auto& path = file->second;
  if (path.empty()) return false;
  auto [it, added] = s.exports.try_emplace(std::make_pair(path, std::string(version)));
  if (added) {
    try { it->second = elf::exported_names(path, version); }
    catch (...) { s.exports.erase(it); throw; }
  }
  return it->second.contains(std::string(name));
}
std::shared_ptr<Library> load(State& s, std::string_view name, int flags, bool backing = false,
                              const elf::Placement* placement = nullptr) {
  auto key = std::string(name);
  auto base = paths::utf8(paths::from_utf8(key).filename());
  std::shared_ptr<Library> lib;
  if (auto it = s.names.find(key); it != s.names.end()) lib = it->second;
  else if (auto it = s.names.find(base); it != s.names.end()) lib = it->second;
  if (auto* slot = dynamic_cast<HleLibrary*>(lib.get()); slot && !(flags & kRtldNoLoad) &&
      (!s.backing_files.contains(slot) || key != base)) s.backing_files[slot] = library_file(s, name, base);
  if (lib && (!backing || !lib->is_hle() || lib->mapped_image() || (flags & kRtldNoLoad))) return lib;
  if (!lib && (flags & kRtldNoLoad)) throw std::runtime_error("RTLD_NOLOAD: library is not loaded: " + key);
  const bool registered = bool(lib);
  auto path = library_file(s, name, base);
  if (backing) if (auto* slot = dynamic_cast<HleLibrary*>(lib.get()); slot && s.backing_files.contains(slot)) path = s.backing_files.at(slot);
  if (!path.empty()) {
    auto guest = process_config().guest_native_lib_dir() + "/" + base;
    auto image = std::make_shared<elf::Image>(path, guest, placement);
    if (image->isa() != process_config().isa()) throw std::runtime_error("ELF ISA differs from selected process ABI: " + base);
    image->global |= (flags & kRtldGlobal) != 0;
    image->nodelete |= (flags & kRtldNoDelete) != 0;
    if (auto* hle = dynamic_cast<HleLibrary*>(lib.get())) hle->set_backing(std::move(image));
    else lib = std::move(image);
  } else if (lib) {
    return lib;
  } else if (system_library(base) || key.starts_with(process_config().guest_system_lib_dir() + "/")) {
    // Empty virtual libraries represent absent HLE providers; imports still fail individually.
    auto hle = std::make_shared<HleLibrary>(base);
    if (base == "libc.so") hle->add_function("__tls_get_addr", tls::get_addr_thunk());
    lib = std::move(hle);
  } else throw std::runtime_error("library not found: " + key);
  if (!registered) s.libraries.push_back(lib);
  if (auto* image = lib->mapped_image(); image && image->isa() == GuestIsa::a32) {
    auto it = std::lower_bound(s.exidx_images.begin(), s.exidx_images.end(), image->map_start,
                              [](auto* p, auto start) { return p->map_start < start; });
    s.exidx_images.insert(it, image);
  }
  if (lib->global) promote(s, lib.get());
  s.names[key] = lib; s.names[base] = lib; s.names[lib->soname()] = lib; s.names[lib->path()] = lib;
  return lib;
}
std::vector<Library*> local_group(Library* root) {
  std::vector<Library*> result;
  std::set<Library*> seen;
  if (root) { result.push_back(root); seen.insert(root); }
  for (std::size_t i = 0; i < result.size(); ++i)
    if (auto* image = result[i]->mapped_image())
      for (auto* dep : image->dependencies) if (seen.insert(dep).second) result.push_back(dep);
  return result;
}
Library* canonical_caller(State& s, Library* caller) {
  for (auto& lib : s.libraries)
    if (lib.get() == caller || lib->mapped_image() == caller) return lib.get();
  return nullptr;
}
struct Definition { Library* library{}; const elf::Sym* symbol{}; GuestAddr address{}; };
Definition lookup_one(Library* lib, std::string_view name, std::string_view version) {
  if (auto* hle = dynamic_cast<HleLibrary*>(lib)) {
    // Registered APIs override the backing, including its own relocations.
    if (auto address = hle->hle_symbol(name)) return {lib, nullptr, address};
    if (!hle->mapped_image()) return {};
  }
  if (auto* image = lib->mapped_image()) {
    auto* symbol = image->lookup(name, version);
    if (!symbol) return {};
    auto address = (symbol->info & 15) == 6 ? symbol->value : (symbol->shndx == 0xfff1 ? symbol->value : image->load_bias + symbol->value);
    return {lib, symbol, address};
  }
  auto address = lib->find_symbol(name, version);
  return address ? Definition{lib, nullptr, address} : Definition{};
}
Definition resolve(State& s, elf::Image* requester, const std::vector<Library*>& local,
                   std::string_view name, std::string_view version, bool skip_self = false,
                   bool relocation = false) {
  if (requester && requester->symbolic && !skip_self) {
    auto d = lookup_one(requester->owner, name, version); if (d.library) return d;
  }
  // The host executable substitutes for app_process: its startup HLE APIs
  // must not disappear merely because their slots have no guest ELF flags.
  for (auto soname : startup_libraries) {
    auto it = s.names.find(std::string(soname));
    if (it == s.names.end()) continue;
    auto* lib = it->second.get();
    if (skip_self && requester && lib == requester->owner) continue;
    auto d = lookup_one(lib, name, version); if (d.library) return d;
  }
  for (auto* lib : s.globals) {
    if (skip_self && requester && lib == requester->owner) continue;
    // Registered APIs belong to the stable HLE slot, not to an optional APK
    // backing image. Guest backing exports still obey bionic's ELF global group.
    if (auto* hle = dynamic_cast<HleLibrary*>(lib))
      if (auto address = hle->hle_symbol(name)) return {lib, nullptr, address};
    if (auto* image = lib->mapped_image(); relocation && (!image || !(image->dynamic(0x6ffffffb) & 2))) continue;
    auto d = lookup_one(lib, name, version); if (d.library) return d;
  }
  for (auto* lib : local) {
    if (skip_self && requester && lib == requester->owner) continue;
    auto d = lookup_one(lib, name, version); if (d.library) return d;
  }
  return {};
}
GuestAddr ifunc(State& s, elf::Image& image, GuestAddr resolver) {
  if (s.inspect) { ++image.report.deferred_resolvers; return 0; }
  if (auto it = image.ifunc_results.find(resolver - image.load_bias); it != image.ifunc_results.end()) return it->second;
  auto* thread = GuestThread::current();
  if (!thread) throw std::runtime_error("ifunc resolution requires a guest thread");
  // bionic/libc/bionic/bionic_call_ifunc_resolver.cpp and sys/ifunc.h.
  struct IfuncArg { std::uint64_t size, hwcap, hwcap2; } argument{sizeof(IfuncArg), cpu_backend().info().hwcap, cpu_backend().info().hwcap2};
  auto result = thread->call(resolver, {argument.hwcap | (1ull << 62), h2g(&argument)});
  image.ifunc_results[resolver - image.load_bias] = result;
  return result;
}
std::string infer_provider(const elf::Image& image, std::string_view name, const elf::SymbolVersion& version) {
  if (!version.provider.empty()) return version.provider;
  auto needed = [&](std::string_view n) { return std::find(image.report.needed.begin(), image.report.needed.end(), n) != image.report.needed.end(); };
  if (name.starts_with("__android_log")) return "liblog.so";
  if (name.starts_with("egl")) return "libEGL.so";
  if (name.starts_with("gl") && !name.starts_with("glob")) return needed("libGLESv3.so") ? "libGLESv3.so" : "libGLESv2.so";
  if (name.starts_with("vk")) return "libvulkan.so";
  if (name.starts_with("xr")) return "libopenxr_loader.so";
  if (name.starts_with("vrapi_")) return "libvrapi.so";
  if (image.isa() == GuestIsa::a32) {
    if (name.starts_with("ovrp_")) return "libOVRPlugin.so";
    if (name.starts_with("ovr_") || name.starts_with("ovrID_") || name.starts_with("ovrKeyValuePair_")) return "libovrplatformloader.so";
    if (name.starts_with("vrintegration_")) return "libvrintegrationloader.so";
    if (name == "__gnu_Unwind_Find_exidx" || name == "dl_unwind_find_exidx") return "libdl.so";
  }
  if (name.starts_with("AAsset") || name.starts_with("ANative") || name.starts_with("ALooper") || name.starts_with("AInput") || name.starts_with("ASensor")) return "libandroid.so";
  if (name.starts_with("AAudio")) return "libaaudio.so";
  if (name.starts_with("AMedia")) return "libmediandk.so";
  if (name.starts_with("sl") || name.starts_with("SL_")) return "libOpenSLES.so";
  if (name.starts_with("inflate") || name.starts_with("deflate") || name == "crc32" || name == "adler32" || name == "uncompress" || name == "compress") return "libz.so";
  if (name == "dlopen" || name == "dlsym" || name == "dlclose" || name == "dlerror" || name == "dladdr" || name == "dl_iterate_phdr" || name.starts_with("android_dlopen")) return "libdl.so";
  static const std::set<std::string_view> math{"sin", "sinf", "cos", "cosf", "tan", "tanf", "asin", "asinf", "acos", "acosf", "atan", "atanf", "atan2", "atan2f", "sqrt", "sqrtf", "pow", "powf", "exp", "expf", "exp2", "exp2f", "log", "logf", "log2", "log2f", "log10", "log10f", "floor", "floorf", "ceil", "ceilf", "round", "roundf", "fmod", "fmodf", "fmin", "fminf", "fmax", "fmaxf", "sincos", "sincosf", "ldexp", "ldexpf", "frexp", "frexpf", "modf", "modff", "hypot", "hypotf", "trunc", "truncf", "tanh", "tanhf"};
  if (math.contains(name)) return "libm.so";
  if (name.starts_with("pthread_") || name.starts_with("__") || name.starts_with("str") || name.starts_with("mem") ||
      name.starts_with("wcs") || name.starts_with("sig") || name.starts_with("sem_") || name.starts_with("clock_") ||
      name.starts_with("sched_") || name.starts_with("socket") || name.starts_with("getaddr")) return "libc.so";
  static const std::set<std::string_view> libc{"malloc", "calloc", "realloc", "free", "abort", "exit", "atexit", "open", "open64", "close", "read", "write", "fopen", "fclose", "fread", "fwrite", "printf", "snprintf", "vsnprintf", "fprintf", "sprintf", "sscanf", "qsort", "bsearch", "mmap", "mmap64", "munmap", "mprotect", "madvise", "syscall", "getenv", "setenv", "getauxval", "fstat", "stat", "access", "lseek", "lseek64", "time", "gettimeofday", "getpid", "gettid", "usleep", "nanosleep", "sleep", "poll", "select"};
  if (libc.contains(name)) return "libc.so";
  return "unknown";
}
void relocate_a64(State& s, elf::Image& image, const std::vector<Library*>& group,
              std::vector<std::function<void()>>& resolvers) {
  if (image.relocated) return;
  std::set<std::pair<std::string, std::string>> missing;
  image.relative_relocations([&](auto offset) {
    auto p = image.address(offset, 8); std::uint64_t value;
    std::memcpy(&value, g2h(p), 8); value += image.load_bias; std::memcpy(g2h(p), &value, 8);
    ++image.report.relocations["RELR"];
  });
  image.relocations([&](const elf::Rela& r) {
    auto type = static_cast<std::uint32_t>(r.info);
    ++image.report.relocations[elf::relocation_name(type)];
    if (!type) return;
    auto p = image.address(r.offset, type == 1031 ? 16 : ((type == 258 || type == 261) ? 4 : 8));
    auto put = [p](std::uint64_t value) { std::memcpy(g2h(p), &value, 8); };
    if (type == 1027) { put(image.load_bias + r.addend); return; }
    if (type == 1032) {
      resolvers.push_back([&, p, addend = r.addend] { auto value = ifunc(s, image, image.load_bias + addend); std::memcpy(g2h(p), &value, 8); });
      return;
    }
    auto index = static_cast<std::uint32_t>(r.info >> 32);
    const auto& symbol = image.symbol_at(index);
    auto name = image.symbol_name(symbol);
    auto version = image.symbol_version(index);
    Definition def;
    if (symbol.shndx && ((symbol.info >> 4) == 0 || (symbol.other & 3) == 2 || (symbol.other & 3) == 3))
      def = {image.owner, &symbol, (symbol.info & 15) == 6 ? symbol.value : (symbol.shndx == 0xfff1 ? symbol.value : image.load_bias + symbol.value)};
    else def = resolve(s, &image, group, name, version.name, type == 1024, true);
    if (!index && (type == 1028 || type == 1029 || type == 1030 || type == 1031)) def = {image.owner, &symbol, 0};
    if (!def.library && name == "__tls_get_addr") def.address = tls::get_addr_thunk();
    if (!def.library && !def.address && (symbol.info >> 4) != 2) {
      auto provider = infer_provider(image, name, version);
      s.unresolved.insert(provider + "!" + name);
      if (missing.emplace(name, version.name).second) image.report.unresolved.push_back({name, version.name, provider, false, 0, 0, 0, {}, {}});
      def.address = hle::missing_symbol(provider, name);
    }
    auto apply = [&, r, type, p, def, symbol] {
      auto value = def.address;
      if (def.symbol && (def.symbol->info & 15) == 10)
        value = ifunc(s, *def.library->mapped_image(), def.address);
      value += r.addend;
      switch (type) {
        case 257: case 1025: case 1026: std::memcpy(g2h(p), &value, 8); break;
        case 258: case 261: {
          if (type == 261) value -= p;
          auto narrow = static_cast<std::uint32_t>(value); std::memcpy(g2h(p), &narrow, 4); break;
        }
        case 260: value -= p; std::memcpy(g2h(p), &value, 8); break;
        case 1024:
          log::warn("linker", "COPY relocation in {}", image.soname());
          image.address(r.offset, symbol.size);
          if (!def.library && def.address) throw std::runtime_error("COPY source is an unresolved import in " + image.soname());
          if (!def.address) std::memset(g2h(p), 0, symbol.size);
          else std::memcpy(g2h(p), g2h(def.address), std::min(symbol.size, def.symbol ? def.symbol->size : symbol.size));
          break;
        case 1028: value = def.library ? def.library->tls_module_id : 0; std::memcpy(g2h(p), &value, 8); break;
        case 1029: std::memcpy(g2h(p), &value, 8); break;
        case 1030: value += def.library ? def.library->tls_offset : 0; std::memcpy(g2h(p), &value, 8); break;
        case 1031: {
          auto resolver = def.library ? tls::descriptor_resolver() :
              ((symbol.info >> 4) == 2 ? tls::weak_descriptor_resolver() : def.address);
          std::array<std::uint64_t, 2> descriptor{resolver, value + (def.library ? def.library->tls_offset : 0)};
          std::memcpy(g2h(p), descriptor.data(), 16); break;
        }
        default: throw std::runtime_error("unsupported " + elf::relocation_name(type) + " in " + image.soname());
      }
    };
    if (def.symbol && (def.symbol->info & 15) == 10) resolvers.push_back(std::move(apply));
    else apply();
  });
  image.relocated = true;
}
Definition resolve_arm_import(State& s, elf::Image& image, const std::vector<Library*>& group,
                              const elf::Sym& symbol, std::string_view name, std::string_view version) {
  Definition def;
  if (symbol.shndx && ((symbol.info >> 4) == 0 || (symbol.other & 3) == 2 || (symbol.other & 3) == 3))
    def = {image.owner, &symbol, symbol.shndx == 0xfff1 ? symbol.value : image.load_bias + symbol.value};
  else def = resolve(s, &image, group, name, version, false, true);
  if (def.symbol && ((def.symbol->info & 15) == 10 || (def.symbol->info & 15) == 6))
    throw std::runtime_error("ARM32 IFUNC/native TLS symbol is not implemented: " + std::string(name));
  // OBJECT imports require actual data storage, even if a provider mistakenly
  // registers that spelling as callable.
  if (def.library && (symbol.info & 15) == 1) {
    if (auto* hle = dynamic_cast<HleLibrary*>(def.library); hle && hle->is_function(name)) return {};
    if (def.symbol && (def.symbol->info & 15) != 1) return {};
  }
  return def;
}
void relocate_a32(State& s, elf::Image& image, const std::vector<Library*>& group) {
  if (image.relocated) return;
  // No pointer truncation: AddressSpace supplies the memory packet's arena
  // policy. Standalone inspection tests supply a minimal low allocator.
  if (image.map_start >= (1ull << 32) || image.map_size > (1ull << 32) - image.map_start)
    throw std::runtime_error("ARM32 image requires the low-4 GiB AddressSpace arena: " + image.soname());
  std::map<std::size_t, Definition> definitions;
  std::set<std::size_t> missing;
  auto definition = [&](std::size_t index) -> Definition {
    if (!index) return {}; // ELF's null symbol denotes S=0, not a missing import.
    if (auto it = definitions.find(index); it != definitions.end()) return it->second;
    const auto& symbol = image.symbol_at(index);
    auto name = image.symbol_name(symbol);
    auto version = image.symbol_version(index);
    auto def = resolve_arm_import(s, image, group, symbol, name, version.name);
    if (def.address > UINT32_MAX) {
      if (!s.inspect) throw std::runtime_error("ARM32 import address exceeds 32 bits: " + name);
      def = {}; // unavailable storage is reported, never truncated
    }
    if (!def.library && !def.address) {
      missing.insert(index);
      if ((symbol.info >> 4) != 2) {
        auto provider = infer_provider(image, name, version);
        s.unresolved.insert(provider + "!" + name);
        if (!s.inspect && (symbol.info & 15) != 1) {
          def.address = hle::missing_symbol(provider, name);
          if (def.address > UINT32_MAX) throw std::runtime_error("ARM32 diagnostic thunk exceeds 32 bits: " + name);
        } else if (!s.inspect) throw std::runtime_error("unresolved ARM32 data import: " + name);
      }
    }
    definitions[index] = def; return def;
  };
  image.relative_relocations([&](auto offset) {
    auto p = image.address(offset, 4); std::uint32_t value;
    std::memcpy(&value, g2h(p), 4); value += static_cast<std::uint32_t>(image.load_bias); std::memcpy(g2h(p), &value, 4);
    ++image.report.relocations["RELR"];
  });
  image.relocations([&](const elf::Rela& r) {
    const auto type = static_cast<std::uint32_t>(r.info);
    auto op = std::find_if(image.arm_apply_table.begin(), image.arm_apply_table.end(),
                           [&](const auto& entry) { return entry.type == type; });
    if (op == image.arm_apply_table.end())
      throw std::runtime_error("unsupported dynamic " + elf::relocation_name32(type) + " in " + image.soname());
    ++image.report.relocations[op->name];
    if (op->operation == elf::ArmRelocation::none) return;
    auto p = image.address(r.offset, 4);
    auto base = op->operation == elf::ArmRelocation::relative ? image.load_bias : definition(r.info >> 32).address;
    // AAELF32 arithmetic is modulo 2^32. S retains the Thumb bit; GOT/PLT
    // addends were normalized to zero by the REL reader.
    const auto value = static_cast<std::uint32_t>(base + r.addend);
    std::memcpy(g2h(p), &value, 4);
  });
  for (std::size_t i = 1; i < image.symbols().size(); ++i) {
    const auto& symbol = image.symbol_at(i);
    if (symbol.shndx || !symbol.name) continue;
    auto name = image.symbol_name(symbol);
    auto version = image.symbol_version(i);
    auto def = definition(i);
    elf::Import import{name, version.name, def.library ? def.library->soname() : infer_provider(image, name, version),
                       (symbol.info >> 4) == 2, static_cast<std::uint8_t>(symbol.info & 15), i, def.address, {}, {}};
    import.resolution = missing.contains(i) ? (import.weak ? "weak-zero" : "unresolved") :
        (def.library && !def.symbol ? "hle" : "guest");
    import.coverage = s.import_coverage ? s.import_coverage(image, import) : "unassessed";
    if (missing.contains(i)) image.report.unresolved.push_back(import);
    image.report.imports.push_back(std::move(import));
  }
  image.relocated = true;
}
void relocate(State& s, elf::Image& image, const std::vector<Library*>& group,
              std::vector<std::function<void()>>& resolvers) {
  // One selection per image; A64's existing apply path remains unchanged.
  if (image.isa() == GuestIsa::a32) relocate_a32(s, image, group);
  else relocate_a64(s, image, group, resolvers);
}
GuestAddr symbol_address(State& s, Definition d) {
  if (!d.library) return d.address;
  if (d.symbol && (d.symbol->info & 15) == 10) return ifunc(s, *d.library->mapped_image(), d.address);
  if (d.symbol && (d.symbol->info & 15) == 6) {
    auto* thread = GuestThread::current(); return thread ? tls::get_addr(*thread, d.library->tls_module_id, d.symbol->value) : 0;
  }
  return d.address;
}
GuestAddr MainLibrary::find_symbol(std::string_view name, std::string_view version) const {
  auto& s = state(); std::lock_guard lock(s.mutex);
  // bionic/linker/linker.cpp: find_library(nullptr) selects somain, and
  // dlsym_handle_lookup(somain) searches its dependencies plus RTLD_GLOBAL.
  // Venture's native process replaces app_process, whose basic system
  // dependencies precede libraries subsequently opened by Java or the guest.
  for (auto dependency : startup_libraries) {
    auto it = s.names.find(std::string(dependency));
    if (it == s.names.end()) continue;
    auto d = lookup_one(it->second.get(), name, version);
    if (d.library) return symbol_address(s, d);
  }
  auto d = resolve(s, nullptr, {}, name, version);
  return d.library ? symbol_address(s, d) : 0;
}
}
Linker& Linker::get() { static Linker instance; return instance; }
void Linker::register_hle_library(std::shared_ptr<HleLibrary> lib) {
  auto& s = state(); std::lock_guard lock(s.mutex);
  if (lib->soname() == "libc.so" && !lib->hle_symbol("__tls_get_addr")) lib->add_function("__tls_get_addr", tls::get_addr_thunk());
  s.libraries.push_back(lib); s.names[lib->soname()] = lib; s.names[lib->path()] = lib;
  if (process_config().isa() == GuestIsa::a64) s.names[process_image::system_path(lib->soname())] = lib;
  for (const auto& alias : lib->aliases()) s.names[alias] = lib;
}
void Linker::set_search_paths(std::vector<std::string> paths) { auto& s = state(); std::lock_guard lock(s.mutex); s.search_paths = std::move(paths); s.exports.clear(); s.backing_files.clear(); }
Library* Linker::open_main(int flags) {
  // bionic accepts these flag bits even when no filename is supplied.
  if (flags & ~(kRtldNow | kRtldLazy | kRtldGlobal | kRtldNoDelete | kRtldNoLoad)) {
    last_error = "invalid flags to dlopen";
    log::error("linker", "{}: {:#x}", last_error, flags);
    return nullptr;
  }
  return &state().main;
}
Library* Linker::open(std::string_view name, int flags, Library*, const elf::Placement* placement) {
  // bionic matches the executable by realpath, like any other loaded object.
  if (process_image::published() && name == process_image::program_path()) return open_main(flags);
  return open_impl(name, flags, false, placement);
}
void Linker::publish_process_view() {
  auto& s = state(); auto lock = guest_loader_lock();
  process_image::sync(s.libraries);
}
Library* Linker::open_impl(std::string_view name, int flags, bool backing, const elf::Placement* placement) {
  auto& s = state(); auto lock = guest_loader_lock();
  const auto initial_count = s.libraries.size();
  const auto initial_globals = s.globals;
  const auto initial_names = s.names;
  const auto initial_files = s.backing_files;
  std::set<elf::Image*> initial_images;
  std::vector<Library*> initial_groups;
  for (const auto& library : s.libraries) {
    initial_groups.push_back(library->group_root);
    if (auto* image = library->mapped_image()) initial_images.insert(image);
  }
  std::shared_ptr<Library> root;
  try {
    root = load(s, name, flags, backing, placement);
    struct InitializationOwnership {
      Library& owner;
      explicit InitializationOwnership(Library& root) : owner(root) { ++owner.lifecycle_references; }
      ~InitializationOwnership() { --owner.lifecycle_references; }
    } initializing(*root);
    auto* group_root = root->group_root ? root->group_root : root.get();
    auto queue = local_group(group_root);
    std::set<Library*> seen(queue.begin(), queue.end());
    auto closure = [&] {
      for (std::size_t i = 0; i < queue.size(); ++i) {
        auto* image = queue[i]->mapped_image();
        auto* slot = dynamic_cast<HleLibrary*>(queue[i]);
        // A pure host provider has no ELF local group to retain. Only an
        // original backing (including a not-yet-mapped one) owns that scope.
        const bool original = image || !slot ||
            (s.backing_files.contains(slot) && !s.backing_files.at(slot).empty());
        if (original && !queue[i]->group_root) queue[i]->group_root = group_root;
        if (!image) continue;
        image->group_root = queue[i]->group_root;
        if (image->dependencies.empty()) for (const auto& needed : image->report.needed) image->dependencies.push_back(load(s, needed, 0).get());
        for (auto* dep : image->dependencies) if (seen.insert(dep).second) queue.push_back(dep);
      }
    };
    closure();
    // Inspect actual relocation demand before mapping a replacement's original
    // ELF. Ordinary providers in the Android lookup scope always win first.
    bool activated;
    do {
      activated = false;
      for (std::size_t i = 0; i < queue.size(); ++i) {
        auto* dependent = queue[i]->mapped_image();
        if (!dependent || dependent->relocated) continue;
        std::set<std::size_t> imports;
        dependent->relocations([&](const elf::Rela& relocation) {
          if (const auto index = relocation.info >> 32) imports.insert(index);
        });
        for (auto index : imports) {
          const auto& symbol = dependent->symbol_at(index);
          if (symbol.shndx || !symbol.name) continue;
          const auto name = dependent->symbol_name(symbol);
          const auto version = dependent->symbol_version(index).name;
          if (resolve(s, dependent, queue, name, version, false, true).library) continue;
          const auto candidates = queue;
          for (auto* candidate : candidates) {
            auto* slot = dynamic_cast<HleLibrary*>(candidate);
            if (!slot || !backing_exports(s, slot, name, version)) continue;
            load(s, slot->soname(), 0, true);
            activated = true;
            closure();
            log::info("linker", "Activated guest backing {} for {}", slot->soname(), name);
            if (resolve(s, dependent, queue, name, version, false, true).library) break;
          }
        }
      }
    } while (activated);
    std::vector<std::function<void()>> resolvers;
    if (flags & kRtldGlobal) for (auto* lib : local_group(root.get())) promote(s, lib);
    for (auto* lib : queue) if (auto* image = lib->mapped_image()) relocate(s, *image, queue, resolvers);
    for (auto& resolver : resolvers) resolver();
    for (auto* library : queue) if (auto* image = library->mapped_image(); image && !initial_images.contains(image)) image->protect_segments();
    if (!backing) ++root->references;
    root->global |= (flags & kRtldGlobal) != 0; root->nodelete |= (flags & kRtldNoDelete) != 0;
    const bool initialize = !s.inspect;
    // bionic publishes new objects to the debugger interface before running
    // their constructors (notify_gdb_of_load in find_libraries).
    process_image::sync(s.libraries);
    if (initialize) for (auto* library : queue) if (auto* image = library->mapped_image()) image->initialize();
    return root.get();
  } catch (const ProcessExit&) { throw; }
  catch (const ThreadExit&) { throw; }
  catch (const std::exception& e) {
    last_error = e.what(); log::error("linker", "{}", last_error);
    // Roll back every newly introduced object after a failed load; retain previously live images.
    std::set<Library*> removed;
    for (std::size_t i = initial_count; i < s.libraries.size(); ++i) removed.insert(s.libraries[i].get());
    s.names = initial_names;
    s.backing_files = initial_files;
    std::erase_if(s.exidx_images, [&](auto* image) { return removed.contains(image); });
    s.globals = initial_globals;
    std::erase_if(s.exidx_images, [&](auto* image) { return !initial_images.contains(image); });
    for (std::size_t i = 0; i < initial_count; ++i)
      if (auto* hle = dynamic_cast<HleLibrary*>(s.libraries[i].get()); hle && hle->mapped_image() && !initial_images.contains(hle->mapped_image())) hle->set_backing({});
    for (std::size_t i = 0; i < initial_count; ++i) {
      s.libraries[i]->group_root = initial_groups[i];
      s.libraries[i]->global = std::find(initial_globals.begin(), initial_globals.end(), s.libraries[i].get()) != initial_globals.end();
    }
    s.libraries.resize(initial_count);
    return nullptr;
  }
}
bool Linker::activate_backing(HleLibrary* slot, std::string_view name, std::string_view version) {
  auto& s = state(); std::lock_guard lock(s.mutex);
  if (std::none_of(s.libraries.begin(), s.libraries.end(), [&](const auto& p) { return p.get() == slot; })) return false;
  bool live = slot->references || slot->nodelete || slot->lifecycle_references;
  for (const auto& root : s.libraries) if (!live && (root->references || root->nodelete || root->lifecycle_references)) {
    const auto group = local_group(root.get());
    live = std::find(group.begin(), group.end(), slot) != group.end();
  }
  if (!live || !backing_exports(s, slot, name, version)) return false;
  return open_impl(slot->soname(), slot->global ? kRtldGlobal : 0, true) != nullptr;
}
void Linker::prepare_java_exports(const Library& library) {
  auto& s = state(); std::lock_guard lock(s.mutex);
  auto* slot = dynamic_cast<HleLibrary*>(canonical_caller(s, const_cast<Library*>(&library)));
  if (!slot || slot->mapped_image()) return;
  // Populate the same verified ELF export-name cache used for scoped misses.
  backing_exports(s, slot, {}, {});
  const auto file = s.backing_files.find(slot);
  if (file == s.backing_files.end() || file->second.empty()) return;
  const auto names = s.exports.find({file->second, {}});
  if (names == s.exports.end()) return;
  for (const auto& name : names->second) if (name.starts_with("Java_")) {
    if (!activate_backing(slot, name))
      throw std::runtime_error(last_error.empty() ? "cannot activate Java native backing" : last_error);
    return;
  }
}
int Linker::close(Library* lib) {
  auto& s = state(); auto lock = guest_loader_lock();
  if (lib == &s.main) return 0; // The process itself cannot be unloaded.
  if (!lib || std::none_of(s.libraries.begin(), s.libraries.end(), [&](const auto& p) { return p.get() == lib; })) { last_error = "invalid dlclose handle"; return -1; }
  if (lib->references) --lib->references;
  // References from other images keep the dependency closure alive.
  auto retained = [&] {
    std::set<Library*> reachable;
    for (const auto& p : s.libraries) if (p->references || p->nodelete || p->lifecycle_references)
      for (auto* dependency : local_group(p.get())) reachable.insert(dependency);
    // A dependency keeps its original local lookup group alive, even if its root's
    // dlopen handle has been closed. This also preserves lookup through group_root.
    bool added;
    do {
      added = false;
      auto previous = reachable;
      for (auto* lib : previous) if (lib->group_root)
        for (auto* member : local_group(lib->group_root)) added |= reachable.insert(member).second;
    } while (added);
    return reachable;
  };
  auto reachable = retained();
  std::vector<std::shared_ptr<Library>> retired;
  for (const auto& p : s.libraries) if (p->mapped_image() && !reachable.contains(p.get())) retired.push_back(p);
  elf::finalize_images(retired);
  // Finalizers may reopen a library; retain that new reference and closure.
  reachable = retained();
  std::set<Library*> dead;
  std::set<elf::Image*> dead_images;
  for (auto& p : retired) if (!reachable.contains(p.get())) { dead.insert(p.get()); dead_images.insert(p->mapped_image()); }
  std::erase_if(s.names, [&](const auto& p) { return !p.second->is_hle() && dead.contains(p.second.get()); });
  std::erase_if(s.libraries, [&](const auto& p) { return !p->is_hle() && dead.contains(p.get()); });
  std::erase_if(s.globals, [&](auto* p) {
    if (!dead.contains(p) && (!p->is_hle() || reachable.contains(p))) return false;
    p->global = false; return true;
  });
  std::erase_if(s.exidx_images, [&](auto* p) { return dead_images.contains(p); });
  for (auto& p : retired) if (auto* hle = dynamic_cast<HleLibrary*>(p.get()); hle && dead.contains(p.get())) { hle->set_backing({}); hle->global = false; }
  for (auto& p : s.libraries) if (!reachable.contains(p.get())) p->group_root = nullptr;
  process_image::sync(s.libraries);
  return 0;
}
GuestAddr Linker::symbol(Library* lib, std::string_view name) {
  auto& s = state(); auto lock = guest_loader_lock();
  if (!lib) return symbol_default(name, nullptr);
  if (lib != &s.main && std::none_of(s.libraries.begin(), s.libraries.end(), [&](const auto& p) { return p.get() == lib; })) {
    last_error = "invalid dlsym handle"; return 0;
  }
  for (auto* candidate : local_group(lib)) {
    auto d = lookup_one(candidate, name, {}); if (d.library) return symbol_address(s, d);
  }
  for (auto* candidate : local_group(lib))
    if (auto* slot = dynamic_cast<HleLibrary*>(candidate); slot && activate_backing(slot, name)) return symbol(lib, name);
  last_error = "symbol not found: " + std::string(name); return 0;
}
GuestAddr Linker::symbol_default(std::string_view name, Library* caller) {
  auto& s = state(); auto lock = guest_loader_lock();
  if (caller && !(caller = canonical_caller(s, caller))) {
    last_error = "invalid RTLD_DEFAULT caller"; return 0;
  }
  auto* image = (caller ? caller->mapped_image() : nullptr);
  auto local = local_group(image && image->group_root ? image->group_root : caller);
  auto d = resolve(s, (caller ? caller->mapped_image() : nullptr), local, name, {});
  if (d.library) return symbol_address(s, d);
  auto scope = s.globals; scope.insert(scope.end(), local.begin(), local.end());
  for (auto* candidate : scope)
    if (auto* slot = dynamic_cast<HleLibrary*>(candidate); slot && activate_backing(slot, name)) return symbol_default(name, caller);
  last_error = "symbol not found: " + std::string(name); return 0;
}
GuestAddr Linker::symbol_next(std::string_view name, Library* caller) {
  auto& s = state(); auto lock = guest_loader_lock();
  if (caller && !(caller = canonical_caller(s, caller))) {
    last_error = "invalid RTLD_NEXT caller"; return 0;
  }
  std::vector<Library*> scope;
  std::set<Library*> seen;
  for (auto* lib : s.globals) { scope.push_back(lib); seen.insert(lib); }
  auto* image = (caller ? caller->mapped_image() : nullptr);
  for (auto* lib : local_group(image && image->group_root ? image->group_root : caller))
    if (seen.insert(lib).second) scope.push_back(lib);
  bool after = !caller;
  for (auto* lib : scope) {
    if (after) { auto d = lookup_one(lib, name, {}); if (d.library) return symbol_address(s, d); }
    if (lib == caller) after = true;
  }
  after = !caller;
  for (auto* lib : scope) {
    if (after) if (auto* slot = dynamic_cast<HleLibrary*>(lib); slot && activate_backing(slot, name)) return symbol_next(name, caller);
    if (lib == caller) after = true;
  }
  last_error = "RTLD_NEXT symbol not found: " + std::string(name); return 0;
}
Library* Linker::library_at(GuestAddr p) {
  auto& s = state(); std::lock_guard lock(s.mutex);
  for (auto& lib : s.libraries)
    if (lib->mapped_image() && p >= lib->map_start && p - lib->map_start < lib->map_size) return lib.get();
  return nullptr;
}
bool Linker::address_info(GuestAddr p, DlInfo* out) {
  auto& s = state(); auto lock = guest_loader_lock();
  for (auto& lib : s.libraries) if (lib->mapped_image() && p >= lib->map_start && p - lib->map_start < lib->map_size) {
    if (out) { *out = {lib->path(), lib->load_bias, {}, 0}; lib->mapped_image()->nearest_symbol(p, *out); }
    return true;
  }
  return process_image::address_info(p, out);
}
bool Linker::try_address_info(GuestAddr p, DlInfo* out) {
  if (out) *out = {};
  auto& s = state(); std::unique_lock lock(s.mutex, std::try_to_lock);
  return lock.owns_lock() && address_info(p, out);
}
GuestAddr Linker::try_find_exidx(GuestAddr pc, std::int32_t* count) {
  if (count) *count = 0;
  auto& s = state(); std::unique_lock lock(s.mutex, std::try_to_lock);
  return lock.owns_lock() ? find_exidx(pc, count) : 0;
}
std::string Linker::take_error() { auto text = std::move(last_error); last_error.clear(); return text; }
std::vector<std::shared_ptr<const HleLibrary>> Linker::hle_libraries() const {
  auto& s = state(); std::lock_guard lock(s.mutex);
  std::vector<std::shared_ptr<const HleLibrary>> result;
  for (const auto& lib : s.libraries)
    if (auto hle = std::dynamic_pointer_cast<const HleLibrary>(lib)) result.push_back(std::move(hle));
  return result;
}
void Linker::for_each_image(const std::function<void(const Library&)>& visit) {
  std::vector<std::shared_ptr<Library>> copy;
  std::vector<const Library*> unwind;
  { auto& s = state(); auto lock = guest_loader_lock();
    for (const auto& lib : s.libraries) {
      if (auto* hle = dynamic_cast<HleLibrary*>(lib.get())) { if (auto image = hle->backing()) copy.push_back(std::move(image)); }
      else if (lib->mapped_image()) copy.push_back(lib);
    }
    // A32 personality selection queries loader metadata. Snapshot it under
    // the cancellable entry acquisition, before callbacks can initiate exit.
    // Published synthetic unwind images are retained until process exit.
    hle::for_each_unwind_image([&](const Library& image) { unwind.push_back(&image); });
  }
  for (auto& lib : copy) visit(*lib);
  for (const auto* image : unwind) visit(*image);
}
bool Linker::try_for_each_image(const std::function<void(const Library&)>& visit) {
  std::vector<std::shared_ptr<Library>> copy;
  {
    auto& s = state(); std::unique_lock lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    for (const auto& lib : s.libraries) {
      if (auto* hle = dynamic_cast<HleLibrary*>(lib.get())) { if (auto image = hle->backing()) copy.push_back(std::move(image)); }
      else if (lib->mapped_image()) copy.push_back(lib);
    }
  }
  for (auto& lib : copy) visit(*lib);
  return true;
}
std::vector<std::string> Linker::unresolved_symbols() const {
  auto& s = state(); std::lock_guard lock(s.mutex); return {s.unresolved.begin(), s.unresolved.end()};
}
GuestAddr Linker::find_exidx(GuestAddr pc, std::int32_t* count) {
  auto& s = state(); auto lock = guest_loader_lock();
  if (auto table = hle::unwind_exidx(pc & ~GuestAddr{1}, count)) return table;
  if (count) *count = 0;
  pc &= ~GuestAddr{1};
  auto it = std::upper_bound(s.exidx_images.begin(), s.exidx_images.end(), pc,
                             [](auto address, auto* image) { return address < image->map_start; });
  if (it == s.exidx_images.begin()) return 0;
  auto* image = *std::prev(it);
  if (pc - image->map_start >= image->map_size || !image->exidx) return 0;
  if (count) *count = static_cast<std::int32_t>(image->exidx_count);
  return image->exidx;
}
void linker_set_import_coverage_hook(ImportCoverageHook hook) {
  auto& s = state(); std::lock_guard lock(s.mutex); s.import_coverage = std::move(hook);
}
void linker_set_inspection(bool enabled) { auto& s = state(); std::lock_guard lock(s.mutex); s.inspect = enabled; }
std::vector<elf::Report> linker_reports() {
  auto& s = state(); std::lock_guard lock(s.mutex);
  std::vector<elf::Report> result;
  for (auto& lib : s.libraries) if (auto* image = lib->mapped_image()) result.push_back(image->report);
  return result;
}
void LoaderMutex::lock_guest() {
  if (mutex_.try_lock()) return; // Includes same-thread constructor reentry.
  auto* thread = GuestThread::current();
  if (!thread) { mutex_.lock(); return; }
  // Use the library's timed native lock, retaining immediate unlock wakeups.
  // A finalizer can join this worker while holding the loader lock; bounded
  // acquisition lets the normal shutdown interruption unwind that worker.
  do { check_guest_exit(*thread); }
  while (!mutex_.try_lock_for(std::chrono::milliseconds(10)));
  try { check_guest_exit(*thread); }
  catch (...) { mutex_.unlock(); throw; }
}
bool LoaderMutex::lock_cleanup() noexcept {
  try {
    if (mutex_.try_lock()) return true;
    auto* thread = GuestThread::current();
    do {
      if (guest_process_exit_status() >= 0 || (thread && thread->exit_requested.load())) return false;
    } while (!mutex_.try_lock_for(std::chrono::milliseconds(10)));
    return true;
  } catch (...) { return false; }
}
LoaderMutex& linker_lifecycle_mutex() { return state().mutex; }
std::unique_lock<LoaderMutex> guest_loader_lock() {
  auto& mutex = linker_lifecycle_mutex(); mutex.lock_guest();
  return std::unique_lock<LoaderMutex>(mutex, std::adopt_lock);
}
void linker_finalize() {
  auto& s = state(); std::lock_guard lock(s.mutex);
  auto libraries = s.libraries;
  elf::finalize_images(libraries);
}
}
