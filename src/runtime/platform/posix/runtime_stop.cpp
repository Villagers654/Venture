#include "platform/runtime_stop.h"
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <stdexcept>
#ifdef __linux__
#include <sys/syscall.h>
#endif
namespace venture::platform {
namespace {
volatile std::sig_atomic_t stopped = 0;
void control(int) { stopped = 1; }
}
// --parent-pid names any process whose exit requests normal teardown: the
// launcher, a harness guardian or the supervisor, not necessarily our parent
// (Windows waits on a process handle). A pidfd watches it without PID reuse.
struct RuntimeStop::Impl {
  std::uint32_t parent = 0;
  int pidfd = -1;
  bool parent_gone = false, reparent_check = false;
  struct sigaction interrupt{}, terminate{};
  ~Impl() { if (pidfd >= 0) ::close(pidfd); }
};
RuntimeStop::RuntimeStop(std::uint32_t pid) : impl_(std::make_unique<Impl>()) {
  impl_->parent = pid;
  if (pid) {
#if defined(__linux__) && defined(SYS_pidfd_open)
    impl_->pidfd = static_cast<int>(syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0));
    if (impl_->pidfd < 0 && errno == ESRCH) impl_->parent_gone = true;
#endif
    // Without pidfd (older kernels), reparenting still detects a real parent's
    // exit; any other process is probed by PID.
    if (impl_->pidfd < 0 && !impl_->parent_gone)
      impl_->reparent_check = static_cast<std::uint32_t>(getppid()) == pid;
  }
  stopped = 0;
  struct sigaction action{};
  action.sa_handler = control;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, &impl_->interrupt) != 0)
    throw std::runtime_error("Cannot install runtime SIGINT handler");
  if (sigaction(SIGTERM, &action, &impl_->terminate) != 0) {
    sigaction(SIGINT, &impl_->interrupt, nullptr);
    throw std::runtime_error("Cannot install runtime SIGTERM handler");
  }
  // A supervisor spawns workers with both signals blocked so an early request
  // stays pending (runtime_child.cpp); deliver it now that it is handled.
  sigset_t signals;
  sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);
}
RuntimeStop::~RuntimeStop() {
  sigaction(SIGINT, &impl_->interrupt, nullptr);
  sigaction(SIGTERM, &impl_->terminate, nullptr);
}
bool RuntimeStop::requested() const {
  if (stopped || impl_->parent_gone) return true;
  if (!impl_->parent) return false;
  if (impl_->pidfd >= 0) {
    pollfd watch{impl_->pidfd, POLLIN, 0};
    return ::poll(&watch, 1, 0) > 0;  // readable once the process has exited
  }
  if (impl_->reparent_check) return static_cast<std::uint32_t>(getppid()) != impl_->parent;
  return kill(static_cast<pid_t>(impl_->parent), 0) != 0 && errno == ESRCH;
}
bool request_runtime_stop(std::uint32_t pid) { return kill(static_cast<pid_t>(pid), SIGTERM) == 0; }
}
