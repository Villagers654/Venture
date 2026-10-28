#include "java/natives/horizon_bridge.h"
#include "java/jvm.h"
#include "hle/ovrplatform/horizon.h"
#include "common/log.h"
#include <jni.h>

namespace venture::java {
namespace {
std::unique_ptr<ovrplatform::Horizon> horizon;
constexpr auto core = "horizonos/supplement/hzplatformclientcore/HorizonPlatformCore";
template<class F> auto guarded(JNIEnv* env, F action) -> decltype(action()) {
  try { return action(); }
  catch (...) {
    log::warn("horizon", "JNI bridge failed");
    if (!env->ExceptionCheck()) {
      auto cls = env->FindClass("java/lang/IllegalStateException");
      if (cls) { env->ThrowNew(cls, "Horizon Platform bridge failed"); env->DeleteLocalRef(cls); }
    }
    return {};
  }
}
std::string ascii(JNIEnv* env, jstring value) {
  if (!value || env->GetStringLength(value) > 256) throw std::invalid_argument("Invalid Horizon name");
  auto chars = env->GetStringUTFChars(value, nullptr);
  if (!chars) throw std::bad_alloc();
  std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}
jint JNICALL setup(JNIEnv* env, jclass, jstring app) {
  return guarded(env, [&] { return static_cast<jint>(horizon->setup(ascii(env, app))); });
}
jlong JNICALL request(JNIEnv* env, jclass, jstring module, jstring method, jint version,
                      jbyteArray bytes, jint cookie, jboolean subscription) {
  return guarded(env, [&]() -> jlong {
    if (!bytes || env->GetArrayLength(bytes) > 1024 * 1024) throw std::invalid_argument("Invalid Horizon data");
    std::string data(static_cast<std::size_t>(env->GetArrayLength(bytes)), '\0');
    env->GetByteArrayRegion(bytes, 0, static_cast<jsize>(data.size()), reinterpret_cast<jbyte*>(data.data()));
    if (env->ExceptionCheck()) return 0;
    return static_cast<jlong>(horizon->request(ascii(env, module), ascii(env, method), version,
                                             data, cookie, subscription));
  });
}
jobject JNICALL pop(JNIEnv* env, jclass, jlong session) {
  return guarded(env, [&]() -> jobject {
    auto message = horizon->pop(static_cast<std::uint64_t>(session));
    if (!message) return nullptr;
    auto cls = env->FindClass("horizonos/supplement/hzplatformclientcore/HorizonPlatformCore$Message");
    auto ctor = cls ? env->GetMethodID(cls, "<init>", "(JIILjava/lang/String;Ljava/lang/String;)V") : nullptr;
    auto status = ctor ? env->NewStringUTF(message->status_message.c_str()) : nullptr;
    // The facade emits ASCII JSON (including escaped supplementary characters).
    auto data = status ? env->NewStringUTF(message->data.c_str()) : nullptr;
    auto out = data ? env->NewObject(cls, ctor, static_cast<jlong>(message->request_id),
                                  static_cast<jint>(message->status_code), static_cast<jint>(message->cookie),
                                  status, data) : nullptr;
    if (data) env->DeleteLocalRef(data);
    if (status) env->DeleteLocalRef(status);
    if (cls) env->DeleteLocalRef(cls);
    return out;
  });
}
jint JNICALL count(JNIEnv* env, jclass, jlong session) {
  return guarded(env, [&] { return static_cast<jint>(horizon->count(static_cast<std::uint64_t>(session))); });
}
void JNICALL stop(JNIEnv* env, jclass, jlong session) {
  guarded(env, [&] { horizon->stop(static_cast<std::uint64_t>(session)); return 0; });
}
void register_natives(JNIEnv* env) {
  auto cls = env->FindClass(core);
  if (!cls) {  // runs without venture-framework.jar (guest tests, --boot-native) have no Horizon core
    env->ExceptionClear();
    return;
  }
  JNINativeMethod methods[] = {
    {const_cast<char*>("nativeSetup"), const_cast<char*>("(Ljava/lang/String;)I"), reinterpret_cast<void*>(setup)},
    {const_cast<char*>("nativeRequest"), const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;I[BIZ)J"), reinterpret_cast<void*>(request)},
    {const_cast<char*>("nativePop"), const_cast<char*>("(J)Ljava/lang/Object;"), reinterpret_cast<void*>(pop)},
    {const_cast<char*>("nativeCount"), const_cast<char*>("(J)I"), reinterpret_cast<void*>(count)},
    {const_cast<char*>("nativeStop"), const_cast<char*>("(J)V"), reinterpret_cast<void*>(stop)},
  };
  env->RegisterNatives(cls, methods, static_cast<jint>(std::size(methods)));
  env->DeleteLocalRef(cls);
}
}  // namespace
void install_horizon_bridge(ovrplatform::Session& session) {
  horizon = std::make_unique<ovrplatform::Horizon>(session);
  add_framework_natives(register_natives);
}
}  // namespace venture::java
