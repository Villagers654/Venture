#pragma once
#include "core/guest.h"

// Compatible with JNI's forward declaration, without requiring host jni.h.
class _jobject;
namespace venture::android {
struct NativeWindow;
// One process-wide opaque AAssetManager for the package. The framework may call
// this directly from its AssetManager native. A null Java object returns 0.
GuestAddr asset_manager_from_java(_jobject* object);
GuestAddr package_asset_manager();

// Install once before running game code. env is the opaque GUEST JNIEnv*;
// implementations must obtain the host JNIEnv from java::host_env(), never
// dereference env as a host JNI table. from_surface returns an ACQUIRED window;
// to_surface returns a host JNI local reference for the calling thread.
// The framework must resolve JNI refs using JNI (never compare raw jobjects).
struct SurfaceBridge {
  NativeWindow* (*from_surface)(void* env, _jobject* surface) = nullptr;
  _jobject* (*to_surface)(void* env, NativeWindow* window) = nullptr;
};
void set_surface_bridge(SurfaceBridge bridge);
SurfaceBridge surface_bridge();
}  // namespace venture::android
