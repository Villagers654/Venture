// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/platform/android/jni.h"

#include <pthread.h>
#include <stdlib.h>

#include <atomic>
#include <stdexcept>

namespace venture::android {
namespace {
JavaVM* g_vm = nullptr;
jobject g_context = nullptr;
App g_app;
Classes g_classes;
std::atomic<bool> g_ready{false};
pthread_key_t g_detach_key;
pthread_once_t g_detach_once = PTHREAD_ONCE_INIT;

void detach_thread(void*) {
  if (g_vm) g_vm->DetachCurrentThread();
}
void make_detach_key() { pthread_key_create(&g_detach_key, detach_thread); }

jclass global_class(JNIEnv* env, const char* name) {
  jclass local = env->FindClass(name);
  if (!local) {
    env->ExceptionClear();
    return nullptr;
  }
  auto global = static_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  return global;
}

// UTF-8 to UTF-16, replacing malformed sequences with U+FFFD.
std::u16string utf16(std::string_view text) {
  std::u16string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    auto c = static_cast<unsigned char>(text[i]);
    char32_t cp = 0xFFFD;
    std::size_t length = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c >> 5) == 6 || (c >> 4) == 14 || (c >> 3) == 30) {
      length = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
      if (i + length <= text.size()) {
        char32_t value = c & (0x7F >> length);
        bool valid = true;
        for (std::size_t k = 1; k < length; ++k) {
          auto next = static_cast<unsigned char>(text[i + k]);
          if ((next >> 6) != 2) valid = false;
          value = value << 6 | (next & 0x3F);
        }
        if (valid && value <= 0x10FFFF && !(value >= 0xD800 && value <= 0xDFFF)) cp = value;
        else length = 1;
      } else {
        length = 1;
      }
    }
    i += length;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out += char16_t(0xD800 + (cp >> 10));
      out += char16_t(0xDC00 + (cp & 0x3FF));
    } else {
      out += char16_t(cp);
    }
  }
  return out;
}
}  // namespace

bool ready() { return g_ready.load(std::memory_order_acquire); }
const App& app() { return g_app; }
const Classes& classes() { return g_classes; }
JavaVM* vm() { return g_vm; }
jobject context() { return g_context; }

JNIEnv* env() {
  if (!g_vm) throw std::runtime_error("The Venture app's Java side is not available.");
  JNIEnv* env = nullptr;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
  if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env)
    throw std::runtime_error("Could not attach a thread to the Java VM.");
  pthread_once(&g_detach_once, make_detach_key);
  pthread_setspecific(g_detach_key, env);
  return env;
}

std::string utf8(JNIEnv* env, jstring text) {
  if (!text) return {};
  const jsize length = env->GetStringLength(text);
  std::u16string units(std::size_t(length), u'\0');
  env->GetStringRegion(text, 0, length, reinterpret_cast<jchar*>(units.data()));
  std::string out;
  out.reserve(units.size());
  for (std::size_t i = 0; i < units.size(); ++i) {
    char32_t cp = units[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF)
      cp = 0x10000 + ((cp - 0xD800) << 10) + (units[++i] - 0xDC00);
    else if (cp >= 0xD800 && cp <= 0xDFFF)
      cp = 0xFFFD;
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | cp >> 6);
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | cp >> 12);
      out += char(0x80 | (cp >> 6 & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | cp >> 18);
      out += char(0x80 | (cp >> 12 & 0x3F));
      out += char(0x80 | (cp >> 6 & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }
  return out;
}

jstring java_string(JNIEnv* env, std::string_view text) {
  const auto units = utf16(text);
  return env->NewString(reinterpret_cast<const jchar*>(units.data()), jsize(units.size()));
}

jbyteArray java_bytes(JNIEnv* env, std::span<const unsigned char> data) {
  jbyteArray array = env->NewByteArray(jsize(data.size()));
  if (array && !data.empty())
    env->SetByteArrayRegion(array, 0, jsize(data.size()), reinterpret_cast<const jbyte*>(data.data()));
  return array;
}

std::vector<unsigned char> bytes(JNIEnv* env, jbyteArray array) {
  if (!array) return {};
  std::vector<unsigned char> out(std::size_t(env->GetArrayLength(array)));
  if (!out.empty()) env->GetByteArrayRegion(array, 0, jsize(out.size()), reinterpret_cast<jbyte*>(out.data()));
  return out;
}

bool open_url(std::string_view url) {
  if (!ready() || !g_classes.device) return false;
  JNIEnv* e = env();
  LocalFrame frame(e);
  jmethodID id = e->GetStaticMethodID(g_classes.device, "openUrl", "(Ljava/lang/String;)Z");
  if (!id) {
    e->ExceptionClear();
    return false;
  }
  const bool opened = e->CallStaticBooleanMethod(g_classes.device, id, java_string(e, url));
  return take_exception(e).empty() && opened;
}

std::string take_exception(JNIEnv* env) {
  if (!env->ExceptionCheck()) return {};
  jthrowable error = env->ExceptionOccurred();
  env->ExceptionClear();
  std::string text = "Java exception";
  if (error) {
    jclass throwable = env->FindClass("java/lang/Throwable");
    jmethodID to_string = throwable ? env->GetMethodID(throwable, "toString", "()Ljava/lang/String;") : nullptr;
    if (to_string) {
      auto message = static_cast<jstring>(env->CallObjectMethod(error, to_string));
      if (!env->ExceptionCheck() && message) text = utf8(env, message);
      env->ExceptionClear();
      if (message) env->DeleteLocalRef(message);
    }
    if (throwable) env->DeleteLocalRef(throwable);
    env->DeleteLocalRef(error);
  }
  return text;
}
}  // namespace venture::android

using namespace venture;

namespace venture::android {
void attach(JNIEnv* env, jobject context, App app) {
  if (g_ready.load()) return;
  env->GetJavaVM(&g_vm);
  g_context = env->NewGlobalRef(context);
  g_app = std::move(app);
  // Classes a process lacks stay null: a game's Platform loader has only the
  // shared ones (org.venture_emu.net).
  auto& c = g_classes;
  c.http = global_class(env, "org/venture_emu/net/Http");
  c.transfer = global_class(env, "org/venture_emu/net/Http$Transfer");
  c.keys = global_class(env, "org/venture_emu/venture/Keys");
  c.sign_in = global_class(env, "org/venture_emu/venture/SignIn");
  c.device = global_class(env, "org/venture_emu/venture/Device");
  c.installer = global_class(env, "org/venture_emu/venture/Installer");
  c.documents = global_class(env, "org/venture_emu/venture/Documents");
  g_ready.store(true, std::memory_order_release);
}
}  // namespace venture::android

// Native.nativeInit (src/android_app/java/org/venture_emu/venture/Native.java).
extern "C" JNIEXPORT void JNICALL Java_org_venture_1emu_venture_Native_nativeInit(
    JNIEnv* env, jclass, jobject context, jstring data, jstring cache, jstring apk, jint sdk, jstring locales) {
  if (android::g_ready.load()) return;
  android::attach(env, context, {android::utf8(env, data), android::utf8(env, cache), android::utf8(env, apk), sdk});
  // All of Venture's state lives in the app's private storage.
  setenv("VENTURE_DATA_DIR", android::g_app.data.c_str(), 1);
  setenv("TMPDIR", android::g_app.cache.c_str(), 1);
  if (!getenv("HOME")) setenv("HOME", android::g_app.data.c_str(), 1);
  // The device's interface languages as gettext's LANGUAGE list, which the
  // launcher's "auto" language follows.
  std::string languages = android::utf8(env, locales);
  for (auto& c : languages)
    if (c == ',') c = ':';
  if (!languages.empty()) {
    setenv("LANGUAGE", languages.c_str(), 1);
    setenv("LANG", languages.substr(0, languages.find(':')).c_str(), 1);
  }
}
