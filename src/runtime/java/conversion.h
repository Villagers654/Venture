#pragma once
#include <string>
#include <vector>
namespace venture::java {
// Paths crossing the Java/JNI boundary are UTF-8 on every host.
struct ConversionOptions {
  std::string java;   // the bundled JDK's java executable (java/jdk/bin/java)
  std::string tools;  // bundled DEX tools (java/dex/classes) with com.venture.dex.ApkCache
  std::string apk, package, cache;
  std::vector<std::string> framework_jars;
  std::vector<std::string> hierarchy_metadata;  // compiler-only ancestry; never a JVM classpath
};
// Exit 0 means a hash-matched, verified app.jar was published. Any converter or
// verification error is a failure, including unresolved framework dependencies.
bool convert_apk(const ConversionOptions&, std::string* error);
struct ConvertedApk {
  std::string app_jar, report;
  // All declarations/methods survived conversion; eager failures, if any, are
  // unresolved dependencies. This does NOT mean all classes are verified.
  // An application may use normal lazy linking with HotSpot verification on.
  bool translation_complete = false;
};
bool convert_apk(const ConversionOptions&, ConvertedApk* output, std::string* error);
}  // namespace venture::java
