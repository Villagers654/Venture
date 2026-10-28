#include <chrono>
#include <filesystem>
#include <string>
#include <doctest/doctest.h>
#include <jni.h>
#include "java/jvm.h"
#include "java/natives/runtime_bridge.h"
#include "java/natives/sqlite_bridge.h"

TEST_CASE("android.database.sqlite runs on the host system SQLite through production JNI") {
  using namespace venture;
  const auto directory = std::filesystem::temp_directory_path() /
      ("venture-sqlite-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  java::install_runtime_framework_bridges();
  java::register_sqlite_natives();
  std::string error;
  REQUIRE_MESSAGE(java::start_jvm({VENTURE_TEST_JAVA_HOME, {VENTURE_TEST_FRAMEWORK_JAR, VENTURE_TEST_JAVA_CLASSES},
                                   {"-Xcheck:jni", "-Xverify:all"}}, &error), error);
  auto* env = java::host_env();
  auto fixture = env->FindClass("SQLiteTest");
  REQUIRE(fixture);
  auto run = env->GetStaticMethodID(fixture, "run", "(Ljava/lang/String;)V");
  REQUIRE(run);
  const auto text = directory.u16string();
  auto path = env->NewString(reinterpret_cast<const jchar*>(text.data()), static_cast<jsize>(text.size()));
  env->CallStaticVoidMethod(fixture, run, path);
  const bool failed = env->ExceptionCheck();
  if (failed) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  CHECK_FALSE(failed);
  env->DeleteLocalRef(path);
  env->DeleteLocalRef(fixture);
  for (const auto& entry : std::filesystem::directory_iterator(directory)) std::filesystem::remove(entry.path());
  std::filesystem::remove(directory);
}
