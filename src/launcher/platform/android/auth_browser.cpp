// SPDX-License-Identifier: GPL-3.0-or-later
// Meta sign-in on Android: the app's SignInActivity, a WebView kept on Meta's
// pages by the same rules as the desktop windows (meta::auth_navigation_allowed,
// meta::auth_frame_allowed, called from Java below), hands back Meta's
// oculus:// address. Events arrive on the UI thread and wait here for poll().
#include "launcher/auth_browser.h"

#include "common/meta/auth.h"
#include "common/paths.h"
#include "common/platform/android/jni.h"
#include "launcher/sign_in_link.h"

#include <deque>
#include <mutex>
#include <stdexcept>

namespace venture::launcher {
namespace jni = venture::android;
namespace {
std::mutex g_mutex;
std::deque<AuthBrowser::Event> g_events;
bool g_open = false;

void call_sign_in(const char* method, const char* signature, jstring argument, bool* result = nullptr) {
  if (!jni::ready() || !jni::classes().sign_in) throw std::runtime_error("Could not open the Meta sign-in window.");
  JNIEnv* env = jni::env();
  jmethodID id = env->GetStaticMethodID(jni::classes().sign_in, method, signature);
  if (!id) {
    env->ExceptionClear();
    throw std::runtime_error("Could not open the Meta sign-in window.");
  }
  if (result) *result = env->CallStaticBooleanMethod(jni::classes().sign_in, id, argument);
  else env->CallStaticVoidMethod(jni::classes().sign_in, id);
  jni::take_exception(env);
}
}  // namespace

struct AuthBrowser::Impl {};
AuthBrowser::AuthBrowser() : impl_(std::make_unique<Impl>()) {}
AuthBrowser::~AuthBrowser() { close(); }

void AuthBrowser::open(std::string url, const fs::path&) {
  close();
  if (!meta::auth_navigation_allowed(url)) throw std::runtime_error("Sign-in URL is outside allowed Meta domains.");
  {
    std::lock_guard lock(g_mutex);
    g_events.clear();
    g_open = true;
  }
  JNIEnv* env = jni::env();
  jni::LocalFrame frame(env);
  bool opened = false;
  call_sign_in("open", "(Ljava/lang/String;)Z", jni::java_string(env, url), &opened);
  if (!opened) {
    std::lock_guard lock(g_mutex);
    g_open = false;
    throw std::runtime_error("Could not open the Meta sign-in window.");
  }
}

void AuthBrowser::close() {
  bool was_open;
  {
    std::lock_guard lock(g_mutex);
    was_open = g_open;
    g_open = false;
  }
  if (was_open && jni::ready()) {
    try {
      call_sign_in("close", "()V", nullptr);
    } catch (...) {
    }
  }
}

std::optional<AuthBrowser::Event> AuthBrowser::poll() {
  std::lock_guard lock(g_mutex);
  if (g_events.empty()) return std::nullopt;
  auto event = std::move(g_events.front());
  g_events.pop_front();
  return event;
}

int auth_browser_helper(int, char**) { return -1; }
}  // namespace venture::launcher

using namespace venture;

extern "C" {
JNIEXPORT void JNICALL Java_org_venture_1emu_venture_Native_signInEvent(JNIEnv* env, jclass, jint kind,
                                                                        jstring value) {
  using Kind = launcher::AuthBrowser::Kind;
  auto text = android::utf8(env, value);
  launcher::AuthBrowser::Event event{Kind::error, {}};
  switch (kind) {
    case 0: event = {Kind::ready, {}}; break;
    case 1:
      if (!meta::is_callback(text)) return;
      event = {Kind::callback, std::move(text)};
      break;
    case 2: event = {Kind::closed, {}}; break;
    default: event = {Kind::error, text.empty() ? "Could not open the Meta sign-in window." : text}; break;
  }
  std::lock_guard lock(launcher::g_mutex);
  if (!launcher::g_open && event.kind != Kind::closed) return;  // a window from an earlier sign-in
  if (event.kind == Kind::closed) launcher::g_open = false;
  launcher::g_events.push_back(std::move(event));
}
JNIEXPORT jboolean JNICALL Java_org_venture_1emu_venture_Native_navigationAllowed(JNIEnv* env, jclass, jstring url) {
  return meta::auth_navigation_allowed(android::utf8(env, url));
}
JNIEXPORT jboolean JNICALL Java_org_venture_1emu_venture_Native_frameAllowed(JNIEnv* env, jclass, jstring url) {
  return meta::auth_frame_allowed(android::utf8(env, url));
}
JNIEXPORT jboolean JNICALL Java_org_venture_1emu_venture_Native_isCallback(JNIEnv* env, jclass, jstring url) {
  return meta::is_callback(android::utf8(env, url));
}
JNIEXPORT void JNICALL Java_org_venture_1emu_venture_Native_signInLink(JNIEnv* env, jclass, jstring url) {
  launcher::sign_in_link::deliver(paths::data_dir(), android::utf8(env, url));
}
}
