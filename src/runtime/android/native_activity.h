// Framework bridge for android.app.NativeActivity (not AGDK GameActivity).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/guest.h"

namespace venture {
class Library;
}
namespace venture::android {
struct NativeWindow;
struct InputQueue;

// AOSP frameworks/native/include/android/native_activity.h, AArch64 LP64.
// clazz is an Activity instance global reference, despite its historical name.
struct GuestNativeActivity {
  GuestAddr callbacks, vm, env, clazz, internalDataPath, externalDataPath;
  std::int32_t sdkVersion;
  std::uint32_t padding;
  GuestAddr instance, assetManager, obbPath;
};
struct GuestActivityCallbacks {
  GuestAddr onStart, onResume, onSaveInstanceState, onPause, onStop, onDestroy;
  GuestAddr onWindowFocusChanged, onNativeWindowCreated, onNativeWindowResized;
  GuestAddr onNativeWindowRedrawNeeded, onNativeWindowDestroyed;
  GuestAddr onInputQueueCreated, onInputQueueDestroyed, onContentRectChanged;
  GuestAddr onConfigurationChanged, onLowMemory;
};
static_assert(sizeof(GuestNativeActivity) == 80);
static_assert(offsetof(GuestNativeActivity, callbacks) == 0);
static_assert(offsetof(GuestNativeActivity, vm) == 8);
static_assert(offsetof(GuestNativeActivity, env) == 16);
static_assert(offsetof(GuestNativeActivity, clazz) == 24);
static_assert(offsetof(GuestNativeActivity, internalDataPath) == 32);
static_assert(offsetof(GuestNativeActivity, externalDataPath) == 40);
static_assert(offsetof(GuestNativeActivity, sdkVersion) == 48);
static_assert(offsetof(GuestNativeActivity, instance) == 56);
static_assert(offsetof(GuestNativeActivity, assetManager) == 64);
static_assert(offsetof(GuestNativeActivity, obbPath) == 72);
static_assert(sizeof(GuestActivityCallbacks) == 128);
static_assert(offsetof(GuestActivityCallbacks, onSaveInstanceState) == 16);
static_assert(offsetof(GuestActivityCallbacks, onWindowFocusChanged) == 48);
static_assert(offsetof(GuestActivityCallbacks, onNativeWindowCreated) == 56);
static_assert(offsetof(GuestActivityCallbacks, onInputQueueCreated) == 88);
static_assert(offsetof(GuestActivityCallbacks, onLowMemory) == 120);

// Same AOSP native_activity.h fields, ARM ILP32 (NDK clang layout probe).
struct GuestNativeActivity32 {
  std::uint32_t callbacks, vm, env, clazz, internalDataPath, externalDataPath;
  std::int32_t sdkVersion;
  std::uint32_t instance, assetManager, obbPath;
};
struct GuestActivityCallbacks32 {
  std::uint32_t slots[16];
};
static_assert(sizeof(GuestNativeActivity32) == 40 && offsetof(GuestNativeActivity32, instance) == 28);
static_assert(sizeof(GuestActivityCallbacks32) == 64);

enum class ActivityEvent { start, resume, pause, stop, configuration_changed, low_memory };
class NativeActivity {
 public:
  // Framework owns the global clazz ref until after destroy(). Calls below
  // must all run on the creating Java/UI thread. The bridge adopts that thread.
  // Resolve manifest android.app.lib_name before calling; pass func_name metadata
  // or leave the default. VM/env are guest JNI tables supplied by java/jvm.h.
  static std::unique_ptr<NativeActivity> create(Library& library, GuestAddr vm, GuestAddr env,
                                                GuestAddr clazz, std::string* error,
                                                std::string_view entry = "ANativeActivity_onCreate",
                                                std::span<const std::byte> saved_state = {});
  ~NativeActivity();
  GuestAddr address() const;
  void event(ActivityEvent event);
  void focus(bool focused);
  void window(NativeWindow* window);  // destroys old callback before creating new
  void window_resized();
  void redraw();
  void input_queue(InputQueue* queue);
  void content_rect(std::int32_t left, std::int32_t top, std::int32_t right, std::int32_t bottom);
  // Framework resolves guest libc free and passes its thunk. The NDK callback
  // returns malloc-owned memory; copy it, then free through that same allocator.
  std::vector<std::byte> save_state(GuestAddr guest_free);
  void destroy();  // onDestroy, then release guest memory; idempotent
 private:
  NativeActivity();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
void native_activity_set_window_format(GuestAddr activity, std::int32_t format);
}  // namespace venture::android
