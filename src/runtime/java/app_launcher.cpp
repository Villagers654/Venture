#include "java/app_launcher.h"

#include <jni.h>

#include <array>
#include <exception>

#include "android/package_assets.h"
#include "common/log.h"
#include "core/process.h"
#include "core/thread.h"
#include "java/jvm.h"

namespace venture::java {
namespace {
std::string path_utf8(const std::filesystem::path& path) {
  auto text = path.u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}
}  // namespace

bool run_app_main(const std::string& converted_jar, std::string* error) {
  auto fail = [&](const std::string& message) {
    if (error) *error = message;
    log::error("java", "{}", message);
    return false;
  };
  try {
    if (!host_vm())
      return fail("JVM must be started before Java application launch");
    GuestThread::adopt_current();
    auto* env = host_env();
    if (!env) return fail("Cannot attach Android main thread to JVM");
    const auto& config = process_config();
    if (!android::PackageAssets::get().open_apk(path_utf8(config.apk)))
      return fail("Cannot open application APK");
    if (!android::PackageAssets::get().prepare_unity_extraction(config.data_dir / "external-data" / "files"))
      return fail("Cannot complete packaged engine data extraction");
    if (env->PushLocalFrame(32) != JNI_OK)
      return fail("Cannot allocate launch JNI frame");
    const std::array<std::string, 8> args{
        config.package,
        path_utf8(config.apk),
        path_utf8(config.data_dir),
        path_utf8(config.guest_cache_dir),
        path_utf8(config.obb_dir),
        path_utf8(config.native_lib_dir),
        std::to_string(config.sdk_level),
        converted_jar,
    };
    auto string_class = env->FindClass("java/lang/String");
    auto array = string_class
                     ? env->NewObjectArray(static_cast<jsize>(args.size()),
                                           string_class, nullptr)
                     : nullptr;
    if (array)
      for (std::size_t i = 0; i < args.size() && !env->ExceptionCheck(); ++i) {
        // UTF-8 paths can contain supplementary characters: use the JDK UTF-8
        // decoder rather than passing ordinary UTF-8 to JNI's modified UTF-8
        // API.
        auto bytes = env->NewByteArray(static_cast<jsize>(args[i].size()));
        if (!bytes) break;
        env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(args[i].size()),
                                reinterpret_cast<const jbyte*>(args[i].data()));
        auto ctor =
            env->GetMethodID(string_class, "<init>", "([BLjava/lang/String;)V");
        auto encoding = env->NewStringUTF("UTF-8");
        auto value = ctor && encoding
                         ? env->NewObject(string_class, ctor, bytes, encoding)
                         : nullptr;
        if (value)
          env->SetObjectArrayElement(array, static_cast<jsize>(i), value);
        env->DeleteLocalRef(bytes);
        env->DeleteLocalRef(encoding);
        env->DeleteLocalRef(value);
      }
    if (!env->ExceptionCheck() && array) {
      auto runtime = env->FindClass("com/venture/runtime/RuntimeEnvironment");
      auto configure = runtime ? env->GetStaticMethodID(runtime, "configure", "([Ljava/lang/String;)V") : nullptr;
      if (configure) env->CallStaticVoidMethod(runtime, configure, array);
      if (!env->ExceptionCheck()) {
        auto get = env->GetStaticMethodID(runtime, "get", "()Lcom/venture/runtime/RuntimeEnvironment;");
        auto instance = get ? env->CallStaticObjectMethod(runtime, get) : nullptr;
        if (!env->ExceptionCheck() && instance) {
          auto loader_field = env->GetFieldID(runtime, "loader", "Ljava/lang/ClassLoader;");
          auto loader = loader_field ? env->GetObjectField(instance, loader_field) : nullptr;
          if (loader) set_app_class_loader(loader);
        }
      }
      if (!env->ExceptionCheck()) {
        auto main = env->FindClass("com/venture/runtime/AppMain");
        auto method = main ? env->GetStaticMethodID(main, "run", "()V") : nullptr;
        if (method) env->CallStaticVoidMethod(main, method);
      }
    }
    bool failed = env->ExceptionCheck();
    if (failed) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    }
    if (!failed) {
      auto main = env->FindClass("com/venture/runtime/AppMain");
      auto status = main ? env->GetStaticMethodID(main, "exitStatus", "()I") : nullptr;
      failed = !status || env->CallStaticIntMethod(main, status) != 0;
      if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); failed = true; }
    }
    env->PopLocalFrame(nullptr);
    return failed
               ? fail(
                     "Java application launch failed; see Java exception trace")
               : true;
  } catch (const std::exception& exception) {
    return fail(exception.what());
  }
}
}  // namespace venture::java
