// Desktop-portal clients against a fake portal on a private session bus, never
// the desktop's: requests are recorded, nothing opens.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "common/platform/posix/dbus.h"
#include "common/process.h"

extern char** environ;
namespace fs = std::filesystem;
namespace {
struct Recorded {
  std::mutex mutex;
  std::string uri, file;
  std::uint64_t pid = 0, tid = 0;
  std::int32_t nice = 0;
} recorded;

int reply_handle(sd_bus_message* m) {
  return sd_bus_reply_method_return(m, "o", "/org/freedesktop/portal/desktop/request/1_1/venture");
}
int skip_options(sd_bus_message* m) { return sd_bus_message_skip(m, "a{sv}"); }
int get_connectivity(sd_bus_message* m, void*, sd_bus_error*) { return sd_bus_reply_method_return(m, "u", 4u); }
int open_uri(sd_bus_message* m, void*, sd_bus_error*) {
  const char *parent = nullptr, *uri = nullptr;
  if (int r = sd_bus_message_read(m, "ss", &parent, &uri); r < 0) return r;
  if (int r = skip_options(m); r < 0) return r;
  {
    std::lock_guard lock(recorded.mutex);
    recorded.uri = uri;
  }
  return reply_handle(m);
}
int open_file(sd_bus_message* m, void*, sd_bus_error*) {
  const char* parent = nullptr;
  int fd = -1;
  if (int r = sd_bus_message_read(m, "sh", &parent, &fd); r < 0) return r;
  if (int r = skip_options(m); r < 0) return r;
  std::error_code ec;
  const auto target = fs::read_symlink("/proc/self/fd/" + std::to_string(fd), ec);
  {
    std::lock_guard lock(recorded.mutex);
    recorded.file = target.string();
  }
  return reply_handle(m);
}
int high_priority(sd_bus_message* m, void*, sd_bus_error*) {
  std::uint64_t pid = 0, tid = 0;
  std::int32_t nice = 0;
  if (int r = sd_bus_message_read(m, "tti", &pid, &tid, &nice); r < 0) return r;
  {
    std::lock_guard lock(recorded.mutex);
    recorded.pid = pid;
    recorded.tid = tid;
    recorded.nice = nice;
  }
  return sd_bus_reply_method_return(m, "");
}
int min_nice(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
  return sd_bus_message_append(reply, "i", -11);
}
const sd_bus_vtable network_monitor[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetConnectivity", "", "u", get_connectivity, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};
const sd_bus_vtable open_uri_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("OpenURI", "ssa{sv}", "o", open_uri, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("OpenFile", "sha{sv}", "o", open_file, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};
const sd_bus_vtable realtime[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("MinNiceLevel", "i", min_nice, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_METHOD("MakeThreadHighPriorityWithPID", "tti", "", high_priority, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

// A private session bus daemon and a fake portal serving it.
struct PrivateBus {
  pid_t daemon = -1;
  std::string address;
  std::atomic<bool> stop{false};
  std::atomic<bool> ready{false};
  std::thread service;
  PrivateBus() {
    int fds[2];
    if (pipe(fds) != 0) return;
    const auto fd = std::to_string(fds[1]);
    const std::string print = "--print-address=" + fd;
    const char* argv[] = {"dbus-daemon", "--session", "--nofork", "--nopidfile", print.c_str(), nullptr};
    if (posix_spawnp(&daemon, "dbus-daemon", nullptr, nullptr, const_cast<char**>(argv), environ) != 0) daemon = -1;
    close(fds[1]);
    char buffer[512]{};
    const auto n = daemon > 0 ? read(fds[0], buffer, sizeof buffer - 1) : 0;
    close(fds[0]);
    if (n <= 0) return;
    address.assign(buffer, std::strcspn(buffer, "\n"));
    service = std::thread([this] { serve(); });
    for (int i = 0; i < 200 && !ready; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  void serve() {
    sd_bus* bus = nullptr;
    if (sd_bus_new(&bus) < 0) return;
    sd_bus_slot *a = nullptr, *b = nullptr, *c = nullptr;
    const char* path = "/org/freedesktop/portal/desktop";
    if (sd_bus_set_address(bus, address.c_str()) >= 0 && sd_bus_set_bus_client(bus, 1) >= 0 && sd_bus_start(bus) >= 0 &&
        sd_bus_add_object_vtable(bus, &a, path, "org.freedesktop.portal.NetworkMonitor", network_monitor, nullptr) >= 0 &&
        sd_bus_add_object_vtable(bus, &b, path, "org.freedesktop.portal.OpenURI", open_uri_vtable, nullptr) >= 0 &&
        sd_bus_add_object_vtable(bus, &c, path, "org.freedesktop.portal.Realtime", realtime, nullptr) >= 0 &&
        sd_bus_request_name(bus, "org.freedesktop.portal.Desktop", 0) >= 0) {
      ready = true;
      while (!stop) {
        int r = sd_bus_process(bus, nullptr);
        if (r < 0) break;
        if (r == 0) sd_bus_wait(bus, 50'000);
      }
    }
    sd_bus_slot_unref(a);
    sd_bus_slot_unref(b);
    sd_bus_slot_unref(c);
    sd_bus_flush_close_unref(bus);
  }
  ~PrivateBus() {
    stop = true;
    if (service.joinable()) service.join();
    if (daemon > 0) {
      kill(daemon, SIGTERM);
      waitpid(daemon, nullptr, 0);
    }
  }
};
}  // namespace

TEST_CASE("desktop portal clients reach the session bus portal") {
  PrivateBus bus;
  if (bus.address.empty()) {
    MESSAGE("No dbus-daemon; portal clients are not exercised");
    return;
  }
  REQUIRE(bus.ready);
  // Before the first session-bus use in this process: only the private bus.
  setenv("DBUS_SESSION_BUS_ADDRESS", bus.address.c_str(), 1);
  // No fallback may reach a real desktop: xdg-open is not on PATH.
  const auto empty = fs::temp_directory_path() / ("venture-portal-" + std::to_string(getpid()));
  fs::create_directories(empty);
  setenv("PATH", empty.c_str(), 1);
  unsetenv("DISPLAY");
  unsetenv("WAYLAND_DISPLAY");

  CHECK(venture::dbus::portal_connectivity() == 4u);
  REQUIRE(venture::dbus::portal_open_uri("https://example.invalid/venture"));
  venture::open_url("https://example.invalid/link");
  venture::open_folder(empty);
  CHECK(venture::dbus::portal_min_nice() == -11);
  CHECK(venture::dbus::portal_thread_high_priority(std::uint64_t(getpid()), 1234, -11));
  {
    std::lock_guard lock(recorded.mutex);
    CHECK(recorded.uri == "https://example.invalid/link");
    CHECK(fs::path(recorded.file) == fs::canonical(empty));
    CHECK(recorded.pid == std::uint64_t(getpid()));
    CHECK(recorded.tid == 1234);
    CHECK(recorded.nice == -11);
  }
  fs::remove_all(empty);
}
