#include "platform/audio_endpoints.h"

#include <dlfcn.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>

#include "common/log.h"

namespace venture::platform {
namespace {
// The PulseAudio client protocol, which PipeWire also serves (pipewire-pulse):
// sink, source, card and server (default endpoint) events. libpulse is loaded
// at run time, as miniaudio loads it for playback and enumeration, so a host
// without it keeps per-call enumeration. Declarations follow <pulse/*.h>; the
// handles stay opaque.
struct pa_mainloop;
struct pa_mainloop_api;
struct pa_context;
struct pa_operation;
using StateCallback = void (*)(pa_context*, void*);
using SubscribeCallback = void (*)(pa_context*, int, std::uint32_t, void*);
using SuccessCallback = void (*)(pa_context*, int, void*);
constexpr int context_ready = 4, context_failed = 5, context_terminated = 6;
constexpr int noautospawn = 1;
constexpr int mask_sink = 0x0001, mask_source = 0x0002, mask_server = 0x0080, mask_card = 0x0200;

struct Pulse {
  pa_mainloop* (*mainloop_new)();
  pa_mainloop_api* (*mainloop_get_api)(pa_mainloop*);
  int (*mainloop_run)(pa_mainloop*, int*);
  void (*mainloop_quit)(pa_mainloop*, int);
  void (*mainloop_free)(pa_mainloop*);
  pa_context* (*context_new)(pa_mainloop_api*, const char*);
  void (*context_set_state_callback)(pa_context*, StateCallback, void*);
  int (*context_connect)(pa_context*, const char*, int, const void*);
  int (*context_get_state)(const pa_context*);
  void (*context_set_subscribe_callback)(pa_context*, SubscribeCallback, void*);
  pa_operation* (*context_subscribe)(pa_context*, int, SuccessCallback, void*);
  void (*context_disconnect)(pa_context*);
  void (*context_unref)(pa_context*);
  void (*operation_unref)(pa_operation*);
};
bool load(Pulse& p) {
  void* library = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
  if (!library) return false;
  bool ok = true;
  auto bind = [&](auto& function, const char* name) {
    function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
    ok = ok && function;
  };
  bind(p.mainloop_new, "pa_mainloop_new");
  bind(p.mainloop_get_api, "pa_mainloop_get_api");
  bind(p.mainloop_run, "pa_mainloop_run");
  bind(p.mainloop_quit, "pa_mainloop_quit");
  bind(p.mainloop_free, "pa_mainloop_free");
  bind(p.context_new, "pa_context_new");
  bind(p.context_set_state_callback, "pa_context_set_state_callback");
  bind(p.context_connect, "pa_context_connect");
  bind(p.context_get_state, "pa_context_get_state");
  bind(p.context_set_subscribe_callback, "pa_context_set_subscribe_callback");
  bind(p.context_subscribe, "pa_context_subscribe");
  bind(p.context_disconnect, "pa_context_disconnect");
  bind(p.context_unref, "pa_context_unref");
  bind(p.operation_unref, "pa_operation_unref");
  if (!ok) dlclose(library);  // kept loaded otherwise: the watcher lives until exit
  return ok;
}

// One connection at a time on a detached thread, alive until exit. A lost
// server reports a change (cached answers may be stale) and the thread
// reconnects; reconnecting reports another, as endpoints may have changed
// meanwhile. Until the first subscription is live, watch() waits; if it does
// not come up in time the watcher is abandoned and callers keep querying.
struct Watcher {
  Pulse pulse{};
  void (*changed)(void*) = nullptr;
  void* context = nullptr;
  std::mutex mutex;
  std::condition_variable cv;
  bool settled = false, live = false, abandoned = false;
  bool reconnecting = false;  // watcher thread only
  pa_mainloop* loop = nullptr;

  static void on_subscribed(pa_context*, int success, void* self) {
    auto* w = static_cast<Watcher*>(self);
    if (!success) return w->pulse.mainloop_quit(w->loop, 1);
    if (w->reconnecting) w->changed(w->context);
    std::lock_guard lock(w->mutex);
    if (w->abandoned) return w->pulse.mainloop_quit(w->loop, 1);
    if (!w->settled) {
      w->settled = w->live = true;
      w->cv.notify_all();
    }
  }
  static void on_event(pa_context*, int, std::uint32_t, void* self) {
    auto* w = static_cast<Watcher*>(self);
    w->changed(w->context);
  }
  static void on_state(pa_context* c, void* self) {
    auto* w = static_cast<Watcher*>(self);
    const int state = w->pulse.context_get_state(c);
    if (state == context_ready) {
      w->pulse.context_set_subscribe_callback(c, on_event, w);
      if (auto* op = w->pulse.context_subscribe(c, mask_sink | mask_source | mask_server | mask_card, on_subscribed, w))
        w->pulse.operation_unref(op);
      else w->pulse.mainloop_quit(w->loop, 1);
    } else if (state == context_failed || state == context_terminated) {
      w->pulse.mainloop_quit(w->loop, 1);
    }
  }
  // Runs one connection until it fails; false once watching ends.
  bool connect_once() {
    loop = pulse.mainloop_new();
    if (!loop) return false;
    auto* c = pulse.context_new(pulse.mainloop_get_api(loop), "Venture audio endpoints");
    bool keep = c != nullptr;
    if (c) {
      pulse.context_set_state_callback(c, on_state, this);
      if (pulse.context_connect(c, nullptr, noautospawn, nullptr) >= 0) {
        int result = 0;
        pulse.mainloop_run(loop, &result);
      }
      pulse.context_set_state_callback(c, nullptr, nullptr);
      pulse.context_disconnect(c);
      pulse.context_unref(c);
    }
    pulse.mainloop_free(loop);
    loop = nullptr;
    std::lock_guard lock(mutex);
    if (!settled) {
      settled = true;
      cv.notify_all();
    }
    return keep && live && !abandoned;
  }
  void run() {
    while (connect_once()) {
      changed(context);
      reconnecting = true;
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
};
}  // namespace

bool watch_audio_endpoints(void (*changed)(void*), void* context) {
  auto watcher = std::make_unique<Watcher>();
  if (!load(watcher->pulse)) return false;
  watcher->changed = changed;
  watcher->context = context;
  // Lives until exit, as notifications may; an abandoned watcher's thread
  // ends with its connection.
  auto* w = watcher.release();
  std::thread([w] { w->run(); }).detach();
  std::unique_lock lock(w->mutex);
  if (!w->cv.wait_for(lock, std::chrono::seconds(2), [w] { return w->settled; }) || !w->live) {
    w->abandoned = true;
    log::info("audio", "No PulseAudio-protocol endpoint events; default devices are queried per call");
    return false;
  }
  return true;
}
}  // namespace venture::platform
