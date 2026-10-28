#pragma once

#include <cstdint>
#include <string>

#include "core/guest.h"

struct JNIEnv_;

namespace venture::java {
// Installed by libandroid before Java app launch. No ANativeActivity layout is
// duplicated here: libandroid owns it, its global activity reference,
// callbacks, guest VM/env, assets and paths. create runs
// ANativeActivity_onCreate on the calling Android main thread and returns its
// guest activity address. Null hooks produce UnsupportedOperationException,
// never a successful stub.
enum class NativeActivityEvent : std::int32_t {
  start = 1,
  resume,
  pause,
  stop,
  destroy,
  focus_gained,
  focus_lost,
  low_memory,
  surface_created,
  surface_changed,
  surface_destroyed,
  surface_redraw,
};
struct NativeActivityBridge {
  GuestAddr (*create)(JNIEnv_* env, void* activity, const std::string& library,
                      const std::string& entry, std::string* error) = nullptr;
  // surface holds a borrowed NativeWindow guest pointer valid for the callback.
  // On destroy the implementation releases its owned state/global JNI refs.
  bool (*event)(GuestAddr activity, NativeActivityEvent event,
                GuestAddr surface, std::string* error) = nullptr;
};
void set_native_activity_bridge(NativeActivityBridge bridge);
}  // namespace venture::java
