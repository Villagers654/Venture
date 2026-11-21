#include "java/support_files.h"
#include "common/paths.h"
#include "common/layoutlib_component.h"

namespace venture::java {
SupportFiles support_files() {
  const auto bin = paths::executable().parent_path();
  auto home = bin / "java/jdk";
  // The build always stages the converter and its JDK beside the runtime.
#ifdef _WIN32
  const char* java = "java/jdk/bin/java.exe";
  const char* agent = "java/agent/venture-jvmti-agent.dll";
#else
  const char* java = "java/jdk/bin/java";
  const char* agent = "java/agent/venture-jvmti-agent.so";
#endif
  return {home, bin / "venture-framework.jar",
          bin / "venture-java-bridge.jar",
          bin / "java/android-32.hierarchy.tsv", bin / "java/dex/classes", bin / java, bin / agent,
          bin / "venture-framework-2d.jar", layoutlib_component::find(bin, paths::data_dir())};
}
}  // namespace venture::java
