set(VENTURE_JAVA_HOME "${PROJECT_SOURCE_DIR}/.deps/jdk" CACHE PATH "JDK 21 for the Android framework")
find_program(VENTURE_JAVAC javac HINTS "${VENTURE_JAVA_HOME}/bin" REQUIRED)
find_program(VENTURE_JAR jar HINTS "${VENTURE_JAVA_HOME}/bin" REQUIRED)
set(VENTURE_ANDROID_RESOURCES "${PROJECT_SOURCE_DIR}/.deps/framework-res/root")
if(NOT EXISTS "${VENTURE_ANDROID_RESOURCES}/venture/android-32/resources.arsc")
  message(FATAL_ERROR "Missing Android resources: python tools/bootstrap.py --jdk")
endif()
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/.deps/framework-res/android-32.hierarchy.tsv")
  message(FATAL_ERROR "Missing compiler-only Android ancestry: python tools/bootstrap.py --jdk")
endif()
file(GLOB_RECURSE VENTURE_FRAMEWORK_JAVA CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.java")
list(APPEND VENTURE_FRAMEWORK_JAVA "${CMAKE_CURRENT_SOURCE_DIR}/../bridge/com/venture/runtime/NativeMethods.java")
list(APPEND VENTURE_FRAMEWORK_JAVA "${CMAKE_CURRENT_SOURCE_DIR}/../bridge/com/venture/runtime/MethodHooks.java")
# A pre-rebrand build tree may still contain classes from the released package.
# Use a fresh generation and package only the current resource subtree.
set(VENTURE_FRAMEWORK_CLASSES "${CMAKE_CURRENT_BINARY_DIR}/classes-venture")
set(VENTURE_FRAMEWORK_JAR "${CMAKE_BINARY_DIR}/bin/venture-framework.jar" CACHE INTERNAL "Android framework jar")
set(VENTURE_FRAMEWORK_ARGS "")
foreach(source IN LISTS VENTURE_FRAMEWORK_JAVA)
  string(APPEND VENTURE_FRAMEWORK_ARGS "\"${source}\"\n")
endforeach()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sources.txt" CONTENT "${VENTURE_FRAMEWORK_ARGS}")
add_custom_command(OUTPUT "${VENTURE_FRAMEWORK_JAR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${VENTURE_FRAMEWORK_CLASSES}" "${CMAKE_BINARY_DIR}/bin"
  COMMAND "${VENTURE_JAVAC}" -encoding UTF-8 --release 21 -d "${VENTURE_FRAMEWORK_CLASSES}" "@${CMAKE_CURRENT_BINARY_DIR}/sources.txt"
  COMMAND "${VENTURE_JAR}" --create --file "${VENTURE_FRAMEWORK_JAR}" -C "${VENTURE_FRAMEWORK_CLASSES}" . -C "${VENTURE_ANDROID_RESOURCES}" venture
    -C "${CMAKE_CURRENT_SOURCE_DIR}" NOTICE -C "${CMAKE_CURRENT_SOURCE_DIR}" APACHE-2.0.txt
    -C "${CMAKE_CURRENT_SOURCE_DIR}" org/xmlpull/v1/LICENSE.txt
  DEPENDS ${VENTURE_FRAMEWORK_JAVA} "${CMAKE_CURRENT_BINARY_DIR}/sources.txt" NOTICE APACHE-2.0.txt org/xmlpull/v1/LICENSE.txt
    "${VENTURE_ANDROID_RESOURCES}/venture/android-32/resources.arsc" "${VENTURE_ANDROID_RESOURCES}/venture/android-32/NOTICE.txt"
  VERBATIM)
add_custom_target(venture-framework ALL DEPENDS "${VENTURE_FRAMEWORK_JAR}")

# The library compiles against contracts alone. The integrator supplies JVM,
# core and libandroid definitions when the launch entry is called.
add_library(venture_java_framework STATIC ../app_launcher.cpp ../natives/framework_natives.cpp ../natives/display_bridge.cpp ../natives/path_bridge.cpp ../natives/runtime_bridge.cpp ../natives/audio_bridge.cpp)
target_sources(venture_java_framework PRIVATE ../natives/graphics_bridge.cpp ../natives/sqlite_bridge.cpp)
target_sources(venture_java_framework PRIVATE ../natives/media_bridge.cpp ../natives/surface_texture_bridge.cpp)
target_include_directories(venture_java_framework PUBLIC "${PROJECT_SOURCE_DIR}/src/runtime"
  PRIVATE "${VENTURE_JAVA_HOME}/include")
if(WIN32)
  target_include_directories(venture_java_framework PRIVATE "${VENTURE_JAVA_HOME}/include/win32")
  target_sources(venture_java_framework PRIVATE ../../platform/win32/network.cpp ../../platform/win32/sqlite_library.cpp)
  target_link_libraries(venture_java_framework PRIVATE iphlpapi ole32 uuid)
else()
  target_include_directories(venture_java_framework PRIVATE "${VENTURE_JAVA_HOME}/include/linux")
  target_sources(venture_java_framework PRIVATE ../../platform/posix/network.cpp ../../platform/posix/sqlite_library.cpp)
  target_link_libraries(venture_java_framework PRIVATE ${CMAKE_DL_LIBS})
endif()
target_sources(venture_java_framework PRIVATE ../natives/horizon_bridge.cpp)
target_link_libraries(venture_java_framework PUBLIC venture_common venture_java venture_android venture_audio venture_gfx venture_ovrplatform)
target_link_libraries(venture_java_framework PRIVATE venture_hle)
target_link_libraries(venture_java_framework PRIVATE venture_vulkan)
add_dependencies(venture_java_framework venture-framework)
