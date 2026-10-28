#include "vfs.h"
#include "vfs_net.h"
#include "common/environment.h"
#include "common/log.h"
#include "abi32.h"
#include "core/token32.h"
#include "core/guest_storage.h"
#include "hle/libc/libc_a.h"
#include <algorithm>
#include <climits>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace venture::hle::libc {
void register_files_extra(HleLibrary&);
namespace {
std::atomic<std::uint32_t> creation_mask{0022};
struct Flock {
  std::int16_t type, whence;
  std::uint32_t pad;
  std::int64_t start, len;
  std::int32_t pid, pad2;
};
static_assert(sizeof(Flock) == 32);
std::mutex locks_mutex;
struct Lock {
  std::string path;
  std::int64_t start, end;
  int type;
};
std::vector<Lock> record_locks;
int close_fd(int fd) {
  auto f = FdTable::get().file(fd);
  if (!f) return failure(err::ebadf);
  {
    std::lock_guard lock(locks_mutex);
    std::erase_if(record_locks, [&](const Lock& l) { return l.path == f->path; });
  }
  return static_cast<int>(result(FdTable::get().close(fd)));
}
int open_file(const char* p, int flags, std::uint32_t mode) {
  return static_cast<int>(result(fd_open(p, flags, mode & ~creation_mask.load())));
}
int open_at(int dir, const char* p, int flags, std::uint32_t mode) {
  return static_cast<int>(result(fd_open(p, flags, mode & ~creation_mask.load(), dir)));
}
int open_checked(const char* p, int flags) {
  if (flags & 0100) return failure(err::einval);
  return open_file(p, flags, 0);
}
int openat_checked(int d, const char* p, int flags) {
  if (flags & 0100) return failure(err::einval);
  return open_at(d, p, flags, 0);
}
int creat_file(const char* p, std::uint32_t mode) { return open_file(p, 0100 | 01000 | 1, mode); }
// VENTURE_TRACE_NET also covers read/write on sockets (OpenSSL socket BIOs).
const bool trace_socket_io = environment::getenv("VENTURE_TRACE_NET") != nullptr;
std::int64_t trace_socket(const char* op, int fd, std::uint64_t n, std::int64_t r) {
  if (r != -err::eagain && std::dynamic_pointer_cast<SocketFile>(FdTable::get().file(fd)))
    log::info("net-trace", "{} fd={} n={} -> {}", op, fd, n, r);
  return r;
}
std::int64_t read_file(int fd, void* p, std::uint64_t n) {
  auto r = fd_read(fd, p, n);
  return result(trace_socket_io ? trace_socket("read", fd, n, r) : r);
}
std::int64_t write_file(int fd, const void* p, std::uint64_t n) {
  auto r = fd_write(fd, p, n);
  return result(trace_socket_io ? trace_socket("write", fd, n, r) : r);
}
std::int64_t pread_file(int fd, void* p, std::uint64_t n, std::int64_t off) {
  return result(fd_pread(fd, p, n, off));
}
std::int64_t pwrite_file(int fd, const void* p, std::uint64_t n, std::int64_t off) {
  return result(fd_pwrite(fd, p, n, off));
}
std::int64_t read_checked(int fd, void* p, std::uint64_t n, std::uint64_t size) {
  return n > size ? failure(err::eoverflow) : read_file(fd, p, n);
}
std::int64_t write_checked(int fd, const void* p, std::uint64_t n, std::uint64_t size) {
  return n > size ? failure(err::eoverflow) : write_file(fd, p, n);
}
std::int64_t pread_checked(int fd, void* p, std::uint64_t n, std::int64_t off, std::uint64_t size) {
  return n > size ? failure(err::eoverflow) : pread_file(fd, p, n, off);
}
std::int64_t pwrite_checked(int fd, const void* p, std::uint64_t n, std::int64_t off,
                            std::uint64_t size) {
  return n > size ? failure(err::eoverflow) : pwrite_file(fd, p, n, off);
}
std::int64_t seek_file(int fd, std::int64_t off, int whence) {
  return result(fd_seek(fd, off, whence));
}
std::int64_t vector_io(int fd, const Iovec* v, int count, bool write) {
  if (count < 0 || count > 1024) return failure(err::einval);
  if (count && !v) return failure(err::efault);
  std::uint64_t total = 0;
  for (int i = 0; i < count; ++i) {
    if (v[i].length > INT64_MAX - total) return failure(err::einval);
    total += v[i].length;
  }
  std::int64_t done = 0;
  for (int i = 0; i < count; ++i) {
    auto n = write ? fd_write(fd, g2h(v[i].base), v[i].length)
                   : fd_read(fd, g2h(v[i].base), v[i].length);
    if (n < 0) return done ? done : result(n);
    done += n;
    if (static_cast<std::uint64_t>(n) < v[i].length) break;
  }
  return done;
}
std::int64_t read_vector(int fd, const Iovec* v, int n) { return vector_io(fd, v, n, false); }
std::int64_t write_vector(int fd, const Iovec* v, int n) { return vector_io(fd, v, n, true); }
int dup_file(int fd) { return static_cast<int>(result(FdTable::get().duplicate(fd, 0, false))); }
int dup2_file(int fd, int to) {
  if (to < 0) return failure(err::ebadf);
  return static_cast<int>(result(FdTable::get().duplicate(fd, 0, false, to)));
}
int dup3_file(int fd, int to, int flags) {
  if (fd == to || (flags & ~o_cloexec)) return failure(err::einval);
  if (to < 0) return failure(err::ebadf);
  return static_cast<int>(result(FdTable::get().duplicate(fd, 0, flags, to)));
}
std::int64_t fcntl_file(int fd, int cmd, std::uint64_t arg) {
  return result(fd_fcntl(fd, cmd, arg));
}
int ioctl_file(int fd, std::uint64_t request, void* arg) {
  return static_cast<int>(result(fd_ioctl(fd, request, arg)));
}
int sync_file(int fd) {
  auto f = FdTable::get().file(fd);
  return static_cast<int>(result(f ? f->sync() : -err::ebadf));
}
int truncate_fd(int fd, std::int64_t size) {
  auto f = FdTable::get().file(fd);
  return static_cast<int>(result(!f                       ? -err::ebadf
                                 : !(f->flags.load() & 3) ? -err::einval
                                                          : f->truncate(size)));
}
int truncate_file(const char* p, std::int64_t size) {
  int fd = open_file(p, 1, 0);
  if (fd < 0) return -1;
  int r = truncate_fd(fd, size);
  close_fd(fd);
  return r;
}
int stat_file(const char* p, Stat* s) { return static_cast<int>(result(path_stat(p, s))); }
int lstat_file(const char* p, Stat* s) {
  return static_cast<int>(result(path_stat(p, s, at_cwd, false)));
}
int fstat_file(int fd, Stat* s) { return static_cast<int>(result(fd_stat(fd, s))); }
int stat_at(int fd, const char* p, Stat* s, int flags) {
  if (flags & ~(0x100 | 0x1000 | 0x800)) return failure(err::einval);
  if (p && !*p && (flags & 0x1000)) return fstat_file(fd, s);
  return static_cast<int>(result(path_stat(p, s, fd, !(flags & 0x100))));
}
int access_at(int dir, const char* p, int mode, int flags) {
  if (mode & ~7 || flags & ~(0x100 | 0x200)) return failure(err::einval);
  MappedPath m;
  int r = resolve_path(p, dir, &m);
  if (r) return result(r);
  Stat s;
  r = m.device ? path_stat(m, &s, !(flags & 0x100)) : host::exists(m.host, !(flags & 0x100));
  if (r) return result(r);
  if ((mode & 2) && m.readonly) return failure(err::erofs);
  return 0;
}
int access_file(const char* p, int mode) { return access_at(at_cwd, p, mode, 0); }
int mutate(int op, int dir, const char* a, int dir2, const char* b, std::uint32_t mode = 0) {
  if (!a || (b == nullptr && (op == 4 || op == 5))) return failure(err::efault);
  MappedPath x, y;
  int r = resolve_path(a, dir, &x);
  if (r) return result(r);
  if (x.readonly || x.device) {
    Stat existing;
    if (op == 1 && path_stat(x, &existing) == 0) return failure(err::eexist);
    return failure(err::erofs);
  }
  if (b) {
    r = resolve_path(b, dir2, &y);
    if (r) return result(r);
    if (y.readonly || y.device) return failure(err::erofs);
  }
  return static_cast<int>(result(host::mutate(op, x, b ? &y : nullptr, mode)));
}
int mkdir_at(int fd, const char* p, std::uint32_t mode) {
  return mutate(1, fd, p, 0, nullptr, mode & ~creation_mask.load());
}
int mkdir_file(const char* p, std::uint32_t mode) { return mkdir_at(at_cwd, p, mode); }
int unlink_at(int fd, const char* p, int flags) {
  if (flags & ~0x200) return failure(err::einval);
  return mutate(flags ? 3 : 2, fd, p, 0, nullptr);
}
int unlink_file(const char* p) { return unlink_at(at_cwd, p, 0); }
int rmdir_file(const char* p) { return unlink_at(at_cwd, p, 0x200); }
int rename_at(int a, const char* p, int b, const char* q) { return mutate(4, a, p, b, q); }
int rename_file(const char* p, const char* q) { return rename_at(at_cwd, p, at_cwd, q); }
int link_file(const char* p, const char* q) { return mutate(5, at_cwd, p, at_cwd, q); }
int remove_file(const char* p) {
  Stat s;
  int r = path_stat(p, &s, at_cwd, false);
  if (r) return result(r);
  return (s.mode & 0170000) == 0040000 ? rmdir_file(p) : unlink_file(p);
}
int chdir_file(const char* p) { return static_cast<int>(result(change_cwd(p))); }
int fchdir_file(int fd) {
  auto f = FdTable::get().file(fd);
  return f ? chdir_file(f->path.c_str()) : failure(err::ebadf);
}
char* getcwd_file(char* p, std::uint64_t size) {
  auto s = cwd();
  if (!p) {
    if (!size) size = s.size() + 1;
    if (size <= s.size()) return failure(err::erange), nullptr;
    p = static_cast<char*>(allocate(size));
    if (!p) {
      failure(err::enomem);
      return nullptr;
    }
  }
  if (size <= s.size()) {
    failure(err::erange);
    return nullptr;
  }
  std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}
char* realpath_file(const char* p, char* out) {
  MappedPath m;
  int r = p ? resolve_path(p, at_cwd, &m) : -err::efault;
  Stat s;
  if (!r) r = path_stat(p, &s);
  if (r) {
    result(r);
    return nullptr;
  }
  if (!out) out = static_cast<char*>(allocate(m.guest.size() + 1));
  if (!out) {
    failure(err::enomem);
    return nullptr;
  }
  std::strcpy(out, m.guest.c_str());
  return out;
}
std::int64_t readlink_at(int dir, const char* p, char* out, std::uint64_t size) {
  if (!size) return failure(err::einval);
  MappedPath m;
  int r = resolve_path(p, dir, &m);
  if (r) return result(r);
  std::string target;
  if (m.guest == "/proc/self/exe")
    target = process_config().guest_apk_path();
  else if (m.guest.starts_with("/proc/self/fd/")) {
    auto text = m.guest.substr(14);
    char* end;
    auto fd = std::strtol(text.c_str(), &end, 10);
    if (*end || fd < 0 || fd > INT_MAX) return failure(err::enoent);
    auto f = FdTable::get().file(static_cast<int>(fd));
    if (!f) return failure(err::enoent);
    target = f->path;
  } else {
    std::error_code ec;
    auto link = std::filesystem::read_symlink(m.host, ec);
    if (ec) return failure(err::einval);
    auto text = link.generic_u8string();
    target.assign(reinterpret_cast<const char*>(text.data()), text.size());
  }
  auto n = std::min<std::uint64_t>(size, target.size());
  std::memcpy(out, target.data(), n);
  return n;
}
std::int64_t readlink_file(const char* p, char* out, std::uint64_t n) {
  return readlink_at(at_cwd, p, out, n);
}
struct Directory {
  int fd;
  std::vector<Dirent> entries;
  std::size_t position = 0;
  std::mutex mutex;
};
Directory* fdopendir_file(int fd) {
  auto f = FdTable::get().file(fd);
  if (!f) {
    failure(err::ebadf);
    return nullptr;
  }
  auto d = std::make_unique<Directory>();
  d->fd = fd;
  int r = f->entries(d->entries);
  if (r) {
    result(r);
    return nullptr;
  }
  for (std::size_t i = 0; i < d->entries.size(); ++i) d->entries[i].off = i + 1;
  return d.release();
}
Directory* opendir_file(const char* p) {
  int fd = open_file(p, 0200000 | o_cloexec, 0);
  if (fd < 0) return nullptr;
  auto d = fdopendir_file(fd);
  if (!d) close_fd(fd);
  return d;
}
Dirent* readdir_file(Directory* d) {
  std::lock_guard lock(d->mutex);
  if (d->position == d->entries.size()) return nullptr;
  return &d->entries[d->position++];
}
int readdir_r_file(Directory* d, Dirent* out, Dirent** resultp) {
  std::lock_guard lock(d->mutex);
  if (d->position == d->entries.size()) {
    *resultp = nullptr;
    return 0;
  }
  *out = d->entries[d->position++];
  *resultp = out;
  return 0;
}
int closedir_file(Directory* d) {
  if (!d) return failure(err::ebadf);
  int r = close_fd(d->fd);
  delete d;
  return r;
}
void seekdir_file(Directory* d, std::int64_t n) {
  std::lock_guard lock(d->mutex);
  if (n >= 0 && static_cast<std::uint64_t>(n) <= d->entries.size()) d->position = n;
}
void rewinddir_file(Directory* d) { seekdir_file(d, 0); }
std::int64_t telldir_file(Directory* d) {
  std::lock_guard lock(d->mutex);
  return d->position;
}
int dirfd_file(Directory* d) { return d->fd; }
int alphasort_file(const Dirent** a, const Dirent** b) {
  return std::strcmp((*a)->name, (*b)->name);
}
int scandir_file(const char* p, Dirent*** list, GuestAddr filter, GuestAddr compare) {
  auto d = opendir_file(p);
  if (!d) return -1;
  std::vector<Dirent*> found;
  while (auto e = readdir_file(d)) {
    if (filter && !GuestThread::current()->call(filter, {h2g(e)})) continue;
    auto copy = static_cast<Dirent*>(allocate(sizeof(Dirent)));
    if (!copy) {
      for (auto v : found) deallocate(v);
      closedir_file(d);
      return failure(err::enomem);
    }
    *copy = *e;
    found.push_back(copy);
  }
  closedir_file(d);
  if (compare)
    std::sort(found.begin(), found.end(), [&](Dirent* a, Dirent* b) {
      return static_cast<std::int32_t>(GuestThread::current()->call(compare, {h2g(&a), h2g(&b)})) <
             0;
    });
  *list = static_cast<Dirent**>(allocate(found.size() * sizeof(Dirent*)));
  if (!*list && !found.empty()) {
    for (auto v : found) deallocate(v);
    return failure(err::enomem);
  }
  std::copy(found.begin(), found.end(), *list);
  return static_cast<int>(found.size());
}
int make_temp(char* p, bool directory, int suffix = 0, int flags = 0) {
  if (!p || suffix < 0) return failure(err::einval);
  auto n = std::strlen(p);
  if (n < 6 + static_cast<unsigned>(suffix) || std::strncmp(p + n - suffix - 6, "XXXXXX", 6))
    return failure(err::einval);
  auto* tail = p + n - suffix - 6;
  constexpr char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  for (int attempt = 0; attempt < 128; ++attempt) {
    unsigned char random[6];
    int e = host::random(random, 6);
    if (e) return result(e);
    for (int i = 0; i < 6; ++i) tail[i] = chars[random[i] % 62];
    int r = directory ? mkdir_file(p, 0700) : open_file(p, 2 | 0100 | 0200 | flags, 0600);
    if (r >= 0) return r;
    if (*GuestThread::current()->errno_location() != err::eexist) return -1;
  }
  return failure(err::eexist);
}
int mkstemp_file(char* p) { return make_temp(p, false); }
int mkstemps_file(char* p, int suffix) { return make_temp(p, false, suffix); }
int mkostemp_file(char* p, int flags) { return make_temp(p, false, 0, flags); }
char* mkdtemp_file(char* p) { return make_temp(p, true) == 0 ? p : nullptr; }
std::uint32_t umask_file(std::uint32_t mask) { return creation_mask.exchange(mask & 0777); }
std::uint32_t umask_checked(std::uint32_t mask) {
  if (mask & ~0777u) {
    failure(err::einval);
    return UINT32_MAX;
  }
  return umask_file(mask);
}
std::int64_t sendfile_file(int out, int in, std::int64_t* off, std::uint64_t count) {
  std::array<char, 65536> bytes;
  std::int64_t done = 0;
  while (count) {
    auto n = std::min<std::uint64_t>(count, bytes.size());
    auto r = off ? fd_pread(in, bytes.data(), n, *off) : fd_read(in, bytes.data(), n);
    if (r <= 0) return done ? done : result(r);
    auto w = fd_write(out, bytes.data(), r);
    if (w < 0) return done ? done : result(w);
    if (off)
      *off += w;
    else if (w < r)
      fd_seek(in, w - r, 1);
    done += w;
    count -= w;
    if (w < r) break;
  }
  return done;
}
}  // namespace
void release_record_locks(const std::shared_ptr<File>& file) {
  std::lock_guard lock(locks_mutex);
  std::erase_if(record_locks, [&](const Lock& l) { return l.path == file->path; });
}
std::int64_t fd_fcntl(int fd, int command, std::uint64_t arg) {
  auto& table = FdTable::get();
  auto f = table.file(fd);
  if (!f) return -err::ebadf;
  switch (command) {
    case 0:
      return table.duplicate(fd, static_cast<int>(arg), false);
    case 1030:
      return table.duplicate(fd, static_cast<int>(arg), true);
    case 1:
      return table.get_fd_flags(fd);
    case 2:
      return table.set_fd_flags(fd, static_cast<int>(arg));
    case 3:
      return f->flags.load();
    case 4: {
      constexpr int mutable_flags = o_nonblock | 02000;
      auto flags = f->flags.load();
      f->flags = (flags & ~mutable_flags) | (static_cast<int>(arg) & mutable_flags);
      return 0;
    }
    case 5:
    case 6:
    case 7: {
      if (!arg) return -err::efault;
      auto* l = g2h<Flock>(arg);
      if (l->type < 0 || l->type > 2 || l->whence < 0 || l->whence > 2) return -err::einval;
      std::int64_t base = 0;
      if (l->whence == 1) {
        base = f->seek(0, 1);
        if (base < 0) return base;
      }
      if (l->whence == 2) {
        Stat s;
        int r = f->stat(&s);
        if (r) return r;
        base = s.size;
      }
      if (l->start < -base || l->start > INT64_MAX - base) return -err::einval;
      auto start = base + l->start;
      if (l->len < 0 || l->len > INT64_MAX - start) return -err::einval;
      auto end = l->len ? start + l->len : INT64_MAX;
      // POSIX record locks are process-owned: all guest threads have one owner.
      // Record ranges, split unlocks, and remove locks on close; queries cannot
      // conflict with this process's own locks.
      std::lock_guard lock(locks_mutex);
      if (command == 5) {
        l->type = 2;
        return 0;
      }
      std::vector<Lock> next;
      for (auto& old : record_locks) {
        if (old.path != f->path || old.end <= start || old.start >= end)
          next.push_back(old);
        else {
          if (old.start < start) next.push_back({old.path, old.start, start, old.type});
          if (old.end > end) next.push_back({old.path, end, old.end, old.type});
        }
      }
      if (l->type != 2) next.push_back({f->path, start, end, l->type});
      record_locks = std::move(next);
      return 0;
    }
    default:
      return f->control(command, arg);
  }
}
int fd_ioctl(int fd, std::uint64_t request, void* arg) {
  auto f = FdTable::get().file(fd);
  if (!f) return -err::ebadf;
  if (request == 0x541b) {
    if (!arg) return -err::efault;
    auto n = f->available();
    if (n < 0) return n;
    *static_cast<int*>(arg) = static_cast<int>(std::min<std::int64_t>(n, INT_MAX));
    return 0;
  }
  return -err::enotty;
}
std::int64_t fd_getdents(int fd, void* out, std::uint64_t size) {
  auto f = FdTable::get().file(fd);
  if (!f) return -err::ebadf;
  std::vector<Dirent> entries;
  int r = f->entries(entries);
  if (r) return r;
  auto position = f->seek(0, 1);
  if (position < 0) return position;
  std::uint64_t used = 0;
  while (static_cast<std::uint64_t>(position) < entries.size()) {
    auto& d = entries[position];
    auto len = align_up(19 + std::strlen(d.name) + 1, 8);
    if (used + len > size) break;
    d.off = ++position;
    d.reclen = static_cast<std::uint16_t>(len);
    std::memcpy(static_cast<char*>(out) + used, &d, len);
    used += len;
  }
  if (!used && static_cast<std::uint64_t>(position) < entries.size()) return -err::einval;
  f->seek(position, 0);
  return used;
}
namespace {
// bionic sys/stat.h __STAT64_BODY for ARM: stat and stat64 share this
// 104-byte layout, including the legacy truncated inode slot at offset 12.
struct Stat32 {
  std::uint64_t dev;
  std::uint32_t pad0, old_ino, mode, nlink, uid, gid;
  std::uint64_t rdev;
  std::uint32_t pad3, align_size;
  std::int64_t size;
  std::uint32_t blksize, align_blocks;
  std::int64_t blocks;
  abi32::Timespec atim, mtim, ctim;
  std::uint64_t ino;
};
static_assert(sizeof(Stat32) == 104 && offsetof(Stat32, size) == 48 && offsetof(Stat32, ino) == 96);
int copy_stat32(const Stat& s, Stat32* out) {
  if (!out) return failure(err::efault);
  for (auto t : {s.atim.sec, s.mtim.sec, s.ctim.sec}) if (t < INT32_MIN || t > INT32_MAX) return failure(err::eoverflow);
  *out = {s.dev, 0, std::uint32_t(s.ino), s.mode, s.nlink, s.uid, s.gid, s.rdev, 0, 0, s.size,
          std::uint32_t(s.blksize), 0, s.blocks, {std::int32_t(s.atim.sec), std::int32_t(s.atim.nsec)},
          {std::int32_t(s.mtim.sec), std::int32_t(s.mtim.nsec)}, {std::int32_t(s.ctim.sec), std::int32_t(s.ctim.nsec)}, s.ino};
  return 0;
}
int stat32(const char* p, Stat32* out) { Stat s; int r = stat_file(p, &s); return r ? r : copy_stat32(s, out); }
int lstat32(const char* p, Stat32* out) { Stat s; int r = lstat_file(p, &s); return r ? r : copy_stat32(s, out); }
int fstat32(int fd, Stat32* out) { Stat s; int r = fstat_file(fd, &s); return r ? r : copy_stat32(s, out); }
int statat32(int fd, const char* p, Stat32* out, int flags) { Stat s; int r = stat_at(fd, p, &s, flags); return r ? r : copy_stat32(s, out); }
struct Iovec32 { std::uint32_t base, length; };
std::int64_t vector32(int fd, const Iovec32* v, int n, bool write) {
  if (n < 0 || n > 1024 || (!v && n)) return failure(err::einval);
  std::vector<Iovec> wide;
  std::uint64_t total = 0;
  for (int i = 0; i < n; ++i) {
    total += v[i].length;
    if (total > INT32_MAX) return failure(err::einval);
    wide.push_back({v[i].base, v[i].length});
  }
  return vector_io(fd, wide.data(), n, write);
}
std::int64_t readv32(int fd, const Iovec32* v, int n) { return vector32(fd, v, n, false); }
std::int64_t writev32(int fd, const Iovec32* v, int n) { return vector32(fd, v, n, true); }
int open32(const char* p, int flags, a32::VarArgs32 args) { return open_file(p, flags, flags & 0100 ? args.next<std::uint32_t>() : 0); }
int openat32(int fd, const char* p, int flags, a32::VarArgs32 args) { return open_at(fd, p, flags, flags & 0100 ? args.next<std::uint32_t>() : 0); }
int ioctl32(int fd, int request, a32::VarArgs32 args) {
  // fd_ioctl accepts only FIONREAD (one int) and FIOCLEX/FIONCLEX (no payload).
  // Unknown requests return ENOTTY before touching their ABI-dependent payload.
  if (request != 0x541b && request != 0x5451 && request != 0x5450) return failure(err::enotty);
  return ioctl_file(fd, std::uint32_t(request), request == 0x541b ? args.next<void*>() : nullptr);
}
int fcntl32(int fd, int cmd, a32::VarArgs32 args) {
  if (cmd == 5 || cmd == 6 || cmd == 7) {
    struct Flock32 { std::int16_t type, whence; std::int32_t start, len, pid; };
    auto* p = args.next<Flock32*>(); if (!p) return failure(err::efault);
    Flock wide{p->type, p->whence, 0, p->start, p->len, p->pid, 0};
    auto r = fcntl_file(fd, cmd, h2g(&wide));
    if (!r && cmd == 5) {
      if (wide.start < INT32_MIN || wide.start > INT32_MAX || wide.len < INT32_MIN || wide.len > INT32_MAX) return failure(err::eoverflow);
      *p = {wide.type, wide.whence, std::int32_t(wide.start), std::int32_t(wide.len), wide.pid};
    }
    return static_cast<int>(r);
  }
  if (cmd >= 12 && cmd <= 14) return static_cast<int>(fcntl_file(fd, cmd - 7, args.next<std::uint32_t>())); // ARM flock64
  return static_cast<int>(fcntl_file(fd, cmd, cmd == 1 || cmd == 3 ? 0 : args.next<std::uint32_t>()));
}
struct Directory32 {
  std::unique_ptr<Directory> directory;
  GuestStorage entry;
  bool closed = false;
  explicit Directory32(Directory* p) : directory(p) {}
};
Token32Registry directory_tokens;
std::uint32_t directory_token(Directory* d) { return d ? directory_tokens.insert(std::make_shared<Directory32>(d)).value : 0; }
std::uint32_t opendir32(const char* p) { return directory_token(opendir_file(p)); }
std::uint32_t fdopendir32(int fd) { return directory_token(fdopendir_file(fd)); }
std::shared_ptr<Directory32> directory32(std::uint32_t token) {
  auto p = directory_tokens.resolve(Token32<Directory32>{token});
  if (!p) failure(err::ebadf);
  return p;
}
Dirent* readdir32(std::uint32_t token) {
  auto p = directory32(token); if (!p) return nullptr;
  auto& d = *p->directory; std::lock_guard lock(d.mutex);
  if (p->closed) { failure(err::ebadf); return nullptr; }
  if (d.position == d.entries.size()) return nullptr;
  auto* out = g2h<Dirent>(p->entry.resize(sizeof(Dirent))); *out = d.entries[d.position++]; return out;
}
int readdir_r32(std::uint32_t token, Dirent* out, std::uint32_t* resultp) {
  auto p = directory32(token); if (!p) return err::ebadf;
  auto& d = *p->directory; std::lock_guard lock(d.mutex);
  if (p->closed) return err::ebadf;
  if (d.position == d.entries.size()) *resultp = 0;
  else { *out = d.entries[d.position++]; *resultp = h2g32(out); }
  return 0;
}
int closedir32(std::uint32_t token) {
  auto p = directory32(token); if (!p) return -1;
  auto& d = *p->directory; std::lock_guard lock(d.mutex);
  if (p->closed) return failure(err::ebadf);
  p->closed = true; int r = close_fd(d.fd); directory_tokens.erase(Token32<Directory32>{token}); return r;
}
void seekdir32(std::uint32_t token, std::int32_t n) {
  auto p = directory32(token); if (!p) return;
  auto& d = *p->directory; std::lock_guard lock(d.mutex);
  if (p->closed) { failure(err::ebadf); return; }
  if (n >= 0 && std::size_t(n) <= d.entries.size()) d.position = n;
}
void rewinddir32(std::uint32_t token) { seekdir32(token, 0); }
std::int32_t telldir32(std::uint32_t token) {
  auto p = directory32(token); if (!p) return -1;
  auto& d = *p->directory; std::lock_guard lock(d.mutex);
  if (p->closed) return failure(err::ebadf);
  return static_cast<std::int32_t>(d.position);
}
int dirfd32(std::uint32_t token) {
  auto p = directory32(token); if (!p) return -1;
  std::lock_guard lock(p->directory->mutex); return p->closed ? failure(err::ebadf) : p->directory->fd;
}
int alphasort32(const std::uint32_t* a, const std::uint32_t* b) { return std::strcmp(g2h<Dirent>(*a)->name, g2h<Dirent>(*b)->name); }
char* basename_fn(const char* path) {
  // NDK libgen.h / bionic basename.cpp: POSIX variant strips trailing
  // slashes without modifying input; its result may use thread-local storage.
  thread_local GuestStorage storage;
  std::string_view name = path ? path : "";
  while (!name.empty() && name.back() == '/') name.remove_suffix(1);
  if (name.empty()) return const_cast<char*>(storage.string(path && *path ? "/" : "."));
  if (auto slash = name.rfind('/'); slash != name.npos) name.remove_prefix(slash + 1);
  return const_cast<char*>(storage.string(name));
}
char* dirname_fn(const char* path) {
  // bionic/libc/bionic/libgen.cpp: never changes the argument, uses a
  // per-thread buffer, collapses a slash-only parent to "/", and rejects a
  // directory component of PATH_MAX bytes (including room for the terminator).
  thread_local GuestStorage storage;
  std::string_view parent = path ? path : "";
  while (!parent.empty() && parent.back() == '/') parent.remove_suffix(1);
  if (parent.empty()) parent = path && *path ? "/" : ".";
  else if (auto slash = parent.rfind('/'); slash == parent.npos) parent = ".";
  else {
    parent = parent.substr(0, slash);
    while (!parent.empty() && parent.back() == '/') parent.remove_suffix(1);
    if (parent.empty()) parent = "/";
  }
  if (parent.size() >= 4096) {
    failure(err::enametoolong);
    return nullptr;
  }
  return const_cast<char*>(storage.string(parent));
}
}
void register_files(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
#define A(n, f) abi32::adapt<&f>(n)
    A("open", open32); A("open64", open32); A("openat", openat32); A("openat64", openat32);
    A("fcntl", fcntl32); A("fcntl64", fcntl32); A("ioctl", ioctl32);
    A("stat", stat32); A("stat64", stat32); A("lstat", lstat32); A("lstat64", lstat32);
    A("fstat", fstat32); A("fstat64", fstat32); A("fstatat", statat32); A("fstatat64", statat32);
    A("readv", readv32); A("writev", writev32);
    A("opendir", opendir32); A("fdopendir", fdopendir32); A("closedir", closedir32);
    A("readdir", readdir32); A("readdir64", readdir32); A("readdir_r", readdir_r32); A("readdir64_r", readdir_r32);
    A("seekdir", seekdir32); A("rewinddir", rewinddir32); A("telldir", telldir32); A("dirfd", dirfd32);
    A("alphasort", alphasort32); A("alphasort64", alphasort32);
#undef A
  }
  register_files_extra(lib);
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("basename", basename_fn);
  B("dirname", dirname_fn);
  B("open", open_file);
  B("open64", open_file);
  B("openat", open_at);
  B("openat64", open_at);
  B("__open_2", open_checked);
  B("__openat_2", openat_checked);
  B("creat", creat_file);
  B("creat64", creat_file);
  B("close", close_fd);
  B("read", read_file);
  B("write", write_file);
  B("pread", pread_file);
  B("pread64", pread_file);
  B("pwrite", pwrite_file);
  B("pwrite64", pwrite_file);
  B("readv", read_vector);
  B("writev", write_vector);
  B("__read_chk", read_checked);
  B("__write_chk", write_checked);
  B("__pread_chk", pread_checked);
  B("__pread64_chk", pread_checked);
  B("__pwrite_chk", pwrite_checked);
  B("__pwrite64_chk", pwrite_checked);
  B("lseek", seek_file);
  B("lseek64", seek_file);
  B("dup", dup_file);
  B("dup2", dup2_file);
  B("dup3", dup3_file);
  B("fcntl", fcntl_file);
  B("fcntl64", fcntl_file);
  B("ioctl", ioctl_file);
  B("fsync", sync_file);
  B("fdatasync", sync_file);
  B("ftruncate", truncate_fd);
  B("ftruncate64", truncate_fd);
  B("truncate", truncate_file);
  B("truncate64", truncate_file);
  B("stat", stat_file);
  B("stat64", stat_file);
  B("lstat", lstat_file);
  B("lstat64", lstat_file);
  B("fstat", fstat_file);
  B("fstat64", fstat_file);
  B("fstatat", stat_at);
  B("fstatat64", stat_at);
  B("access", access_file);
  B("faccessat", access_at);
  B("mkdir", mkdir_file);
  B("mkdirat", mkdir_at);
  B("unlink", unlink_file);
  B("unlinkat", unlink_at);
  B("rmdir", rmdir_file);
  B("rename", rename_file);
  B("renameat", rename_at);
  B("link", link_file);
  B("remove", remove_file);
  B("chdir", chdir_file);
  B("fchdir", fchdir_file);
  B("getcwd", getcwd_file);
  B("realpath", realpath_file);
  B("readlink", readlink_file);
  B("readlinkat", readlink_at);
  B("opendir", opendir_file);
  B("fdopendir", fdopendir_file);
  B("readdir", readdir_file);
  B("readdir64", readdir_file);
  B("readdir_r", readdir_r_file);
  B("closedir", closedir_file);
  B("rewinddir", rewinddir_file);
  B("seekdir", seekdir_file);
  B("telldir", telldir_file);
  B("dirfd", dirfd_file);
  B("scandir", scandir_file);
  B("alphasort", alphasort_file);
  B("mkstemp", mkstemp_file);
  B("mkstemps", mkstemps_file);
  B("mkostemp", mkostemp_file);
  B("mkdtemp", mkdtemp_file);
  B("umask", umask_file);
  B("sendfile", sendfile_file);
  B("sendfile64", sendfile_file);
  B("__umask_chk", umask_checked);
#undef B
  register_stdio(lib);
}
}  // namespace venture::hle::libc
