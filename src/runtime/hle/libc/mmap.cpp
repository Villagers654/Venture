#include "vfs.h"
#include "file_io_trace.h"
#include "abi32.h"
#include "core/address_space.h"
#include "core/hle_runtime.h"
#include "common/log.h"
#include "platform/libc_host.h"
#include <algorithm>
#include <cstring>
namespace venture::hle::libc {
namespace {
std::mutex break_mutex;
GuestAddr break_base = 0, break_current = 0, break_mapped_end = 0;
GuestAddr break_reserved_end = 0;
bool initialize_break() {
  if (break_base) return true;
  // Linux keeps the brk area apart from top-down mmap allocations. Our arena
  // allocators need an explicit reservation to give either ABI that headroom.
  const bool a32 = process_config().isa() == GuestIsa::a32;
  std::uint64_t reserve = a32 ? 32ull << 20 : 1ull << 30;
  auto& space = AddressSpace::get();
  auto map_heap = [&](std::uint64_t size) {
    return space.map(0, size, kProtNone, kMapPrivate | kMapAnonymous | kMapNoReserve, nullptr, 0, "[heap]");
  };
  auto p = map_heap(reserve);
  // A crowded A32 arena may lack that gap, yet sbrk(0) must not fail. A hintless
  // map fits any gap at least its size, so binary-search the largest gap left,
  // down to one page, and search again if another thread takes it first.
  while (p < 0 && a32) {
    std::uint64_t fits = 0;
    for (std::uint64_t low = kGuestPageSize, high = reserve - kGuestPageSize; low <= high;) {
      const auto size = align_down(low + (high - low) / 2, kGuestPageSize);
      if (auto probe = map_heap(size); probe >= 0) {
        space.unmap(probe, size);
        fits = size;
        low = size + kGuestPageSize;
      } else
        high = size - kGuestPageSize;
    }
    if (!fits) return false;
    p = map_heap(reserve = fits);
  }
  if (p < 0) return false;
  if (space.protect(p, kGuestPageSize, kProtRead | kProtWrite) < 0) {
    space.unmap(p, reserve);
    return false;
  }
  break_base = break_current = static_cast<GuestAddr>(p);
  break_mapped_end = break_base + kGuestPageSize;
  break_reserved_end = break_base + reserve;
  return true;
}
GuestAddr resize_break(GuestAddr desired) {
  // Linux mm/mmap.c: requests below the initial break leave it unchanged.
  // Keep its first page as the ELF/BSS tail page would be on Android.
  if (desired < break_base || desired > UINT64_MAX - kGuestPageSize) return break_current;
  auto end = std::max(break_base + kGuestPageSize, align_up(desired, kGuestPageSize));
  auto& space = AddressSpace::get();
  if (end > break_mapped_end) {
    // A guest may explicitly replace part of the reservation with MAP_FIXED.
    // Do not turn such a mapping into heap storage when growing the break.
    // Not atomic: another thread's MAP_FIXED can still land between this check and protect().
    for (auto p = break_mapped_end; p < std::min(end, break_reserved_end);) {
      MemoryRegion region;
      if (!space.find(p, &region) || region.name != "[heap]" || region.prot != kProtNone)
        return break_current;
      p = region.start + region.size;
    }
    if (end > break_reserved_end) {
      auto mapped = space.map(break_reserved_end, end - break_reserved_end, kProtNone,
          kMapPrivate | kMapAnonymous | kMapNoReserve | kMapFixedNoReplace, nullptr, 0, "[heap]");
      if (mapped < 0) return break_current;
      break_reserved_end = end;
    }
    const auto size = end - break_mapped_end;
    if (space.protect(break_mapped_end, size, kProtRead | kProtWrite) < 0) {
      // protect() commits region by region; return any committed part to the reservation.
      space.protect(break_mapped_end, size, kProtNone);
      space.advise(break_mapped_end, size, 4 /* MADV_DONTNEED */);
      return break_current;
    }
  } else if (end < break_mapped_end) {
    const auto size = break_mapped_end - end;
    if (space.protect(end, size, kProtNone) < 0 || space.advise(end, size, 4 /* MADV_DONTNEED */) < 0)
      return break_current;
  }
  break_mapped_end = end;
  return break_current = desired;
}
int brk_fn(GuestAddr desired) {
  std::lock_guard lock(break_mutex);
  if (!initialize_break() || resize_break(desired) < desired) return failure(err::enomem);
  return 0;
}
GuestAddr sbrk_fn(std::int64_t increment) {
  std::lock_guard lock(break_mutex);
  if (!initialize_break()) { failure(err::enomem); return UINT64_MAX; }
  const auto old = break_current;
  // bionic/libc/bionic/brk.cpp returns the OLD break, and (void*)-1/ENOMEM
  // on arithmetic overflow or refused growth. Avoid negating INT64_MIN.
  const auto amount = increment < 0 ? 0ull - static_cast<std::uint64_t>(increment) :
                                     static_cast<std::uint64_t>(increment);
  if ((increment > 0 && amount > UINT64_MAX - old) || (increment < 0 && amount > old)) {
    failure(err::enomem); return UINT64_MAX;
  }
  const auto desired = increment < 0 ? old - amount : old + amount;
  if (increment && resize_break(desired) < desired) { failure(err::enomem); return UINT64_MAX; }
  return old;
}
std::uint32_t sbrk32(std::int32_t increment) {
  // The failure sentinel is an ILP32 pointer, not an overflowing host address.
  return static_cast<std::uint32_t>(sbrk_fn(increment));
}
GuestAddr mmap_impl(GuestAddr hint, std::uint64_t size, int prot, int flags, int fd,
                  std::int64_t offset) {
  if (offset < 0 || offset % 4096 || !size) {
    failure(err::einval);
    return UINT64_MAX;
  }
  AddressSpace::FileReader reader;
  std::shared_ptr<File> file;
  std::string name = "[anon]";
  if (!(flags & kMapAnonymous)) {
    file = FdTable::get().file(fd);
    if (!file) {
      failure(err::ebadf);
      return UINT64_MAX;
    }
    if ((file->flags.load() & 3) == 1 ||
        ((flags & kMapShared) && (prot & kProtWrite) && !(file->flags.load() & 3))) {
      failure(err::eacces);
      return UINT64_MAX;
    }
    if (flags & kMapShared) {
      const int host_fd = file->duplicate_host_fd();
      if (host_fd < 0) { result(host_fd); return UINT64_MAX; }
      auto mapped = AddressSpace::get().map_shared_file(host_fd, size, prot, flags, offset, file->path);
      platform::close_host_fd(host_fd);
      if (mapped < 0) {
        log::error("memory", "shared mmap {} size={} offset={} prot={} failed: Linux errno {}", file->path, size, offset, prot, -mapped);
        result(mapped); return UINT64_MAX;
      }
      return mapped;
    }
    name = file->path;
    reader = [file](std::uint64_t off, void* p, std::uint64_t n) {
      auto r = file->pread(p, n, off);
      if (r < 0) return false;
      if (static_cast<std::uint64_t>(r) < n) std::memset(static_cast<char*>(p) + r, 0, n - r);
      return true;
    };
  }
  auto r =
      AddressSpace::get().map(hint, size, prot, flags, reader ? &reader : nullptr, offset, name);
  if (r < 0) {
    result(r);
    return UINT64_MAX;
  }
  return r;
}
GuestAddr mmap_fn(GuestAddr hint, std::uint64_t size, int prot, int flags, int fd,
                  std::int64_t offset) {
  if (file_io_trace::enabled && !(flags & kMapAnonymous)) {
    auto file = FdTable::get().file(fd);
    auto mapped = mmap_impl(hint, size, prot, flags, fd, offset);
    file_io_trace::mapped(fd, file ? file->path : "[invalid fd]", size, offset,
                          mapped == UINT64_MAX ? -1 : static_cast<std::int64_t>(mapped));
    return mapped;
  }
  if (!census_enabled || (flags & kMapAnonymous)) return mmap_impl(hint, size, prot, flags, fd, offset);
  auto file = FdTable::get().file(fd);
  auto mapped = mmap_impl(hint, size, prot, flags, fd, offset);
  auto* thread = GuestThread::current();
  census_file_mmap(file ? file->path : "[invalid fd]", fd, size, prot, flags, offset,
                   mapped == UINT64_MAX ? -(thread ? *thread->errno_location() : err::eio)
                                        : static_cast<std::int64_t>(mapped));
  return mapped;
}
int munmap_fn(GuestAddr p, std::uint64_t n) { return result(AddressSpace::get().unmap(p, n)); }
std::uint32_t mmap32(GuestAddr p, std::uint64_t n, int prot, int flags, int fd, std::int64_t off) {
  return static_cast<std::uint32_t>(mmap_fn(p, n, prot, flags, fd, off));
}
int mprotect_fn(GuestAddr p, std::uint64_t n, int prot) {
  return result(AddressSpace::get().protect(p, n, prot));
}
int madvise_fn(GuestAddr p, std::uint64_t n, int advice) {
  return result(AddressSpace::get().advise(p, n, advice));
}
int posix_madvise_fn(GuestAddr p, std::uint64_t n, int advice) {
  if (advice < 0 || advice > 4) return err::einval;
  if (advice == 4) return 0;
  return -AddressSpace::get().advise(p, n, advice);
}
int mincore_fn(GuestAddr p, std::uint64_t n, unsigned char* out) {
  if (p % 4096 || !out || n > UINT64_MAX - 4095) return failure(err::einval);
  for (std::uint64_t off = 0; off < align_up(n, 4096); off += 4096) {
    MemoryRegion r;
    if (!AddressSpace::get().find(p + off, &r)) return failure(err::enomem);
    out[off / 4096] = 1;
  }
  return 0;
}
int msync_fn(GuestAddr p, std::uint64_t n, int flags) {
  if (p % 4096 || flags & ~7 || (flags & 5) == 5 || n > UINT64_MAX - p) return failure(err::einval);
  return result(AddressSpace::get().sync(p, n, flags & 4));
}
int mlock_fn(GuestAddr p, std::uint64_t n) { return result(host::memory_lock(p, n, true)); }
int munlock_fn(GuestAddr p, std::uint64_t n) { return result(host::memory_lock(p, n, false)); }
GuestAddr mremap_fn(GuestAddr p, std::uint64_t old_size, std::uint64_t size, int flags, GuestAddr) {
  if (p % 4096 || !old_size || !size || flags & ~1 || old_size > UINT64_MAX - 4095 ||
      size > UINT64_MAX - 4095) {
    failure(err::einval);
    return UINT64_MAX;
  }
  old_size = align_up(old_size, 4096);
  size = align_up(size, 4096);
  MemoryRegion r;
  if (!AddressSpace::get().find(p, &r) || r.start != p || r.size != old_size) {
    failure(err::efault);
    return UINT64_MAX;
  }
  if (size == old_size) return p;
  if (size < old_size) {
    auto e = AddressSpace::get().unmap(p + size, old_size - size);
    if (e) {
      result(e);
      return UINT64_MAX;
    }
    return p;
  }
  if (!(flags & 1)) {
    failure(err::enomem);
    return UINT64_MAX;
  }
  if (!r.name.starts_with("[anon") || !(r.prot & 1)) {
    failure(err::enotsup);
    return UINT64_MAX;
  }
  auto next = AddressSpace::get().map(0, size, 3, kMapPrivate | kMapAnonymous, nullptr, 0, r.name);
  if (next < 0) {
    result(next);
    return UINT64_MAX;
  }
  std::memcpy(g2h(next), g2h(p), old_size);
  auto e = AddressSpace::get().protect(next, size, r.prot);
  if (!e) e = AddressSpace::get().unmap(p, old_size);
  if (e) {
    AddressSpace::get().unmap(next, size);
    result(e);
    return UINT64_MAX;
  }
  return next;
}
std::int64_t process_read(int pid, const Iovec* local, std::uint64_t local_count,
                          const Iovec* remote, std::uint64_t remote_count, std::uint64_t flags) {
  if (pid != 1000) return failure(err::esrch);
  if (flags || local_count > 1024 || remote_count > 1024) return failure(err::einval);
  if ((local_count && !local) || (remote_count && !remote)) return failure(err::efault);
  std::uint64_t li = 0, ri = 0, lo = 0, ro = 0, done = 0;
  while (li < local_count && ri < remote_count) {
    if (lo == local[li].length) {
      ++li;
      lo = 0;
      continue;
    }
    if (ro == remote[ri].length) {
      ++ri;
      ro = 0;
      continue;
    }
    auto address = remote[ri].base + ro;
    MemoryRegion r;
    if (!AddressSpace::get().find(address, &r) || !(r.prot & 1))
      return done ? static_cast<std::int64_t>(done) : failure(err::efault);
    auto n = std::min({local[li].length - lo, remote[ri].length - ro, r.start + r.size - address});
    if (n > INT64_MAX - done) return failure(err::einval);
    std::memcpy(g2h(local[li].base + lo), g2h(address), n);
    done += n;
    lo += n;
    ro += n;
  }
  return done;
}
}  // namespace
GuestAddr linux_program_break(GuestAddr requested) {
  // Linux mm/mmap.c SYSCALL_DEFINE1(brk): the raw syscall returns the new
  // break, or the unchanged current break when the request is refused.
  std::lock_guard lock(break_mutex);
  return initialize_break() ? resize_break(requested) : 0;
}
void register_memory_map(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
    abi32::adapt<&sbrk32>("sbrk");
    abi32::adapt<&mmap32>("mmap"); abi32::adapt<&mmap32>("mmap64");
  }
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("brk", brk_fn);
  B("sbrk", sbrk_fn);
  B("mmap", mmap_fn);
  B("mmap64", mmap_fn);
  B("munmap", munmap_fn);
  B("mprotect", mprotect_fn);
  B("madvise", madvise_fn);
  B("posix_madvise", posix_madvise_fn);
  B("mincore", mincore_fn);
  B("msync", msync_fn);
  B("mlock", mlock_fn);
  B("munlock", munlock_fn);
  B("mremap", mremap_fn);
  B("process_vm_readv", process_read);
#undef B
}
}  // namespace venture::hle::libc
