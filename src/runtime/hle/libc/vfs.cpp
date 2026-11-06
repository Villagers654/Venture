#include "common/paths.h"
#include "common/saves_migration.h"
#include "vfs.h"
#include "file_io_trace.h"
#include "common/log.h"
#include "core/process_paths.h"
#include "core/process_image.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>

namespace venture::hle::libc {
namespace {
std::mutex cwd_mutex;
std::string current_directory = "/";
bool below(std::string_view p, std::string_view root) {
  return p == root || (p.starts_with(root) && p.size() > root.size() && p[root.size()] == '/');
}
// A game that asks for an expansion file its install lacks (an import made
// without its patch OBB) often waits forever with no frame. Name the file once
// so the launcher can say what is missing.
void note_missing_expansion(const std::filesystem::path& host, std::string_view guest) {
  if (!guest.ends_with(".obb")) return;
  std::error_code ec;
  if (std::filesystem::exists(host, ec) || ec) return;
  static std::mutex mutex;
  static std::set<std::string, std::less<>> reported;
  std::lock_guard lock(mutex);
  const auto name = guest.substr(guest.rfind('/') + 1);
  if (!reported.emplace(name).second) return;
  log::warn("vfs", "Missing expansion file: the game looked for {}, which this install does not have", name);
}
// Android gives an app read/write access to its own Android/obb/<package>
// directory: games keep downloaded or unpacked data beside their expansion
// files (CRIWARE titles open their ACF/ACB/AWB banks there). Only the files
// Venture publishes there (the install's own OBB and asset files, often hard
// links into the games folder) stay read-only. The comparison ignores case:
// NTFS opens those names in any case.
bool published_expansion(std::string_view rest) {
  const auto name = rest.substr(0, rest.find('/'));
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
  return std::ranges::any_of(process_config().expansion_files, [&](const std::string& file) {
    return std::ranges::equal(file, name, {}, lower, lower);
  });
}
bool contained(const std::filesystem::path& root, const std::filesystem::path& p) {
  auto a = root.begin(), b = p.begin();
  for (; a != root.end(); ++a, ++b)
    if (b == p.end() || *a != *b) return false;
  return true;
}
class Device final : public File {
 public:
  bool readiness_notifications() const override { return true; }
  explicit Device(int kind, std::string data = {}) : kind_(kind), data_(std::move(data)) {}
  std::int64_t read_line(void* out, std::uint64_t n) override {
    if (kind_ != 4) return -err::enotsup;
    std::lock_guard lock(mutex);
    const auto offset = std::min<std::uint64_t>(offset_, data_.size());
    n = std::min<std::uint64_t>(n, data_.size() - offset);
    const auto* begin = data_.data() + offset;
    if (const auto* end = static_cast<const char*>(std::memchr(begin, '\n', n)))
      n = end - begin + 1;
    std::memcpy(out, begin, n);
    offset_ += n;
    return n;
  }
  std::int64_t read(void* out, std::uint64_t n) override {
    std::lock_guard lock(mutex);
    if (kind_ == 1 || kind_ == 5) return 0;
    if (kind_ == 2) {
      std::memset(out, 0, n);
      return n;
    }
    if (kind_ == 3) {
      int r = host::random(out, n);
      return r ? r : static_cast<std::int64_t>(n);
    }
    n = std::min<std::uint64_t>(n, data_.size() - std::min<std::uint64_t>(offset_, data_.size()));
    std::memcpy(out, data_.data() + std::min<std::uint64_t>(offset_, data_.size()), n);
    offset_ += n;
    return n;
  }
  std::int64_t write(const void* in, std::uint64_t n) override {
    if (kind_ == 4) return -err::ebadf;
    if (kind_ == 6 || kind_ == 7) return host::write_standard(kind_ == 6 ? 1 : 2, in, n);
    return n;
  }
  std::int64_t pread(void* out, std::uint64_t n, std::int64_t off) override {
    if (off < 0) return -err::einval;
    if (kind_ != 4) return File::pread(out, n, off);
    n = std::min<std::uint64_t>(n, data_.size() - std::min<std::uint64_t>(off, data_.size()));
    std::memcpy(out, data_.data() + std::min<std::uint64_t>(off, data_.size()), n);
    return n;
  }
  std::int64_t seek(std::int64_t off, int whence) override {
    std::lock_guard lock(mutex);
    if (kind_ != 4) return 0;
    if (whence < 0 || whence > 2) return -err::einval;
    auto base = whence == 0 ? 0 : whence == 1 ? offset_ : static_cast<std::int64_t>(data_.size());
    if (off < -base || off > INT64_MAX - base) return -err::einval;
    return offset_ = base + off;
  }
  int stat(Stat* s) override {
    *s = {};
    s->mode = kind_ == 4 ? 0100444 : 0020666;
    s->size = data_.size();
    return 0;
  }
  int sync() override { return 0; }

 private:
  int kind_;
  std::string data_;
  std::int64_t offset_ = 0;
};
class VirtualDirectory final : public File {
 public:
  std::int64_t read(void*, std::uint64_t) override { return -err::eisdir; }
  int stat(Stat* out) override {
    *out = {};
    out->mode = 0040555;
    return 0;
  }
  int entries(std::vector<Dirent>& out) override {
    return proc_directory(path, &out) ? 0 : -err::enoent;
  }
  std::int64_t seek(std::int64_t off, int whence) override {
    std::lock_guard lock(mutex);
    auto next = whence == 0 ? off : whence == 1 ? position_ + off : -1;
    if (next < 0) return -err::einval;
    return position_ = next;
  }

 private:
  std::int64_t position_ = 0;
};
}  // namespace
std::uint32_t cpu_count() { return std::clamp(std::thread::hardware_concurrency(), 1u, 8u); }
std::string cwd() {
  std::lock_guard lock(cwd_mutex);
  return current_directory;
}
int resolve_path(std::string_view path, int dirfd, MappedPath* out) {
  if (path.empty()) return -err::enoent;
  if (path.size() > 4096) return -err::enametoolong;
  // Java File paths are host paths. Accept only aliases of configured mounts;
  // the usual guest mount permissions and canonical containment still apply.
  const auto alias = host_path_to_guest(path);
  if (alias) path = *alias;
  // Linux treats backslash as an ordinary byte. Reject it instead of allowing Win32
  // separators. Colons and the other characters NTFS forbids are stored escaped
  // (paths::host_name), so they never form drive roots or alternate data streams.
  if (path.find('\\') != path.npos) return -err::einval;
  std::string full(path);
  if (full[0] != '/') {
    std::string base;
    if (dirfd == at_cwd)
      base = cwd();
    else {
      auto f = FdTable::get().file(dirfd);
      if (!f) return -err::ebadf;
      Stat s;
      int r = f->stat(&s);
      if (r) return r;
      if ((s.mode & 0170000) != 0040000) return -err::enotdir;
      base = f->path;
    }
    full = base + "/" + full;
  }
  std::vector<std::string> parts;
  for (std::size_t i = 0; i < full.size();) {
    auto j = full.find('/', i);
    if (j == full.npos) j = full.size();
    auto part = full.substr(i, j - i);
    i = j + 1;
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    // Avoid Windows trimming which would make '.. ' escape the root.
    if (part.back() == '.' || part.back() == ' ') return -err::einval;
    parts.push_back(std::move(part));
  }
  *out = {};
  out->guest = "";
  for (auto& p : parts) out->guest += "/" + p;
  if (out->guest.empty()) out->guest = "/";
  auto& p = out->guest;
  // Linux procfs: /proc/<own pid> is /proc/self and /proc/thread-self is the
  // calling task's directory. Tools build these paths from getpid()/gettid().
  const auto alias_proc = [&](std::string_view prefix, std::string replacement) {
    if (p == prefix || (p.starts_with(prefix) && p[prefix.size()] == '/')) p = replacement + p.substr(prefix.size());
  };
  alias_proc("/proc/" + std::to_string(kGuestPid), "/proc/self");
  if (auto* thread = GuestThread::current()) alias_proc("/proc/thread-self", "/proc/self/task/" + std::to_string(thread->tid()));
  auto& c = process_config();
  if (proc_directory(p, nullptr))
    out->device = 5;
  else if (p == "/dev/null")
    out->device = 1;
  else if (p == "/dev/zero")
    out->device = 2;
  else if (p == "/dev/urandom" || p == "/dev/random")
    out->device = 3;
  else if (below(p, "/proc") || below(p, "/sys") || process_image::has_file(p))
    out->device = 4;
  if (out->device) return 0;
  auto map = [&](std::string_view prefix, const std::filesystem::path& root, bool ro) {
    if (!below(p, prefix) || root.empty()) return false;
    out->root = root;
    out->host = root;
    out->readonly = ro;
    if (p.size() > prefix.size()) {
      std::string_view rest(p.data() + prefix.size() + 1, p.size() - prefix.size() - 1);
      for (std::size_t start = 0; start <= rest.size();) {
        const auto end = std::min(rest.find('/', start), rest.size());
        out->host /= paths::from_utf8(paths::host_name(rest.substr(start, end - start)));
        start = end + 1;
      }
    }
    return true;
  };
  bool mapped = false;
  if (p == c.guest_apk_path()) {
    out->root = c.apk.parent_path();
    out->host = c.apk;
    out->readonly = true;
    mapped = true;
  }
  if (!mapped) mapped = map(c.guest_native_lib_dir(), c.native_lib_dir, true);
  if (!mapped)
    mapped = map(c.guest_data_dir() + "/cache", c.guest_cache_dir, false) ||
             map("/data/data/" + c.package + "/cache", c.guest_cache_dir, false);
  if (!mapped)
    mapped = map(c.guest_data_dir(), c.data_dir, false) ||
             map("/data/data/" + c.package, c.data_dir, false);
  for (auto prefix : {"/sdcard", "/storage/emulated/0", "/storage/self/primary", "/mnt/sdcard"}) {
    if (mapped) break;
    const auto obb = std::string(prefix) + "/Android/obb/" + c.package;
    if (map(obb, c.obb_dir, false)) {
      out->readonly = p.size() > obb.size() && published_expansion(std::string_view(p).substr(obb.size() + 1));
      if (out->readonly && p.find('/', obb.size() + 1) == std::string::npos) out->expansion = p.substr(obb.size() + 1);
      note_missing_expansion(out->host, p);
      mapped = true;
      break;
    }
    mapped = map(std::string(prefix) + "/Android/data/" + c.package, c.data_dir / "external-data",
                 false) ||
             map(prefix, c.data_dir / "external", false);
  }
  if (!mapped && !c.data_dir.empty())
    mapped = map("/data", c.data_dir / "volumes/data", true) ||
             map("/system", c.data_dir / "volumes/system", true);
  if (!mapped) return -err::enoent;
  if (int error = 0; host::unredirected_path(out->root, out->host, &error)) return error;
  std::error_code ec;
  auto root = std::filesystem::weakly_canonical(out->root, ec);
  if (ec) return -err::eacces;
  auto target = std::filesystem::weakly_canonical(out->host, ec);
  if (ec || !contained(root, target)) return -err::eacces;
  // Windows uses the volume's case semantics. Mapping prefixes remain Linux
  // case-sensitive; NTFS-backed children are case-insensitive unless the
  // directory's case-sensitive flag is enabled. No host root is exposed.
  return 0;
}
int change_cwd(const char* p) {
  if (!p) return -err::efault;
  MappedPath m;
  int r = resolve_path(p, at_cwd, &m);
  if (r) return r;
  Stat s;
  r = path_stat(p, &s);
  if (r) return r;
  if ((s.mode & 0170000) != 0040000) return -err::enotdir;
  std::lock_guard lock(cwd_mutex);
  current_directory = m.guest;
  return 0;
}
FdTable::FdTable() {
  for (int i = 0; i < 3; ++i) {
    auto f = std::make_shared<Device>(5 + i);
    f->flags = i ? 1 : 0;
    f->path = "/dev/stdio";
    fds_[i] = {f, 0};
  }
}
FdTable& FdTable::get() {
  static FdTable table;
  return table;
}
std::shared_ptr<File> FdTable::file(int fd) {
  std::lock_guard lock(mutex_);
  auto i = fds_.find(fd);
  return i == fds_.end() ? nullptr : i->second.file;
}
int FdTable::watch(int fd, short events, ReadinessWaitSet& wait) {
  std::shared_ptr<File> file;
  {
    std::lock_guard lock(mutex_);
    auto it = fds_.find(fd);
    if (it == fds_.end()) return -err::ebadf;
    if (!it->second.closed) it->second.closed = std::make_shared<ReadinessSource>();
    wait.add(*it->second.closed, 0);
    file = it->second.file;
  }
  return file->watch(wait, events);
}
int File::wait_ready(short events, std::int64_t end) {
  ReadinessWaitSet wait;
  for (;;) {
    if (ready(events)) return 0;
    if (monotonic_ns() >= end) return -err::eagain;
    wait.clear();
    const auto sequence = wait.sequence();
    if (auto status = watch(wait, events)) return status;
    if (ready(events)) return 0;
    if (auto status = wait.wait(*GuestThread::current(), sequence, end); status && status != -err::etimedout)
      return status;
  }
}
int FdTable::insert(std::shared_ptr<File> f, int minimum, bool cloexec) {
  if (minimum < 0 || minimum >= 32768) return -err::einval;
  std::lock_guard lock(mutex_);
  while (fds_.contains(minimum) && minimum < 32768) ++minimum;
  if (minimum == 32768) return -err::emfile;
  fds_[minimum] = {std::move(f), cloexec ? 1 : 0};
  return minimum;
}
int FdTable::close(int fd) {
  std::shared_ptr<File> dying;
  {
    std::lock_guard lock(mutex_);
    auto i = fds_.find(fd);
    if (i == fds_.end()) return -err::ebadf;
    dying = std::move(i->second.file);
    fds_.erase(i);
  }
  release_record_locks(dying);
  if (file_io_trace::enabled) file_io_trace::closed(fd, 0);
  return 0;
}
int FdTable::duplicate(int fd, int minimum, bool cloexec, int exact) {
  std::shared_ptr<File> dying;
  std::lock_guard lock(mutex_);
  auto i = fds_.find(fd);
  if (i == fds_.end()) return -err::ebadf;
  if (exact >= 0) {
    if (exact >= 32768) return -err::ebadf;
    if (exact == fd) return fd;
    auto& e = fds_[exact];
    dying = std::move(e.file);
    e = {i->second.file, cloexec ? 1 : 0};
    if (dying) release_record_locks(dying);
    return exact;
  }
  if (minimum < 0 || minimum >= 32768) return -err::einval;
  while (fds_.contains(minimum) && minimum < 32768) ++minimum;
  if (minimum == 32768) return -err::emfile;
  fds_[minimum] = {i->second.file, cloexec ? 1 : 0};
  return minimum;
}
int FdTable::get_fd_flags(int fd) {
  std::lock_guard lock(mutex_);
  auto i = fds_.find(fd);
  return i == fds_.end() ? -err::ebadf : i->second.flags;
}
int FdTable::set_fd_flags(int fd, int flags) {
  std::lock_guard lock(mutex_);
  auto i = fds_.find(fd);
  if (i == fds_.end()) return -err::ebadf;
  i->second.flags = flags & 1;
  return 0;
}
std::vector<std::pair<int, std::string>> FdTable::paths() {
  std::lock_guard lock(mutex_);
  std::vector<std::pair<int, std::string>> p;
  for (auto& [fd, e] : fds_) p.emplace_back(fd, e.file->path);
  return p;
}
namespace {
// Android lets a game write its own Android/obb/<package> files. Venture's
// entries there are hard links into the install, so a writable open reads the
// link and the first write gives the game its own copy (detach_obb_file).
// FileStream(path, FileMode.Open) asks for read/write and usually only reads,
// so a multi-GB asset is copied only if the game really writes it.
class ExpansionFile final : public File {
 public:
  ExpansionFile(std::shared_ptr<File> inner, MappedPath m, int flags)
      : inner_(std::move(inner)), m_(std::move(m)), open_flags_(flags) {}
  int duplicate_host_fd() override { return current()->duplicate_host_fd(); }
  std::int64_t read(void* p, std::uint64_t n) override { return current()->read(p, n); }
  std::int64_t read_line(void* p, std::uint64_t n) override { return current()->read_line(p, n); }
  std::int64_t pread(void* p, std::uint64_t n, std::int64_t o) override { return current()->pread(p, n, o); }
  std::int64_t seek(std::int64_t o, int w) override { return current()->seek(o, w); }
  int stat(Stat* s) override { return current()->stat(s); }
  short ready(short events) override { return current()->ready(events); }
  std::int64_t control(int c, std::uint64_t a) override { return current()->control(c, a); }
  std::int64_t write(const void* p, std::uint64_t n) override {
    if (int r = prepare_write()) return r;
    return current()->write(p, n);
  }
  std::int64_t pwrite(const void* p, std::uint64_t n, std::int64_t o) override {
    if (int r = prepare_write()) return r;
    return current()->pwrite(p, n, o);
  }
  int truncate(std::int64_t n) override {
    if (int r = prepare_write()) return r;
    return current()->truncate(n);
  }
  int sync() override { return current()->sync(); }
  int set_times(const Timespec* t) override {
    if (int r = prepare_write()) return r;
    return current()->set_times(t);
  }
  int prepare_write() override {
    std::lock_guard lock(detach_);
    if (detached_) return 0;
    auto old = current();
    const auto offset = old->seek(0, 1);
    try {
      detach_obb_file(process_config().obb_dir, m_.expansion);
    } catch (const std::exception& e) {
      log::error("vfs", "Could not give the game its own copy of {}: {}", m_.guest, e.what());
      return -err::eacces;
    }
    auto writable = m_;
    writable.readonly = false;
    writable.expansion.clear();
    int error = 0;
    auto f = host::open(writable, open_flags_ & ~(0100 | 0200 | 01000), 0, &error);
    if (!f) return error;
    f->flags = old->flags.load();
    f->path = old->path;
    if (offset >= 0) f->seek(offset, 0);
    {
      std::lock_guard swap(swap_);
      inner_ = std::move(f);
    }
    detached_ = true;
    return 0;
  }

 private:
  std::shared_ptr<File> current() {
    std::lock_guard lock(swap_);
    return inner_;
  }
  std::mutex swap_, detach_;
  std::shared_ptr<File> inner_;
  MappedPath m_;
  int open_flags_;
  bool detached_ = false;
};
}  // namespace
std::int64_t fd_open(const char* path, int flags, std::uint32_t mode, int dirfd) {
  if (!path) return -err::efault;
  if ((flags & 3) == 3) return -err::einval;
  MappedPath m;
  int r = resolve_path(path, dirfd, &m);
  if (trace_vfs()) log::info("vfs", "open {} flags={:#x}, resolve={}", m.guest.empty() ? path : m.guest.c_str(), flags, r);
  if (r) return r;
  const bool writes = (flags & 3) || (flags & (0100 | 01000 | 02000));
  if (m.readonly && writes && m.expansion.empty()) return -err::erofs;
  std::shared_ptr<File> f;
  if (m.readonly && writes) {
    // A published expansion entry: it exists, so O_CREAT|O_EXCL fails as on Linux.
    if ((flags & 0300) == 0300) return -err::eexist;
    f = host::open(m, flags & ~(3 | 0100 | 01000 | 02000), mode, &r);
    if (!f) return r;
    f->flags = flags & ~(o_cloexec | 0100 | 0200 | 01000);
    f->path = m.guest;
    auto expansion = std::make_shared<ExpansionFile>(f, m, flags);
    if (flags & 01000)
      if (int t = expansion->truncate(0)) return t;
    f = expansion;
  } else if (m.device) {
    std::string text;
    if (m.device == 4) {
      bool exists;
      text = proc_file(m.guest, &exists);
      if (!exists) return -err::enoent;
      if (flags & 3) return -err::eacces;
    }
    if (m.device == 5) {
      if (flags & 3) return -err::eisdir;
      f = std::make_shared<VirtualDirectory>();
    } else {
      if (flags & 0200000) return -err::enotdir;
      f = std::make_shared<Device>(m.device, std::move(text));
    }
  } else {
    f = host::open(m, flags, mode, &r);
    if (trace_vfs() && !f) log::info("vfs", "open {} failed with Linux errno {}", m.guest, -r);
    if (!f) return r;
  }
  f->flags = flags & ~(o_cloexec | 0100 | 0200 | 01000);
  f->path = m.guest;
  const auto fd = FdTable::get().insert(f, 0, flags & o_cloexec);
  if (file_io_trace::enabled) file_io_trace::opened(m.guest, flags, fd);
  return fd;
}
std::int64_t fd_read(int fd, void* p, std::uint64_t n) {
  auto f = FdTable::get().file(fd);
  if (!f || (f->flags.load() & 3) == 1) return -err::ebadf;
  if (n > INT64_MAX) return -err::einval;
  if (n && !p) return -err::efault;
  const auto start = file_io_trace::start();
  auto r = n ? f->read(p, n) : 0;
  if (file_io_trace::enabled) file_io_trace::read('r', fd, f->path, n, -1, r, start);
  if (trace_vfs() && f->path.ends_with(".apk")) log::info("vfs", "APK read {} -> {}", n, r);
  return r;
}
std::int64_t fd_write(int fd, const void* p, std::uint64_t n) {
  auto f = FdTable::get().file(fd);
  if (!f || !(f->flags.load() & 3)) return -err::ebadf;
  if (n > INT64_MAX) return -err::einval;
  if (n && !p) return -err::efault;
  return n ? f->write(p, n) : 0;
}
std::int64_t fd_pread(int fd, void* p, std::uint64_t n, std::int64_t off) {
  auto f = FdTable::get().file(fd);
  if (!f || (f->flags.load() & 3) == 1) return -err::ebadf;
  if (off < 0 || n > INT64_MAX) return -err::einval;
  if (n && !p) return -err::efault;
  const auto start = file_io_trace::start();
  auto r = f->pread(p, n, off);
  if (file_io_trace::enabled) file_io_trace::read('p', fd, f->path, n, off, r, start);
  if (trace_vfs() && f->path.ends_with(".apk")) log::info("vfs", "APK pread {} at {} -> {}", n, off, r);
  return r;
}
std::int64_t fd_pwrite(int fd, const void* p, std::uint64_t n, std::int64_t off) {
  auto f = FdTable::get().file(fd);
  if (!f || !(f->flags.load() & 3)) return -err::ebadf;
  if (off < 0 || n > INT64_MAX) return -err::einval;
  if (n && !p) return -err::efault;
  return f->pwrite(p, n, off);
}
std::int64_t fd_seek(int fd, std::int64_t off, int whence) {
  auto f = FdTable::get().file(fd);
  auto r = f ? f->seek(off, whence) : -err::ebadf;
  if (f && trace_vfs() && f->path.ends_with(".apk")) log::info("vfs", "APK seek {} from {} -> {}", off, whence, r);
  return r;
}
int fd_stat(int fd, Stat* s) {
  if (!s) return -err::efault;
  auto f = FdTable::get().file(fd);
  auto r = f ? f->stat(s) : -err::ebadf;
  if (f && trace_vfs() && f->path.ends_with(".apk")) log::info("vfs", "APK stat {} bytes, result {}", s->size, r);
  return r;
}
int path_stat(const char* p, Stat* s, int dirfd, bool follow) {
  if (!p || !s) return -err::efault;
  MappedPath m;
  int r = resolve_path(p, dirfd, &m);
  if (r) return r;
  return path_stat(m, s, follow);
}
int path_stat(const MappedPath& m, Stat* s, bool follow) {
  if (!s) return -err::efault;
  if (m.device) {
    bool exists = true;
    auto text = m.device == 4 ? proc_file(m.guest, &exists) : "";
    if (!exists) return -err::enoent;
    *s = {};
    s->mode = m.device == 5 ? 0040555 : m.device == 4 ? 0100444 : 0020666;
    s->size = text.size();
    return 0;
  }
  return host::stat(m.host, s, follow);
}
}  // namespace venture::hle::libc
