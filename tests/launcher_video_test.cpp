// The launcher's SDL video backend: Wayland, against a private headless
// Weston (never the desktop's compositor), with no X11 fallback; inside gamescope
// its Xwayland, against a private Xvfb, where gamescope's close reaches the window.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <SDL3/SDL.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <optional>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/environment.h"
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
    set(value);
  }
  ~Env() { set(saved ? saved->c_str() : nullptr); }
  // SDL reads its own copy of the environment, taken when it is first used
  // (WAYLAND_DISPLAY, SDL_VIDEO_DRIVER): both change.
  void set(const char* value) {
    value ? setenv(name.c_str(), value, 1) : unsetenv(name.c_str());
    value ? SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name.c_str(), value, true)
          : SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), name.c_str());
  }
};
std::string video_driver() {
  venture::launcher::prefer_native_video_driver();
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    MESSAGE("SDL video did not start: " << std::string(SDL_GetError()));
    return {};
  }
  std::string name = SDL_GetCurrentVideoDriver();
  // A hidden window confirms the backend is usable, not merely selected.
  if (auto* window = SDL_CreateWindow("test", 64, 48, SDL_WINDOW_HIDDEN)) SDL_DestroyWindow(window);
  else {
    MESSAGE("SDL window failed: " << std::string(SDL_GetError()));
    name.clear();
  }
  SDL_Quit();
  return name;
}
}  // namespace

TEST_CASE("launcher requests Wayland, and gamescope's Xwayland inside gamescope") {
  Env chosen("SDL_VIDEO_DRIVER", nullptr);
  Env gamescope("GAMESCOPE_WAYLAND_DISPLAY", nullptr);
  venture::environment::refresh();
  CHECK_FALSE(venture::environment::gamescope_xwayland());
  venture::launcher::prefer_native_video_driver();
  CHECK(std::string(SDL_GetHint(SDL_HINT_VIDEO_DRIVER)) == "wayland");
  CHECK(std::string(SDL_GetHint(SDL_HINT_OPENGL_LIBRARY)) == "libOpenGL.so.0");  // EGL, never GLX
  // The Wayland app_id is the desktop file's name, so docks show the launcher's icon.
  CHECK(std::string(SDL_GetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING)) ==
        "org.venture_emu.Venture");

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
  // Without a Wayland display video does not start at all (no X11 outside gamescope).
  Env wayland("WAYLAND_DISPLAY", "venture-missing");  // never a real socket
  Env x_display("DISPLAY", ":97");
  venture::environment::refresh();
  CHECK(video_driver().empty());
  CHECK(std::string(venture::launcher::video_unavailable_message()).find("Wayland") != std::string::npos);
  venture::environment::refresh();
}

TEST_CASE("inside gamescope the launcher's window is on its Xwayland, where gamescope's close reaches it") {
  // gamescope closes an app's windows (SteamVR's close on its panel) by sending
  // WM_DELETE_WINDOW to X11 windows only, so SDL carries its X11 backend.
  bool x11 = false;
  for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) x11 = x11 || std::string(SDL_GetVideoDriver(i)) == "x11";
  REQUIRE(x11);
  Env chosen("SDL_VIDEO_DRIVER", nullptr);
  // gamescope's environment: its Wayland display (which main() makes WAYLAND_DISPLAY) and its Xwayland.
  Env wayland("WAYLAND_DISPLAY", "gamescope-0");
  Env gamescope("GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0");
  {
    Env no_x("DISPLAY", nullptr);
    venture::environment::refresh();
    CHECK_FALSE(venture::environment::gamescope_xwayland());  // no Xwayland: Wayland, as before
    venture::launcher::prefer_native_video_driver();
    CHECK(std::string(SDL_GetHint(SDL_HINT_VIDEO_DRIVER)) == "wayland");
  }
  // A display number nothing else uses, for a private Xvfb.
  const std::string number = std::to_string(90 + getpid() % 9);
  Env display("DISPLAY", (":" + number).c_str());
  venture::environment::refresh();
  CHECK(venture::environment::gamescope_xwayland());
  venture::launcher::prefer_native_video_driver();
  CHECK(std::string(SDL_GetHint(SDL_HINT_VIDEO_DRIVER)) == "x11,wayland");
  CHECK(std::string(SDL_GetHint(SDL_HINT_VIDEO_FORCE_EGL)) == "1");  // EGL, never GLX
  CHECK(std::string(SDL_GetHint(SDL_HINT_OPENGL_LIBRARY)) == "libOpenGL.so.0");

  Child xvfb({"Xvfb", ":" + number, "-nolisten", "tcp", "-screen", "0", "640x480x24"}, {});
  if (xvfb.pid <= 0) {
    MESSAGE("No Xvfb; the X11 close path is not exercised");
    venture::environment::refresh();
    return;
  }
  for (int i = 0; i < 100 && !fs::exists("/tmp/.X11-unix/X" + number); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(fs::exists("/tmp/.X11-unix/X" + number));
  REQUIRE(SDL_Init(SDL_INIT_VIDEO));
  CHECK(std::string(SDL_GetCurrentVideoDriver()) == "x11");
  auto* window = SDL_CreateWindow("Venture", 320, 240, SDL_WINDOW_BORDERLESS);
  REQUIRE(window);
  // gamescope's close, as steamcompmgr sends it: a WM_PROTOCOLS client message
  // naming WM_DELETE_WINDOW, sent to the window with no event mask.
  const auto props = SDL_GetWindowProperties(window);
  void* x_display_ptr = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
  const auto x_window = static_cast<unsigned long>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  REQUIRE(x_display_ptr);
  REQUIRE(x_window != 0);
  void* xlib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
  REQUIRE(xlib);
  using Atom = unsigned long;
  auto intern = reinterpret_cast<Atom (*)(void*, const char*, int)>(dlsym(xlib, "XInternAtom"));
  auto send = reinterpret_cast<int (*)(void*, unsigned long, int, long, void*)>(dlsym(xlib, "XSendEvent"));
  auto flush = reinterpret_cast<int (*)(void*)>(dlsym(xlib, "XFlush"));
  REQUIRE((intern && send && flush));
  // XClientMessageEvent's layout (Xlib.h), inside the 24-long XEvent union.
  struct ClientMessage {
    int type;
    unsigned long serial;
    int send_event;
    void* display;
    unsigned long window;
    Atom message_type;
    int format;
    long l[5];
  };
  union {
    ClientMessage message;
    long pad[24];
  } event{};
  constexpr int kClientMessage = 33;
  event.message.type = kClientMessage;
  event.message.window = x_window;
  event.message.message_type = intern(x_display_ptr, "WM_PROTOCOLS", 0);
  event.message.format = 32;
  event.message.l[0] = long(intern(x_display_ptr, "WM_DELETE_WINDOW", 0));
  event.message.l[1] = 0;  // CurrentTime
  send(x_display_ptr, x_window, 0, 0, &event);
  flush(x_display_ptr);
  bool close_requested = false;
  for (int i = 0; i < 200 && !close_requested; ++i) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
      close_requested = close_requested ||
          (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(close_requested);  // the launcher quits on it (app.cpp)
  SDL_DestroyWindow(window);
  SDL_Quit();
  venture::environment::refresh();
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
