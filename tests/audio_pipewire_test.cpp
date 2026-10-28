// The SDL3 audio backend against a private PipeWire with WirePlumber (never
// the desktop's): two null sinks, default-sink follow, and a stream pinned to
// a node by PIPEWIRE_NODE, as the harness pins runs to their private sink.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <SDL3/SDL.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "platform/audio_backend.h"

extern char** environ;
namespace fs = std::filesystem;
namespace {
struct Child {
  pid_t pid = -1;
  explicit Child(std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ) != 0) pid = -1;
    posix_spawn_file_actions_destroy(&actions);
  }
  ~Child() {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
  }
};
std::string output(const std::string& command) {
  std::string out;
  if (FILE* pipe = popen((command + " 2>/dev/null").c_str(), "r")) {
    char buffer[4096];
    while (auto n = fread(buffer, 1, sizeof buffer, pipe)) out.append(buffer, n);
    pclose(pipe);
  }
  return out;
}
bool wait_for(const std::function<bool()>& condition, int ms = 8000) {
  for (int waited = 0; waited < ms; waited += 50) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return condition();
}
bool have(const char* tool) { return !output(std::string("command -v ") + tool).empty(); }

// The sink node the (only) playback stream is linked to, by node name.
std::optional<std::string> linked_sink() {
  nlohmann::json objects;
  try { objects = nlohmann::json::parse(output("pw-dump")); } catch (...) { return std::nullopt; }
  std::map<int, std::string> names;
  std::optional<int> stream;
  for (auto& o : objects) {
    if (o.value("type", "") != "PipeWire:Interface:Node") continue;
    const auto props = o["info"]["props"];
    names[o["id"].get<int>()] = props.value("node.name", "");
    if (props.value("media.class", "") == "Stream/Output/Audio") stream = o["id"].get<int>();
  }
  if (!stream) return std::nullopt;
  for (auto& o : objects) {
    if (o.value("type", "") != "PipeWire:Interface:Link") continue;
    const auto& info = o["info"];
    if (info.value("output-node-id", -1) == *stream) return names[info.value("input-node-id", -1)];
  }
  return std::nullopt;
}
// SDL lists only nodes with a description, as every real sink has.
void make_sink(const char* name) {
  output(std::string("pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=") + name +
         " node.description=" + name + " media.class=Audio/Sink audio.position=[ FL FR ] object.linger=true }'");
}
void set_default(const char* name) {
  output(std::string("pw-metadata -n default 0 default.configured.audio.sink '{ \"name\": \"") + name + "\" }'");
}
struct Playback {
  ma_context context{};
  ma_device device{};
  bool ready = false;
  Playback() {
    auto config = ma_context_config_init();
    venture::platform::configure_audio_context(config);
    const ma_backend backend = ma_backend_custom;
    if (ma_context_init(&backend, 1, &config, &context) != MA_SUCCESS) return;
    auto cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = 48000;
    cfg.periodSizeInMilliseconds = 5;
    cfg.periods = 2;
    cfg.dataCallback = [](ma_device*, void* out, const void*, ma_uint32 frames) {
      std::memset(out, 0, frames * 2 * sizeof(float));
    };
    if (ma_device_init(&context, &cfg, &device) != MA_SUCCESS) return;
    ready = ma_device_start(&device) == MA_SUCCESS;
  }
  ~Playback() {
    if (ready) ma_device_uninit(&device);
    ma_context_uninit(&context);
  }
};
}  // namespace

TEST_CASE("SDL3 audio prefers PipeWire, follows the default sink and honors PIPEWIRE_NODE") {
  // SDL's own order puts PipeWire first, then PulseAudio, then ALSA.
  REQUIRE(SDL_GetNumAudioDrivers() > 0);
  CHECK(std::string(SDL_GetAudioDriver(0)) == "pipewire");
  for (const char* tool : {"pipewire", "wireplumber", "dbus-daemon", "pw-cli", "pw-dump", "pw-metadata"}) {
    if (!have(tool)) {
      MESSAGE("No " << tool << "; the PipeWire routing is not exercised");
      return;
    }
  }
  const auto root = fs::temp_directory_path() / ("venture-pipewire-" + std::to_string(getpid()));
  fs::create_directories(root / "run");
  fs::create_directories(root / "config/wireplumber/wireplumber.conf.d");
  fs::permissions(root / "run", fs::perms::owner_all, fs::perm_options::replace);
  struct Remove { fs::path p; ~Remove() { std::error_code ec; fs::remove_all(p, ec); } } remove{root};
  // No hardware: WirePlumber's device monitors stay off.
  std::ofstream(root / "config/wireplumber/wireplumber.conf.d/venture-test.conf")
      << "wireplumber.profiles = { main = { monitor.alsa = disabled, monitor.alsa-midi = disabled,"
         " monitor.bluez = disabled, monitor.bluez-midi = disabled, monitor.v4l2 = disabled,"
         " monitor.libcamera = disabled } }\n";
  // Everything below, this process included, sees only the private instances.
  setenv("XDG_RUNTIME_DIR", (root / "run").c_str(), 1);
  setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
  setenv("PIPEWIRE_RUNTIME_DIR", (root / "run").c_str(), 1);
  for (const char* name : {"PIPEWIRE_REMOTE", "PIPEWIRE_NODE", "PULSE_SERVER", "PULSE_SINK", "DISPLAY", "WAYLAND_DISPLAY"})
    unsetenv(name);
  // Only PipeWire: a failed private instance must not fall through to ALSA.
  setenv("SDL_AUDIO_DRIVER", "pipewire", 1);
  int fds[2];
  REQUIRE(pipe(fds) == 0);
  pid_t bus_pid = -1;
  const std::string print = "--print-address=" + std::to_string(fds[1]);
  const char* bus_argv[] = {"dbus-daemon", "--session", "--nofork", "--nopidfile", print.c_str(), nullptr};
  REQUIRE(posix_spawnp(&bus_pid, "dbus-daemon", nullptr, nullptr, const_cast<char**>(bus_argv), environ) == 0);
  struct Reap { pid_t pid; ~Reap() { kill(pid, SIGTERM); waitpid(pid, nullptr, 0); } } bus{bus_pid};
  close(fds[1]);
  char address[512]{};
  const auto n = read(fds[0], address, sizeof address - 1);
  close(fds[0]);
  REQUIRE(n > 0);
  setenv("DBUS_SESSION_BUS_ADDRESS", std::string(address, std::strcspn(address, "\n")).c_str(), 1);

  Child pipewire({"pipewire"});
  REQUIRE(wait_for([&] { return fs::exists(root / "run/pipewire-0"); }));
  Child wireplumber({"wireplumber"});
  // WirePlumber owns the "default" metadata; sinks made before it is up have
  // no default to become.
  REQUIRE(wait_for([] { return output("pw-cli ls Metadata").find("\"default\"") != std::string::npos; }));
  make_sink("venture_test_a");
  make_sink("venture_test_b");
  set_default("venture_test_a");
  REQUIRE(wait_for([] { return output("pw-metadata -n default 0 default.audio.sink").find("venture_test_a") !=
                               std::string::npos; }));
  {
    Playback playback;
    INFO("SDL: " << std::string(SDL_GetError()));
    REQUIRE(playback.ready);
    CHECK(std::string(SDL_GetCurrentAudioDriver()) == "pipewire");
    CHECK(wait_for([&] { return linked_sink() == "venture_test_a"; }));
    // The system default moves; the default-device stream follows it.
    set_default("venture_test_b");
    CHECK(wait_for([&] { return linked_sink() == "venture_test_b"; }));
  }
  // A harness pins a run to its own sink, whatever the default.
  set_default("venture_test_a");
  setenv("PIPEWIRE_NODE", "venture_test_b", 1);
  {
    Playback playback;
    REQUIRE(playback.ready);
    CHECK(wait_for([&] { return linked_sink() == "venture_test_b"; }));
  }
  unsetenv("PIPEWIRE_NODE");
}
