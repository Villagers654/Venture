#include "vfs.h"
#include "core/libc_host_files.h"
#include "android/libc_bridge.h"
namespace venture {
int duplicate_host_fd_for_guest(int fd) {
  auto file = hle::libc::FdTable::get().file(fd);
  return static_cast<int>(hle::libc::result(file ? file->duplicate_host_fd() : -hle::libc::err::ebadf));
}
int open_host_fd_for_guest(const char* path, int flags, int mode) {
  auto fd = hle::libc::fd_open(path, flags, mode);
  if (fd < 0) return static_cast<int>(hle::libc::result(fd));
  int host = duplicate_host_fd_for_guest(static_cast<int>(fd));
  hle::libc::FdTable::get().close(static_cast<int>(fd));
  return host;
}
int close_guest_fd_for_native_library(int fd) { return static_cast<int>(hle::libc::result(hle::libc::FdTable::get().close(fd))); }
namespace android {
int libc_open_host_file_readonly(const std::string& path, std::uint64_t offset, std::uint64_t length) {
  using namespace hle::libc;
  MappedPath mapped; mapped.host = path; mapped.readonly = true;
  int error = 0;
  auto file = host::open(mapped, 0, 0, &error);
  if (!file) return error;
  Stat stat;
  if (auto r = file->stat(&stat)) return r;
  if (stat.size < 0 || offset > static_cast<std::uint64_t>(stat.size) || length > static_cast<std::uint64_t>(stat.size) - offset) return -err::einval;
  file->path = path;
  return FdTable::get().insert(std::move(file));
}
int libc_poll_guest_fds(std::span<GuestPollFd> fds) {
  std::vector<hle::libc::PollFd> poll;
  for (auto fd : fds) poll.push_back({fd.fd, fd.events, 0});
  auto result = hle::libc::fd_poll(poll.data(), poll.size(), 0);
  for (std::size_t i = 0; i < fds.size(); ++i) fds[i].revents = poll[i].revents;
  return result;
}
std::int64_t libc_guest_fd_available(int fd) {
  const auto file = hle::libc::FdTable::get().file(fd);
  return file ? file->available() : -hle::libc::err::ebadf;
}
int libc_watch_guest_fds(std::span<const GuestPollFd> fds, ReadinessWaitSet& wait) {
  for (const auto& fd : fds) if (fd.fd >= 0) {
    auto status = hle::libc::FdTable::get().watch(fd.fd, fd.events, wait);
    if (status && status != -hle::libc::err::ebadf) return status;
  }
  return 0;
}
std::int64_t libc_guest_fd_readiness_time(int fd, short events) {
  auto file = hle::libc::FdTable::get().file(fd);
  return file ? file->readiness_time(events) : -1;
}
}
}
