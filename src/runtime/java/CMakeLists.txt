set(VENTURE_JAVA_HOME "${PROJECT_SOURCE_DIR}/.deps/jdk" CACHE PATH "Pinned JDK (python tools/bootstrap.py --jdk)")
if(NOT EXISTS "${VENTURE_JAVA_HOME}/include/jni.h")
  message(FATAL_ERROR "Missing JDK: run python tools/bootstrap.py --jdk")
endif()
enable_language(ASM)
if(WIN32)
  set(java_platform win32)
  set(java_headers win32)
  set(java_exe .exe)
else()
  set(java_platform posix)
  set(java_headers linux)
endif()
add_library(venture_jvmti_agent SHARED tooling_agent.cpp)
set_target_properties(venture_jvmti_agent PROPERTIES PREFIX "" OUTPUT_NAME "venture-jvmti-agent"
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
target_include_directories(venture_jvmti_agent PRIVATE "${VENTURE_JAVA_HOME}/include"
  "${VENTURE_JAVA_HOME}/include/${java_headers}")
target_compile_options(venture_jvmti_agent PRIVATE -Wall -Wextra -Wpedantic)
add_library(venture_java STATIC
  execution_freeze.cpp
  abi.cpp jvm.cpp guest_jni.cpp jni32.cpp native_bridge.cpp elf_exports.cpp conversion.cpp method_hooks.cpp
  ../platform/${java_platform}/java_host.cpp
  ../platform/${java_platform}/java_entry.S)
target_include_directories(venture_java PUBLIC
  ${PROJECT_SOURCE_DIR}/src/runtime "${VENTURE_JAVA_HOME}/include")
if(WIN32)
  target_include_directories(venture_java PUBLIC "${VENTURE_JAVA_HOME}/include/win32")
  add_executable(venture-java tool_main.cpp ../platform/win32/java_host.cpp ../platform/win32/runtime.rc)
  target_include_directories(venture-java PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime
    "${VENTURE_JAVA_HOME}/include" "${VENTURE_JAVA_HOME}/include/win32" ../platform/win32)
  target_link_libraries(venture-java PRIVATE venture_common)
  # Keep the launcher inside the relocatable Java payload copied by pin_runtime.
  add_custom_command(TARGET venture-java POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:venture-java>/java"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:venture-java>"
      "$<TARGET_FILE_DIR:venture-java>/java/venture-java.exe" VERBATIM)
else()
  target_include_directories(venture_java PUBLIC "${VENTURE_JAVA_HOME}/include/linux")
  target_link_libraries(venture_java PRIVATE ${CMAKE_DL_LIBS})
endif()
target_link_libraries(venture_java PUBLIC venture_common venture_runtime)
target_compile_options(venture_java PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>)
set(VENTURE_JAVA_BRIDGE_JAR "${CMAKE_CURRENT_BINARY_DIR}/venture-java-bridge.jar" CACHE INTERNAL "JNI support classes")
set(venture_bridge_classes "${CMAKE_CURRENT_BINARY_DIR}/classes-venture")
add_custom_command(OUTPUT "${VENTURE_JAVA_BRIDGE_JAR}"
  COMMAND "${VENTURE_JAVA_HOME}/bin/javac${java_exe}" -encoding UTF-8 --release 21 -d "${venture_bridge_classes}"
    "${CMAKE_CURRENT_SOURCE_DIR}/bridge/com/venture/runtime/NativeLoader.java"
    "${CMAKE_CURRENT_SOURCE_DIR}/bridge/com/venture/runtime/NativeMethods.java"
    "${CMAKE_CURRENT_SOURCE_DIR}/bridge/com/venture/runtime/MethodHooks.java"
  COMMAND "${VENTURE_JAVA_HOME}/bin/jar${java_exe}" --create --file "${VENTURE_JAVA_BRIDGE_JAR}" -C "${venture_bridge_classes}" .
  DEPENDS bridge/com/venture/runtime/NativeLoader.java bridge/com/venture/runtime/NativeMethods.java
    bridge/com/venture/runtime/MethodHooks.java VERBATIM)
add_custom_target(venture_java_bridge_jar ALL DEPENDS "${VENTURE_JAVA_BRIDGE_JAR}")
add_dependencies(venture_java venture_java_bridge_jar)
