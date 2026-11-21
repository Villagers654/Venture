#pragma once
#include <filesystem>

namespace venture::java {
struct SupportFiles {
  std::filesystem::path home, framework, bridge, hierarchy;
  // APK conversion: java/dex/classes runs on java/jdk; no development fallback.
  std::filesystem::path dex_tools, dex_java;
  std::filesystem::path agent;
  // Android 2D apps (docs/design/layoutlib.md): venture-framework-2d.jar and
  // layoutlib/layoutlib.jar; absent where the build has no layoutlib.
  std::filesystem::path framework_2d, layoutlib;
};
// Distribution layout, relative to paths::executable().parent_path():
// venture-{framework,java-bridge}.jar and java/{jdk,agent,dex/classes,asm,dex2jar,android-32.hierarchy.tsv}.
// All support files are staged by the build; missing files are installation errors.
SupportFiles support_files();
}  // namespace venture::java
