// ELF loading and symbol resolution with Android (bionic linker) semantics.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/guest.h"

namespace venture {
namespace elf { class Image; struct Placement; }

// A library the guest can see: either a real ELF image mapped into guest
// memory, or an HLE library whose symbols are host-implemented.
class Library {
 public:
  virtual ~Library() = default;
  const std::string& soname() const { return soname_; }
  const std::string& path() const { return path_; }  // guest-visible path
  bool is_hle() const { return hle_; }
  virtual GuestIsa isa() const { return GuestIsa::a64; }
  virtual elf::Image* mapped_image() const { return nullptr; }

  // Defined symbol lookup (exported, default visibility). `version` may be
  // empty. Returns 0 when absent.
  virtual GuestAddr find_symbol(std::string_view name, std::string_view version = {}) const = 0;

  // Lifetime belongs to the stable slot, including before a backing is mapped.
  std::size_t references = 0;
  // Under linker_lifecycle_mutex: callbacks retain their dependency closure
  // even if a same-thread dlclose consumes the last explicit reference.
  std::size_t lifecycle_references = 0;
  bool global = false, nodelete = false;
  Library* group_root = nullptr;  // Original load group, including inactive HLE slots.

  // ELF images only.
  GuestAddr load_bias = 0;
  GuestAddr phdr = 0;
  std::uint16_t phnum = 0;
  GuestAddr map_start = 0;
  std::uint64_t map_size = 0;
  std::uint64_t tls_module_id = 0;   // 0 = no PT_TLS
  std::int64_t tls_offset = 0;       // static TLS offset from the thread pointer

 protected:
  std::string soname_;
  std::string path_;
  bool hle_ = false;
};

// Symbols provided by Venture for one Android system library.
class HleLibrary final : public Library {
 public:
  HleLibrary(std::string soname, std::vector<std::string> aliases = {});
  void add_function(std::string_view name, GuestAddr thunk);
  void add_data(std::string_view name, GuestAddr address);
  GuestAddr find_symbol(std::string_view name, std::string_view version = {}) const override;
  const std::vector<std::string>& aliases() const { return aliases_; }
  bool is_function(std::string_view name) const;
  GuestAddr hle_symbol(std::string_view name) const;
  GuestIsa isa() const override;
  elf::Image* mapped_image() const override;
  std::shared_ptr<elf::Image> backing() const;
  void set_backing(std::shared_ptr<elf::Image> image);
  // Registration snapshots for ABI coverage and inspection (no dispatch work).
  std::vector<std::pair<std::string, GuestAddr>> functions() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  std::vector<std::string> aliases_;
};

enum DlopenFlags : int {
  kRtldLazy = 0x00001,
  kRtldNow = 0x00002,
  kRtldNoLoad = 0x00004,
  kRtldGlobal = 0x00100,
  kRtldLocal = 0,
  kRtldNoDelete = 0x01000,
};

struct DlInfo {
  std::string path;
  GuestAddr base = 0;
  std::string symbol;
  GuestAddr symbol_address = 0;
};

class Linker {
 public:
  static Linker& get();

  void register_hle_library(std::shared_ptr<HleLibrary> library);
  // Directories searched for bare names (the APK's extracted lib dir first).
  void set_search_paths(std::vector<std::string> host_directories);

  // dlopen(): loads `name` (bare soname or guest path) and its DT_NEEDED
  // closure, relocates, assigns TLS, runs initializers on the calling guest
  // thread. Returns nullptr and sets the dlerror() message on failure.
  Library* open(std::string_view name, int flags, Library* caller, const elf::Placement* placement = nullptr);
  // dlopen(nullptr): the process's system dependencies and RTLD_GLOBAL images.
  // Kept separate from open("") so an empty filename still fails normally.
  Library* open_main(int flags);
  int close(Library* library);
  // Demand an original export from this registered slot only.
  bool activate_backing(HleLibrary* library, std::string_view name, std::string_view version = {});
  // Java native enumeration is a demand even without JNI_OnLoad.
  void prepare_java_exports(const Library& library);
  GuestAddr symbol(Library* library, std::string_view name);   // dlsym; library==nullptr: global
  GuestAddr symbol_default(std::string_view name, Library* caller);  // RTLD_DEFAULT
  GuestAddr symbol_next(std::string_view name, Library* caller);     // RTLD_NEXT
  bool address_info(GuestAddr address, DlInfo* out);                  // dladdr
  Library* library_at(GuestAddr address);  // current guest code's defining image
  // Runtime diagnostics must not hold up a worker which a constructor awaits.
  // False/zero means unavailable or unknown; callers retain raw guest PCs.
  bool try_address_info(GuestAddr address, DlInfo* out);
  GuestAddr try_find_exidx(GuestAddr pc, std::int32_t* count);
  std::string take_error();                                           // dlerror

  // dl_iterate_phdr() data for loaded ELF images and the synthetic unwind image.
  void for_each_image(const std::function<void(const Library&)>& visit);
  // Diagnostics must not wait on a guest holding the loader lock.
  bool try_for_each_image(const std::function<void(const Library&)>& visit);
  std::vector<std::shared_ptr<const HleLibrary>> hle_libraries() const;
  // Mirrors the loaded libraries into the guest's soinfo list (process_image).
  void publish_process_view();
  std::vector<std::string> unresolved_symbols() const;  // diagnostics
  GuestAddr find_exidx(GuestAddr pc, std::int32_t* count); // A32 EHABI, count in entries
 private:
  Library* open_impl(std::string_view name, int flags, bool backing, const elf::Placement* placement = nullptr);
};

}  // namespace venture
