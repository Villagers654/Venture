# Venture's Android 2D stack on desktop hosts (docs/design/layoutlib.md):
# Android Studio's layoutlib with Venture's host services and window manager.
#
#   bin/venture-framework-2d.jar  Venture's classes for a 2D process
#   bin/layoutlib/layoutlib.jar   layoutlib's framework, less the classes Venture supplies
#   bin/layoutlib/framework-res.apk, lib/, data/, licenses/
#
# Never built for Android (VENTURE_BUILD_RUNTIME is off there, and
# tools/android/check_apk.py refuses layoutlib files and classes in the APK).
include(${PROJECT_SOURCE_DIR}/cmake/Layoutlib.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/AndroidSdk.cmake)

set(venture_asm "${PROJECT_SOURCE_DIR}/.deps/asm/asm.jar")
if(NOT EXISTS "${venture_asm}")
  message(FATAL_ERROR "Missing ASM: python tools/bootstrap.py --jdk")
endif()
set(venture_java "${VENTURE_HOST_JAVA_HOME}/bin/java${VENTURE_HOST_JAVA_SUFFIX}")
set(venture_tools "${PROJECT_SOURCE_DIR}/tools/layoutlib")
set(venture_out "${CMAKE_BINARY_DIR}/bin/layoutlib")
set(venture_work "${CMAKE_CURRENT_BINARY_DIR}")
set(VENTURE_FRAMEWORK_2D_JAR "${CMAKE_BINARY_DIR}/bin/venture-framework-2d.jar" CACHE INTERNAL "Android 2D framework jar")
set(VENTURE_LAYOUTLIB_JAR "${venture_out}/layoutlib.jar" CACHE INTERNAL "layoutlib framework jar")
# Processed but not yet pruned of the member classes Venture's jar supplies.
set(venture_processed "${CMAKE_CURRENT_BINARY_DIR}/layoutlib-processed.jar")

set(venture_framework "${PROJECT_SOURCE_DIR}/src/runtime/java/framework")
set(venture_overlay "${CMAKE_CURRENT_SOURCE_DIR}/java")
set(venture_extra
  "${PROJECT_SOURCE_DIR}/src/runtime/java/bridge/com/venture/runtime/NativeMethods.java"
  "${PROJECT_SOURCE_DIR}/src/runtime/java/bridge/com/venture/runtime/MethodHooks.java"
  "${PROJECT_SOURCE_DIR}/src/runtime/java/bridge/com/venture/runtime/GuestReflection.java")
file(GLOB_RECURSE venture_framework_java CONFIGURE_DEPENDS "${venture_framework}/*.java")
file(GLOB_RECURSE venture_overlay_java CONFIGURE_DEPENDS "${venture_overlay}/*.java")
# Venture's changed layoutlib bridge classes live in its layoutlib fork.
include(FetchContent)
include("${PROJECT_SOURCE_DIR}/cmake/DataPaths.cmake")
FetchContent_Declare(venture_layoutlib_fork
  URL https://github.com/Villagers654/venture-layoutlib/archive/8ff654b48ff2922c7b1b0d20416ec0911ed9dac4.tar.gz
  URL_HASH SHA256=4da66e2506ba6ef0826befb0d9b7333ebd5df189e85eda90f77ae21d1a6dbf48
  DOWNLOAD_DIR "${VENTURE_GFX_DOWNLOAD_CACHE}"
  DOWNLOAD_NAME venture-layoutlib-8ff654b48ff2922c7b1b0d20416ec0911ed9dac4.tar.gz
  SOURCE_SUBDIR venture-no-build)
FetchContent_MakeAvailable(venture_layoutlib_fork)
set(venture_fork_classes
  android/content/res/AssetManager_Delegate android/os/ServiceManager android/util/Xml_Delegate
  android/view/SurfaceView com/android/internal/lang/System_Delegate
  com/android/layoutlib/bridge/Bridge com/android/layoutlib/bridge/android/AndroidLocale
  dalvik/system/VMRuntime_Delegate)
set(venture_fork_src "${venture_layoutlib_fork_SOURCE_DIR}/bridge/src")
set(venture_extra_args --fork-root "${venture_fork_src}")
set(venture_fork_java)
foreach(name IN LISTS venture_fork_classes)
  list(APPEND venture_extra_args --fork-class "${name}")
  list(APPEND venture_fork_java "${venture_fork_src}/${name}.java")
endforeach()
foreach(source IN LISTS venture_extra)
  list(APPEND venture_extra_args --extra "${source}")
endforeach()

# Which classes come from Venture (sources.txt) and which layoutlib classes
# they replace (replaced.txt, rules for ProcessLayoutlib).
add_custom_command(OUTPUT "${venture_work}/sources.txt" "${venture_work}/replaced.txt"
  COMMAND Python3::Interpreter "${venture_tools}/select_sources.py"
    --framework "${venture_framework}" --overlay "${venture_overlay}" ${venture_extra_args}
    --layoutlib "${VENTURE_LAYOUTLIB_CLASSES}" --keep "${CMAKE_CURRENT_SOURCE_DIR}/venture-keeps.txt"
    --sources "${venture_work}/sources.txt" --replaced "${venture_work}/replaced.txt"
  DEPENDS "${venture_tools}/select_sources.py" "${CMAKE_CURRENT_SOURCE_DIR}/venture-keeps.txt"
    ${venture_framework_java} ${venture_overlay_java} ${venture_extra} ${venture_fork_java}
  VERBATIM)

add_custom_command(OUTPUT "${venture_processed}" "${venture_work}/delegates.txt"
  COMMAND "${venture_java}" -cp "${venture_asm}" "${venture_tools}/ProcessLayoutlib.java"
    "${VENTURE_LAYOUTLIB_CLASSES}" "${venture_processed}" "${venture_work}/delegates.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/layoutlib-rules.txt" "${venture_work}/replaced.txt"
  DEPENDS "${venture_tools}/ProcessLayoutlib.java" "${CMAKE_CURRENT_SOURCE_DIR}/layoutlib-rules.txt"
    "${venture_work}/replaced.txt" "${VENTURE_LAYOUTLIB_CLASSES}/android/view/View.class"
  VERBATIM)

set(venture_classes "${venture_work}/classes-2d")
set(venture_resources "${PROJECT_SOURCE_DIR}/.deps/framework-res/root")
add_custom_command(OUTPUT "${VENTURE_FRAMEWORK_2D_JAR}" "${VENTURE_LAYOUTLIB_JAR}"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${venture_classes}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${venture_classes}" "${venture_out}"
  COMMAND "${VENTURE_JAVAC}" -encoding UTF-8 --release 21 -nowarn -d "${venture_classes}"
    -cp "${venture_processed}" "@${venture_work}/sources.txt"
  COMMAND "${VENTURE_JAR}" --create --file "${VENTURE_FRAMEWORK_2D_JAR}" -C "${venture_classes}" .
    -C "${venture_resources}" venture -C "${venture_framework}" NOTICE -C "${venture_framework}" APACHE-2.0.txt
  COMMAND "${venture_java}" -cp "${venture_asm}" "${venture_tools}/ProcessLayoutlib.java" --prune
    "${venture_processed}" "${VENTURE_FRAMEWORK_2D_JAR}" "${VENTURE_LAYOUTLIB_JAR}"
  # Every member either side names on the other must exist (NoSuchMethodError
  # otherwise, deep inside a layout pass), less the allowed unreachable ones.
  COMMAND "${venture_java}" -cp "${venture_asm}" "${venture_tools}/CheckLinkage.java"
    "${VENTURE_LAYOUTLIB_JAR}" "${VENTURE_FRAMEWORK_2D_JAR}" "${CMAKE_CURRENT_SOURCE_DIR}/linkage-allowed.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/linkage-known.txt"
  DEPENDS "${venture_work}/sources.txt" "${venture_processed}" "${venture_tools}/CheckLinkage.java"
    "${CMAKE_CURRENT_SOURCE_DIR}/linkage-allowed.txt" "${CMAKE_CURRENT_SOURCE_DIR}/linkage-known.txt" ${venture_framework_java} ${venture_overlay_java} ${venture_extra}
  VERBATIM)

# The framework resources layoutlib's classes were compiled against, built from
# the SDK platform's sources with their IDs pinned to layoutlib's.
cmake_path(GET VENTURE_ANDROID_JAR PARENT_PATH venture_platform)
# tests/ builds a 2D test APK with the same SDK (panel_lifecycle_test.py).
set_property(GLOBAL PROPERTY VENTURE_LAYOUTLIB_ANDROID_JAR "${VENTURE_ANDROID_JAR}")
set_property(GLOBAL PROPERTY VENTURE_LAYOUTLIB_BUILD_TOOLS "${VENTURE_ANDROID_BUILD_TOOLS}")
if(CMAKE_HOST_WIN32)
  set(venture_aapt2 "${VENTURE_ANDROID_BUILD_TOOLS}/aapt2.exe")
else()
  set(venture_aapt2 "${VENTURE_ANDROID_BUILD_TOOLS}/aapt2")
endif()
add_custom_command(OUTPUT "${venture_out}/framework-res.apk"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${venture_out}"
  COMMAND Python3::Interpreter "${venture_tools}/framework_res.py" --aapt2 "${venture_aapt2}" --java "${venture_java}"
    --layoutlib "${VENTURE_LAYOUTLIB_CLASSES}" --res "${venture_platform}/data/res"
    --out "${venture_out}/framework-res.apk" --work "${venture_work}/framework-res"
  DEPENDS "${venture_tools}/framework_res.py" "${venture_tools}/FrameworkResourceIds.java"
    "${VENTURE_LAYOUTLIB_CLASSES}/android/view/View.class"
  VERBATIM)

# The native runtime and its data. The source directories are named by their
# archive's hash and never change, so a stamp per version copies them once.
set(venture_stamp "${venture_work}/runtime-${VENTURE_LAYOUTLIB_VERSION}-2.stamp")
set(venture_copy_natives)
foreach(native IN LISTS VENTURE_LAYOUTLIB_NATIVES)
  list(APPEND venture_copy_natives COMMAND ${CMAKE_COMMAND} -E copy "${VENTURE_LAYOUTLIB_NATIVE_DIR}/${native}" "${venture_out}/lib/${native}")
endforeach()
add_custom_command(OUTPUT "${venture_stamp}"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${venture_out}/lib" "${venture_out}/data" "${venture_out}/licenses"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${venture_out}/lib" "${venture_out}/data"
  ${venture_copy_natives}
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${VENTURE_LAYOUTLIB_RUNTIME}/data/fonts" "${venture_out}/data/fonts"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${VENTURE_LAYOUTLIB_RUNTIME}/data/icu" "${venture_out}/data/icu"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${VENTURE_LAYOUTLIB_RUNTIME}/data/keyboards" "${venture_out}/data/keyboards"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${VENTURE_LAYOUTLIB_RUNTIME}/data/hyphen-data" "${venture_out}/data/hyphen-data"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${VENTURE_LAYOUTLIB_RUNTIME}/licenses" "${venture_out}/licenses"
  COMMAND ${CMAKE_COMMAND} -E copy "${VENTURE_LAYOUTLIB_RUNTIME}/build.prop" "${venture_out}/build.prop"
  COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_SOURCE_DIR}/LICENSES/Apache-2.0.txt" "${venture_out}/LICENSE"
  COMMAND ${CMAKE_COMMAND} -E touch "${venture_stamp}"
  DEPENDS "${PROJECT_SOURCE_DIR}/LICENSES/Apache-2.0.txt"
  VERBATIM)

add_custom_target(venture-layoutlib ALL DEPENDS "${VENTURE_FRAMEWORK_2D_JAR}" "${VENTURE_LAYOUTLIB_JAR}"
  "${venture_out}/framework-res.apk" "${venture_stamp}")
