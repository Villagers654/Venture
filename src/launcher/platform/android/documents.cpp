// SPDX-License-Identifier: GPL-3.0-or-later
// Game files the user picks on the headset (launcher/document_picker.h and
// common/library/import.h's documents), through the app's Documents class:
// Android's document picker, then reads through file descriptors it opens on
// the picked documents. No storage permission is involved.
#include <jni.h>
#include <unistd.h>

#include <format>
#include <mutex>

#include "common/json.h"
#include "common/library/import.h"
#include "common/log.h"
#include "common/platform/android/jni.h"
#include "launcher/document_picker.h"

using namespace venture;

namespace {
namespace jni = venture::android;

jmethodID method(JNIEnv* env, const char* name, const char* signature) {
  if (!jni::ready() || !jni::classes().documents) return nullptr;
  jmethodID id = env->GetStaticMethodID(jni::classes().documents, name, signature);
  if (!id) jni::take_exception(env);
  return id;
}

// A picked document opened for reading: /proc/self/fd/<fd> while it lives.
struct Opened {
  explicit Opened(int descriptor) : fd(descriptor), path(std::format("/proc/self/fd/{}", descriptor)) {}
  Opened(const Opened&) = delete;
  Opened& operator=(const Opened&) = delete;
  int fd;
  fs::path path;
  ~Opened() { close(fd); }
};

class AndroidDocuments final : public import::Documents {
public:
  bool handles(std::string_view picked) const override { return picked.starts_with("content://"); }

  Listing list(const std::vector<std::string>& picks, std::stop_token) const override {
    JNIEnv* env = jni::env();
    jni::LocalFrame frame(env, int(picks.size()) + 8);
    jmethodID list = method(env, "list", "([Ljava/lang/String;)Ljava/lang/String;");
    if (!list) throw std::runtime_error("Venture cannot read picked files on this device.");
    auto array = env->NewObjectArray(jsize(picks.size()), env->FindClass("java/lang/String"), nullptr);
    for (std::size_t i = 0; i < picks.size(); ++i)
      env->SetObjectArrayElement(array, jsize(i), jni::java_string(env, picks[i]));
    auto result = static_cast<jstring>(env->CallStaticObjectMethod(jni::classes().documents, list, array));
    if (auto error = jni::take_exception(env); !error.empty() || !result)
      throw std::runtime_error("Could not read the picked files: " + error);
    const auto json = parse_json(jni::utf8(env, result));
    if (const auto error = str(json, "error"); !error.empty())
      throw std::runtime_error("Could not read the picked files: " + error);
    Listing out;
    out.root = str(json, "root");
    if (auto it = json.find("files"); it != json.end() && it->is_array())
      for (const auto& f : *it)
        if (f.is_object())
          out.files.push_back({str(f, "path"), static_cast<std::uint64_t>(number(f, "size")), str(f, "uri")});
    return out;
  }

  std::shared_ptr<const fs::path> open(std::string_view uri) const override {
    JNIEnv* env = jni::env();
    jni::LocalFrame frame(env);
    jmethodID open = method(env, "open", "(Ljava/lang/String;)I");
    if (!open) return nullptr;
    const jint fd = env->CallStaticIntMethod(jni::classes().documents, open, jni::java_string(env, uri));
    if (!jni::take_exception(env).empty() || fd < 0) return nullptr;
    auto opened = std::make_shared<Opened>(fd);
    return {opened, &opened->path};
  }
};

// Picked documents are read through this host's Documents, once the library loads.
const bool g_registered = [] {
  import::set_documents(std::make_shared<AndroidDocuments>());
  return true;
}();

std::mutex g_pick_mutex;
std::function<void(std::vector<std::string>)> g_pick_done;
}  // namespace

namespace venture::launcher {
bool pick_documents(bool folder, std::function<void(std::vector<std::string>)> done) {
  JNIEnv* env = jni::env();
  jni::LocalFrame frame(env);
  jmethodID pick = method(env, "pick", "(Z)Z");
  if (!pick) return false;
  {
    std::lock_guard lock(g_pick_mutex);
    g_pick_done = std::move(done);
  }
  const bool shown = env->CallStaticBooleanMethod(jni::classes().documents, pick, jboolean(folder));
  if (!jni::take_exception(env).empty() || !shown) {
    std::lock_guard lock(g_pick_mutex);
    g_pick_done = nullptr;
    return false;
  }
  return true;
}
}  // namespace venture::launcher

// Native.documentsPicked (src/android_app/java/org/venture_emu/venture/Native.java).
extern "C" JNIEXPORT void JNICALL Java_org_venture_1emu_venture_Native_documentsPicked(JNIEnv* env, jclass,
                                                                                       jobjectArray uris) {
  std::vector<std::string> picked;
  const jsize n = uris ? env->GetArrayLength(uris) : 0;
  for (jsize i = 0; i < n; ++i) {
    auto uri = static_cast<jstring>(env->GetObjectArrayElement(uris, i));
    if (uri) picked.push_back(jni::utf8(env, uri));
    env->DeleteLocalRef(uri);
  }
  std::function<void(std::vector<std::string>)> done;
  {
    std::lock_guard lock(g_pick_mutex);
    done = std::move(g_pick_done);
    g_pick_done = nullptr;
  }
  if (done) done(std::move(picked));
  else log::warn("import", "documents were picked with no pick waiting");
}
