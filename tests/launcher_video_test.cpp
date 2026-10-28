// The launcher's SDL video backend: Wayland only, against a private headless
// Weston (never the desktop's compositor). There is no X11 fallback.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <SDL3/SDL.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "launcher/ui/chrome.h"
#include "launcher/window_style.h"

extern char** environ;
namespace fs = std::filesystem;
namespace {
struct Child {
  pid_t pid = -1;
  Child(std::vector<std::string> args, const std::vector<std::string>& extra_env) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> env_storage(extra_env);
    for (char** e = environ; *e; ++e) env_storage.emplace_back(*e);
    std::vector<char*> envp;
    for (auto& e : env_storage) envp.push_back(e.data());
    envp.push_back(nullptr);
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), envp.data()) != 0) pid = -1;
  }
  ~Child() {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
  }
};
// Sets or clears an environment variable for one scope.
struct Env {
  std::string name;
  std::optional<std::string> saved;
  Env(const char* n, const char* value) : name(n) {
    if (const char* old = std::getenv(n)) saved = old;
    value ? setenv(n, value, 1) : unsetenv(n);
  }
  ~Env() { saved ? setenv(name.c_str(), saved->c_str(), 1) : unsetenv(name.c_str()); }
};
std::string video_driver() {
  venture::launcher::prefer_native_video_driver();
  if (!SDL_Init(SDL_INIT_VIDEO)) return {};
  std::string name = SDL_GetCurrentVideoDriver();
  // A hidden window confirms the backend is usable, not merely selected.
  if (auto* window = SDL_CreateWindow("test", 64, 48, SDL_WINDOW_HIDDEN)) SDL_DestroyWindow(window);
  else name.clear();
  SDL_Quit();
  return name;
}
}  // namespace

TEST_CASE("launcher requests Wayland only and never falls back to X11") {
  // SDL is built without X11: no such driver exists to fall back to.
  for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) CHECK(std::string(SDL_GetVideoDriver(i)) != "x11");
  Env chosen("SDL_VIDEO_DRIVER", nullptr);
  venture::launcher::prefer_native_video_driver();
  CHECK(std::string(SDL_GetHint(SDL_HINT_VIDEO_DRIVER)) == "wayland");
  CHECK(std::string(SDL_GetHint(SDL_HINT_OPENGL_LIBRARY)) == "libOpenGL.so.0");  // EGL, never GLX

  const auto runtime = fs::temp_directory_path() / ("venture-wayland-" + std::to_string(getpid()));
  fs::create_directories(runtime);
  fs::permissions(runtime, fs::perms::owner_all, fs::perm_options::replace);
  struct Remove { fs::path p; ~Remove() { std::error_code ec; fs::remove_all(p, ec); } } remove{runtime};
  Env runtime_dir("XDG_RUNTIME_DIR", runtime.c_str());
  Env display("DISPLAY", nullptr);
  Env session("XDG_SESSION_TYPE", nullptr);
  {
    Child weston({"weston", "--backend=headless", "--socket=venture-test", "--idle-time=0"},
                 {"XDG_RUNTIME_DIR=" + runtime.string()});
    if (weston.pid <= 0) {
      MESSAGE("No weston; the Wayland path is not exercised");
    } else {
      for (int i = 0; i < 100 && !fs::exists(runtime / "venture-test"); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      REQUIRE(fs::exists(runtime / "venture-test"));
      Env wayland("WAYLAND_DISPLAY", "venture-test");
      CHECK(video_driver() == "wayland");
      // The launcher's own frame: a borderless window that accepts a hit test.
      CHECK(venture::launcher::client_side_decorations());
      venture::launcher::prefer_native_video_driver();
      REQUIRE(SDL_Init(SDL_INIT_VIDEO));
      auto* window = SDL_CreateWindow("test", 64, 48, SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS);
      REQUIRE(window);
      CHECK((SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS) != 0);
      CHECK(SDL_SetWindowHitTest(window, [](SDL_Window*, const SDL_Point*, void*) { return SDL_HITTEST_NORMAL; },
                                 nullptr));
      SDL_DestroyWindow(window);
      SDL_Quit();
      // The user's choice still wins.
      Env user("SDL_VIDEO_DRIVER", "offscreen");
      CHECK(video_driver() == "offscreen");
    }
  }
  // Without a Wayland display video does not start at all.
  Env wayland("WAYLAND_DISPLAY", "venture-missing");  // never a real socket
  CHECK(video_driver().empty());
  CHECK(std::string(venture::launcher::video_unavailable_message()).find("Wayland") != std::string::npos);
}

TEST_CASE("launcher chrome moves from the title strip and resizes from the borders") {
  using venture::launcher::chrome::Frame;
  using venture::launcher::chrome::hit_test;
  Frame f{1000, 600, false, 40, 6, {862, 0, 138, 32}, false};
  CHECK(hit_test(f, {500, 20}) == SDL_HITTEST_DRAGGABLE);
  CHECK(hit_test(f, {900, 10}) == SDL_HITTEST_NORMAL);       // window buttons
  CHECK(hit_test(f, {500, 300}) == SDL_HITTEST_NORMAL);      // content
  CHECK(hit_test(f, {2, 300}) == SDL_HITTEST_RESIZE_LEFT);
  CHECK(hit_test(f, {998, 300}) == SDL_HITTEST_RESIZE_RIGHT);
  CHECK(hit_test(f, {500, 2}) == SDL_HITTEST_RESIZE_TOP);
  CHECK(hit_test(f, {500, 598}) == SDL_HITTEST_RESIZE_BOTTOM);
  CHECK(hit_test(f, {1, 1}) == SDL_HITTEST_RESIZE_TOPLEFT);
  CHECK(hit_test(f, {999, 599}) == SDL_HITTEST_RESIZE_BOTTOMRIGHT);
  f.over_item = true;  // a button in the strip gets the press
  CHECK(hit_test(f, {500, 20}) == SDL_HITTEST_NORMAL);
  f.over_item = false;
  f.maximized = true;  // no borders to resize while maximized
  CHECK(hit_test(f, {500, 2}) == SDL_HITTEST_DRAGGABLE);
  CHECK(hit_test(f, {2, 300}) == SDL_HITTEST_NORMAL);
}
