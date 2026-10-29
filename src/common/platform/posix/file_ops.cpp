#include "common/file_ops.h"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace venture {
struct FileLock::Impl {
  int fd = -1;
  ~Impl() {
    if (fd >= 0) {
      flock(fd, LOCK_UN);
      close(fd);
    }
  }
};
FileLock::FileLock(const fs::path &file, bool wait) : impl_(std::make_unique<Impl>()) {
  if (is_link(file) || is_link(file.parent_path()))
    throw std::runtime_error("Cannot lock account data through a linked path.");
  fs::create_directories(file.parent_path());
  impl_->fd =
      open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (impl_->fd < 0 || flock(impl_->fd, LOCK_EX | (wait ? 0 : LOCK_NB)))
    throw std::runtime_error("Could not lock Venture account data.");
}
FileLock::~FileLock() = default;
std::string random_account_key() {
  std::array<unsigned char, 6> bytes{};
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error("Could not generate an account key.");
  std::size_t done = 0;
  while (done < bytes.size()) {
    auto n = read(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      close(fd);
      throw std::runtime_error("Could not generate an account key.");
    }
    done += n;
  }
  close(fd);
  std::string key;
  for (auto b : bytes) {
    key += "0123456789abcdef"[b >> 4];
    key += "0123456789abcdef"[b & 15];
  }
  return key;
}
bool is_link(const fs::path &path) {
  return fs::is_symlink(fs::symlink_status(path));
}
std::optional<FileIdentity> file_identity(const fs::path &path) {
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return std::nullopt;
  return FileIdentity{static_cast<std::uint64_t>(info.st_dev),
      static_cast<std::uint64_t>(info.st_ino), static_cast<std::uint64_t>(info.st_size),
      static_cast<std::int64_t>(info.st_mtim.tv_sec),
      static_cast<std::uint32_t>(info.st_mtim.tv_nsec)};
}
std::error_code move_directory(const fs::path &source, const fs::path &target) {
  // The migration lock serializes all cooperating writers; never overwrite a
  // save root.
  if (fs::exists(target))
    return std::make_error_code(std::errc::file_exists);
  std::error_code ec;
  fs::rename(source, target, ec);
  return ec;
}
void remove_tree(const fs::path &root, const fs::path &target) {
  auto r = fs::absolute(root).lexically_normal(),
       t = fs::absolute(target).lexically_normal();
  auto relative = t.lexically_relative(r);
  if (relative.empty() || relative == "." || relative.has_root_path())
    throw std::runtime_error("Invalid local data removal boundary.");
  for (const auto &part : relative)
    if (part == "..")
      throw std::runtime_error("Local data removal escapes its root.");
  for (auto p = t.parent_path(); p != r; p = p.parent_path())
    if (is_link(p))
      throw std::runtime_error(
          "Cannot remove data through a linked directory.");
  if (is_link(r))
    throw std::runtime_error("Cannot remove data through a linked root.");
  fs::remove_all(t); // std::filesystem does not follow POSIX symlinks.
}
void link_directory(const fs::path &link, const fs::path &target) {
  fs::create_directory_symlink(fs::absolute(target).lexically_normal(), link);
}
void flush_file(const fs::path &file) {
  const auto fd = open(file.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0)
    throw std::system_error(errno, std::generic_category());
  const auto result = fsync(fd), error = errno;
  close(fd);
  if (result)
    throw std::system_error(error, std::generic_category());
}
void flush_directory(const fs::path &directory) {
  const auto fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0)
    throw std::system_error(errno, std::generic_category());
  const auto result = fsync(fd), error = errno;
  close(fd);
  if (result)
    throw std::system_error(error, std::generic_category());
}
} // namespace venture
