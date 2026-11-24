#include "xr/panel_window.h"
#include "common/first_frame_progress.h"
#include "common/log.h"
#include "common/startup_progress.h"
#include "java/jvm.h"
#include <SDL3/SDL.h>
#include <jni.h>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace venture::xr::panel {

std::int32_t android_keycode(std::uint32_t k) {
  if (k >= SDLK_A && k <= SDLK_Z) return 29 + std::int32_t(k - SDLK_A);
  if (k >= SDLK_0 && k <= SDLK_9) return 7 + std::int32_t(k - SDLK_0);
  if (k >= SDLK_F1 && k <= SDLK_F12) return 131 + std::int32_t(k - SDLK_F1);
  switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 66;
    case SDLK_BACKSPACE: return 67;
    case SDLK_DELETE: return 112;
    case SDLK_TAB: return 61;
    case SDLK_SPACE: return 62;
    case SDLK_ESCAPE: return 4;  // Back, as Android maps a PC keyboard's Escape
    case SDLK_UP: return 19;
    case SDLK_DOWN: return 20;
    case SDLK_LEFT: return 21;
    case SDLK_RIGHT: return 22;
    case SDLK_HOME: return 122;
    case SDLK_END: return 123;
    case SDLK_PAGEUP: return 92;
    case SDLK_PAGEDOWN: return 93;
    case SDLK_INSERT: return 124;
    case SDLK_LSHIFT: return 59;
    case SDLK_RSHIFT: return 60;
    case SDLK_LCTRL: return 113;
    case SDLK_RCTRL: return 114;
    case SDLK_LALT: return 57;
    case SDLK_RALT: return 58;
    case SDLK_COMMA: return 55;
    case SDLK_PERIOD: return 56;
    case SDLK_GRAVE: return 68;
    case SDLK_MINUS: return 69;
    case SDLK_EQUALS: return 70;
    case SDLK_LEFTBRACKET: return 71;
    case SDLK_RIGHTBRACKET: return 72;
    case SDLK_BACKSLASH: return 73;
    case SDLK_SEMICOLON: return 74;
    case SDLK_APOSTROPHE: return 75;
    case SDLK_SLASH: return 76;
    default: return 0;
  }
}

std::int32_t android_meta(std::uint16_t mod) {
  std::int32_t meta = 0;
  if (mod & SDL_KMOD_SHIFT) meta |= 0x1 | ((mod & SDL_KMOD_LSHIFT) ? 0x40 : 0x80);
  if (mod & SDL_KMOD_ALT) meta |= 0x2 | ((mod & SDL_KMOD_LALT) ? 0x10 : 0x20);
  if (mod & SDL_KMOD_CTRL) meta |= 0x1000 | ((mod & SDL_KMOD_LCTRL) ? 0x2000 : 0x4000);
  if (mod & SDL_KMOD_CAPS) meta |= 0x100000;
  if (mod & SDL_KMOD_NUM) meta |= 0x200000;
  return meta;
}

namespace {
std::atomic<bool> closing{false};
}
bool close_requested() { return closing.load(std::memory_order_relaxed); }
void request_close() {
  if (!closing.exchange(true)) log::info("panel", "The 2D app's window was closed; ending the app as a removed task");
}

namespace {
// Android's MotionEvent actions and sources, as Panel.pointer takes them.
constexpr std::int32_t kDown = 0, kUp = 1, kMove = 2, kHoverMove = 7, kHoverExit = 10;
constexpr std::int32_t kTouchscreen = 0x1002, kMouse = 0x2002;

using Event = std::array<std::int32_t, 6>;

// A document picker request (Java's DocumentPicker): the host's own file or
// folder dialog, shown by the window's thread, answered to the waiting caller.
enum class PickKind : std::int32_t { folder = 0, open = 1, save = 2 };
struct Pick {
  PickKind kind = PickKind::folder;
  std::string name;
  bool many = false, shown = false, done = false;
  std::vector<std::string> paths;  // empty when cancelled
};

struct Shared {
  std::mutex mutex;
  std::condition_variable events_ready, pick_done;
  std::shared_ptr<Pick> pick;
  std::deque<Event> events;
  std::vector<std::uint8_t> pixels;  // RGBA, premultiplied, row 0 at the top
  int width = 0, height = 0;
  std::uint64_t generation = 0;
  std::string title = "Venture";
  // closed: the person closed the window (the app is ending); no_window: it
  // could not open, and the panel goes on in the headset only.
  bool started = false, closed = false, no_window = false;
  Uint32 wake = 0;
};
Shared& shared() {
  static Shared s;
  return s;
}

std::int32_t bits(float value) { return std::bit_cast<std::int32_t>(value); }

void push(Shared& s, Event event) {
  {
    std::lock_guard lock(s.mutex);
    s.events.push_back(event);
  }
  s.events_ready.notify_all();
}

class Window {
 public:
  void run();

 private:
  bool open();
  void draw(bool fresh);
  void handle(const SDL_Event& event);
  void show_pick();

  Shared& s_ = shared();
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  int width_ = 0, height_ = 0;
  std::uint64_t drawn_ = 0;
  bool pressed_ = false, hovering_ = false;
};

bool Window::open() {
  // As the desktop mirror: the runtime keeps its signals, timer resolution and
  // focus; closing the window is the app's to handle.
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "0");
  SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
#if !defined(_WIN32) && !defined(__ANDROID__)
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
#endif
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    log::warn("panel", "No desktop window for the 2D app: {}", SDL_GetError());
    return false;
  }
  std::string title;
  int width, height;
  {
    std::lock_guard lock(s_.mutex);
    title = s_.title;
    width = s_.width;
    height = s_.height;
  }
  // 80% of the panel's size, at most: a 1600x1000 panel opens at 1280x800.
  window_ = SDL_CreateWindow(title.c_str(), width * 4 / 5, height * 4 / 5, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window_) {
    log::warn("panel", "No desktop window for the 2D app: {}", SDL_GetError());
    return false;
  }
  renderer_ = SDL_CreateRenderer(window_, nullptr);
  if (!renderer_) renderer_ = SDL_CreateRenderer(window_, SDL_SOFTWARE_RENDERER);
  if (!renderer_) {
    log::warn("panel", "No renderer for the 2D app's window: {}", SDL_GetError());
    return false;
  }
  log::info("panel", "2D app window opened ({} renderer)", SDL_GetRendererName(renderer_));
  s_.wake = SDL_RegisterEvents(1);
  SDL_StartTextInput(window_);
  return true;
}

void Window::draw(bool fresh) {
  if (fresh) {
    std::lock_guard lock(s_.mutex);
    if (s_.generation == drawn_ && texture_) {
      fresh = false;
    } else {
      if (!texture_ || width_ != s_.width || height_ != s_.height) {
        if (texture_) SDL_DestroyTexture(texture_);
        width_ = s_.width;
        height_ = s_.height;
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width_, height_);
        if (!texture_) return;
        SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
        SDL_SetRenderLogicalPresentation(renderer_, width_, height_, SDL_LOGICAL_PRESENTATION_LETTERBOX);
      }
      SDL_UpdateTexture(texture_, nullptr, s_.pixels.data(), width_ * 4);
      drawn_ = s_.generation;
    }
  }
  if (!texture_) return;
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
  SDL_RenderClear(renderer_);
  SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
  SDL_RenderPresent(renderer_);
}

void Window::handle(const SDL_Event& original) {
  SDL_Event event = original;
  SDL_ConvertEventToRenderCoordinates(renderer_, &event);
  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (event.button.button != SDL_BUTTON_LEFT || pressed_) break;
      if (hovering_) push(s_, {pointer, kHoverExit, bits(event.button.x), bits(event.button.y), 0, kMouse});
      hovering_ = false;
      pressed_ = true;
      SDL_CaptureMouse(true);
      push(s_, {pointer, kDown, bits(event.button.x), bits(event.button.y), 0, kTouchscreen});
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (event.button.button != SDL_BUTTON_LEFT || !pressed_) break;
      pressed_ = false;
      SDL_CaptureMouse(false);
      push(s_, {pointer, kUp, bits(event.button.x), bits(event.button.y), 0, kTouchscreen});
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (pressed_) {
        push(s_, {pointer, kMove, bits(event.motion.x), bits(event.motion.y), 0, kTouchscreen});
      } else {
        hovering_ = true;
        push(s_, {pointer, kHoverMove, bits(event.motion.x), bits(event.motion.y), 0, kMouse});
      }
      break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      if (hovering_ && !pressed_) push(s_, {pointer, kHoverExit, 0, 0, 0, kMouse});
      hovering_ = false;
      break;
    case SDL_EVENT_MOUSE_WHEEL: {
      float x = event.wheel.mouse_x, y = event.wheel.mouse_y;
      push(s_, {scroll, bits(x), bits(y), bits(event.wheel.x), bits(event.wheel.y), 0});
      break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      const auto code = android_keycode(event.key.key);
      if (!code) break;
      // Printable keys type through SDL_EVENT_TEXT_INPUT (layouts, dead keys,
      // shift); as keys they go only with Ctrl or Alt held (Ctrl+C, Ctrl+V).
      const bool printable = event.key.key >= 0x20 && event.key.key < 0x7f;
      if (printable && !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) break;
      push(s_, {key, event.type == SDL_EVENT_KEY_DOWN ? 0 : 1, code, android_meta(event.key.mod),
                event.key.repeat ? 1 : 0, 0});
      break;
    }
    case SDL_EVENT_TEXT_INPUT: {
      // UTF-8 to code points; Java turns each into a character key event.
      const char* text = event.text.text;
      while (text && *text) {
        Uint32 codepoint = SDL_StepUTF8(&text, nullptr);
        if (codepoint == 0) break;
        if (codepoint != SDL_INVALID_UNICODE_CODEPOINT) push(s_, {panel::text, std::int32_t(codepoint), 0, 0, 0, 0});
      }
      break;
    }
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      draw(false);
      break;
    default:
      break;
  }
}

// SDL calls back once, on this thread or its dialog's: null on an error, an
// empty list when the person cancelled.
void SDLCALL picked(void* userdata, const char* const* files, int) {
  auto& s = shared();
  std::lock_guard lock(s.mutex);
  auto* pick = static_cast<Pick*>(userdata);
  if (files)
    for (; *files; ++files) pick->paths.emplace_back(*files);
  if (!files) log::warn("panel", "File dialog failed: {}", SDL_GetError());
  pick->done = true;
  s.pick_done.notify_all();
}

void Window::show_pick() {
  std::shared_ptr<Pick> pick;
  {
    std::lock_guard lock(s_.mutex);
    if (!s_.pick || s_.pick->shown) return;
    pick = s_.pick;
    pick->shown = true;
  }
  // The request stays alive in s_.pick until its caller has read the answer.
  const char* name = pick->name.empty() ? nullptr : pick->name.c_str();
  switch (pick->kind) {
    case PickKind::folder: SDL_ShowOpenFolderDialog(picked, pick.get(), window_, nullptr, false); break;
    case PickKind::open: SDL_ShowOpenFileDialog(picked, pick.get(), window_, nullptr, 0, nullptr, pick->many); break;
    case PickKind::save: SDL_ShowSaveFileDialog(picked, pick.get(), window_, nullptr, 0, name); break;
  }
}

void Window::run() {
  if (!open()) {
    std::lock_guard lock(s_.mutex);
    s_.no_window = true;
    s_.pick_done.notify_all();
    return;
  }
  draw(true);
  bool running = true;
  while (running) {
    SDL_Event event;
    if (!SDL_WaitEventTimeout(&event, 250)) continue;
    do {
      if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED || event.type == SDL_EVENT_QUIT) {
        request_close();
        running = false;
      } else if (event.type == s_.wake) {
        draw(true);
        show_pick();
      } else {
        handle(event);
      }
    } while (running && SDL_PollEvent(&event));
  }
  SDL_HideWindow(window_);
  std::lock_guard lock(s_.mutex);
  s_.closed = true;
  s_.pick_done.notify_all();
}

// ---- Java natives ----
void JNICALL java_present(JNIEnv* env, jclass, jint width, jint height, jobject buffer) {
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || !buffer) return;
  const auto size = std::size_t(width) * std::size_t(height) * 4;
  auto* data = static_cast<const std::uint8_t*>(env->GetDirectBufferAddress(buffer));
  if (!data || std::size_t(env->GetDirectBufferCapacity(buffer)) < size) return;
  auto& s = shared();
  bool start = false;
  Uint32 wake;
  {
    std::lock_guard lock(s.mutex);
    if (s.closed) return;
    s.pixels.assign(data, data + size);
    s.width = width;
    s.height = height;
    ++s.generation;
    start = !s.started;
    s.started = true;
    wake = s.wake;
  }
  // The panel's image is a 2D app's frame, as an XR frame is an immersive
  // game's: the first one ends its startup (no "No first frame" stall report,
  // the ordinary quit path). While an XR session has focus its frames alone
  // count, so a frozen XR loop is still seen as one.
  if (start) {
    log::info("panel", "2D app's first frame shown ({}x{})", width, height);
    startup_diagnostics::arm();
  }
  if (!startup_diagnostics::host_session_focused()) startup_diagnostics::note_frame_submitted();
  if (start) {
    std::thread([] {
      Window window;
      window.run();
    }).detach();
    start_headset();
  } else if (wake) {
    SDL_Event event{};
    event.type = wake;
    SDL_PushEvent(&event);
  }
}

void JNICALL java_title(JNIEnv* env, jclass, jstring title) {
  if (!title) return;
  const char* utf = env->GetStringUTFChars(title, nullptr);
  if (!utf) return;
  {
    std::lock_guard lock(shared().mutex);
    shared().title = utf;
  }
  env->ReleaseStringUTFChars(title, utf);
}

// Blocks up to a second for events; null when none came.
void JNICALL java_close(JNIEnv*, jclass) { request_close(); }

jintArray JNICALL java_events(JNIEnv* env, jclass) {
  auto& s = shared();
  std::vector<std::int32_t> values;
  {
    std::unique_lock lock(s.mutex);
    s.events_ready.wait_for(lock, std::chrono::seconds(1), [&] { return !s.events.empty(); });
    values.reserve(s.events.size() * 6);
    for (const auto& event : s.events) values.insert(values.end(), event.begin(), event.end());
    s.events.clear();
  }
  if (values.empty()) return nullptr;
  auto out = env->NewIntArray(jsize(values.size()));
  if (out) env->SetIntArrayRegion(out, 0, jsize(values.size()), values.data());
  return out;
}

// The host's file or folder dialog, from a Java worker thread: blocks until the
// person answers. Null when there is no window to show it or they cancelled.
jobjectArray JNICALL java_pick(JNIEnv* env, jclass, jint kind, jstring name, jboolean many) {
  if (kind < 0 || kind > 2) return nullptr;
  auto& s = shared();
  auto pick = std::make_shared<Pick>();
  pick->kind = PickKind(kind);
  pick->many = many;
  if (name) {
    if (const char* utf = env->GetStringUTFChars(name, nullptr)) {
      pick->name = utf;
      env->ReleaseStringUTFChars(name, utf);
    }
  }
  Uint32 wake;
  {
    std::lock_guard lock(s.mutex);
    if (!s.started || s.closed || s.no_window || !s.wake || s.pick) return nullptr;
    s.pick = pick;
    wake = s.wake;
  }
  SDL_Event event{};
  event.type = wake;
  SDL_PushEvent(&event);
  std::vector<std::string> paths;
  {
    std::unique_lock lock(s.mutex);
    // A shown dialog always calls back, which `pick` outlives; one the window
    // closed before showing never will.
    s.pick_done.wait(lock, [&] { return pick->done || ((s.closed || s.no_window) && !pick->shown); });
    paths = pick->paths;
    s.pick.reset();
  }
  if (paths.empty()) return nullptr;
  auto string_class = env->FindClass("java/lang/String");
  auto out = env->NewObjectArray(jsize(paths.size()), string_class, nullptr);
  for (jsize i = 0; out && i < jsize(paths.size()); ++i) {
    auto value = env->NewStringUTF(paths[std::size_t(i)].c_str());
    env->SetObjectArrayElement(out, i, value);
    env->DeleteLocalRef(value);
  }
  env->DeleteLocalRef(string_class);
  return out;
}

jboolean JNICALL java_in_headset(JNIEnv*, jclass) { return in_headset() ? JNI_TRUE : JNI_FALSE; }

void register_natives(JNIEnv* env) {
  auto cls = env->FindClass("com/venture/runtime/layoutlib/PanelHost");
  if (!cls) {  // the classic framework: no 2D panel
    env->ExceptionClear();
    return;
  }
  JNINativeMethod methods[] = {
      {const_cast<char*>("nativePresent"), const_cast<char*>("(IILjava/nio/ByteBuffer;)V"), reinterpret_cast<void*>(java_present)},
      {const_cast<char*>("nativeTitle"), const_cast<char*>("(Ljava/lang/String;)V"), reinterpret_cast<void*>(java_title)},
      {const_cast<char*>("nativeEvents"), const_cast<char*>("()[I"), reinterpret_cast<void*>(java_events)},
      {const_cast<char*>("nativeClose"), const_cast<char*>("()V"), reinterpret_cast<void*>(java_close)},
      {const_cast<char*>("nativeInHeadset"), const_cast<char*>("()Z"), reinterpret_cast<void*>(java_in_headset)},
      {const_cast<char*>("nativePick"), const_cast<char*>("(ILjava/lang/String;Z)[Ljava/lang/String;"), reinterpret_cast<void*>(java_pick)},
  };
  if (env->RegisterNatives(cls, methods, jint(std::size(methods))) != JNI_OK) {
    env->ExceptionClear();
    log::warn("panel", "2D panel natives were not registered");
  }
  env->DeleteLocalRef(cls);
}
} // namespace

std::uint64_t copy_frame(std::uint64_t seen, std::vector<std::uint8_t>& out, int& width, int& height) {
  auto& s = shared();
  std::lock_guard lock(s.mutex);
  if (!s.started || s.generation == seen) return 0;
  out = s.pixels;
  width = s.width;
  height = s.height;
  return s.generation;
}

std::array<int, 2> frame_size() {
  auto& s = shared();
  std::lock_guard lock(s.mutex);
  return {s.width, s.height};
}

void post_events(const std::vector<std::array<std::int32_t, 6>>& events) {
  if (events.empty()) return;
  auto& s = shared();
  {
    std::lock_guard lock(s.mutex);
    // Nobody reads a closed panel's input; never grow without bound.
    if (s.events.size() < 4096) s.events.insert(s.events.end(), events.begin(), events.end());
  }
  s.events_ready.notify_all();
}

void install_java_bridge() {
  static std::once_flag once;
  std::call_once(once, [] {
    try {
      java::add_framework_natives(register_natives);
    } catch (const std::exception& e) {
      log::warn("panel", "2D panel window unavailable: {}", e.what());
    }
  });
}
} // namespace venture::xr::panel
