// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/platform/android/http_transfer.h"

#include "common/platform/android/jni.h"

#include <vector>

namespace venture::http::android {
namespace jni = venture::android;
namespace {
struct Methods {
  jmethodID open, read, close;
  jfieldID status, headers, failure, message;
};
const Methods& methods(JNIEnv* env) {
  static const Methods m = [env] {
    const auto& c = jni::classes();
    if (!c.http || !c.transfer) throw std::runtime_error("The Venture app's network bridge is missing.");
    Methods out{};
    out.open = env->GetStaticMethodID(
        c.http, "open",
        "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;[BII)Lorg/venture_emu/net/Http$Transfer;");
    out.read = env->GetStaticMethodID(c.http, "read", "(Lorg/venture_emu/net/Http$Transfer;[B)I");
    out.close = env->GetStaticMethodID(c.http, "close", "(Lorg/venture_emu/net/Http$Transfer;)V");
    out.status = env->GetFieldID(c.transfer, "status", "I");
    out.headers = env->GetFieldID(c.transfer, "headers", "[Ljava/lang/String;");
    out.failure = env->GetFieldID(c.transfer, "failure", "I");
    out.message = env->GetFieldID(c.transfer, "message", "Ljava/lang/String;");
    if (!out.open || !out.read || !out.close || !out.status || !out.headers || !out.failure || !out.message) {
      env->ExceptionClear();
      throw std::runtime_error("The Venture app's network bridge does not match this build.");
    }
    return out;
  }();
  return m;
}
Failure failure_of(int kind) {
  switch (kind) {
    case 0: return Failure::none;
    case 1: return Failure::timeout;
    case 2: return Failure::tls;
    default: return Failure::network;
  }
}
}  // namespace

Result perform(const Request& request, std::stop_token stop, Callback header, void* header_data, Callback write,
               void* write_data) {
  Result result;
  if (stop.stop_requested()) {
    result.failure = Failure::stopped;
    return result;
  }
  JNIEnv* env = jni::env();
  const auto& m = methods(env);
  jni::LocalFrame frame(env, 32);
  const auto& c = jni::classes();

  jclass string_class = env->FindClass("java/lang/String");
  auto headers = env->NewObjectArray(jsize(request.headers.size() * 2), string_class, nullptr);
  for (std::size_t i = 0; i < request.headers.size(); ++i) {
    env->SetObjectArrayElement(headers, jsize(2 * i), jni::java_string(env, request.headers[i].first));
    env->SetObjectArrayElement(headers, jsize(2 * i + 1), jni::java_string(env, request.headers[i].second));
  }
  jbyteArray body = nullptr;
  if (request.body)
    body = jni::java_bytes(env, {reinterpret_cast<const unsigned char*>(request.body->data()), request.body->size()});
  jobject transfer = env->CallStaticObjectMethod(
      c.http, m.open, jni::java_string(env, request.url), jni::java_string(env, request.method), headers, body,
      jint(request.connect_seconds * 1000), jint(request.read_seconds * 1000));
  if (auto error = jni::take_exception(env); !error.empty() || !transfer) {
    result.failure = Failure::network;
    result.message = error.empty() ? "no transfer" : error;
    return result;
  }
  struct Close {
    JNIEnv* env;
    jclass http;
    jmethodID close;
    jobject transfer;
    ~Close() {
      env->CallStaticVoidMethod(http, close, transfer);
      env->ExceptionClear();
    }
  } closer{env, c.http, m.close, transfer};

  auto fail_from_java = [&] {
    result.failure = failure_of(env->GetIntField(transfer, m.failure));
    result.message = jni::utf8(env, static_cast<jstring>(env->GetObjectField(transfer, m.message)));
  };
  if (env->GetIntField(transfer, m.failure) != 0) {
    fail_from_java();
    return result;
  }
  result.status = env->GetIntField(transfer, m.status);
  std::string line = "HTTP/1.1 " + std::to_string(result.status) + " \r\n";
  header(line.data(), 1, line.size(), header_data);
  auto pairs = static_cast<jobjectArray>(env->GetObjectField(transfer, m.headers));
  const jsize count = pairs ? env->GetArrayLength(pairs) : 0;
  for (jsize i = 0; i + 1 < count; i += 2) {
    auto name = static_cast<jstring>(env->GetObjectArrayElement(pairs, i));
    auto value = static_cast<jstring>(env->GetObjectArrayElement(pairs, i + 1));
    line = jni::utf8(env, name) + ": " + jni::utf8(env, value) + "\r\n";
    header(line.data(), 1, line.size(), header_data);
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(value);
  }
  line = "\r\n";
  header(line.data(), 1, line.size(), header_data);

  constexpr jsize chunk = 64 * 1024;
  jbyteArray buffer = env->NewByteArray(chunk);
  std::vector<char> bytes(chunk);
  for (;;) {
    if (stop.stop_requested()) {
      result.failure = Failure::stopped;
      return result;
    }
    const jint n = env->CallStaticIntMethod(c.http, m.read, transfer, buffer);
    if (auto error = jni::take_exception(env); !error.empty()) {
      result.failure = Failure::network;
      result.message = error;
      return result;
    }
    if (n == -1) break;
    if (n < 0) {
      fail_from_java();
      return result;
    }
    if (n == 0) continue;
    env->GetByteArrayRegion(buffer, 0, n, reinterpret_cast<jbyte*>(bytes.data()));
    if (write(bytes.data(), 1, std::size_t(n), write_data) != std::size_t(n)) {
      result.failure = Failure::write;
      return result;
    }
  }
  return result;
}
}  // namespace venture::http::android
