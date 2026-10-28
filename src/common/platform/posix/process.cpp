#include "common/process.h"
#include "common/platform/posix/dbus.h"
#include <algorithm>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
extern char **environ;
namespace venture {
std::uint32_t current_process_id() {
  return static_cast<std::uint32_t>(getpid());
}
struct Process::Impl {
  pid_t pid = -1;
  std::optional<int> exit;
  std::shared_ptr<PipeChannel> channel;
  std::jthread broker;
  ~Impl() {
    if (broker.joinable()) {
      broker.request_stop();
      if (channel)
        channel->close();
      broker.join();
    }
  }
};
Process::Process() : impl_(std::make_unique<Impl>()) {}
Process::~Process() = default;
Process::Process(Process &&) noexcept = default;
Process &Process::operator=(Process &&) noexcept = default;
void Process::spawn(const fs::path &exe, const std::vector<std::string> &args,
                    const fs::path &log, bool) {
  spawn(exe, args, log, false, {});
}
void Process::spawn(const fs::path &exe, const std::vector<std::string> &args,
                    const fs::path &log, bool, PipeChannel::Handler broker,
                    const Environment &environment) {
  if (active())
    throw std::runtime_error("This game is already running.");
  fs::create_directories(log.parent_path());
  std::vector<std::string> storage{exe.string()};
  storage.insert(storage.end(), args.begin(), args.end());
  std::array<std::shared_ptr<PipeChannel>, 2> channels{};
  if (broker) {
    channels = PipeChannel::pair();
    storage.insert(storage.end(),
                   {"--credential-read", "64", "--credential-write", "65"});
  }
  std::vector<char *> argv;
  for (auto &a : storage)
    argv.push_back(a.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, log.c_str(),
                                   O_CREAT | O_WRONLY | O_TRUNC, 0600);
  posix_spawn_file_actions_adddup2(&actions, 1, 2);
  if (broker) {
    auto h = channels[1]->handles();
    posix_spawn_file_actions_adddup2(&actions, static_cast<int>(h[0]), 64);
    posix_spawn_file_actions_adddup2(&actions, static_cast<int>(h[1]), 65);
  }
  // The current environment with `environment` applied.
  std::vector<std::string> variables;
  for (auto &[key, value] : environment)
    variables.push_back(key + "=" + value);
  for (char **e = environ; e && *e; ++e) {
    std::string_view entry(*e);
    auto name = entry.substr(0, entry.find('='));
    if (std::none_of(environment.begin(), environment.end(),
                     [&](auto &o) { return o.first == name; }))
      variables.emplace_back(entry);
  }
  std::vector<char *> envp;
  for (auto &v : variables)
    envp.push_back(v.data());
  envp.push_back(nullptr);
  pid_t pid = -1;
  auto error = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(),
                           envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (error)
    throw std::runtime_error(
        "Could not start runtime. Check its executable path and permissions.");
  impl_ = std::make_unique<Impl>();
  impl_->pid = pid;
  if (broker) {
    channels[1]->close();
    impl_->channel = channels[0];
    impl_->broker = std::jthread(
        [channel = channels[0], handler = std::move(broker)](
            std::stop_token stop) { channel->serve(handler, stop); });
  }
}
bool Process::active() const { return impl_->pid > 0 && !impl_->exit; }
bool Process::request_stop() {
  return active() && kill(impl_->pid, SIGTERM) == 0;
}
std::optional<int> Process::poll() {
  int status = 0;
  if (active() && waitpid(impl_->pid, &status, WNOHANG) > 0)
    impl_->exit =
        WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  return impl_->exit;
}
int run_tool(const fs::path &exe, const std::vector<std::string> &args,
             const std::function<void(std::string_view)> &output,
             std::string *error, std::stop_token stop) {
  int out[2], err[2];
  if (pipe2(out, O_CLOEXEC))
    throw std::runtime_error("Could not start " + exe.filename().string() + ".");
  if (pipe2(err, O_CLOEXEC)) {
    close(out[0]);
    close(out[1]);
    throw std::runtime_error("Could not start " + exe.filename().string() + ".");
  }
  std::vector<std::string> storage{exe.string()};
  storage.insert(storage.end(), args.begin(), args.end());
  std::vector<char *> argv;
  for (auto &a : storage)
    argv.push_back(a.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, out[1], 1);
  posix_spawn_file_actions_adddup2(&actions, err[1], 2);
  pid_t pid = -1;
  const int failed = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(out[1]);
  close(err[1]);
  if (failed) {
    close(out[0]);
    close(err[0]);
    throw std::runtime_error("Could not start " + exe.filename().string() + ".");
  }
  bool stopped = false;
  std::optional<int> status;
  pollfd fds[2]{{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
  char buffer[64 * 1024];
  auto finish = [&] {
    for (auto &f : fds)
      if (f.fd >= 0) close(f.fd), f.fd = -1;
    int raw = 0;
    if (!status) {
      while (waitpid(pid, &raw, 0) < 0 && errno == EINTR) {
      }
      status = raw;
    }
  };
  while (fds[0].fd >= 0 || fds[1].fd >= 0) {
    if (!stopped && stop.stop_requested()) {
      kill(pid, SIGTERM);
      stopped = true;
    }
    const int ready = poll(fds, 2, 100);
    if (ready < 0 && errno != EINTR)
      break;
    if (!status) {
      int raw = 0;
      if (waitpid(pid, &raw, WNOHANG) == pid) status = raw;
    }
    // A daemon the tool started (adb's server) may hold the pipes open; once
    // the tool exited and they are drained, reading ends.
    if (ready == 0 && status)
      break;
    for (auto &fd : fds) {
      if (fd.fd < 0 || !(fd.revents & (POLLIN | POLLHUP | POLLERR)))
        continue;
      const auto n = read(fd.fd, buffer, sizeof buffer);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        close(fd.fd);
        fd.fd = -1;
      } else if (&fd == &fds[0]) {
        // A throwing consumer (a full disk) still ends the tool.
        try {
          if (!stopped) output({buffer, std::size_t(n)});
        } catch (...) {
          if (!status) kill(pid, SIGTERM);
          finish();
          throw;
        }
      } else if (error && error->size() < 64 * 1024) {
        error->append(buffer, std::size_t(n));
      }
    }
  }
  finish();
  const int raw = *status;
  return WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw);
}
namespace {
// xdg-open, for desktops without the OpenURI portal.
bool xdg_open(const char *target) {
  const char *argv[] = {"xdg-open", target, nullptr};
  pid_t pid = -1;
  if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char **>(argv), environ) != 0)
    return false;
  // Reap it in the background; xdg-open returns once the handler started.
  std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
  return true;
}
} // namespace
void open_folder(const fs::path &p) {
  // The desktop portal opens the folder in the file manager (also inside a
  // sandbox); the descriptor names it without a path the portal must parse.
  if (const int fd = open(p.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC); fd >= 0) {
    const bool opened = dbus::portal_open_file(fd);
    close(fd);
    if (opened) return;
  }
  if (!fs::is_directory(p) || !xdg_open(p.c_str()))
    throw std::runtime_error("Could not open the game folder.");
}
void reveal_file(const fs::path &file) { open_folder(file.parent_path()); }
void open_url(const std::string &url) {
  if (!dbus::portal_open_uri(url) && !xdg_open(url.c_str()))
    throw std::runtime_error("Could not open the link.");
}
} // namespace venture
