#include "vfs.h"
#include "abi32.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <functional>
#include "common/log.h"
namespace venture::hle::libc {
void register_files_memory(HleLibrary&);
namespace {
// bionic libc/include/sys/stat.h: mode_t is uint32_t on Android.
int fchmod_fn(int fd, std::uint32_t mode) {
  auto file = FdTable::get().file(fd);
  if (!file) return failure(err::ebadf);
  const auto r = file->chmod(mode & 07777u);
  if (r == -err::enotsup)
    log::warn("libc", "fchmod: host cannot represent mode {:#o} for fd {}", mode & 07777u, fd);
  return result(r);
}
int chmod_fn(const char* p, std::uint32_t mode) {
  if (!p) return failure(err::efault);
  MappedPath path;
  int r = resolve_path(p, at_cwd, &path);
  if (r) return result(r);
  if (path.readonly || path.device) return failure(err::erofs);
  r = host::chmod(path.host, mode & 07777u);
  if (r == -err::enotsup)
    log::warn("libc", "chmod: host cannot represent mode {:#o} for {}", mode & 07777u, path.guest);
  return result(r);
}
struct Statfs {
  std::int64_t type, bsize;
  std::uint64_t blocks, bfree, bavail, files, ffree;
  std::int32_t fsid[2];
  std::int64_t namelen, frsize, flags, spare[4];
};
struct Statvfs {
  std::uint64_t bsize, frsize, blocks, bfree, bavail, files, ffree, favail, fsid, flags, namemax;
  std::int32_t spare[6];
};
static_assert(sizeof(Statfs) == 120 && sizeof(Statvfs) == 112);
int statfs_fn(const char* p, Statfs* out) {
  if (!p || !out) return failure(err::efault);
  MappedPath path;
  int r = resolve_path(p, at_cwd, &path);
  if (r) return result(r);
  Stat s;
  if ((r = path_stat(p, &s))) return result(r);
  *out = {};
  out->type = path.device ? 0x9fa0 : 0xef53;
  out->bsize = out->frsize = 4096;
  out->namelen = 255;
  out->flags = path.readonly ? 1 : 0;
  if (!path.device) {
    std::error_code ec;
    auto space = std::filesystem::space(path.host, ec);
    if (ec) return failure(err::eio);
    out->blocks = space.capacity / 4096;
    out->bfree = space.free / 4096;
    out->bavail = space.available / 4096;
    out->files = out->blocks;
    out->ffree = out->bfree;
  }
  return 0;
}
int fstatfs_fn(int fd, Statfs* out) {
  auto f = FdTable::get().file(fd);
  return f ? statfs_fn(f->path.c_str(), out) : failure(err::ebadf);
}
int statvfs_fn(const char* p, Statvfs* out) {
  if (!out) return failure(err::efault);
  Statfs s;
  int r = statfs_fn(p, &s);
  if (r) return r;
  *out = {static_cast<std::uint64_t>(s.bsize),
          static_cast<std::uint64_t>(s.frsize),
          s.blocks,
          s.bfree,
          s.bavail,
          s.files,
          s.ffree,
          s.ffree,
          0,
          static_cast<std::uint64_t>(s.flags),
          255,
          {}};
  return 0;
}
int fstatvfs_fn(int fd, Statvfs* out) {
  auto f = FdTable::get().file(fd);
  return f ? statvfs_fn(f->path.c_str(), out) : failure(err::ebadf);
}
int times_at(int dir, const char* p, const Timespec* t, int flags) {
  if (flags & ~0x100) return failure(err::einval);
  if (!p) {
    auto f = FdTable::get().file(dir);
    return f ? result(f->set_times(t)) : failure(err::ebadf);
  }
  MappedPath path;
  int r = resolve_path(p, dir, &path);
  if (r) return result(r);
  if (path.readonly || path.device) return failure(err::erofs);
  return result(host::set_times(path.host, t, !(flags & 0x100)));
}
int futimens_fn(int fd, const Timespec* t) { return times_at(fd, nullptr, t, 0); }
int utimes_fn(const char* p, const Timeval* t) {
  Timespec times[2]{};
  if (t)
    for (int i = 0; i < 2; ++i) {
      if (t[i].usec < 0 || t[i].usec >= 1000000) return failure(err::einval);
      times[i] = {t[i].sec, t[i].usec * 1000};
    }
  return times_at(at_cwd, p, t ? times : nullptr, 0);
}
int utime_fn(const char* p, const std::int64_t* t) {
  Timespec times[2]{};
  if (t) {
    times[0] = {t[0], 0};
    times[1] = {t[1], 0};
  }
  return times_at(at_cwd, p, t ? times : nullptr, 0);
}
// NDK sys/vfs.h: both ARM statfs spellings use 64-bit counters, with
// 32-bit scalar fields. sys/statvfs.h uses 32-bit fsblkcnt_t on ARM,
// including the statvfs64 spelling (NDK clang layout probe).
struct Statfs32 {
  std::uint32_t type, bsize;
  std::uint64_t blocks, bfree, bavail, files, ffree;
  std::int32_t fsid[2];
  std::uint32_t namelen, frsize, flags, spare[4];
};
struct Statvfs32 {
  std::uint32_t bsize, frsize;
  std::uint32_t blocks, bfree, bavail, files, ffree, favail;
  std::uint32_t fsid, flags, namemax;
};
static_assert(sizeof(Statfs32) == 88 && sizeof(Statvfs32) == 44);
void copy_statfs32(Statfs32* out, const Statfs& s) {
  *out = {std::uint32_t(s.type), std::uint32_t(s.bsize), s.blocks, s.bfree, s.bavail,
    s.files, s.ffree, {s.fsid[0], s.fsid[1]}, std::uint32_t(s.namelen), std::uint32_t(s.frsize), std::uint32_t(s.flags), {}};
}
int statfs32(const char* path, Statfs32* out) {
  if (!out) return failure(err::efault);
  Statfs s{}; int r = statfs_fn(path, &s); if (!r) copy_statfs32(out, s); return r;
}
int fstatfs32(int fd, Statfs32* out) {
  if (!out) return failure(err::efault);
  Statfs s{}; int r = fstatfs_fn(fd, &s); if (!r) copy_statfs32(out, s); return r;
}
int copy_statvfs32(Statvfs32* out, const Statvfs& s) {
  if (s.blocks > UINT32_MAX || s.bfree > UINT32_MAX || s.bavail > UINT32_MAX ||
      s.files > UINT32_MAX || s.ffree > UINT32_MAX || s.favail > UINT32_MAX) return failure(err::eoverflow);
  *out = {std::uint32_t(s.bsize), std::uint32_t(s.frsize), std::uint32_t(s.blocks), std::uint32_t(s.bfree), std::uint32_t(s.bavail),
    std::uint32_t(s.files), std::uint32_t(s.ffree), std::uint32_t(s.favail), std::uint32_t(s.fsid), std::uint32_t(s.flags), std::uint32_t(s.namemax)};
  return 0;
}
int statvfs32(const char* path, Statvfs32* out) {
  if (!out) return failure(err::efault);
  Statvfs s{}; int r = statvfs_fn(path, &s); return r ? r : copy_statvfs32(out, s);
}
int fstatvfs32(int fd, Statvfs32* out) {
  if (!out) return failure(err::efault);
  Statvfs s{}; int r = fstatvfs_fn(fd, &s); return r ? r : copy_statvfs32(out, s);
}
int times_at32(int fd, const char* path, const abi32::Timespec* time, int flags) {
  Timespec t[2]{};
  if (time) for (int i = 0; i < 2; ++i) t[i] = {time[i].sec, time[i].nsec};
  return times_at(fd, path, time ? t : nullptr, flags);
}
int futimens32(int fd, const abi32::Timespec* t) { return times_at32(fd, nullptr, t, 0); }
int utimes32(const char* path, const abi32::Timeval* time) {
  Timeval t[2]{};
  if (time) for (int i = 0; i < 2; ++i) t[i] = {time[i].sec, time[i].usec};
  return utimes_fn(path, time ? t : nullptr);
}
int utime32(const char* path, const std::int32_t* time) {
  std::int64_t t[2]{}; if (time) { t[0] = time[0]; t[1] = time[1]; }
  return utime_fn(path, time ? t : nullptr);
}
int symlink_at(const char* target, int dir, const char* link) {
  if (!target || !link) return failure(err::efault);
  MappedPath a, b;
  int r = resolve_path(link, dir, &b);
  if (r) return result(r);
  if (b.readonly || b.device) return failure(err::erofs);
  std::string full = target;
  if (full.empty()) return failure(err::enoent);
  if (full[0] != '/') full = b.guest.substr(0, b.guest.rfind('/') + 1) + full;
  r = resolve_path(full, at_cwd, &a);
  if (r) return result(r);
  if (a.device || a.root != b.root) return failure(err::exdev);
  return result(host::mutate(6, a, &b, 0));
}
int symlink_fn(const char* target, const char* link) { return symlink_at(target, at_cwd, link); }
int nftw_fn(const char* root, GuestAddr callback, int maxfds, int flags) {
  if (!root || !callback || maxfds < 1 || flags & ~15) return failure(err::einval);
  MappedPath start;
  int error = resolve_path(root, at_cwd, &start);
  if (error) return result(error);
  auto original = cwd();
  struct Restore {
    std::string path;
    bool active;
    ~Restore() {
      if (active) change_cwd(path.c_str());
    }
  } restore{original, (flags & 8) != 0};
  std::set<std::pair<std::uint64_t, std::uint64_t>> visited;
  std::uint64_t device = 0;
  std::function<int(const std::string&, int)> walk = [&](const std::string& path,
                                                         int level) -> int {
    if (level > 128) return failure(err::eloop);
    Stat s;
    int r = path_stat(path.c_str(), &s, at_cwd, !(flags & 1));
    int type = 0;
    if (r)
      type = 4;
    else if ((s.mode & 0170000) == 0040000)
      type = (flags & 4) ? 3 : 1;
    else if ((s.mode & 0170000) == 0120000)
      type = 5;
    if (!level) device = s.dev;
    if (!r && (flags & 2) && s.dev != device) return 0;
    struct Ftw {
      int base, level;
    } ftw{static_cast<int>(path.rfind('/') + 1), level};
    auto invoke = [&](int kind) {
      if (flags & 8) {
        auto parent = path.substr(0, path.rfind('/'));
        if (parent.empty()) parent = "/";
        change_cwd(parent.c_str());
      }
      return static_cast<int>(GuestThread::current()->call(
          callback, {h2g(path.c_str()), h2g(&s), static_cast<unsigned>(kind), h2g(&ftw)}));
    };
    if (type != 1 && type != 3) return invoke(type);
    if (!visited.emplace(s.dev, s.ino).second) return 0;
    int fd = fd_open(path.c_str(), 0200000 | o_cloexec);
    if (fd < 0) return invoke(2);
    std::vector<Dirent> entries;
    r = FdTable::get().file(fd)->entries(entries);
    FdTable::get().close(fd);
    if (r) return invoke(2);
    if (!(flags & 4)) {
      r = invoke(1);
      if (r) return r;
    }
    for (auto& entry : entries) {
      if (!std::strcmp(entry.name, ".") || !std::strcmp(entry.name, "..")) continue;
      r = walk(path + (path == "/" ? "" : "/") + entry.name, level + 1);
      if (r) return r;
    }
    return flags & 4 ? invoke(3) : 0;
  };
  return walk(start.guest, 0);
}
}  // namespace
void register_files_extra(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
#define A(n, f) abi32::adapt<&f>(n)
    A("statfs", statfs32); A("statfs64", statfs32); A("fstatfs", fstatfs32); A("fstatfs64", fstatfs32);
    A("statvfs", statvfs32); A("statvfs64", statvfs32); A("fstatvfs", fstatvfs32); A("fstatvfs64", fstatvfs32);
    A("utimensat", times_at32); A("futimens", futimens32); A("utimes", utimes32); A("utime", utime32);
#undef A
  }
  register_files_memory(lib);
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("statfs", statfs_fn);
  B("chmod", chmod_fn);
  B("fchmod", fchmod_fn);
  B("statfs64", statfs_fn);
  B("fstatfs", fstatfs_fn);
  B("fstatfs64", fstatfs_fn);
  B("statvfs", statvfs_fn);
  B("statvfs64", statvfs_fn);
  B("fstatvfs", fstatvfs_fn);
  B("fstatvfs64", fstatvfs_fn);
  B("utimensat", times_at);
  B("futimens", futimens_fn);
  B("utimes", utimes_fn);
  B("utime", utime_fn);
  B("symlink", symlink_fn);
  B("symlinkat", symlink_at);
  B("nftw", nftw_fn);
  B("nftw64", nftw_fn);
#undef B
}
}  // namespace venture::hle::libc
