#include "launcher/auth_browser.h"
#include "common/log.h"
#include "common/meta/auth.h"
#include "webview_compat.h"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>

namespace venture::launcher {
namespace {
template <class I, class... Args> class Handler final : public I {
  std::atomic<ULONG> refs_{1};
  std::function<HRESULT(Args...)> fn_;

public:
  explicit Handler(std::function<HRESULT(Args...)> fn) : fn_(std::move(fn)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, __uuidof(I))) {
      *out = static_cast<I *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = --refs_;
    if (!n)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE Invoke(Args... args) override {
    try {
      return fn_(args...);
    } catch (...) {
      return E_FAIL;
    }
  }
};
template <class I, class... Args, class F> auto handler(F &&fn) {
  return new Handler<I, Args...>(std::forward<F>(fn));
}
std::string utf8(LPCWSTR s) {
  return s ? paths::utf8(fs::path(s)) : std::string{};
}
// A window.open() from the sign-in page, for example Facebook login and its
// security check. A real window keeps window.opener, so the popup can hand its
// result back and close itself.
struct Popup {
  HWND window = nullptr;
  ICoreWebView2Controller *controller = nullptr;
  ICoreWebView2 *web = nullptr;
  void close_view() {
    if (controller) {
      controller->Close();
      controller->Release();
      controller = nullptr;
    }
    if (web) {
      web->Release();
      web = nullptr;
    }
  }
};
struct BrowserState {
  HWND window = nullptr;
  ICoreWebView2Controller *controller = nullptr;
  ICoreWebView2 *web = nullptr;
  ICoreWebView2Environment10 *env = nullptr;
  std::vector<std::unique_ptr<Popup>> popups;
  std::string url;
  std::vector<meta::Cookie> cookies;
  std::function<void(AuthBrowser::Event)> emit;
  bool done = false, callback_sent = false;
  void fail() {
    emit({AuthBrowser::Kind::error,
          "Could not open Meta sign-in. Install or repair the Microsoft Edge "
          "WebView2 Runtime."});
    if (window)
      PostMessageW(window, WM_CLOSE, 0, 0);
  }
  void navigate(std::string_view uri) {
    if (!meta::is_callback(uri))
      return;
    if (!callback_sent) {
      callback_sent = true;
      emit({AuthBrowser::Kind::callback, std::string(uri)});
    }
    if (window)
      PostMessageW(window, WM_CLOSE, 0, 0);
  }
  void shutdown() {
    done = true;
    for (auto &popup : popups) {
      popup->close_view();
      if (popup->window && IsWindow(popup->window))
        DestroyWindow(popup->window);
      popup->window = nullptr;
    }
    if (controller) {
      controller->Close();
      controller->Release();
      controller = nullptr;
    }
    if (web) {
      web->Release();
      web = nullptr;
    }
    if (env) {
      env->Release();
      env = nullptr;
    }
  }
};
LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  auto *state =
      reinterpret_cast<BrowserState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    state = static_cast<BrowserState *>(
        reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (state && msg == WM_SIZE && state->controller) {
    RECT r{};
    GetClientRect(hwnd, &r);
    state->controller->put_Bounds(r);
    return 0;
  }
  if (state && msg == WM_CLOSE) {
    state->shutdown();
    DestroyWindow(hwnd);
    return 0;
  }
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, w, l);
}
LRESULT CALLBACK popup_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  auto *popup = reinterpret_cast<Popup *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    popup = static_cast<Popup *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(popup));
  }
  if (popup && msg == WM_SIZE && popup->controller) {
    RECT r{};
    GetClientRect(hwnd, &r);
    popup->controller->put_Bounds(r);
    return 0;
  }
  if (popup && msg == WM_CLOSE) {
    popup->close_view();
    popup->window = nullptr;
    DestroyWindow(hwnd);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, w, l);
}
// Chrome's cookies.sameSite has no "unspecified" counterpart in WebView2;
// treated as the least restrictive, same as a cookie with no SameSite
// attribute.
COREWEBVIEW2_COOKIE_SAME_SITE_KIND same_site_kind(std::string_view same_site) {
  if (same_site == "lax")
    return COREWEBVIEW2_COOKIE_SAME_SITE_KIND_LAX;
  if (same_site == "strict")
    return COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT;
  return COREWEBVIEW2_COOKIE_SAME_SITE_KIND_NONE;
}
// Seeds this sign-in's fresh private profile with the browser session the
// sideloaded extension last delivered, so the Meta page below recognizes the
// account instead of asking the user to sign in again. Best-effort: sign-in
// proceeds the normal way if any step fails. Never logs a cookie's name,
// value or domain.
void seed_cookies(ICoreWebView2 *web, const std::vector<meta::Cookie> &cookies) {
  ICoreWebView2_2 *web2 = nullptr;
  if (FAILED(web->QueryInterface(IID_ICoreWebView2_2,
                                 reinterpret_cast<void **>(&web2))) ||
      !web2) {
    log::warn("meta", "Could not seed the Meta sign-in window with the "
                      "browser session: WebView2 is too old.");
    return;
  }
  ICoreWebView2CookieManager *manager = nullptr;
  web2->get_CookieManager(&manager);
  web2->Release();
  if (!manager) {
    log::warn("meta", "Could not seed the Meta sign-in window with the browser session.");
    return;
  }
  unsigned seeded = 0;
  for (auto &c : cookies) {
    auto name = paths::from_utf8(c.name), value = paths::from_utf8(c.value),
        domain = paths::from_utf8(c.domain),
        path = paths::from_utf8(c.path.empty() ? "/" : c.path);
    ICoreWebView2Cookie *cookie = nullptr;
    if (FAILED(manager->CreateCookie(name.c_str(), value.c_str(), domain.c_str(),
                                     path.c_str(), &cookie)) ||
        !cookie)
      continue;
    cookie->put_IsSecure(c.secure);
    cookie->put_IsHttpOnly(c.http_only);
    cookie->put_SameSite(same_site_kind(c.same_site));
    if (c.expiration_date > 0)
      cookie->put_Expires(c.expiration_date);
    if (SUCCEEDED(manager->AddOrUpdateCookie(cookie)))
      ++seeded;
    cookie->Release();
  }
  manager->Release();
  log::info("meta", "Seeded the Meta sign-in window with {} browser session cookie(s).",
           seeded);
}
void open_popup(const std::shared_ptr<BrowserState> &state,
                ICoreWebView2NewWindowRequestedEventArgs *args);
// The same rules for the sign-in window and its popups: pages stay on Meta
// domains, embedded frames may be any plain HTTPS origin, the callback ends
// sign-in, and device permissions are refused.
void attach(const std::shared_ptr<BrowserState> &state, ICoreWebView2 *web,
            Popup *popup) {
  ICoreWebView2Settings *settings = nullptr;
  web->get_Settings(&settings);
  if (settings) {
    settings->put_AreDevToolsEnabled(FALSE);
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsWebMessageEnabled(FALSE);
    settings->Release();
  }
  EventRegistrationToken t{};
  auto nav = handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2 *,
                     ICoreWebView2NavigationStartingEventArgs *>(
      [state](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) {
        LPWSTR raw = nullptr;
        args->get_Uri(&raw);
        auto uri = utf8(raw);
        CoTaskMemFree(raw);
        if (!meta::auth_navigation_allowed(uri))
          args->put_Cancel(TRUE);
        state->navigate(uri);
        return S_OK;
      });
  web->add_NavigationStarting(nav, &t);
  nav->Release();
  auto frame = handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2 *,
                       ICoreWebView2NavigationStartingEventArgs *>(
      [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) {
        LPWSTR raw = nullptr;
        args->get_Uri(&raw);
        auto uri = utf8(raw);
        CoTaskMemFree(raw);
        if (!meta::auth_frame_allowed(uri))
          args->put_Cancel(TRUE);
        return S_OK;
      });
  web->add_FrameNavigationStarting(frame, &t);
  frame->Release();
  auto window = handler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2 *,
                        ICoreWebView2NewWindowRequestedEventArgs *>(
      [state](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) {
        LPWSTR raw = nullptr;
        args->get_Uri(&raw);
        auto uri = utf8(raw);
        CoTaskMemFree(raw);
        if (meta::is_callback(uri)) {
          args->put_Handled(TRUE);
          state->navigate(uri);
        } else if (uri.empty() || uri == "about:blank" ||
                   meta::auth_navigation_allowed(uri))
          open_popup(state, args);
        else
          args->put_Handled(TRUE);
        return S_OK;
      });
  web->add_NewWindowRequested(window, &t);
  window->Release();
  auto permissions =
      handler<ICoreWebView2PermissionRequestedEventHandler, ICoreWebView2 *,
              ICoreWebView2PermissionRequestedEventArgs *>(
          [](ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs *args) {
            args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            return S_OK;
          });
  web->add_PermissionRequested(permissions, &t);
  permissions->Release();
  if (popup) {
    auto close = handler<ICoreWebView2WindowCloseRequestedEventHandler,
                         ICoreWebView2 *, IUnknown *>(
        [popup](ICoreWebView2 *, IUnknown *) {
          if (popup->window)
            PostMessageW(popup->window, WM_CLOSE, 0, 0);
          return S_OK;
        });
    web->add_WindowCloseRequested(close, &t);
    close->Release();
    return;
  }
  auto completed =
      handler<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2 *,
              ICoreWebView2NavigationCompletedEventArgs *>(
          [state](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) {
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            if (success)
              state->emit({AuthBrowser::Kind::ready, {}});
            return S_OK;
          });
  web->add_NavigationCompleted(completed, &t);
  completed->Release();
}
// A private-mode controller in the sign-in environment. Popups share the
// opener's private profile, which put_NewWindow requires.
HRESULT create_controller(ICoreWebView2Environment10 *env, HWND window,
                          ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *cb) {
  ICoreWebView2ControllerOptions *options = nullptr;
  auto result = env->CreateCoreWebView2ControllerOptions(&options);
  if (SUCCEEDED(result))
    result = options->put_IsInPrivateModeEnabled(TRUE);
  if (SUCCEEDED(result))
    result = env->CreateCoreWebView2ControllerWithOptions(window, options, cb);
  if (options)
    options->Release();
  return result;
}
void open_popup(const std::shared_ptr<BrowserState> &state,
                ICoreWebView2NewWindowRequestedEventArgs *args) {
  ICoreWebView2Deferral *deferral = nullptr;
  if (state->done || !state->env || FAILED(args->GetDeferral(&deferral))) {
    args->put_Handled(TRUE);
    return;
  }
  auto owned = std::make_unique<Popup>();
  auto *popup = owned.get();
  popup->window = CreateWindowExW(0, L"VentureMetaAuthPopup", L"Sign in to Meta - Venture",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 560, 760,
                                  state->window, nullptr, GetModuleHandleW(nullptr), popup);
  state->popups.push_back(std::move(owned));
  args->AddRef();
  // Without a new window the request is refused rather than opened by WebView2
  // in an unfiltered default window.
  auto finish = [args, deferral](ICoreWebView2 *web) {
    if (web)
      args->put_NewWindow(web);
    else
      args->put_Handled(TRUE);
    deferral->Complete();
    deferral->Release();
    args->Release();
  };
  if (!popup->window) {
    finish(nullptr);
    return;
  }
  auto cb = handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT,
                    ICoreWebView2Controller *>(
      [state, popup, finish](HRESULT result, ICoreWebView2Controller *controller) {
        if (state->done || !popup->window || FAILED(result) || !controller ||
            FAILED(controller->get_CoreWebView2(&popup->web))) {
          finish(nullptr);
          if (popup->window)
            PostMessageW(popup->window, WM_CLOSE, 0, 0);
          return S_OK;
        }
        popup->controller = controller;
        controller->AddRef();
        attach(state, popup->web, popup);
        RECT r{};
        GetClientRect(popup->window, &r);
        controller->put_Bounds(r);
        controller->put_IsVisible(TRUE);
        finish(popup->web);
        ShowWindow(popup->window, SW_SHOW);
        SetForegroundWindow(popup->window);
        return S_OK;
      });
  if (FAILED(create_controller(state->env, popup->window, cb))) {
    finish(nullptr);
    PostMessageW(popup->window, WM_CLOSE, 0, 0);
  }
  cb->Release();
}
} // namespace
struct AuthBrowser::Impl {
  std::mutex mutex;
  std::deque<Event> events;
  std::thread thread;
  std::atomic<HWND> window{nullptr};
  std::atomic<bool> cancel{false};
  void emit(Event event) {
    std::lock_guard lock(mutex);
    events.push_back(std::move(event));
  }
};
AuthBrowser::AuthBrowser() : impl_(std::make_unique<Impl>()) {}
AuthBrowser::~AuthBrowser() { close(); }
void AuthBrowser::close() {
  impl_->cancel = true;
  if (auto hwnd = impl_->window.load())
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
  if (impl_->thread.joinable())
    impl_->thread.join();
}
std::optional<AuthBrowser::Event> AuthBrowser::poll() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->events.empty())
    return {};
  auto event = std::move(impl_->events.front());
  impl_->events.pop_front();
  return event;
}
void AuthBrowser::open(std::string url, const fs::path &data,
                       std::vector<meta::Cookie> cookies) {
  close();
  if (!meta::auth_navigation_allowed(url))
    throw std::runtime_error("Sign-in URL is outside allowed Meta domains.");
  {
    std::lock_guard lock(impl_->mutex);
    impl_->events.clear();
  }
  impl_->cancel = false;
  impl_->thread = std::thread([this, url = std::move(url), data,
                               cookies = std::move(cookies)] {
    auto emit = [this](Event e) { impl_->emit(std::move(e)); };
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      emit({Kind::error, "Could not initialize the Windows sign-in window."});
      return;
    }
    auto state = std::make_shared<BrowserState>();
    state->url = url;
    state->cookies = cookies;
    state->emit = emit;
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VentureMetaAuth";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    WNDCLASSW popup_class = wc;
    popup_class.lpfnWndProc = popup_proc;
    popup_class.lpszClassName = L"VentureMetaAuthPopup";
    RegisterClassW(&popup_class);
    state->window =
        CreateWindowExW(0, wc.lpszClassName, L"Sign in to Meta - Venture",
                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 650,
                        820, nullptr, nullptr, wc.hInstance, state.get());
    impl_->window = state->window;
    if (!state->window) {
      emit({Kind::error, "Could not create the sign-in window."});
      CoUninitialize();
      return;
    }
    if (impl_->cancel) {
      PostMessageW(state->window, WM_CLOSE, 0, 0);
    } else {
      ShowWindow(state->window, SW_SHOW);
      SetForegroundWindow(state->window);
      // Dynamic loading avoids the MSVC-only static loader and preserves
      // -static.
      auto dll = paths::executable().parent_path() / "WebView2Loader.dll";
      HMODULE loader = LoadLibraryExW(dll.c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                          LOAD_LIBRARY_SEARCH_SYSTEM32);
      using Create = HRESULT(STDAPICALLTYPE *)(
          PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
          ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
      auto create =
          loader ? reinterpret_cast<Create>(GetProcAddress(
                       loader, "CreateCoreWebView2EnvironmentWithOptions"))
                 : nullptr;
      if (!create)
        state->fail();
      else {
        auto cb = handler<
            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT,
            ICoreWebView2Environment *>([state](HRESULT hr,
                                                ICoreWebView2Environment *env) {
          if (state->done)
            return S_OK;
          if (FAILED(hr) || !env) {
            state->fail();
            return S_OK;
          }
          auto controller_cb = handler<
              ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
              HRESULT, ICoreWebView2Controller *>(
              [state](HRESULT result, ICoreWebView2Controller *controller) {
                if (state->done)
                  return S_OK;
                if (FAILED(result) || !controller) {
                  state->fail();
                  return S_OK;
                }
                state->controller = controller;
                controller->AddRef();
                if (FAILED(controller->get_CoreWebView2(&state->web))) {
                  state->fail();
                  return S_OK;
                }
                attach(state, state->web, nullptr);
                if (!state->cookies.empty())
                  seed_cookies(state->web, state->cookies);
                RECT r{};
                GetClientRect(state->window, &r);
                controller->put_Bounds(r);
                controller->put_IsVisible(TRUE);
                if (FAILED(state->web->Navigate(
                        paths::from_utf8(state->url).c_str())))
                  state->fail();
                return S_OK;
              });
          // WebView2 SDK 1.0.3537.50: Environment10/controller options isolate
          // cookies for each sign-in. Fail closed if private mode is
          // unavailable.
          auto result =
              env->QueryInterface(__uuidof(ICoreWebView2Environment10),
                                  reinterpret_cast<void **>(&state->env));
          if (SUCCEEDED(result))
            result = create_controller(state->env, state->window, controller_cb);
          controller_cb->Release();
          if (FAILED(result))
            state->fail();
          return S_OK;
        });
        auto result = create(nullptr, (data / "auth-private-webview").c_str(),
                             nullptr, cb);
        cb->Release();
        if (FAILED(result))
          state->fail();
      }
      // Keep loader resident while any asynchronous COM completion may be
      // queued. Windows reclaims this module when the launcher exits.
    }
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    state->shutdown();
    impl_->window = nullptr;
    emit({Kind::closed, {}});
    CoUninitialize();
  });
}
} // namespace venture::launcher
