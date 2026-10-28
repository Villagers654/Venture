// The guest's view of process memory (Linux mmap semantics on any host).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "core/guest.h"

namespace venture {

// Linux values, as passed by guest code.
enum : std::uint32_t {
  kProtNone = 0,
  kProtRead = 1,
  kProtWrite = 2,
  kProtExec = 4,
};
enum : std::uint32_t {
  kMapShared = 0x01,
  kMapPrivate = 0x02,
  kMapFixed = 0x10,
  kMapAnonymous = 0x20,
  kMapGrowDown = 0x100,
  kMapStack = 0x20000,
  kMapNoReserve = 0x4000,
  kMapFixedNoReplace = 0x100000,
};

inline constexpr std::uint64_t kStackGuardGap = 256 * kGuestPageSize;
inline constexpr std::uint64_t kDefaultStackLimit = 8 * 1024 * 1024;

struct MemoryRegion {
  GuestAddr start = 0;
  std::uint64_t size = 0;
  std::uint32_t prot = 0;
  std::string name;  // "[anon:...]", "/data/app/.../lib/arm64/libunity.so", "[stack]"
  std::uint64_t file_offset = 0;
  bool grow_down = false;
  GuestAddr gap_start() const {
    return grow_down ? (start >= kStackGuardGap ? start - kStackGuardGap : 0) : start;
  }
};

// Guest-executable pages are kept readable but never host-executable; only JIT
// output executes. Protection changes / unmapping of executable ranges call
// cpu_backend().invalidate().
class AddressSpace {
 public:
  static AddressSpace& get();

  // Startup, after ISA selection and before HLE/JVM/graphics initialization.
  // A32 failure throws; no mappings may precede selection of the A32 policy.
  void initialize(GuestIsa isa);
  bool is_guest_accessible(GuestAddr address, std::uint64_t size, bool write) const;
  // Resolve a guest access fault below an anonymous VM_GROWSDOWN region.
  // Failure leaves the mapping unchanged and the caller delivers SIGSEGV.
  bool grow_stack(GuestAddr address, std::uint32_t access);
  std::uint64_t stack_limit() const;
  void set_stack_limit(std::uint64_t bytes);
  // Native read-only file view in the A32 arena. Private writable files use
  // map(FileReader), which supplies a private copy and supports mprotect.
  std::int64_t map_readonly_file(int host_fd, std::uint64_t size, std::uint32_t prot,
                                 std::uint64_t file_offset, std::string_view name);

  // mmap(2). `file` reads the backing bytes for file mappings (may be null
  // for anonymous). Returns the address or a negative Linux errno.
  using FileReader = std::function<bool(std::uint64_t offset, void* out, std::uint64_t size)>;
  std::int64_t map(GuestAddr hint, std::uint64_t size, std::uint32_t prot, std::uint32_t flags,
                   const FileReader* file, std::uint64_t file_offset, std::string_view name);
  // Coherent OS-backed MAP_SHARED file views. The host fd is borrowed only
  // during this call. Fixed placement and partial view unmapping are unsupported.
  std::int64_t map_shared_file(int host_fd, std::uint64_t size, std::uint32_t prot,
                               std::uint32_t flags, std::uint64_t file_offset, std::string_view name);
  std::int64_t sync(GuestAddr address, std::uint64_t size, bool synchronous);
  std::int64_t unmap(GuestAddr address, std::uint64_t size);
  std::int64_t protect(GuestAddr address, std::uint64_t size, std::uint32_t prot);
  std::int64_t advise(GuestAddr address, std::uint64_t size, int advice);  // MADV_DONTNEED zeroes

  // Runtime-owned guest memory (stacks, TLS, thunks, data symbols).
  GuestAddr allocate(std::uint64_t size, std::uint32_t prot, std::string_view name,
                     std::uint64_t alignment = kGuestPageSize);
  void free(GuestAddr address);

  bool find(GuestAddr address, MemoryRegion* out) const;
  bool is_guest_executable(GuestAddr address) const;
  void for_each(const std::function<void(const MemoryRegion&)>& visit) const;  // /proc/self/maps
  std::string proc_maps() const;  // immutable content for one open, refreshed on mapping changes
  std::uint64_t mapped_size() const;
};

}  // namespace venture
