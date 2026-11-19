# The two JDK roles. VENTURE_JAVA_HOME is the target's JDK: its JNI headers
# build the runtime's Java code, its JVM is staged beside the runtime, and tests
# run it. VENTURE_HOST_JAVA_HOME runs at build time (javac, jar). They are the
# same JDK except in a cross build (Linux arm64 from Windows or x64 Linux), where
# the target JDK is the pinned linux-aarch64 one (tools/linux_arm64/sysroot.py).
if(CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(venture_target_jdk "${PROJECT_SOURCE_DIR}/.deps/jdk-linux-aarch64")
else()
  set(venture_target_jdk "${PROJECT_SOURCE_DIR}/.deps/jdk")
endif()
set(VENTURE_JAVA_HOME "${venture_target_jdk}" CACHE PATH "The target's pinned JDK 21 (python tools/bootstrap.py --jdk)")
set(VENTURE_HOST_JAVA_HOME "${PROJECT_SOURCE_DIR}/.deps/jdk" CACHE PATH "The build host's JDK 21 for javac and jar")
if(CMAKE_HOST_WIN32)
  set(VENTURE_HOST_JAVA_SUFFIX .exe)
else()
  set(VENTURE_HOST_JAVA_SUFFIX "")
endif()
