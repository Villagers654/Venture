#include "common/platform/pipe.h"
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace venture::platform {
std::array<std::uintptr_t, 4> make_pipes() {
  int a[2]{-1, -1}, b[2]{-1, -1};
  if (pipe2(a, O_CLOEXEC) || pipe2(b, O_CLOEXEC)) {
    for (int fd : {a[0], a[1], b[0], b[1]})
      if (fd >= 0)
        close(fd);
    throw std::runtime_error("Could not create credential broker pipes.");
  }
  // Process uses child descriptors 64/65. Keep sources above them so dup2
  // actions cannot clobber one another, and reserve zero for a closed channel.
  for (auto [fd, minimum] :
       {std::pair{&a[0], 3}, {&b[1], 3}, {&b[0], 66}, {&a[1], 66}}) {
    auto copy = fcntl(*fd, F_DUPFD_CLOEXEC, minimum);
    if (copy < 0) {
      for (int opened : {a[0], a[1], b[0], b[1]})
        close(opened);
      throw std::runtime_error("Could not duplicate credential broker pipes.");
    }
    close(*fd);
    *fd = copy;
  }
  return {static_cast<std::uintptr_t>(a[0]), static_cast<std::uintptr_t>(b[1]),
          static_cast<std::uintptr_t>(b[0]), static_cast<std::uintptr_t>(a[1])};
}
bool valid_pipe(std::uintptr_t h) {
  struct stat s{};
  return h && !fstat(static_cast<int>(h), &s) && S_ISFIFO(s.st_mode);
}
void close_pipe(std::uintptr_t h) {
  if (h)
    close(static_cast<int>(h));
}
void read_pipe(std::uintptr_t h, void *out, std::size_t size) {
  auto *data = static_cast<unsigned char *>(out);
  while (size) {
    pollfd p{static_cast<int>(h), POLLIN, 0};
    auto ready = poll(&p, 1, 250);
    if (!ready || (ready < 0 && errno == EINTR))
      continue;
    if (ready < 0 || (p.revents & POLLNVAL))
      throw std::runtime_error("Credential broker pipe closed.");
    auto n = read(static_cast<int>(h), data, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw std::runtime_error("Credential broker pipe closed.");
    data += n;
    size -= n;
  }
}
void write_pipe(std::uintptr_t h, const void *in, std::size_t size) {
  sigset_t blocked{}, old{};
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &blocked, &old);
  struct Restore {
    sigset_t &old;
    ~Restore() {
      if (!sigismember(&old, SIGPIPE)) {
        timespec zero{};
        sigset_t set{};
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        sigtimedwait(&set, nullptr, &zero);
      }
      pthread_sigmask(SIG_SETMASK, &old, nullptr);
    }
  } restore{old};
  auto *data = static_cast<const unsigned char *>(in);
  while (size) {
    auto n = write(static_cast<int>(h), data, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw std::runtime_error("Credential broker pipe closed.");
    data += n;
    size -= n;
  }
}
} // namespace venture::platform
