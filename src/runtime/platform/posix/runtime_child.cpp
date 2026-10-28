#include "platform/profile.h"
#include <algorithm>
#include "core/thread_runtime.h"
#include "core/process_exit.h"
#include "platform/runtime_child.h"
#include "platform/runtime_stop.h"
#include "platform/runtime_teardown_grace.h"
#include <optional>
#include "common/log.h"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <pthread.h>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <sys/wait.h>
extern char** environ;
namespace venture::platform {
bool request_runtime_stall_dump(unsigned) {
  log::info("startup-snapshot", "native stall dump unavailable on this platform");
  return false;
}
namespace {
int restart_channel = -1;
int launch_channel = -1;
int teardown_channel = -1;
struct Fd { int value; ~Fd() { if (value >= 0) close(value); } };
void check(int error, const char* operation) {
  if (error) throw std::system_error(error, std::generic_category(), operation);
}
int flags(int fd, int command) {
  auto value = fcntl(fd, command);
  if (value < 0) check(errno, "read pipe flags");
  return value;
}
void write_packet(int fd, std::string_view packet) {
  // Linux pipes have at least PIPE_BUF capacity. Check the actual POSIX limit
  // before a bounded, atomic write that precedes the reader's startup.
  if (fpathconf(fd, _PC_PIPE_BUF) < static_cast<long>(packet.size()))
    throw std::length_error("Launch intent exceeds pipe capacity");
  ssize_t count;
  do { count = write(fd, packet.data(), packet.size()); } while (count < 0 && errno == EINTR);
  if (count != static_cast<ssize_t>(packet.size())) check(count < 0 ? errno : EIO, "write launch intent");
}
}
std::optional<int> run_runtime_dump_helper(int, char**) { return std::nullopt; }
void set_runtime_restart_channel(std::uintptr_t channel) { restart_channel = static_cast<int>(channel); }
void set_runtime_launch_channel(std::uintptr_t channel) { launch_channel = static_cast<int>(channel); }
void set_runtime_teardown_channel(std::uintptr_t channel) { teardown_channel = static_cast<int>(channel); }
namespace {
void notify_worker(std::string_view packet) noexcept {
  if (teardown_channel < 0) return;
  // O_NONBLOCK and packets <= PIPE_BUF give all-or-nothing admission. Block
  // SIGPIPE only on this host thread and consume only a newly raised signal;
  // diagnostics must not kill the worker when its supervisor has gone away.
  sigset_t blocked, previous, pending;
  sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
  if (pthread_sigmask(SIG_BLOCK, &blocked, &previous)) return;
  sigpending(&pending);
  const bool already_pending = sigismember(&pending, SIGPIPE) == 1;
  const auto count = write(teardown_channel, packet.data(), packet.size());
  if (count < 0 && errno == EPIPE && !already_pending) {
    timespec zero{};
    sigtimedwait(&blocked, nullptr, &zero);
  }
  pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}
}
void notify_runtime_teardown_posted() { notify_worker(std::string_view("\1", 1)); }
void notify_runtime_save_wait() { notify_worker(std::string_view("\3", 1)); }
const char* runtime_exit_wait_name(RuntimeExitWait wait) {
  switch (wait) {
    case RuntimeExitWait::arbitration: return "exit-arbitration";
    case RuntimeExitWait::cache_flush: return "cache-flush";
    case RuntimeExitWait::profiler_flush: return "profiler-flush";
    case RuntimeExitWait::restart_publication: return "restart-publication";
    case RuntimeExitWait::log_flush: return "log-flush";
    case RuntimeExitWait::kernel_exit: return "kernel-exit";
    case RuntimeExitWait::audio_capture_flush: return "audio-capture-flush";
    case RuntimeExitWait::av_flush: return "av-flush";
    case RuntimeExitWait::gpu_telemetry_flush: return "gpu-telemetry-flush";
    case RuntimeExitWait::guest_execution_stop: return "guest-execution-stop";
    case RuntimeExitWait::java_execution_stop: return "java-execution-stop";
    case RuntimeExitWait::save_flush: return "save-flush";
    case RuntimeExitWait::audio_shutdown: return "audio-shutdown";
    case RuntimeExitWait::vulkan_shutdown: return "vulkan-shutdown";
    case RuntimeExitWait::xr_shutdown: return "xr-shutdown";
    case RuntimeExitWait::os_termination: return "os-termination-after-destroy";
    default: return "lifecycle-teardown";
  }
}
void notify_runtime_exit_wait(RuntimeExitWait wait, int status) {
  char packet[10]{2, static_cast<char>(wait)};
  const auto tid = profile_thread_id();
  for (unsigned i = 0; i != 4; ++i) {
    packet[2+i] = char(tid >> (8*i)); packet[6+i] = char(unsigned(status) >> (8*i));
  }
  notify_worker(std::string_view(packet, sizeof(packet)));
}
[[noreturn]] void terminate_runtime_process(int status) {
  notify_runtime_exit_wait(RuntimeExitWait::kernel_exit, status);
  std::_Exit(status);
 }
std::string take_runtime_launch_payload() {
  if (launch_channel < 0) return {};
  Fd channel{std::exchange(launch_channel, -1)};
  std::string payload(runtime_launch_payload_limit, '\0');
  ssize_t count;
  do { count = read(channel.value, payload.data(), payload.size()); } while (count < 0 && errno == EINTR);
  if (count < 0) check(errno, "read launch intent");
  payload.resize(static_cast<std::size_t>(count));
  return payload;
}
[[noreturn]] void restart_runtime_process() { restart_runtime_process({}); }
[[noreturn]] void restart_runtime_process(std::string_view launch_payload) {
  notify_runtime_exit_wait(RuntimeExitWait::arbitration, 0);
  std::lock_guard decision(worker_exit_mutex);
  if (restart_channel < 0) throw std::runtime_error("Application restart requires a runtime supervisor");
  if (launch_payload.size() > runtime_launch_payload_limit) throw std::length_error("Launch intent too large");
  notify_runtime_exit_wait(RuntimeExitWait::restart_publication, 0);
  write_packet(restart_channel, std::string(1, '\1') + std::string(launch_payload));
  notify_runtime_exit_wait(RuntimeExitWait::log_flush, 0);
  flush_guest_process_files(0);
  const auto status = guest_process_exit_status();
  terminate_runtime_process(status > 0 ? status : 0);
}
RuntimeChildResult run_runtime_child(const std::filesystem::path& executable,
    const std::vector<std::string>& arguments, const std::vector<std::uintptr_t>& inherited,
    const std::function<bool()>& stop) {
  return run_runtime_child(executable, arguments, inherited, stop, {});
}
RuntimeChildResult run_runtime_child(const std::filesystem::path& executable,
    const std::vector<std::string>& arguments, const std::vector<std::uintptr_t>& inherited,
    const std::function<bool()>& stop, std::string_view launch_payload, bool acknowledged_teardown,
    const std::filesystem::path& diagnostic_run_directory) {
  if (launch_payload.size() > runtime_launch_payload_limit) throw std::length_error("Launch intent too large");
  int pipe_fds[2];
  if (pipe(pipe_fds)) throw std::system_error(errno, std::generic_category(), "create restart pipe");
  Fd read{pipe_fds[0]}, writer{pipe_fds[1]};
  if (fcntl(read.value, F_SETFD, flags(read.value, F_GETFD) | FD_CLOEXEC) < 0 ||
      fcntl(read.value, F_SETFL, flags(read.value, F_GETFL) | O_NONBLOCK) < 0)
    check(errno, "protect restart pipe");
  Fd teardown_read{-1}, teardown_write{-1};
  if (acknowledged_teardown) {
    int fds[2];
    if (pipe(fds)) check(errno, "create lifecycle acknowledgement");
    teardown_read.value = fds[0]; teardown_write.value = fds[1];
    if (fcntl(teardown_write.value, F_SETFL, flags(teardown_write.value, F_GETFL) | O_NONBLOCK) < 0)
      check(errno, "make lifecycle telemetry nonblocking");
    if (fcntl(teardown_read.value, F_SETFD, flags(teardown_read.value, F_GETFD) | FD_CLOEXEC) < 0 ||
        fcntl(teardown_read.value, F_SETFL, flags(teardown_read.value, F_GETFL) | O_NONBLOCK) < 0)
      check(errno, "protect lifecycle acknowledgement");
  }
  Fd launch_read{-1};
  if (!launch_payload.empty()) {
    int fds[2];
    if (pipe(fds)) check(errno, "create launch intent pipe");
    launch_read.value = fds[0];
    Fd writer{fds[1]};
    write_packet(writer.value, launch_payload);
  }
  posix_spawn_file_actions_t actions;
  check(posix_spawn_file_actions_init(&actions), "initialize app descriptors");
  struct DestroyActions {
    posix_spawn_file_actions_t* value;
    ~DestroyActions() { posix_spawn_file_actions_destroy(value); }
  } destroy_actions{&actions};
  for (const auto value : inherited) {
    // POSIX spawn dup2 with equal descriptors clears CLOEXEC in the child,
    // without mutating the parent's descriptor flags or numeric broker IDs.
    const auto fd = static_cast<int>(value);
    check(posix_spawn_file_actions_adddup2(&actions, fd, fd), "inherit credential channel");
  }
  std::vector<std::string> storage{std::filesystem::absolute(executable).string()};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  storage.insert(storage.end(), {"--runtime-restart-write", std::to_string(writer.value)});
  if (teardown_write.value >= 0)
    storage.insert(storage.end(), {"--runtime-teardown-write", std::to_string(teardown_write.value)});
  if (launch_read.value >= 0)
    storage.insert(storage.end(), {"--runtime-launch-read", std::to_string(launch_read.value)});
  std::vector<char*> argv;
  for (auto& value : storage) argv.push_back(value.data());
  argv.push_back(nullptr);
  posix_spawnattr_t attributes;
  check(posix_spawnattr_init(&attributes), "initialize app spawn");
  struct DestroyAttributes {
    posix_spawnattr_t* value;
    ~DestroyAttributes() { posix_spawnattr_destroy(value); }
  } cleanup{&attributes};
  // Like the worker's Windows stop event, a stop request must neither kill a
  // worker that has no handler yet nor be lost: the worker starts with
  // SIGINT/SIGTERM blocked (pending) until RuntimeStop installs its handler.
  sigset_t stop_signals;
  sigemptyset(&stop_signals); sigaddset(&stop_signals, SIGINT); sigaddset(&stop_signals, SIGTERM);
  check(posix_spawnattr_setsigmask(&attributes, &stop_signals), "configure app stop signals");
  check(posix_spawnattr_setsigdefault(&attributes, &stop_signals), "configure app stop signals");
  check(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                 POSIX_SPAWN_SETSIGDEF), "configure app process group");
  check(posix_spawnattr_setpgroup(&attributes, 0), "configure app process group");
  pid_t child;
  auto error = posix_spawn(&child, storage[0].c_str(), &actions, &attributes, argv.data(), environ);
  check(error, "start app worker");
  struct Child {
    pid_t pid;
    bool reaped = false;
    ~Child() {
      kill(-pid, SIGKILL);
      if (!reaped) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (waitpid(pid, nullptr, WNOHANG) == 0) {
          if (std::chrono::steady_clock::now() >= deadline) {
            log::warn("launch", "Worker {} post-exit waiter=supervisor stage=kernel-termination-completion still pending", pid);
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
      }
    }
  } owned{child};
  close(writer.value); writer.value = -1;
  if (teardown_write.value >= 0) { close(teardown_write.value); teardown_write.value = -1; }
  int status = 0;
  RuntimeTeardownGrace grace(acknowledged_teardown);
  for (;;) {
    auto result = waitpid(child, &status, WNOHANG);
    if (result == child) { owned.reaped = true; break; }
    if (result < 0 && errno != EINTR) throw std::system_error(errno, std::generic_category(), "wait for app worker");
    const auto now = RuntimeTeardownGrace::Clock::now();
    if (stop() && grace.request(now)) request_runtime_stop(child);
    if (teardown_read.value >= 0) {
      char bytes[256];
      const auto count = ::read(teardown_read.value, bytes, sizeof(bytes));
      if (count > 0) grace.consume(std::span(bytes, count), now, [&](auto wait, auto tid, auto status) {
        log::info("launch", "Worker {} post-exit waiter={} stage={} status={}", child, tid, runtime_exit_wait_name(wait), status);
      });
      else if (count < 0 && errno != EAGAIN && errno != EINTR) check(errno, "read lifecycle acknowledgement");
    }
    if (grace.expired(now)) {
      RuntimeChildResult failed{124, false};
      failed.supervisor_stopped = true;
      failed.exit_wait = grace.exit_wait; failed.exit_wait_tid = grace.exit_wait_tid;
      return failed;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::string packet(runtime_launch_payload_limit + 1, '\0');
  ssize_t count;
  do { count = ::read(read.value, packet.data(), packet.size()); } while (count < 0 && errno == EINTR);
  if (count < 0 && errno != EAGAIN) check(errno, "read application restart");
  if (count > 0 && packet[0] != 1) throw std::runtime_error("Invalid application restart notification");
  const bool restart = count > 0;
  // The process-group owner also terminates any descendants left by a worker.
  if (teardown_read.value >= 0) {
    char bytes[256];
    for (;;) {
      const auto received = ::read(teardown_read.value, bytes, sizeof(bytes));
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) break;
      grace.consume(std::span(bytes, received), std::chrono::steady_clock::now(), [](auto, auto, auto) {});
    }
  }
  RuntimeChildResult result{WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), restart,
      restart ? packet.substr(1, static_cast<std::size_t>(count - 1)) : std::string{}};
  result.os_termination = grace.os_termination;
  return result;
}
}
