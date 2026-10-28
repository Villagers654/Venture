#include "android/errors.h"
#include "android/native_activity.h"
#include "core/os_quit.h"
#include "android/native_window.h"
#include "android/input.h"
#include "android/framework_bridge.h"
#include "android/handles32.h"
#include "core/address_space.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/thread.h"
#include "core/thread_runtime.h"
#include "core/a32_types.h"
#include "java/internal.h"
#include "java/jni32.h"
#include "common/log.h"
#include <cstring>
#include <map>
#include <thread>

namespace venture::android {
namespace {
struct WindowState {
  std::mutex mutex;
  NativeWindow* window = nullptr;
  int format = 1;
};
std::mutex activities_mutex;
std::map<GuestAddr, std::weak_ptr<WindowState>> activities;
}  // namespace
struct NativeActivity::Impl {
  GuestAddr memory = 0;
  GuestAddr callbacks = 0;
  bool a32 = process_config().isa() == GuestIsa::a32;
  GuestAddr callback(std::size_t index) const {
    return a32 ? g2h<std::uint32_t>(callbacks)[index] : g2h<GuestAddr>(callbacks)[index];
  }
  std::thread::id owner = std::this_thread::get_id();
  std::shared_ptr<WindowState> state = std::make_shared<WindowState>();
  InputQueue* queue = nullptr;
  bool destroying = false;
  bool on_thread() const {
    if (owner == std::this_thread::get_id()) return true;
    log::error("activity", "NativeActivity lifecycle called off its creating UI thread");
    return false;
  }
  std::uint64_t call(GuestAddr callback, std::initializer_list<std::uint64_t> args) {
    if (!callback || !on_thread()) return 0;
    auto& thread = GuestThread::adopt_current();
    if (a32) {
      // NativeActivity callbacks enter native code from the Java lifecycle.
      // Invalidate JNI local tokens when that transition's host frame ends.
      java::LocalFrame frame(java::host_env());
      if (!frame.valid()) throw std::bad_alloc();
      java::jni32::PinScope pins;
      java::jni32::NativeScope tokens;
      // native_activity.h callbacks carry only pointer/int/size_t words.
      hle::a32::CallBuilder32 words;
      for (auto value : args) words.pointer(value);
      return thread.call32(callback, words).x0;
    } else
      return thread.call(callback, args);
  }
  void release_storage() {
    if (!memory) return;
    {
      std::lock_guard lock(activities_mutex);
      activities.erase(memory);
    }
    {
      std::lock_guard lock(state->mutex);
      if (state->window) release(state->window);
      state->window = nullptr;
    }
    queue = nullptr;
    AddressSpace::get().free(memory);
    memory = 0;
  }
};
NativeActivity::NativeActivity() : impl_(std::make_unique<Impl>()) {}
NativeActivity::~NativeActivity() {
  try { destroy(); }
  catch (const ThreadExit&) { log::debug("activity", "onDestroy interrupted by guest thread termination"); }
  catch (const ProcessExit&) { log::debug("activity", "onDestroy interrupted by guest process termination"); }
  catch (const std::exception& error) { log::error("activity", "onDestroy failed: {}", error.what()); }
  catch (...) { log::error("activity", "onDestroy interrupted by guest termination"); }
  impl_->release_storage();
}
std::unique_ptr<NativeActivity> NativeActivity::create(Library& library, GuestAddr vm, GuestAddr env,
                                                       GuestAddr clazz, std::string* error,
                                                       std::string_view name,
                                                       std::span<const std::byte> saved) {
  auto fail = [&](const char* message) -> std::unique_ptr<NativeActivity> {
    if (error) *error = message;
    log::error("activity", "{}", message);
    return nullptr;
  };
  auto entry = library.find_symbol(name);
  if (!entry) return fail("NativeActivity entrypoint is missing");
  if (!vm || !env || !clazz)
    return fail("NativeActivity requires guest JNI tables and an Activity global reference");
  auto activity = std::unique_ptr<NativeActivity>(new NativeActivity());
  auto& p = *activity->impl_;
  auto& config = process_config();
  // Android supplies usable files directories before entering the native callback,
  // even if the Activity never called Context.getFilesDir/getExternalFilesDir.
  for (const auto& directory : {config.data_dir / "files", config.data_dir / "external-data" / "files"}) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return fail("NativeActivity files directory could not be created");
  }
  auto internal = config.guest_data_dir() + "/files", external = config.guest_external_files_dir(),
       obb = config.guest_obb_dir();
  auto activity_size = p.a32 ? sizeof(GuestNativeActivity32) : sizeof(GuestNativeActivity);
  auto callback_size = p.a32 ? sizeof(GuestActivityCallbacks32) : sizeof(GuestActivityCallbacks);
  auto size =
      activity_size + callback_size + internal.size() + external.size() + obb.size() + 3 + saved.size();
  p.memory = AddressSpace::get().allocate(size, kProtRead | kProtWrite, "[native-activity]");
  if (!p.memory) return fail("NativeActivity memory allocation failed");
  std::memset(g2h(p.memory), 0, size);
  auto cb = p.memory + activity_size;
  p.callbacks = cb;
  auto cursor = cb + callback_size;
  auto copy_string = [&](const std::string& s) {
    auto addr = cursor;
    std::memcpy(g2h(cursor), s.c_str(), s.size() + 1);
    cursor += s.size() + 1;
    return addr;
  };
  GuestNativeActivity a{cb,
                        vm,
                        env,
                        clazz,
                        copy_string(internal),
                        copy_string(external),
                        config.sdk_level,
                        0,
                        0,
                        package_asset_manager(),
                        copy_string(obb)};
  if (p.a32) {
    auto narrow = [](GuestAddr v) { return h2g32(g2h(v)); };
    *g2h<GuestNativeActivity32>(p.memory) = {narrow(a.callbacks),
                                             narrow(a.vm),
                                             narrow(a.env),
                                             narrow(a.clazz),
                                             narrow(a.internalDataPath),
                                             narrow(a.externalDataPath),
                                             a.sdkVersion,
                                             0,
                                             narrow(a.assetManager),
                                             narrow(a.obbPath)};
  } else
    *g2h<GuestNativeActivity>(p.memory) = a;
  if (!saved.empty()) std::memcpy(g2h(cursor), saved.data(), saved.size());
  {
    std::lock_guard lock(activities_mutex);
    activities.emplace(p.memory, p.state);
  }
  // r2-java startup evidence: UE4 inherits NativeActivity and relies on its
  // native_app_glue callbacks. Enter ANativeActivity_onCreate, never android_main.
  p.call(entry, {p.memory, saved.empty() ? 0 : cursor, saved.size()});
  log::info("activity", "{} returned", name);
  return activity;
}
GuestAddr NativeActivity::address() const { return impl_->memory; }
void NativeActivity::event(ActivityEvent e) {
  auto& p = *impl_;
  if (!p.memory || !p.on_thread()) return;
  GuestAddr cb = 0;
  switch (e) {
    case ActivityEvent::start:
      cb = p.callback(offsetof(GuestActivityCallbacks, onStart) / 8);
      break;
    case ActivityEvent::resume:
      cb = p.callback(offsetof(GuestActivityCallbacks, onResume) / 8);
      break;
    case ActivityEvent::pause:
      cb = p.callback(offsetof(GuestActivityCallbacks, onPause) / 8);
      break;
    case ActivityEvent::stop:
      cb = p.callback(offsetof(GuestActivityCallbacks, onStop) / 8);
      break;
    case ActivityEvent::configuration_changed:
      cb = p.callback(offsetof(GuestActivityCallbacks, onConfigurationChanged) / 8);
      break;
    case ActivityEvent::low_memory:
      cb = p.callback(offsetof(GuestActivityCallbacks, onLowMemory) / 8);
      break;
  }
  p.call(cb, {p.memory});
}
void NativeActivity::focus(bool focused) {
  auto& p = *impl_;
  if (p.memory)
    p.call(p.callback(offsetof(GuestActivityCallbacks, onWindowFocusChanged) / 8),
           {p.memory, focused ? 1u : 0u});
}
void NativeActivity::window(NativeWindow* next) {
  auto& p = *impl_;
  if (!p.memory || !p.on_thread()) return;
  NativeWindow* old;
  {
    std::lock_guard lock(p.state->mutex);
    old = p.state->window;
    if (old == next) return;
  }
  if (next && !pin_native_window(h2g(next))) return;
  if (next) acquire(next);
  if (old)
    p.call(p.callback(offsetof(GuestActivityCallbacks, onNativeWindowDestroyed) / 8),
           {p.memory, handles32::encode(h2g(old), "ANativeWindow")});
  {
    std::lock_guard lock(p.state->mutex);
    p.state->window = next;
    if (next) set_window_geometry(next, 0, 0, p.state->format);
  }
  if (old) release(old);
  if (next)
    p.call(p.callback(offsetof(GuestActivityCallbacks, onNativeWindowCreated) / 8),
           {p.memory, handles32::encode(h2g(next), "ANativeWindow")});
}
void NativeActivity::window_resized() {
  auto& p = *impl_;
  if (p.memory && p.state->window)
    p.call(p.callback(offsetof(GuestActivityCallbacks, onNativeWindowResized) / 8),
           {p.memory, handles32::encode(h2g(p.state->window), "ANativeWindow")});
}
void NativeActivity::redraw() {
  auto& p = *impl_;
  if (p.memory && p.state->window)
    p.call(p.callback(offsetof(GuestActivityCallbacks, onNativeWindowRedrawNeeded) / 8),
           {p.memory, handles32::encode(h2g(p.state->window), "ANativeWindow")});
}
void NativeActivity::input_queue(InputQueue* queue) {
  auto& p = *impl_;
  if (!p.memory || !p.on_thread() || p.queue == queue) return;
  if (p.queue)
    p.call(p.callback(offsetof(GuestActivityCallbacks, onInputQueueDestroyed) / 8),
           {p.memory, handles32::encode(h2g(p.queue), "AInputQueue")});
  p.queue = queue;
  if (queue)
    p.call(p.callback(offsetof(GuestActivityCallbacks, onInputQueueCreated) / 8),
           {p.memory, handles32::encode(h2g(queue), "AInputQueue")});
}
void NativeActivity::content_rect(std::int32_t left, std::int32_t top, std::int32_t right,
                                  std::int32_t bottom) {
  auto& p = *impl_;
  if (!p.memory || !p.callback(offsetof(GuestActivityCallbacks, onContentRectChanged) / 8) || !p.on_thread())
    return;
  auto rect = AddressSpace::get().allocate(sizeof(Rect), kProtRead | kProtWrite, "[activity-rect]");
  if (!rect) return;
  *g2h<Rect>(rect) = {left, top, right, bottom};
  p.call(p.callback(offsetof(GuestActivityCallbacks, onContentRectChanged) / 8), {p.memory, rect});
  AddressSpace::get().free(rect);
}
void NativeActivity::destroy() {
  auto& p = *impl_;
  if (!p.memory || p.destroying || !p.on_thread()) return;
  p.destroying = true;
  struct Cleanup { Impl& state; ~Cleanup() { state.release_storage(); } } cleanup{p};
  input_queue(nullptr);
  window(nullptr);
  os_quit::native_destroy_delivered();
  p.call(p.callback(offsetof(GuestActivityCallbacks, onDestroy) / 8), {p.memory});
}
std::vector<std::byte> NativeActivity::save_state(GuestAddr guest_free) {
  auto& p = *impl_;
  if (!p.memory || !p.on_thread() || !p.callback(offsetof(GuestActivityCallbacks, onSaveInstanceState) / 8))
    return {};
  if (!guest_free) {
    log::error("activity", "save_state requires the guest libc free thunk");
    return {};
  }
  auto size_address = AddressSpace::get().allocate(8, kProtRead | kProtWrite, "[activity-saved-size]");
  if (!size_address) return {};
  *g2h<std::uint64_t>(size_address) = 0;
  auto& thread = GuestThread::adopt_current();
  auto data = p.call(p.callback(offsetof(GuestActivityCallbacks, onSaveInstanceState) / 8),
                     {p.memory, size_address});
  auto size = p.a32 ? std::uint64_t(*g2h<std::uint32_t>(size_address)) : *g2h<std::uint64_t>(size_address);
  AddressSpace::get().free(size_address);
  std::vector<std::byte> result;
  if (data) {
    try {
      result.resize(size);
      if (size) std::memcpy(result.data(), g2h(data), size);
    } catch (...) {
      thread.call(guest_free, {data});
      throw;
    }
    thread.call(guest_free, {data});
  }
  return result;
}
void native_activity_set_window_format(GuestAddr address, std::int32_t format) {
  std::shared_ptr<WindowState> state;
  {
    std::lock_guard lock(activities_mutex);
    auto it = activities.find(address);
    if (it != activities.end()) state = it->second.lock();
  }
  if (!state) return;
  // Android PixelFormat.OPAQUE/TRANSLUCENT are negative aliases.
  if (format == -1) format = 2;
  if (format == -2 || format == -3) format = 1;
  if (format != 1 && format != 2 && format != 4) {
    log::warn("activity", "Unsupported NativeActivity window format {}", format);
    return;
  }
  std::lock_guard lock(state->mutex);
  state->format = format;
  if (state->window) set_window_geometry(state->window, 0, 0, format);
}
}  // namespace venture::android
