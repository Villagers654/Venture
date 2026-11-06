#include "launcher/auth_browser.h"
#include "launcher/desktop_id.h"
#include "common/log.h"
#include "common/meta/auth.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <deque>
#include <dlfcn.h>
#include <fcntl.h>
#include <format>
#include <mutex>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;

// Meta sign-in runs WebKitGTK in a separate helper process (the launcher
// re-executed with kHelperFlag), so GTK never shares a process or main loop
// with SDL. The helper loads WebKitGTK at runtime: the launcher has no link
// dependency on it and reports a clear error where it is not installed.
// Protocol: the sign-in URL arrives on stdin; events leave on fd 3 as lines
// ("ready", "callback <url>", "error <code>"). Closing stdin ends the helper.
// The helper's stdout/stderr are discarded so no page data reaches a log.
namespace venture::launcher {
namespace {
constexpr const char *kHelperFlag = "--auth-browser";
constexpr int kEventFd = 3;
constexpr size_t kMaxLine = 4 * 1024 * 1024;

bool write_all(int fd, std::string_view data) {
  while (!data.empty()) {
    auto n = ::write(fd, data.data(), data.size());
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    data.remove_prefix(size_t(n));
  }
  return true;
}

// --- helper process ---------------------------------------------------------
// Minimal GLib/GTK/WebKitGTK ABI, resolved with dlsym. WebKitGTK 6.0 (GTK 4)
// is preferred; WebKit2GTK 4.1 (GTK 3) covers older distributions.
using Fn = void (*)();
struct Web {
  int api = 0; // 6 or 4
  unsigned long (*signal_connect)(void *, const char *, Fn, void *, void *,
                                  int) = nullptr;
  void *(*object_new)(unsigned long, const char *, ...) = nullptr;
  void *(*loop_new)(void *, int) = nullptr;
  void (*loop_run)(void *) = nullptr;
  void (*loop_quit)(void *) = nullptr;
  int (*init4)() = nullptr;
  int (*init3)(int *, char ***) = nullptr;
  void (*allowed_backends)(const char *) = nullptr;
  void *(*window_new4)() = nullptr;
  void *(*window_new3)(int) = nullptr;
  void (*set_prgname)(const char *) = nullptr;
  void (*set_title)(void *, const char *) = nullptr;
  void (*set_default_size)(void *, int, int) = nullptr;
  void (*set_child)(void *, void *) = nullptr;     // GTK 4
  void (*container_add)(void *, void *) = nullptr; // GTK 3
  void (*present)(void *) = nullptr;
  void (*show_all)(void *) = nullptr; // GTK 3
  unsigned long (*view_type)() = nullptr;
  void *(*session_ephemeral)() = nullptr; // 6.0
  void *(*context_ephemeral)() = nullptr; // 4.1
  void *(*view_with_context)(void *) = nullptr;
  void (*load_uri)(void *, const char *) = nullptr;
  const char *(*view_uri)(void *) = nullptr;
  void (*stop_loading)(void *) = nullptr;
  void (*destroy4)(void *) = nullptr; // gtk_window_destroy
  void (*destroy3)(void *) = nullptr; // gtk_widget_destroy
  void *(*decision_action)(void *) = nullptr;
  void *(*action_request)(void *) = nullptr;
  const char *(*request_uri)(void *) = nullptr;
  void (*decision_ignore)(void *) = nullptr;
  void (*decision_use)(void *) = nullptr;
  void (*permission_deny)(void *) = nullptr;
  void *(*settings_default)() = nullptr;
  void (*object_get)(void *, const char *, ...) = nullptr;
  void (*object_set)(void *, const char *, ...) = nullptr;
  void *(*screen_default)() = nullptr;          // GTK 3
  double (*screen_resolution)(void *) = nullptr; // GTK 3
  // Content filters: block a page load before its request is sent.
  void *(*bytes_new)(const void *, size_t) = nullptr;
  void (*bytes_unref)(void *) = nullptr;
  void (*error_free)(void *) = nullptr;
  const char *(*user_cache_dir)() = nullptr;
  void *(*filter_store_new)(const char *) = nullptr;
  void (*filter_store_save)(void *, const char *, void *, void *, Fn, void *) = nullptr;
  void *(*filter_store_save_finish)(void *, void *, void **) = nullptr;
  void *(*content_manager)(void *) = nullptr;
  void (*add_filter)(void *, void *) = nullptr;
};
template <class T> bool bind(void *lib, T &out, const char *name) {
  out = reinterpret_cast<T>(dlsym(lib, name));
  return out != nullptr;
}
bool load(Web &w) {
  void *lib = dlopen("libwebkitgtk-6.0.so.4", RTLD_NOW | RTLD_LOCAL);
  w.api = 6;
  if (!lib) {
    lib = dlopen("libwebkit2gtk-4.1.so.0", RTLD_NOW | RTLD_LOCAL);
    w.api = 4;
  }
  if (!lib)
    return false;
  bool ok = bind(lib, w.allowed_backends, "gdk_set_allowed_backends") &&
            bind(lib, w.signal_connect, "g_signal_connect_data") &&
            bind(lib, w.object_new, "g_object_new") &&
            bind(lib, w.loop_new, "g_main_loop_new") &&
            bind(lib, w.loop_run, "g_main_loop_run") &&
            bind(lib, w.loop_quit, "g_main_loop_quit") &&
            bind(lib, w.set_prgname, "g_set_prgname") &&
            bind(lib, w.set_title, "gtk_window_set_title") &&
            bind(lib, w.set_default_size, "gtk_window_set_default_size") &&
            bind(lib, w.view_type, "webkit_web_view_get_type") &&
            bind(lib, w.load_uri, "webkit_web_view_load_uri") &&
            bind(lib, w.view_uri, "webkit_web_view_get_uri") &&
            bind(lib, w.stop_loading, "webkit_web_view_stop_loading") &&
            bind(lib, w.decision_action,
                 "webkit_navigation_policy_decision_get_navigation_action") &&
            bind(lib, w.action_request,
                 "webkit_navigation_action_get_request") &&
            bind(lib, w.request_uri, "webkit_uri_request_get_uri") &&
            bind(lib, w.decision_ignore, "webkit_policy_decision_ignore") &&
            bind(lib, w.decision_use, "webkit_policy_decision_use") &&
            bind(lib, w.permission_deny, "webkit_permission_request_deny") &&
            bind(lib, w.settings_default, "gtk_settings_get_default") &&
            bind(lib, w.object_get, "g_object_get") &&
            bind(lib, w.object_set, "g_object_set") &&
            bind(lib, w.bytes_new, "g_bytes_new") && bind(lib, w.bytes_unref, "g_bytes_unref") &&
            bind(lib, w.error_free, "g_error_free") &&
            bind(lib, w.user_cache_dir, "g_get_user_cache_dir") &&
            bind(lib, w.filter_store_new, "webkit_user_content_filter_store_new") &&
            bind(lib, w.filter_store_save, "webkit_user_content_filter_store_save") &&
            bind(lib, w.filter_store_save_finish, "webkit_user_content_filter_store_save_finish") &&
            bind(lib, w.content_manager, "webkit_web_view_get_user_content_manager") &&
            bind(lib, w.add_filter, "webkit_user_content_manager_add_filter");
  if (w.api == 6)
    return ok && bind(lib, w.init4, "gtk_init_check") &&
           bind(lib, w.window_new4, "gtk_window_new") &&
           bind(lib, w.set_child, "gtk_window_set_child") &&
           bind(lib, w.present, "gtk_window_present") &&
           bind(lib, w.destroy4, "gtk_window_destroy") &&
           bind(lib, w.session_ephemeral, "webkit_network_session_new_ephemeral");
  return ok && bind(lib, w.init3, "gtk_init_check") &&
         bind(lib, w.window_new3, "gtk_window_new") &&
         bind(lib, w.container_add, "gtk_container_add") &&
         bind(lib, w.show_all, "gtk_widget_show_all") &&
         bind(lib, w.destroy3, "gtk_widget_destroy") &&
         bind(lib, w.screen_default, "gdk_screen_get_default") &&
         bind(lib, w.screen_resolution, "gdk_screen_get_resolution") &&
         bind(lib, w.context_ephemeral, "webkit_web_context_new_ephemeral") &&
         bind(lib, w.view_with_context, "webkit_web_view_new_with_context");
}
// WebKitGTK zooms every page by the desktop font DPI / 96: GTK 4's
// gtk-xft-dpi, GTK 3's screen resolution. GTK reads that DPI from the settings
// portal's org.gnome.desktop.interface text-scaling-factor, and a portal that
// answers without that key (KDE Plasma's) leaves it 0 (GTK 4) or unset, -1
// (GTK 3). WebKit then lays pages out at a near-zero zoom: Meta's sign-in page
// became a column one letter wide. Outside a sane range the view gets the
// standard 96 DPI, set as an application value so a later portal change cannot
// undo it before the view exists.
constexpr double kMinFontDpi = 24, kMaxFontDpi = 960, kStandardFontDpi = 96;
void ensure_font_dpi(Web &w) {
  void *settings = w.settings_default();
  if (!settings)
    return;
  double dpi = -1;
  if (w.api == 4) {
    if (void *screen = w.screen_default())
      dpi = w.screen_resolution(screen);
  } else {
    int xft = 0;
    w.object_get(settings, "gtk-xft-dpi", &xft, nullptr);
    dpi = xft / 1024.0;
  }
  if (dpi >= kMinFontDpi && dpi <= kMaxFontDpi)
    return;
  w.object_set(settings, "gtk-xft-dpi", int(kStandardFontDpi * 1024), nullptr);
}
struct Helper {
  Web web;
  void *loop = nullptr, *view = nullptr;
  std::string url;
  bool callback_sent = false, load_failed = false;
  void send(std::string_view line) {
    write_all(kEventFd, std::string(line) + "\n");
  }
};
Helper *g_helper = nullptr;
// WebKitPolicyDecisionType / WebKitLoadEvent values (stable across 4.1 and 6.0).
constexpr int kNavigationAction = 0, kNewWindowAction = 1;
constexpr int kLoadStarted = 0, kLoadRedirected = 1, kLoadCommitted = 2, kLoadFinished = 3;

// Pages stay on Meta domains; embedded frames (security checks, captchas) may
// be any plain HTTPS origin. WebKit asks one policy for every frame, so the page
// rule is enforced by the content filter below, before any request is sent,
// and again on each view's own load.
int on_decide_policy(void *, void *decision, int type, void *) {
  auto &h = *g_helper;
  if (type != kNavigationAction && type != kNewWindowAction)
    return 0; // responses keep WebKit's default handling
  auto *action = h.web.decision_action(decision);
  auto *request = action ? h.web.action_request(action) : nullptr;
  const char *raw = request ? h.web.request_uri(request) : nullptr;
  std::string uri = raw ? raw : "";
  if (meta::is_callback(uri)) {
    h.web.decision_ignore(decision);
    if (!h.callback_sent) {
      h.callback_sent = true;
      h.send("callback " + uri);
    }
    h.web.loop_quit(h.loop);
    return 1;
  }
  // An allowed new window continues to "create", which opens a real popup.
  const bool allowed = type == kNewWindowAction
                           ? uri.empty() || uri == "about:blank" || meta::auth_navigation_allowed(uri)
                           : meta::auth_frame_allowed(uri);
  if (allowed)
    h.web.decision_use(decision);
  else
    h.web.decision_ignore(decision);
  return 1;
}
int on_permission(void *, void *request, void *) {
  g_helper->web.permission_deny(request);
  return 1;
}
int on_suppress(void *, void *, void *, void *, void *) { return 1; }
void on_load_changed(void *view, int event, void *) {
  auto &h = *g_helper;
  if (event == kLoadStarted || event == kLoadRedirected || event == kLoadCommitted) {
    const char *raw = h.web.view_uri(view);
    std::string uri = raw ? raw : "";
    if (!uri.empty() && uri != "about:blank" && !meta::auth_navigation_allowed(uri)) {
      h.web.stop_loading(view);
      return;
    }
  }
  if (view != h.view)
    return;
  if (event == kLoadStarted)
    h.load_failed = false;
  if (event == kLoadFinished && !h.load_failed)
    h.send("ready");
}
int on_load_failed(void *view, int, void *, void *, void *) {
  if (view == g_helper->view)
    g_helper->load_failed = true;
  return 0;
}
void connect(void *object, const char *signal, auto fn, void *data = nullptr) {
  g_helper->web.signal_connect(object, signal, reinterpret_cast<Fn>(fn), data, nullptr, 0);
}
void *on_create(void *view, void *, void *);
// The same rules for the sign-in view and its popups.
void attach(void *view) {
  connect(view, "decide-policy", on_decide_policy);
  connect(view, "permission-request", on_permission);
  connect(view, "context-menu", on_suppress);
  connect(view, "load-changed", on_load_changed);
  connect(view, "load-failed", on_load_failed);
  connect(view, "create", on_create);
}
void *new_window(const char *title, int width, int height) {
  auto &w = g_helper->web;
  void *window = w.api == 6 ? w.window_new4() : w.window_new3(0);
  if (window) {
    w.set_title(window, title);
    w.set_default_size(window, width, height);
  }
  return window;
}
void show(void *window, void *view) {
  auto &w = g_helper->web;
  if (w.api == 6) {
    w.set_child(window, view);
    w.present(window);
  } else {
    w.container_add(window, view);
    w.show_all(window);
  }
}
void on_popup_close(void *, void *window) {
  auto &w = g_helper->web;
  w.api == 6 ? w.destroy4(window) : w.destroy3(window);
}
void on_popup_ready(void *view, void *) {
  if (void *window = new_window("Sign in to Meta - Venture", 560, 760)) {
    connect(view, "close", on_popup_close, window);
    show(window, view);
  }
}
// A window.open() from the sign-in page, for example Facebook login and its
// security check. A related view keeps window.opener and the ephemeral session,
// so the popup can hand its result back and close itself.
void *on_create(void *view, void *, void *) {
  auto &w = g_helper->web;
  void *popup = w.object_new(w.view_type(), "related-view", view, nullptr);
  if (!popup)
    return nullptr;
  attach(popup);
  connect(popup, "ready-to-show", on_popup_ready);
  return popup;
}
// Every page (top-frame document) is blocked unless it is on a Meta domain, as
// auth_navigation_allowed() decides; embedded frames are left to the policy
// above. Popups share the view's user content manager.
constexpr std::string_view kPageFilter = R"([
  {"trigger": {"url-filter": ".*", "resource-type": ["document"], "load-context": ["top-frame"]},
   "action": {"type": "block"}},
  {"trigger": {"url-filter": "^https://([a-z0-9-]+\\.)*meta\\.com(:443)?/", "load-context": ["top-frame"]},
   "action": {"type": "ignore-previous-rules"}},
  {"trigger": {"url-filter": "^https://([a-z0-9-]+\\.)*facebook\\.com(:443)?/", "load-context": ["top-frame"]},
   "action": {"type": "ignore-previous-rules"}},
  {"trigger": {"url-filter": "^https://([a-z0-9-]+\\.)*fbsbx\\.com(:443)?/", "load-context": ["top-frame"]},
   "action": {"type": "ignore-previous-rules"}},
  {"trigger": {"url-filter": "^https://([a-z0-9-]+\\.)*oculus\\.com(:443)?/", "load-context": ["top-frame"]},
   "action": {"type": "ignore-previous-rules"}},
  {"trigger": {"url-filter": "^https://([a-z0-9-]+\\.)*instagram\\.com(:443)?/", "load-context": ["top-frame"]},
   "action": {"type": "ignore-previous-rules"}}
])";
void on_filter_saved(void *store, void *result, void *) {
  auto &h = *g_helper;
  void *error = nullptr;
  void *filter = h.web.filter_store_save_finish(store, result, &error);
  if (!filter) {
    // Without it nothing outside Meta may load at all.
    if (error) h.web.error_free(error);
    h.send("error filter");
    h.web.loop_quit(h.loop);
    return;
  }
  h.web.add_filter(h.web.content_manager(h.view), filter);
  h.web.load_uri(h.view, h.url.c_str());
}
void on_crashed(void *, int, void *) {
  g_helper->send("error crashed");
  g_helper->web.loop_quit(g_helper->loop);
}
void on_destroy(void *, void *) { g_helper->web.loop_quit(g_helper->loop); }

int helper_main() {
  // Keep the event pipe out of WebKit's own subprocesses.
  fcntl(kEventFd, F_SETFD, FD_CLOEXEC);
  // Discard anything GTK or WebKit prints; events use kEventFd only.
  if (int null = ::open("/dev/null", O_WRONLY | O_CLOEXEC); null >= 0) {
    dup2(null, 1);
    dup2(null, 2);
    ::close(null);
  }
  std::string url;
  for (char c; url.size() < kMaxLine;) {
    auto n = ::read(0, &c, 1);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0 || c == '\n')
      break;
    url.push_back(c);
  }
  static Helper helper;
  g_helper = &helper;
  if (!meta::auth_navigation_allowed(url))
    return helper.send("error url"), 1;
  // The launcher closes stdin when sign-in is cancelled or it exits.
  std::thread([] {
    char c;
    while (true) {
      auto n = ::read(0, &c, 1);
      if (n == 0 || (n < 0 && errno != EINTR))
        _exit(0);
    }
  }).detach();
  auto &w = helper.web;
  if (!load(w))
    return helper.send("error webkit"), 1;
  // Wayland only: without a Wayland display GTK fails to initialise rather
  // than opening an X11 connection.
  w.allowed_backends("wayland");
  w.set_prgname(kDesktopId);  // the Wayland app_id: the launcher's icon, not a generic one
  if (w.api == 6 ? !w.init4() : !w.init3(nullptr, nullptr))
    return helper.send("error display"), 1;
  // Before any web view exists: WebKit reads the DPI when its context starts.
  ensure_font_dpi(w);
  helper.loop = w.loop_new(nullptr, 0);
  void *window = new_window("Sign in to Meta - Venture", 650, 820);
  // Ephemeral sessions keep cookies in memory, isolated for each sign-in.
  if (w.api == 6)
    helper.view = w.object_new(w.view_type(), "network-session",
                               w.session_ephemeral(), nullptr);
  else
    helper.view = w.view_with_context(w.context_ephemeral());
  if (!window || !helper.view)
    return helper.send("error display"), 1;
  attach(helper.view);
  connect(helper.view, "web-process-terminated", on_crashed);
  connect(window, "destroy", on_destroy);
  show(window, helper.view);
  // The sign-in page loads once the page filter is in place.
  helper.url = url;
  const std::string store = std::string(w.user_cache_dir()) + "/venture/webkit-filters";
  void *bytes = w.bytes_new(kPageFilter.data(), kPageFilter.size());
  w.filter_store_save(w.filter_store_new(store.c_str()), "venture-sign-in-pages", bytes, nullptr,
                      reinterpret_cast<Fn>(on_filter_saved), nullptr);
  w.bytes_unref(bytes);
  w.loop_run(helper.loop);
  // Skip GTK/WebKit teardown; WebKit's subprocesses exit with this one.
  _exit(0);
}
} // namespace

int auth_browser_helper(int argc, char **argv) {
  if (argc != 2 || std::string_view(argv[1]) != kHelperFlag)
    return -1;
  return helper_main();
}

// --- launcher side -------------------------------------------------------------
struct AuthBrowser::Impl {
  std::mutex mutex;
  std::deque<Event> events;
  pid_t pid = -1; // guarded by mutex; -1 once reaped
  int input = -1;
  std::thread reader;
  void emit(Event event) {
    std::lock_guard lock(mutex);
    events.push_back(std::move(event));
  }
};
AuthBrowser::AuthBrowser() : impl_(std::make_unique<Impl>()) {}
AuthBrowser::~AuthBrowser() { close(); }
void AuthBrowser::close() {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->pid > 0)
      kill(impl_->pid, SIGTERM);
  }
  if (impl_->input >= 0) {
    ::close(impl_->input);
    impl_->input = -1;
  }
  if (impl_->reader.joinable())
    impl_->reader.join();
}
std::optional<AuthBrowser::Event> AuthBrowser::poll() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->events.empty())
    return {};
  auto event = std::move(impl_->events.front());
  impl_->events.pop_front();
  return event;
}
namespace {
AuthBrowser::Event translate(std::string_view line) {
  using Kind = AuthBrowser::Kind;
  if (line == "ready")
    return {Kind::ready, {}};
  if (line.starts_with("callback ")) {
    auto uri = line.substr(9);
    if (meta::is_callback(uri))
      return {Kind::callback, std::string(uri)};
  }
  if (line == "error webkit")
    return {Kind::error, "Meta sign-in needs WebKitGTK. Install webkitgtk-6.0 "
                         "(or webkit2gtk-4.1) from your distribution."};
  if (line == "error filter")
    return {Kind::error, "This WebKitGTK is too old to keep the sign-in window on Meta's pages. "
                         "Update webkitgtk-6.0 (or webkit2gtk-4.1)."};
  if (line == "error crashed")
    return {Kind::error, "The Meta sign-in page stopped unexpectedly. Try again."};
  return {Kind::error, "Could not open the Meta sign-in window."};
}
} // namespace
void AuthBrowser::open(std::string url, const fs::path &) {
  close();
  if (!meta::auth_navigation_allowed(url))
    throw std::runtime_error("Sign-in URL is outside allowed Meta domains.");
  {
    std::lock_guard lock(impl_->mutex);
    impl_->events.clear();
  }
  int in[2], out[2];
  if (pipe2(in, O_CLOEXEC))
    throw std::runtime_error("Could not open the Meta sign-in window.");
  if (pipe2(out, O_CLOEXEC)) {
    ::close(in[0]);
    ::close(in[1]);
    throw std::runtime_error("Could not open the Meta sign-in window.");
  }
  // Queued before the helper starts, so a helper that exits early cannot
  // raise SIGPIPE here. The URL is far below the pipe capacity.
  write_all(in[1], url + "\n");
  auto exe = paths::executable();
  std::string exe_arg = exe.string(), flag = kHelperFlag;
  char *argv[] = {exe_arg.data(), flag.data(), nullptr};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in[0], 0);
  posix_spawn_file_actions_adddup2(&actions, out[1], kEventFd);
  pid_t pid = -1;
  int error = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(in[0]);
  ::close(out[1]);
  if (error) {
    ::close(in[1]);
    ::close(out[0]);
    throw std::runtime_error("Could not open the Meta sign-in window.");
  }
  {
    std::lock_guard lock(impl_->mutex);
    impl_->pid = pid;
  }
  impl_->input = in[1];
  impl_->reader = std::thread([impl = impl_.get(), fd = out[0], pid] {
    std::string buffer;
    char chunk[4096];
    bool overflow = false, reported = false;
    while (!overflow) {
      auto n = ::read(fd, chunk, sizeof chunk);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      buffer.append(chunk, size_t(n));
      for (size_t end; (end = buffer.find('\n')) != buffer.npos;) {
        auto event = translate(std::string_view(buffer).substr(0, end));
        reported |= event.kind == Kind::error;
        impl->emit(std::move(event));
        buffer.erase(0, end + 1);
      }
      overflow = buffer.size() > kMaxLine;
    }
    ::close(fd);
    if (overflow)
      kill(pid, SIGTERM);
    // Wait without reaping so close() can never signal a recycled pid.
    siginfo_t info{};
    while (waitid(P_PID, id_t(pid), &info, WEXITED | WNOWAIT) < 0 &&
           errno == EINTR) {
    }
    std::lock_guard lock(impl->mutex);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    impl->pid = -1;
    // A window the user closes exits 0 and close() ends it with SIGTERM; a
    // helper WebKit took down (a crash, or a sandbox it cannot start) is an
    // error, not a cancelled sign-in.
    if (!reported && !overflow &&
        ((WIFSIGNALED(status) && WTERMSIG(status) != SIGTERM) || (WIFEXITED(status) && WEXITSTATUS(status)))) {
      const auto how = WIFSIGNALED(status) ? std::format("signal {}", WTERMSIG(status))
                                           : std::format("exit code {}", WEXITSTATUS(status));
      log::warn("auth", "The Meta sign-in window stopped with {}.", how);
      impl->events.push_back({Kind::error, "The Meta sign-in window stopped unexpectedly (" + how +
                                               "). Send a debug report if it happens again."});
    }
    impl->events.push_back({Kind::closed, {}});
  });
}
} // namespace venture::launcher
