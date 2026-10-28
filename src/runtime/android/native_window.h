// Android native windows (ANativeWindow) shared by the Java framework
// (android.view.Surface), libandroid HLE (ANativeWindow_*), EGL HLE
// (eglCreateWindowSurface) and Vulkan HLE (VK_KHR_android_surface).
//
// A64 ANativeWindow* is the address of a NativeWindow object owned by this
// registry; A32 uses its typed Token32 handle. The public NDK type is opaque;
// only Venture code does. Lifetime is reference counted
// (ANativeWindow_acquire/release, Surface objects, EGL surfaces).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <memory>
#include <span>
#include <vector>

#include "core/guest.h"

namespace venture::android {

struct SurfaceFrame {
  std::int32_t width, height;
  std::uint64_t serial;
  std::int64_t timestamp_ns = 0;
  std::vector<std::uint8_t> rgba;  // top-left origin, Android's default sRGB dataspace
};

struct NativeWindow {
  std::uint32_t magic = 0x5157494e;  // 'QWIN', validated on every guest-supplied pointer
  std::atomic<std::int32_t> refs{1};
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t format = 1;         // AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM / WINDOW_FORMAT_RGBA_8888
  std::uint64_t java_surface = 0;  // host JNI global ref of the android.view.Surface, if any
  // Host presentation target used when the guest presents to this window
  // (desktop mirror / flat apps). nullptr for windows only used as an EGL
  // target in VR (they render offscreen; frames reach the HMD through XR).
  void* host_window = nullptr;
  std::mutex lock;
  // CPU-only backing; not a GPU-importable Android gralloc allocation.
  std::int32_t default_width = 0, default_height = 0;
  GuestAddr cpu_buffer = 0;
  std::uint64_t cpu_buffer_size = 0;
  bool cpu_locked = false;
  bool cpu_posted = false;
  std::uint64_t cpu_frame = 0;  // protected by lock; advances on each posted buffer
  // One consumer and one published frame per ANativeWindow, shared by CPU,
  // EGL and Vulkan producers. GPU paths snapshot their completed buffer before
  // returning it to their producer's pool.
  bool image_consumer = false;
  // XR Surface swapchains have a fixed consumer extent. SurfaceTexture
  // consumers instead preserve each producer frame's source dimensions.
  bool fixed_consumer_extent = false;
  std::shared_ptr<const SurfaceFrame> frame;
  std::function<void()> frame_available;
  float requested_frame_rate = 0;
  ~NativeWindow();
};

NativeWindow* create_native_window(std::int32_t width, std::int32_t height);
// Validates a guest-supplied pointer; returns nullptr for anything else.
NativeWindow* native_window_from_guest(GuestAddr address);
void acquire(NativeWindow* window);
void release(NativeWindow* window);
// Pins storage for a host operation without changing the guest-visible refs.
// The raw contract accessor requires its caller to already own a reference.
std::shared_ptr<NativeWindow> pin_native_window(GuestAddr address);

// AOSP native_window.h, ANativeWindow_Buffer LP64: bits at offset 16.
struct WindowBuffer {
  std::int32_t width, height, stride, format;
  GuestAddr bits;
  std::uint32_t reserved[6];
};
static_assert(offsetof(WindowBuffer, bits) == 16);
static_assert(sizeof(WindowBuffer) == 48);
struct Rect {
  std::int32_t left, top, right, bottom;
};
int set_window_geometry(NativeWindow*, std::int32_t width, std::int32_t height, std::int32_t format);
int lock_window(NativeWindow*, WindowBuffer*, Rect* dirty);
int unlock_window(NativeWindow*);
bool has_window_image_consumer(NativeWindow*);
int publish_window_rgba(NativeWindow*, std::span<const std::uint8_t> rgba,
                        std::int32_t width, std::int32_t height, bool bottom_up = false,
                        std::int64_t timestamp_ns = std::numeric_limits<std::int64_t>::min());
// Mirror renderer may install this hook before guest startup. Without it CPU
// frames are discarded (offscreen VR target). Callback runs with window locked.
using CpuPresenter = void (*)(NativeWindow&, const WindowBuffer&);
void set_cpu_presenter(CpuPresenter presenter);

}  // namespace venture::android
