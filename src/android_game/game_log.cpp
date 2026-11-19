// SPDX-License-Identifier: GPL-3.0-or-later
#include "android_game/game_log.h"

#include <atomic>
#include <string>
#include <system_error>

#include "common/log.h"
#include "common/paths.h"

namespace venture::android_game {
std::filesystem::path obb_dir(JNIEnv* env, jobject context) {
  if (!env || !context) return {};
  std::filesystem::path out;
  jclass cls = env->GetObjectClass(context);
  jmethodID get = cls ? env->GetMethodID(cls, "getObbDir", "()Ljava/io/File;") : nullptr;
  jobject file = get ? env->CallObjectMethod(context, get) : nullptr;
  if (file && !env->ExceptionCheck()) {
    jclass file_class = env->GetObjectClass(file);
    jmethodID path = env->GetMethodID(file_class, "getAbsolutePath", "()Ljava/lang/String;");
    if (auto text = path ? static_cast<jstring>(env->CallObjectMethod(file, path)) : nullptr) {
      const char* chars = env->GetStringUTFChars(text, nullptr);
      if (chars) {
        out = paths::from_utf8(chars);
        env->ReleaseStringUTFChars(text, chars);
      }
      env->DeleteLocalRef(text);
    }
    env->DeleteLocalRef(file_class);
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (file) env->DeleteLocalRef(file);
  if (cls) env->DeleteLocalRef(cls);
  return out;
}

void start_log(const std::filesystem::path& obb, std::string_view name) {
  static std::atomic<bool> started{false};
  if (obb.empty() || started.exchange(true)) return;
  std::error_code ec;
  const auto folder = obb / std::string(kRecordFolder);
  std::filesystem::create_directories(folder, ec);
  const auto file = folder / (std::string(name) + ".log");
  auto previous = folder / (std::string(name) + ".prev.log");
  std::filesystem::rename(file, previous, ec);
  log::set_file(paths::utf8(file));
#if defined(__aarch64__)
  log::info("venture", "{} for arm64-v8a", name);
#elif defined(__arm__)
  log::info("venture", "{} for armeabi-v7a", name);
#endif
  // The game's files are its own (umask 077), and a file does not always get
  // the OBB folder's group (ext_obb_rw), through which installers such as the
  // Venture app read there. So these logs are readable by all: only that
  // group may enter the game's OBB folder.
  using std::filesystem::perms;
  std::filesystem::permissions(folder, perms::group_read | perms::group_exec | perms::others_read | perms::others_exec,
                               std::filesystem::perm_options::add, ec);
  std::filesystem::permissions(file, perms::group_read | perms::others_read, std::filesystem::perm_options::add, ec);
}
}  // namespace venture::android_game
