#include "hle/libc/vfs.h"
#include "core/os_quit.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <sched.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include "libc_errno.h"
namespace venture::hle::libc::host {
namespace {
void fill(const struct ::stat& s, Stat* out) {
  *out = {};
  out->dev = s.st_dev;
  out->ino = s.st_ino;
  out->mode = s.st_mode;
  out->nlink = s.st_nlink;
  out->rdev = s.st_rdev;
  out->size = s.st_size;
  out->blocks = s.st_blocks;
  out->blksize = s.st_blksize;
  out->atim = {s.st_atim.tv_sec, s.st_atim.tv_nsec};
  out->mtim = {s.st_mtim.tv_sec, s.st_mtim.tv_nsec};
  out->ctim = {s.st_ctim.tv_sec, s.st_ctim.tv_nsec};
}
void times(const Timespec* in, struct timespec* out) {
  for (int i = 0; i < 2; ++i)
    out[i] = in ? timespec{in[i].sec, in[i].nsec} : timespec{0, UTIME_NOW};
}
class DiskFile final : public File {
 public:
  bool readiness_notifications() const override { return true; }
  explicit DiskFile(int fd, std::filesystem::path path, bool readonly)
      : fd_(fd), host_path_(std::move(path)), readonly_(readonly) {}
  ~DiskFile() override { ::close(fd_); }
  int duplicate_host_fd() override {
    auto fd = ::fcntl(fd_, F_DUPFD_CLOEXEC, 0);
    return fd < 0 ? native_error() : fd;
  }
  std::int64_t read(void* p, std::uint64_t n) override {
    std::lock_guard lock(mutex);
    auto r = ::read(fd_, p, n);
    return r < 0 ? native_error() : r;
  }
  std::int64_t write(const void* p, std::uint64_t n) override {
    os_quit::DiskWrite writing;
    std::lock_guard lock(mutex);
    auto native = ::fcntl(fd_, F_GETFL);
    if (native < 0) return native_error();
    if (::fcntl(fd_, F_SETFL, (native & ~O_APPEND) | ((flags.load() & 02000) ? O_APPEND : 0)))
      return native_error();
    auto r = ::write(fd_, p, n);
    return r < 0 ? native_error() : r;
  }
  std::int64_t pread(void* p, std::uint64_t n, std::int64_t off) override {
    auto r = ::pread(fd_, p, n, off);
    return r < 0 ? native_error() : r;
  }
  std::int64_t pwrite(const void* p, std::uint64_t n, std::int64_t off) override {
    os_quit::DiskWrite writing;
    auto r = ::pwrite(fd_, p, n, off);
    return r < 0 ? native_error() : r;
  }
  std::int64_t seek(std::int64_t off, int whence) override {
    std::lock_guard lock(mutex);
    auto r = ::lseek(fd_, off, whence);
    return r < 0 ? native_error() : r;
  }
  int stat(Stat* s) override {
    struct ::stat native;
    if (::fstat(fd_, &native)) return native_error();
    fill(native, s);
    return 0;
  }
  int truncate(std::int64_t n) override { return ::ftruncate(fd_, n) ? native_error() : 0; }
  int sync() override { return ::fsync(fd_) ? native_error() : 0; }
  int chmod(std::uint32_t mode) override {
    if (readonly_) return -err::erofs;
    return ::fchmod(fd_, mode) ? native_error() : 0;
  }
  int set_times(const Timespec* t) override {
    struct timespec ts[2];
    times(t, ts);
    return ::futimens(fd_, ts) ? native_error() : 0;
  }
  std::int64_t available() override {
    Stat s;
    if (stat(&s)) return -err::eio;
    auto pos = seek(0, 1);
    return pos < 0 ? pos : std::max<std::int64_t>(0, s.size - pos);
  }
  int entries(std::vector<Dirent>& out) override {
    auto* dir = ::opendir(host_path_.c_str());
    if (!dir) return native_error();
    errno = 0;
    while (auto* e = ::readdir(dir)) {
      Dirent d{};
      d.ino = e->d_ino;
      d.type = e->d_type;
      d.reclen = sizeof(d);
      std::strncpy(d.name, e->d_name, 255);
      out.push_back(d);
    }
    int r = errno ? native_error() : 0;
    ::closedir(dir);
    return r;
  }

 private:
  int fd_;
  std::filesystem::path host_path_;
  bool readonly_;
};
}  // namespace
bool unredirected_path(const std::filesystem::path&, const std::filesystem::path&) {
  return false; // POSIX retains the shared canonical containment check.
}
int exists(const std::filesystem::path& p, bool follow) {
  struct ::stat s;
  const auto r = follow ? ::stat(p.c_str(), &s) : ::lstat(p.c_str(), &s);
  return r < 0 ? native_error() : 0;
}
std::shared_ptr<File> open(const MappedPath& p, int flags, std::uint32_t mode, int* error) {
  auto fd = ::open(p.host.c_str(), flags | O_CLOEXEC, mode);
  if (fd < 0) {
    *error = native_error();
    return {};
  }
  return std::make_shared<DiskFile>(fd, p.host, p.readonly);
}
std::shared_ptr<File> import_readonly_handle(std::intptr_t value) {
  if(value<0 || value>INT_MAX)return {};
  auto fd=::fcntl(static_cast<int>(value),F_DUPFD_CLOEXEC,0);
  if(fd<0)return {};
  struct ::stat info;
  if(::fstat(fd,&info) || !S_ISREG(info.st_mode)){::close(fd);return {};}
  return std::make_shared<DiskFile>(fd,std::filesystem::path{},true);
}
std::int64_t write_standard(int fd, const void* data, std::uint64_t size) {
  auto result = ::write(fd, data, size);
  return result < 0 ? native_error() : result;
}
int stat(const std::filesystem::path& p, Stat* out, bool follow) {
  struct ::stat s;
  int r = follow ? ::stat(p.c_str(), &s) : ::lstat(p.c_str(), &s);
  if (r) return native_error();
  fill(s, out);
  return 0;
}
int mutate(int op, const MappedPath& a, const MappedPath* b, std::uint32_t mode) {
  int r;
  switch (op) {
    case 1:
      r = ::mkdir(a.host.c_str(), mode);
      break;
    case 2:
      r = ::unlink(a.host.c_str());
      break;
    case 3:
      r = ::rmdir(a.host.c_str());
      break;
    case 4:
      r = ::rename(a.host.c_str(), b->host.c_str());
      break;
    case 5:
      r = ::link(a.host.c_str(), b->host.c_str());
      break;
    case 6: {
      auto relative = std::filesystem::relative(a.host, b->host.parent_path());
      r = ::symlink(relative.c_str(), b->host.c_str());
      break;
    }
    default:
      return -err::enosys;
  }
  return r ? native_error() : 0;
}
int set_times(const std::filesystem::path& p, const Timespec* t, bool follow) {
  struct timespec ts[2];
  times(t, ts);
  return ::utimensat(AT_FDCWD, p.c_str(), ts, follow ? 0 : AT_SYMLINK_NOFOLLOW) ? native_error()
                                                                                : 0;
}
int chmod(const std::filesystem::path& p, std::uint32_t mode) {
  return ::chmod(p.c_str(), mode) ? native_error() : 0;
}
int memory_lock(GuestAddr p, std::uint64_t n, bool lock) {
  return (lock ? ::mlock(g2h(p), n) : ::munlock(g2h(p), n)) ? native_error() : 0;
}
int random(void* p, std::uint64_t size) {
  auto* bytes = static_cast<unsigned char*>(p);
  while (size) {
    auto n = ::getrandom(bytes, size, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return native_error();
    }
    bytes += n;
    size -= n;
  }
  return 0;
}
void yield() { ::sched_yield(); }
std::int64_t cpu_time(bool thread) {
  struct timespec t;
  if (::clock_gettime(thread ? CLOCK_THREAD_CPUTIME_ID : CLOCK_PROCESS_CPUTIME_ID, &t))
    return native_error();
  return t.tv_sec * 1000000000ll + t.tv_nsec;
}
}  // namespace venture::hle::libc::host
