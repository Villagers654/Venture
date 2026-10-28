#include "common/environment.h"
#pragma once
#include "vfs_errno.h"
#include "core/hle.h"
#include "core/linker.h"
#include "core/thread.h"
#include "core/process.h"
#include "core/wait.h"
#include "core/readiness.h"
#include "libc_parts.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace venture::hle::libc {
using venture::kGuestPid;
inline std::map<std::string, Handler>& handlers() {
  static std::map<std::string, Handler> value;
  return value;
}
inline std::mutex& handlers_mutex() {
  static std::mutex value;
  return value;
}
inline void add(HleLibrary& lib, const char* name, Handler handler, Execution execution = Execution::RunLoop,
                TryHandler try_handler = nullptr) {
  {
    std::lock_guard lock(handlers_mutex());
    handlers()[name] = handler;
  }
  lib.add_function(name, make_thunk(lib.soname(), name, handler, nullptr, execution, try_handler));
}
#define VENTURE_LIBC_BIND(lib, name, fn) ::venture::hle::libc::add(lib, name, ::venture::hle::wrap<&fn>())
inline std::int64_t result(std::int64_t value) {
  if (value >= 0) return value;
  if (auto* t = GuestThread::current()) t->set_errno(static_cast<int>(-value));
  return -1;
}
inline int failure(int error) { return static_cast<int>(result(-error)); }
// Layouts from bionic libc/kernel/uapi/asm-generic/stat.h, bits/timespec.h,
// and libc/include/dirent.h. Never pass native CRT structs to the guest.
struct Timespec {
  std::int64_t sec, nsec;
};
struct Timeval {
  std::int64_t sec, usec;
};
struct Stat {
  std::uint64_t dev = 0, ino = 0;
  std::uint32_t mode = 0, nlink = 1, uid = 10125, gid = 10125;
  std::uint64_t rdev = 0, pad1 = 0;
  std::int64_t size = 0;
  std::int32_t blksize = 4096, pad2 = 0;
  std::int64_t blocks = 0;
  Timespec atim{}, mtim{}, ctim{};
  std::uint32_t unused[2]{};
};
static_assert(sizeof(Stat) == 128 && offsetof(Stat, size) == 48 && offsetof(Stat, atim) == 72);
struct Dirent {
  std::uint64_t ino;
  std::int64_t off;
  std::uint16_t reclen;
  std::uint8_t type;
  char name[256];
};
static_assert(sizeof(Dirent) == 280 && offsetof(Dirent, name) == 19);
struct Iovec {
  GuestAddr base;
  std::uint64_t length;
};
inline constexpr int at_cwd = -100, o_nonblock = 04000, o_cloexec = 02000000;
inline constexpr short poll_in = 1, poll_out = 4, poll_err = 8, poll_hup = 16, poll_nval = 32;
struct PollFd {
  std::int32_t fd;
  std::int16_t events, revents;
};
struct MappedPath {
  std::string guest;
  std::filesystem::path host, root;
  bool readonly = false;
  int device = 0;
};
// VENTURE_TRACE_VFS, read once: a per-read environment lookup costs more than the read.
inline bool trace_vfs() {
  static const bool enabled = venture::environment::getenv("VENTURE_TRACE_VFS") != nullptr;
  return enabled;
}
// Device: 1 null, 2 zero, 3 random, 4 synthesized proc/sys file.
int resolve_path(std::string_view path, int dirfd, MappedPath* out);
std::string cwd();
int change_cwd(const char* path);
std::string proc_file(std::string_view path, bool* exists);
bool proc_directory(std::string_view path, std::vector<Dirent>* entries);
std::uint32_t cpu_count();
std::int64_t realtime_ns();
int deadline(const Timespec* time, int clock, std::int64_t* out);

// An open file description. dup shares this object (offset and status flags),
// while descriptor flags live in FdTable. Virtual methods return -Linux errno.
class File {
 public:
  virtual ~File() = default;
  virtual int duplicate_host_fd() { return -err::enotsup; }
  std::mutex mutex;
  std::atomic<int> flags{0};
  // Edge generations record a drain/rearm between epoll_wait calls. Backing
  // objects shared by multiple endpoints override the getters below.
  std::atomic<std::uint64_t> read_epoch{0}, write_epoch{0};
  virtual std::uint64_t read_generation() const { return read_epoch.load(); }
  virtual std::uint64_t write_generation() const { return write_epoch.load(); }
  std::string path;
  virtual std::int64_t read(void*, std::uint64_t) { return -err::ebadf; }
  // Complete a line through newline, capacity or EOF, advancing the shared
  // offset. A short result without newline means EOF. Unsupported files keep
  // stdio's byte-read fallback; snapshots can search their existing bytes.
  virtual std::int64_t read_line(void*, std::uint64_t) { return -err::enotsup; }
  virtual std::int64_t write(const void*, std::uint64_t) { return -err::ebadf; }
  virtual std::int64_t pread(void*, std::uint64_t, std::int64_t) { return -err::espipe; }
  virtual std::int64_t pwrite(const void*, std::uint64_t, std::int64_t) { return -err::espipe; }
  virtual std::int64_t seek(std::int64_t, int) { return -err::espipe; }
  virtual int stat(Stat* s) {
    *s = {};
    s->mode = 0020600;
    return 0;
  }
  virtual int truncate(std::int64_t) { return -err::einval; }
  virtual int sync() { return -err::einval; }
  virtual int set_times(const Timespec*) { return -err::enotsup; }
  virtual int chmod(std::uint32_t) { return -err::enotsup; }
  virtual std::int64_t control(int, std::uint64_t) { return -err::einval; }
  virtual short ready(short events) { return events & (poll_in | poll_out); }
  ReadinessSource readiness;
  virtual bool readiness_notifications() const { return false; }
  virtual std::int64_t readiness_deadline(short) { return -1; }
  virtual int watch(ReadinessWaitSet& wait, short events) {
    wait.add(readiness, events);
    wait.deadline(readiness_deadline(events));
    if (!readiness_notifications()) wait.fallback();
    return 0;
  }
  int wait_ready(short events, std::int64_t deadline_ns);
  // Census timestamp of the transition satisfying these readiness bits. -1
  // means this backend cannot timestamp readiness; never invent event latency.
  virtual std::int64_t readiness_time(short) { return -1; }
  std::int64_t read_ready_ns = -1, write_ready_ns = -1; // Protected by mutex; diagnostic only.
  virtual bool epoll_supported() const { return false; }
  virtual std::int64_t available() { return 0; }
  virtual int entries(std::vector<Dirent>&) { return -err::enotdir; }
};
class FdTable {
 public:
  FdTable();
  static FdTable& get();
  std::shared_ptr<File> file(int fd);
  int watch(int fd, short events, ReadinessWaitSet&);
  int insert(std::shared_ptr<File>, int minimum = 0, bool cloexec = false);
  int close(int fd);
  int duplicate(int fd, int minimum, bool cloexec, int exact = -1);
  int get_fd_flags(int fd);
  int set_fd_flags(int fd, int flags);
  std::vector<std::pair<int, std::string>> paths();

 private:
  struct Entry {
    std::shared_ptr<File> file;
    int flags;
    std::shared_ptr<ReadinessSource> closed;
  };
  std::mutex mutex_;
  std::map<int, Entry> fds_;
};
std::int64_t fd_open(const char*, int flags, std::uint32_t mode = 0600, int dirfd = at_cwd);
std::int64_t fd_read(int, void*, std::uint64_t);
std::int64_t fd_write(int, const void*, std::uint64_t);
std::int64_t fd_pread(int, void*, std::uint64_t, std::int64_t);
std::int64_t fd_pwrite(int, const void*, std::uint64_t, std::int64_t);
std::int64_t fd_seek(int, std::int64_t, int);
int fd_stat(int, Stat*);
int path_stat(const char*, Stat*, int dirfd = at_cwd, bool follow = true);
int path_stat(const MappedPath&, Stat*, bool follow = true);
std::int64_t fd_fcntl(int, int, std::uint64_t);
void release_record_locks(const std::shared_ptr<File>&);
int fd_ioctl(int, std::uint64_t, void*);
std::int64_t fd_getdents(int, void*, std::uint64_t);
int fd_poll(PollFd*, std::uint64_t, int, sync_census::IoSource = sync_census::IoSource::poll);
void register_stdio(HleLibrary&);
void flush_stdio();
bool flush_stdio_for_os_quit();
void run_key_destructors(GuestThread&);
std::int64_t syscall_impl(GuestThread&, std::uint64_t, const std::uint64_t*);

// OS operations are implemented only in platform/{win32,posix}/libc_*.cpp.
namespace host {
// True only when mount-relative components cannot redirect to another path.
bool unredirected_path(const std::filesystem::path& root, const std::filesystem::path& path);
int exists(const std::filesystem::path&, bool follow);
std::shared_ptr<File> open(const MappedPath&, int, std::uint32_t, int* error);
// Native Java descriptor bridge: create an owned, read-only open description.
// Windows receives a HANDLE, POSIX receives an fd; positioned reads preserve
// the caller's sequential offset. This is not exported to guest libc.
std::shared_ptr<File> import_readonly_handle(std::intptr_t);
int stat(const std::filesystem::path&, Stat*, bool follow);
int mutate(int operation, const MappedPath&, const MappedPath*, std::uint32_t);
int set_times(const std::filesystem::path&, const Timespec*, bool follow);
int chmod(const std::filesystem::path&, std::uint32_t mode);
int memory_lock(GuestAddr, std::uint64_t, bool lock);
int random(void*, std::uint64_t);
void yield();
std::int64_t write_standard(int fd, const void* data, std::uint64_t size);
std::int64_t cpu_time(bool thread);
}  // namespace host
}  // namespace venture::hle::libc
