#include "android/errors.h"
#include "android/native_window.h"
#include "android/framework_bridge.h"
#include "android/handles32.h"
#include "core/address_space.h"
#include "core/process.h"
#include "common/log.h"
#include <unordered_map>
#include <limits>
#include <chrono>
#include <cstring>

namespace venture::android {
namespace {
std::mutex registry_lock;
std::unordered_map<GuestAddr, std::shared_ptr<NativeWindow>> registry;
std::atomic<CpuPresenter> presenter{nullptr};
std::mutex bridge_lock;
SurfaceBridge bridge;
int pixel_size(int format) { return format == 1 || format == 2 ? 4 : format == 4 ? 2 : 0; }
int publish_locked(NativeWindow& window, std::span<const std::uint8_t> rgba,
                   std::int32_t width, std::int32_t height, bool bottom_up, std::int64_t timestamp_ns = std::numeric_limits<std::int64_t>::min()) {
  if (!window.image_consumer) return 0;
  auto size = std::uint64_t(width) * height * 4;
  if (width <= 0 || height <= 0 || size > INT32_MAX || rgba.size() != size) return -err::invalid;
  try {
    auto frame = std::make_shared<SurfaceFrame>();
    frame->width = width; frame->height = height;frame->timestamp_ns=timestamp_ns==std::numeric_limits<std::int64_t>::min()?
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count():timestamp_ns;
    frame->serial = window.frame ? window.frame->serial + 1 : 1;
    frame->rgba.resize(size);
    for (int y = 0; y < height; ++y)
      std::memcpy(frame->rgba.data() + std::size_t(y) * width * 4,
          rgba.data() + std::size_t(bottom_up ? height - 1 - y : y) * width * 4,
          std::size_t(width) * 4);
    window.frame = std::move(frame);
    return 0;
  } catch (const std::bad_alloc&) { return -err::no_memory; }
}
}  // namespace
NativeWindow::~NativeWindow() {
  if (cpu_buffer) AddressSpace::get().free(cpu_buffer);
}
NativeWindow* create_native_window(std::int32_t width, std::int32_t height) {
  if (width <= 0 || height <= 0) return nullptr;
  auto window = std::make_shared<NativeWindow>();
  window->width = window->default_width = width;
  window->height = window->default_height = height;
  auto* p = window.get();
  handles32::publish(window, "ANativeWindow");
  std::lock_guard lock(registry_lock);
  registry.emplace(h2g(p), std::move(window));
  return p;
}
std::shared_ptr<NativeWindow> pin_native_window(GuestAddr address) {
  // EGL/Vulkan receive the same opaque ANativeWindow as libandroid.
  if (address && address <= UINT32_MAX && process_config().isa() == GuestIsa::a32) {
    try {
      return std::static_pointer_cast<NativeWindow>(
          handles32::resolve(static_cast<std::uint32_t>(address), "ANativeWindow"));
    } catch (const std::invalid_argument&) {
      return {};
    }
  }
  std::lock_guard lock(registry_lock);
  auto it = registry.find(address);
  if (it == registry.end() || it->second->magic != 0x5157494e) return {};
  return it->second;
}
NativeWindow* native_window_from_guest(GuestAddr address) { return pin_native_window(address).get(); }
void acquire(NativeWindow* window) {
  std::lock_guard lock(registry_lock);
  auto it = registry.find(h2g(window));
  if (it == registry.end()) {
    log::warn("window", "acquire: invalid native window");
    return;
  }
  if (it->second->refs == INT32_MAX) {
    log::error("window", "native window reference overflow");
    return;
  }
  ++it->second->refs;
}
void release(NativeWindow* window) {
  std::shared_ptr<NativeWindow> dying;
  {
    std::lock_guard lock(registry_lock);
    auto it = registry.find(h2g(window));
    if (it == registry.end()) {
      log::warn("window", "release: invalid native window");
      return;
    }
    if (--it->second->refs == 0) {
      dying = std::move(it->second);
      registry.erase(it);
    }
  }
  if (dying) handles32::retire(h2g(window));
}
int set_window_geometry(NativeWindow* ptr, std::int32_t w, std::int32_t h, std::int32_t format) {
  auto win = pin_native_window(h2g(ptr));
  if (!win) return -err::invalid;
  if (w < 0 || h < 0 || (!w != !h) || (format && !pixel_size(format))) return -err::invalid;
  std::lock_guard lock(win->lock);
  if (win->cpu_locked) return -err::busy;
  win->cpu_posted = false;
  win->width = w ? w : win->default_width;
  win->height = h ? h : win->default_height;
  win->format = format ? format : 1;
  return 0;
}
int lock_window(NativeWindow* ptr, WindowBuffer* out, Rect* dirty) {
  auto win = pin_native_window(h2g(ptr));
  if (!win || !out) return -err::invalid;
  std::lock_guard lock(win->lock);
  if (win->cpu_locked) return -err::busy;
  int bpp = pixel_size(win->format);
  if (!bpp) return -err::invalid;
  auto size = std::uint64_t(win->width) * win->height * bpp;
  if (size > INT32_MAX) return -err::no_memory;
  if (size != win->cpu_buffer_size) {
    auto next = AddressSpace::get().allocate(size, kProtRead | kProtWrite, "[native-window]");
    if (!next) return -err::no_memory;
    if (win->cpu_buffer) AddressSpace::get().free(win->cpu_buffer);
    win->cpu_buffer = next;
    win->cpu_buffer_size = size;
  }
  *out = {win->width, win->height, win->width, win->format, win->cpu_buffer, {}};
  if (dirty) *dirty = {0, 0, win->width, win->height};
  win->cpu_locked = true;
  return 0;
}
int unlock_window(NativeWindow* ptr) {
  auto win = pin_native_window(h2g(ptr));
  if (!win) return -err::invalid;
  std::unique_lock lock(win->lock);
  if (!win->cpu_locked) return -err::invalid;
  if (win->image_consumer) {
    auto* source = g2h<const std::uint8_t>(win->cpu_buffer);
    int result;
    if (win->format == 1) {
      result = publish_locked(*win, {source, std::size_t(win->width) * win->height * 4},
                              win->width, win->height, false);
    } else {
      try {
        std::vector<std::uint8_t> rgba(std::size_t(win->width) * win->height * 4);
        for (std::size_t i = 0; i < rgba.size() / 4; ++i) {
          if (win->format == 4) {
            std::uint16_t rgb;
            std::memcpy(&rgb, source + i * 2, 2);
            rgba[i*4] = ((rgb >> 11) & 31) * 255 / 31;
            rgba[i*4+1] = ((rgb >> 5) & 63) * 255 / 63;
            rgba[i*4+2] = (rgb & 31) * 255 / 31;
          } else { std::memcpy(rgba.data() + i*4, source + i*4, 3); }
          rgba[i*4+3] = 255;
        }
        result = publish_locked(*win, rgba, win->width, win->height, false);
      } catch (const std::bad_alloc&) { result = -err::no_memory; }
    }
    if (result) return result;
  }
  if (!win->image_consumer)
    if (auto fn = presenter.load())
      fn(*win, {win->width, win->height, win->width, win->format, win->cpu_buffer, {}});
  win->cpu_locked = false;
  win->cpu_posted = true;
  ++win->cpu_frame;
  auto notify=win->frame_available;lock.unlock();if(notify)notify();
  return 0;
}
void set_cpu_presenter(CpuPresenter p) { presenter = p; }
bool has_window_image_consumer(NativeWindow* window) {
  std::lock_guard lock(window->lock);
  return window->image_consumer;
}
int publish_window_rgba(NativeWindow* window, std::span<const std::uint8_t> rgba,
                        std::int32_t width, std::int32_t height, bool bottom_up, std::int64_t timestamp_ns) {
  std::function<void()> notify;
  int result;
  {
    std::lock_guard lock(window->lock);
    result=publish_locked(*window, rgba, width, height, bottom_up,timestamp_ns);
    if(!result && window->frame) {
      notify=window->frame_available;
    }
  }
  if(notify)notify();
  return result;
}
void set_surface_bridge(SurfaceBridge b) {
  std::lock_guard lock(bridge_lock);
  bridge = b;
}
SurfaceBridge surface_bridge() {
  std::lock_guard lock(bridge_lock);
  return bridge;
}
}  // namespace venture::android
