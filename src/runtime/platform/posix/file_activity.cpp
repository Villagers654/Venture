#include "platform/file_activity.h"
#include <sys/inotify.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <map>
#include <fcntl.h>
#include <charconv>
#include "common/paths.h"

namespace venture::platform {
DataWriters data_writers(const std::filesystem::path& root) noexcept {
  try {
    if (!std::filesystem::is_directory(root)) return DataWriters::unknown;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
      const auto name = entry.path().filename().string();
      int fd{};
      const auto [end, error] = std::from_chars(name.data(), name.data()+name.size(), fd);
      if (error != std::errc{} || end != name.data()+name.size()) continue;
      const int flags = ::fcntl(fd, F_GETFL);
      if (flags < 0) { if (errno == EBADF) continue; return DataWriters::unknown; }
      if ((flags & O_ACCMODE) == O_RDONLY) continue;
      std::error_code ec;
      auto target = std::filesystem::read_symlink(entry.path(), ec);
      if (ec) { if (ec.value() == ENOENT) continue; return DataWriters::unknown; }
      if (target.is_absolute() && paths::within(root, target)) return DataWriters::open;
    }
    return DataWriters::closed;
  } catch (...) { return DataWriters::unknown; }
}
struct FileActivity::Impl {
  int fd = -1;
  bool healthy = true;
  int root_wd = -1;
  std::filesystem::path root;
  std::map<int, std::filesystem::path> watched;  // watch descriptor -> directory
  ~Impl() { if (fd >= 0) ::close(fd); }
  void add(const std::filesystem::path& directory) {
    for (const auto& [wd, path] : watched) if (path == directory) return;
    const int wd = inotify_add_watch(fd, directory.c_str(), IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB |
        IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF);
    if (wd < 0) { healthy = false; return; }
    if (directory == root) root_wd = wd;
    watched[wd] = directory;
  }
  void scan() {
    try {
      add(root);
      for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        if (!entry.is_symlink() && entry.is_directory()) add(entry.path());
    } catch (const std::filesystem::filesystem_error&) { healthy = false; }
  }
};
FileActivity::FileActivity(const std::filesystem::path& root) : impl_(std::make_unique<Impl>()) {
  impl_->root = root;
  impl_->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (impl_->fd < 0) impl_->healthy = false;
  else impl_->scan();
}
FileActivity::~FileActivity() = default;
FileActivity::Poll FileActivity::poll() {
  auto& p = *impl_;
  if (!p.healthy) return Poll::failed;
  bool changed = false, rescan = false;
  alignas(inotify_event) std::array<char, 16384> bytes;
  for (;;) {
    const auto count = ::read(p.fd, bytes.data(), bytes.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (count <= 0) { p.healthy = false; break; }
    changed = true;
    for (std::size_t offset = 0; offset < static_cast<std::size_t>(count);) {
      const auto* event = reinterpret_cast<const inotify_event*>(bytes.data() + offset);
      // Overflow or a removed/replaced root leaves a coverage gap. Do not
      // reconstruct a clean verdict from an incomplete event history. A removed
      // or moved subdirectory was already reported by its parent's
      // IN_DELETE/IN_MOVED_FROM; forget its path so a recreated directory is
      // watched again (Windows watches the root recursively and never loses it).
      if (event->mask & IN_Q_OVERFLOW) p.healthy = false;
      else if (event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
        if (event->wd == p.root_wd) p.healthy = false;
        else if (event->mask & IN_IGNORED) p.watched.erase(event->wd);
        else if (auto it = p.watched.find(event->wd); it != p.watched.end()) it->second.clear();
      }
      if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO))) rescan = true;
      offset += sizeof(inotify_event) + event->len;
    }
  }
  if (rescan && p.healthy) p.scan();
  return !p.healthy ? Poll::failed : changed ? Poll::changed : Poll::quiet;
}
}  // namespace venture::platform
