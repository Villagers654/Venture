// SPDX-License-Identifier: GPL-3.0-or-later
// A minimal Quest-style game for tools/android/headset_play_test.py. It is a
// NativeActivity whose APK ships stand-ins for Meta's libvrapi.so and
// libovrplatformloader.so. Venture replaces them with its own libraries when
// it adapts the game. At start the game loads both and calls into them as a
// Quest game does: ovr_PlatformInitializeAndroid, then vrapi_Initialize. It
// logs each step to logcat under VentureTestGame and finishes.
#include <android/log.h>
#include <android/native_activity.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdint.h>

#define TAG "VentureTestGame"
#define say(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

#if defined(__aarch64__)
#define GAME_ABI "arm64-v8a"
#elif defined(__arm__)
#define GAME_ABI "armeabi-v7a"
#else
#define GAME_ABI "other"
#endif

// VrApi_Types.h.
typedef struct {
  JavaVM* vm;
  JNIEnv* env;
  jobject activity;
} Java;
typedef struct {
  int32_t type, product, major, minor, patch, graphics_api;
  Java java;
} InitParms;

typedef int32_t (*PlatformInit)(const char*, jobject, JNIEnv*);
typedef int32_t (*VrapiInit)(const InitParms*);
typedef const char* (*VrapiVersion)(void);

static void run(ANativeActivity* activity) {
  JNIEnv* env = activity->env;
  say("start %s", GAME_ABI);

  void* platform = dlopen("libovrplatformloader.so", RTLD_NOW);
  if (!platform) {
    say("platform: not loaded: %s", dlerror());
  } else {
    PlatformInit init = (PlatformInit)dlsym(platform, "ovr_PlatformInitializeAndroid");
    say("platform: loaded, ovr_PlatformInitializeAndroid %s", init ? "found" : "missing");
    if (init) say("platform: ovr_PlatformInitializeAndroid returned %d", init("1234567890", activity->clazz, env));
  }

  void* vrapi = dlopen("libvrapi.so", RTLD_NOW);
  if (!vrapi) {
    say("vrapi: not loaded: %s", dlerror());
  } else {
    VrapiVersion version = (VrapiVersion)dlsym(vrapi, "vrapi_GetVersionString");
    VrapiInit init = (VrapiInit)dlsym(vrapi, "vrapi_Initialize");
    say("vrapi: loaded, version %s", version ? version() : "(none)");
    if (init) {
      // VRAPI_STRUCTURE_TYPE_INIT_PARMS, VrApi 1.1, OpenGL ES 3.
      InitParms parms = {1, 1, 1, 1, 0, 0x10300, {activity->vm, env, activity->clazz}};
      say("vrapi: vrapi_Initialize returned %d", init(&parms));
    }
  }
  say("done %s", GAME_ABI);
}

static void on_window(ANativeActivity* activity, ANativeWindow* window) {
  (void)window;
  (void)activity;
}

JNIEXPORT void ANativeActivity_onCreate(ANativeActivity* activity, void* saved, size_t size) {
  (void)saved;
  (void)size;
  activity->callbacks->onNativeWindowCreated = on_window;
  run(activity);
  ANativeActivity_finish(activity);
}
