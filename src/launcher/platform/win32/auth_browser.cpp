#include "launcher/auth_browser.h"
#include "common/meta/auth.h"
#include "webview_compat.h"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
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
struct BrowserState {
  HWND window = nullptr;
  ICoreWebView2Controller *controller = nullptr;
  ICoreWebView2 *web = nullptr;
  std::string url;
  std::function<void(AuthBrowser::Event)> emit;
  bool done = false, callback_sent = false;
  void fail() {
    emit({AuthBrowser::Kind::error,
          "Could not open Meta sign-in. Install or repair the Microsoft Edge "
          "WebView2 Runtime."});
    if (window)
      PostMessageW(window, WM_CLOSE, 0, 0);
  }
  void navigate(std::string_view uri, bool popup = false) {
    if (meta::is_callback(uri)) {
      if (!callback_sent) {
        callback_sent = true;
        emit({AuthBrowser::Kind::callback, std::string(uri)});
      }
      if (window)
        PostMessageW(window, WM_CLOSE, 0, 0);
    } else if (popup && meta::auth_navigation_allowed(uri) && web)
      web->Navigate(paths::from_utf8(uri).c_str());
  }
  void shutdown() {
    done = true;
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
void AuthBrowser::open(std::string url, const fs::path &data) {
  close();
  if (!meta::auth_navigation_allowed(url))
    throw std::runtime_error("Sign-in URL is outside allowed Meta domains.");
  {
    std::lock_guard lock(impl_->mutex);
    impl_->events.clear();
  }
  impl_->cancel = false;
  impl_->thread = std::thread([this, url = std::move(url), data] {
    auto emit = [this](Event e) { impl_->emit(std::move(e)); };
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      emit({Kind::error, "Could not initialize the Windows sign-in window."});
      return;
    }
    auto state = std::make_shared<BrowserState>();
    state->url = url;
    state->emit = emit;
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VentureMetaAuth";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
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
                ICoreWebView2Settings *settings = nullptr;
                state->web->get_Settings(&settings);
                if (settings) {
                  settings->put_AreDevToolsEnabled(FALSE);
                  settings->put_AreDefaultContextMenusEnabled(FALSE);
                  settings->put_IsStatusBarEnabled(FALSE);
                  settings->put_IsWebMessageEnabled(FALSE);
                  settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                  settings->Release();
                }
                EventRegistrationToken t{};
                auto nav = handler<ICoreWebView2NavigationStartingEventHandler,
                                   ICoreWebView2 *,
                                   ICoreWebView2NavigationStartingEventArgs *>(
                    [state](ICoreWebView2 *,
                            ICoreWebView2NavigationStartingEventArgs *args) {
                      LPWSTR raw = nullptr;
                      args->get_Uri(&raw);
                      auto uri = utf8(raw);
                      CoTaskMemFree(raw);
                      if (!meta::auth_navigation_allowed(uri))
                        args->put_Cancel(TRUE);
                      state->navigate(uri);
                      return S_OK;
                    });
                state->web->add_NavigationStarting(nav, &t);
                nav->Release();
                auto frame =
                    handler<ICoreWebView2NavigationStartingEventHandler,
                            ICoreWebView2 *,
                            ICoreWebView2NavigationStartingEventArgs *>(
                        [](ICoreWebView2 *,
                           ICoreWebView2NavigationStartingEventArgs *args) {
                          LPWSTR raw = nullptr;
                          args->get_Uri(&raw);
                          auto uri = utf8(raw);
                          CoTaskMemFree(raw);
                          if (!meta::auth_navigation_allowed(uri))
                            args->put_Cancel(TRUE);
                          return S_OK;
                        });
                state->web->add_FrameNavigationStarting(frame, &t);
                frame->Release();
                auto popup =
                    handler<ICoreWebView2NewWindowRequestedEventHandler,
                            ICoreWebView2 *,
                            ICoreWebView2NewWindowRequestedEventArgs *>(
                        [state](
                            ICoreWebView2 *,
                            ICoreWebView2NewWindowRequestedEventArgs *args) {
                          args->put_Handled(TRUE);
                          LPWSTR raw = nullptr;
                          args->get_Uri(&raw);
                          auto uri = utf8(raw);
                          CoTaskMemFree(raw);
                          state->navigate(uri, true);
                          return S_OK;
                        });
                state->web->add_NewWindowRequested(popup, &t);
                popup->Release();
                auto permissions =
                    handler<ICoreWebView2PermissionRequestedEventHandler,
                            ICoreWebView2 *,
                            ICoreWebView2PermissionRequestedEventArgs *>(
                        [](ICoreWebView2 *,
                           ICoreWebView2PermissionRequestedEventArgs *args) {
                          args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                          return S_OK;
                        });
                state->web->add_PermissionRequested(permissions, &t);
                permissions->Release();
                auto completed =
                    handler<ICoreWebView2NavigationCompletedEventHandler,
                            ICoreWebView2 *,
                            ICoreWebView2NavigationCompletedEventArgs *>(
                        [state](
                            ICoreWebView2 *,
                            ICoreWebView2NavigationCompletedEventArgs *args) {
                          BOOL success = FALSE;
                          args->get_IsSuccess(&success);
                          if (success)
                            state->emit({Kind::ready, {}});
                          return S_OK;
                        });
                state->web->add_NavigationCompleted(completed, &t);
                completed->Release();
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
          ICoreWebView2Environment10 *private_env = nullptr;
          ICoreWebView2ControllerOptions *options = nullptr;
          auto result =
              env->QueryInterface(__uuidof(ICoreWebView2Environment10),
                                  reinterpret_cast<void **>(&private_env));
          if (SUCCEEDED(result))
            result = private_env->CreateCoreWebView2ControllerOptions(&options);
          if (SUCCEEDED(result))
            result = options->put_IsInPrivateModeEnabled(TRUE);
          if (SUCCEEDED(result))
            result = private_env->CreateCoreWebView2ControllerWithOptions(
                state->window, options, controller_cb);
          if (options)
            options->Release();
          if (private_env)
            private_env->Release();
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
