// Integration points implemented by libc part B. These are guest fd numbers,
// never CRT descriptors or Win32 handles. Test implementations live in tests/.
#pragma once
#include <cstdint>
#include <string>
#include <span>
namespace venture { class ReadinessWaitSet; }

namespace venture::android {
// Open the WHOLE archive read-only; offset/length describe the asset for bounds
// validation only. Returned fd supports ordinary absolute pread/lseek/mmap.
// AAsset_openFileDescriptor separately returns offset/length to the caller.
// On failure return a negative Linux errno, without writing host errno.
int libc_open_host_file_readonly(const std::string& path, std::uint64_t offset, std::uint64_t length);
struct GuestPollFd {
  std::int32_t fd;
  std::int16_t events;
  std::int16_t revents;
};
static_assert(sizeof(GuestPollFd) == 8);
// Nonblocking poll against the libc guest fd table. Linux POLL* bit values.
// Returns number ready, or negative Linux errno.
int libc_poll_guest_fds(std::span<GuestPollFd> fds);
// Read-only diagnostic: count bytes without consuming a guest descriptor.
std::int64_t libc_guest_fd_available(int fd);
// Subscribe to the same queues and timer deadlines as poll/select/epoll.
int libc_watch_guest_fds(std::span<const GuestPollFd>, ReadinessWaitSet&);
// Optional producer/deadline timestamp for the gated wait census; -1 unknown.
std::int64_t libc_guest_fd_readiness_time(int fd, short events);
}  // namespace venture::android
