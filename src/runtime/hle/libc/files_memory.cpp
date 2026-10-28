#include "vfs.h"
#include <algorithm>
#include <cstring>
namespace venture::hle::libc {
namespace {
class MemoryFd final : public File {
 public:
  explicit MemoryFd(bool allow_seals) : seals_(allow_seals ? 0 : 1) { flags = 2; }
  std::int64_t read(void* p, std::uint64_t n) override {
    std::lock_guard lock(mutex);
    auto r = read_at(p, n, position_);
    position_ += r;
    return r;
  }
  std::int64_t write(const void* p, std::uint64_t n) override {
    std::lock_guard lock(mutex);
    if (flags.load() & 02000) position_ = bytes_.size();
    auto r = write_at(p, n, position_);
    if (r > 0) position_ += r;
    return r;
  }
  std::int64_t pread(void* p, std::uint64_t n, std::int64_t off) override {
    std::lock_guard lock(mutex);
    return off < 0 ? -err::einval : read_at(p, n, off);
  }
  std::int64_t pwrite(const void* p, std::uint64_t n, std::int64_t off) override {
    std::lock_guard lock(mutex);
    return off < 0 ? -err::einval : write_at(p, n, off);
  }
  std::int64_t seek(std::int64_t off, int whence) override {
    std::lock_guard lock(mutex);
    auto base = whence == 0   ? 0
                : whence == 1 ? position_
                : whence == 2 ? static_cast<std::int64_t>(bytes_.size())
                              : -1;
    if (base < 0 || off < -base || off > INT64_MAX - base) return -err::einval;
    return position_ = base + off;
  }
  int stat(Stat* s) override {
    std::lock_guard lock(mutex);
    *s = {};
    s->mode = 0100777;
    s->ino = h2g(this);
    s->size = bytes_.size();
    s->blocks = (s->size + 511) / 512;
    return 0;
  }
  int truncate(std::int64_t size) override {
    if (size < 0) return -err::einval;
    std::lock_guard lock(mutex);
    if ((size < bytes_.size() && (seals_ & 2)) || (size > bytes_.size() && (seals_ & 4)))
      return -err::eperm;
    try {
      bytes_.resize(size);
    } catch (...) {
      return -err::enomem;
    }
    return 0;
  }
  int sync() override { return 0; }
  std::int64_t control(int command, std::uint64_t value) override {
    std::lock_guard lock(mutex);
    if (command == 1034) return seals_;
    if (command != 1033 || value & ~15ull) return -err::einval;
    if (seals_ & 1) return -err::eperm;
    seals_ |= value;
    return 0;
  }

 private:
  std::int64_t read_at(void* p, std::uint64_t n, std::uint64_t off) {
    n = std::min<std::uint64_t>(n, bytes_.size() - std::min<std::uint64_t>(off, bytes_.size()));
    if (n) std::memcpy(p, bytes_.data() + off, n);
    return n;
  }
  std::int64_t write_at(const void* p, std::uint64_t n, std::uint64_t off) {
    if (seals_ & 8) return -err::eperm;
    if (n > INT64_MAX - off) return -err::efbig;
    if (off + n > bytes_.size()) {
      if (seals_ & 4) return -err::eperm;
      try {
        bytes_.resize(off + n);
      } catch (...) {
        return -err::enomem;
      }
    }
    if (n) std::memcpy(bytes_.data() + off, p, n);
    return n;
  }
  std::vector<unsigned char> bytes_;
  std::int64_t position_ = 0;
  int seals_;
};
int memfd_fn(const char* name, unsigned flags) {
  if (!name) return failure(err::efault);
  if (std::strlen(name) > 249 || flags & ~3u) return failure(err::einval);
  auto f = std::make_shared<MemoryFd>(flags & 2);
  f->path = "/memfd:" + std::string(name);
  return result(FdTable::get().insert(f, 0, flags & 1));
}
struct FlockEntry {
  std::weak_ptr<File> owner;
  std::uint64_t dev, ino;
  int operation;
};
std::mutex flock_mutex;
std::vector<FlockEntry> file_locks;
int flock_fn(int fd, int operation) {
  if (operation & ~(1 | 2 | 4 | 8) ||
      ((operation & ~4) != 1 && (operation & ~4) != 2 && (operation & ~4) != 8))
    return failure(err::einval);
  auto f = FdTable::get().file(fd);
  if (!f) return failure(err::ebadf);
  Stat s;
  int r = f->stat(&s);
  if (r) return result(r);
  for (;;) {
    {
      std::lock_guard lock(flock_mutex);
      std::erase_if(file_locks, [&](const FlockEntry& e) {
        auto owner = e.owner.lock();
        return !owner || owner == f;
      });
      if (operation & 8) return 0;
      bool blocked = false;
      for (auto& entry : file_locks)
        if (entry.dev == s.dev && entry.ino == s.ino && ((entry.operation & 2) || (operation & 2)))
          blocked = true;
      if (!blocked) {
        file_locks.push_back({f, s.dev, s.ino, operation & 3});
        return 0;
      }
    }
    if (operation & 4) return failure(err::eagain);
    int r = sleep_until(*GuestThread::current(), monotonic_ns() + 1000000);
    if (r) return result(r);
  }
}
// bionic unistd.h uses its own _PC_* numbering.
std::int64_t pathconf_fn(const char* p, int name) {
  Stat s;
  int r = path_stat(p, &s);
  if (r) return result(r);
  switch (name) {
    case 0:
      return 64;
    case 1:
      return 127;
    case 2:
    case 3:
    case 4:
      return 255;
    case 5:
    case 6:
    case 8:
    case 11:
    case 12:
      return 4096;
    case 7:
    case 14:
    case 15:
      return 1;
    case 13:
      return 4096;
    case 16:
      return 0;
    default:
      return failure(err::einval);
  }
}
int fchmodat_fn(int fd, const char* p, std::uint32_t, int flags) {
  if (flags & ~0x100) return failure(err::einval);
  Stat s;
  return result(path_stat(p, &s, fd, !(flags & 0x100)));
}
}  // namespace
void register_files_memory(HleLibrary& lib) {
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("memfd_create", memfd_fn);
  B("flock", flock_fn);
  B("pathconf", pathconf_fn);
  B("fchmodat", fchmodat_fn);
#undef B
}
}  // namespace venture::hle::libc
