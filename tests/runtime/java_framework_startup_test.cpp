#include "java/jvm.h"
#include "java/tooling_agent.h"
#include "platform/java_host.h"
#include <cstdio>

namespace {
int calls = 0;
jint JNICALL replaced(JNIEnv*, jobject, const jvalue*, jint count, jvalue*, void*) {
  if (count != 1) return -JVMTI_ERROR_ILLEGAL_ARGUMENT;
  ++calls;
  return 1;
}
}
int main() {
  using namespace venture::java;
  JvmOptions options{VENTURE_TEST_JAVA_HOME,
      {VENTURE_TEST_FRAMEWORK_JAR, VENTURE_TEST_BRIDGE_JAR}, {"-Xcheck:jni", "-Xverify:all"},
      VENTURE_TEST_JVM_AGENT};
  std::string error;
  if (!start_jvm(options, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
  auto* env = host_env();
  auto finish = [&](int code) {
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); code = code ? code : 2; }
    if (!stop_jvm(&error)) return 3;
    return code;
  };
  auto loader = env->FindClass("com/venture/runtime/NativeLoader");
  if (!loader || env->ExceptionCheck()) return finish(4);
  auto system = env->FindClass("java/lang/System");
  if (!system || env->ExceptionCheck()) return finish(5);
  auto canonical = env->GetStaticMethodID(system, "loadLibrary", "(Ljava/lang/String;)V");
  if (!canonical || env->ExceptionCheck()) return finish(6);
  auto alias = env->GetStaticMethodID(loader, "loadLibrary", "(Ljava/lang/String;)V");
  if (!alias || env->ExceptionCheck()) return finish(7);
  const auto agent = std::filesystem::path(VENTURE_TEST_JVM_AGENT);
  auto install = reinterpret_cast<decltype(&VentureJvmtiInstallHook)>(
      platform::loaded_agent_symbol(agent, "VentureJvmtiInstallHook"));
  auto remove = reinterpret_cast<decltype(&VentureJvmtiRemoveHook)>(
      platform::loaded_agent_symbol(agent, "VentureJvmtiRemoveHook"));
  if (install(canonical, replaced, nullptr, nullptr) != 0) return finish(8);
  auto name = env->NewStringUTF("public_production_classpath_fixture");
  if (!name || env->ExceptionCheck()) return finish(9);
  env->CallStaticVoidMethod(loader, alias, name);
  if (env->ExceptionCheck() || calls != 1) return finish(10);
  auto reflected = env->ToReflectedMethod(system, canonical, JNI_TRUE);
  if (!reflected || env->ExceptionCheck()) return finish(11);
  auto object = env->FindClass("java/lang/Object");
  if (!object || env->ExceptionCheck()) return finish(12);
  auto args = env->NewObjectArray(1, object, name);
  if (!args || env->ExceptionCheck()) return finish(13);
  auto invoke = env->GetStaticMethodID(loader, "invokeWithLoader",
      "(Ljava/lang/reflect/Method;Ljava/lang/Object;[Ljava/lang/Object;Ljava/lang/ClassLoader;)Ljava/lang/Object;");
  if (!invoke || env->ExceptionCheck()) return finish(14);
  env->CallStaticObjectMethod(loader, invoke, reflected, static_cast<jobject>(nullptr), args,
                             static_cast<jobject>(nullptr));
  if (env->ExceptionCheck() || calls != 2) return finish(15);
  if (remove(canonical) != 0) return finish(16);
  std::puts("Production framework-first classpath: startup, rewritten and reflected hooks PASS");
  return finish(0);
}
