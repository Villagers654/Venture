// SPDX-License-Identifier: GPL-3.0-or-later
// libovrplatformloader.so for games Venture installs on Android headsets: in
// place of Meta's loader, which finds no Meta platform service on these
// devices. Its exports are native entry image slots (generated exports.S);
// each call runs the Platform HLE in place on the calling thread
// (NativePlatform). Credentials come from the Venture app over Binder:
// Venture's PlatformInit provider (merged into the game's dex and manifest)
// loads this library at application start and hands it the context and the
// PlatformBroker class.
#include <jni.h>

#include <filesystem>
#include <stdexcept>

#include "android_game/game_log.h"
#include "android_game/platform/microphone.h"
#include "android_game/platform/native_platform.h"
#include "common/json.h"
#include "common/log.h"
#include "common/platform/android/jni.h"
#include "cpu/native_entry.h"
#include "hle/ovrplatform/provider.h"

using namespace venture;

namespace {
const char* const kNames[] = {
#include "export_names.inc"
};

jclass g_broker = nullptr;
jmethodID g_call = nullptr;
std::filesystem::path g_cloud, g_obb, g_files;

Json broker_call(const Json& request) {
  JNIEnv* env = android::env();
  if (!env || !g_broker) throw std::runtime_error("Venture's PlatformInit did not run in this game");
  android::LocalFrame frame(env);
  auto reply = static_cast<jstring>(
      env->CallStaticObjectMethod(g_broker, g_call, android::java_string(env, request.dump())));
  if (auto error = android::take_exception(env); !error.empty()) throw std::runtime_error(error);
  if (!reply) throw std::runtime_error("the Venture app did not answer");
  return parse_json(android::utf8(env, reply));
}

ovrplatform::NativePlatformConfig configure() {
  ovrplatform::NativePlatformConfig config;
  if (g_broker) config.broker = broker_call;
  else log::warn("ovrplatform", "Venture's PlatformInit did not run in this game: no Meta account is available");
  config.http = ovrplatform::meta_http_transport();
  config.cloud_dir = g_cloud;
  config.obb_dir = g_obb;
  config.extra_exports = ovrplatform::native_microphone_exports();
  if (!g_files.empty()) config.language_state = g_files / "venture" / "language.json";
  return config;
}

ovrplatform::NativePlatform& platform() {
  static ovrplatform::NativePlatform instance(kNames, configure);
  return instance;
}

std::uint32_t dispatch(NativeContext& context) { return platform().dispatch(context); }

__attribute__((constructor)) void install_dispatcher() { set_native_dispatcher(dispatch); }
}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) { return JNI_VERSION_1_6; }

// PlatformInit.nativeAttach (src/android_app/game/org/venture_emu/game/PlatformInit.java).
extern "C" JNIEXPORT void JNICALL Java_org_venture_1emu_game_PlatformInit_nativeAttach(
    JNIEnv* env, jclass, jobject context, jstring files, jstring cache, jstring external, jstring obb, jint sdk,
    jclass broker) {
  android::attach(env, context, {android::utf8(env, files), android::utf8(env, cache), {}, sdk});
  if (!g_broker) {
    g_broker = static_cast<jclass>(env->NewGlobalRef(broker));
    g_call = env->GetStaticMethodID(g_broker, "call", "(Ljava/lang/String;)Ljava/lang/String;");
    if (!g_call) {
      android::take_exception(env);
      env->DeleteGlobalRef(g_broker);
      g_broker = nullptr;
    }
  }
  if (external) g_cloud = std::filesystem::path(android::utf8(env, external)) / "cloud" / "data";
  if (obb) g_obb = android::utf8(env, obb);
  g_files = android::utf8(env, files);
  android_game::start_log(g_obb, "platform");
  log::info("ovrplatform", "Venture's Platform loader is attached ({} exports)", std::size(kNames) - 1);
}
