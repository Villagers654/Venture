#pragma once
#include <string>

namespace venture::java {
// JVM must be running with venture-framework.jar on its classpath and
// framework natives registered before start_jvm(). Call on the runtime's main
// thread. Uses ProcessConfig and mounts its APK through PackageAssets. Adopts
// this thread as a guest thread, attaches it to the JVM, drives Java lifecycle
// then blocks in Looper.loop until shutdown. Returns false with an observable
// Java/native error.
bool run_app_main(const std::string& converted_jar, std::string* error);
}  // namespace venture::java
