#pragma once

#include "core/guest.h"

struct JNIEnv_;
namespace venture::android {
struct NativeWindow;
}

namespace venture::java {
// ANativeWindow_fromSurface adapter: surface is a host JNI jobject, not an ART
// object address. Returns one acquired reference; libandroid's caller releases
// it with android::release. Null, invalid or released Surface -> nullptr.
// JNI failures retain their exception. Uses the Surface monitor so release
// and acquire cannot race.
android::NativeWindow* acquire_surface_window(JNIEnv_* env, void* surface);
}  // namespace venture::java
