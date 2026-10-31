set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/java/hosted_java.js")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/java/hosted_java.js" VENTURE_HOSTED_JAVA_SOURCE)
configure_file(java/hosted_java_source.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/hosted_java_source.h" @ONLY)

add_library(venture_runtime STATIC
  platform/runtime_diagnostic_directory.cpp
  core/low_address_space.cpp core/guest_storage.cpp core/guest_heap.cpp core/tls32.cpp
  core/address_space.cpp core/hle.cpp core/hle_unwind.cpp core/a32_abi.cpp core/a32_call.cpp core/a32_coverage.cpp
  core/generated/a32_ndk.cpp core/generated/a32_graphics.cpp core/generated/a32_vendor.cpp
  core/generated/a32_libc.cpp
  core/thread.cpp core/tls.cpp
  core/dev_game_control.cpp core/dev_il2cpp.cpp core/engine_policy.cpp core/engine_defaults.cpp
  core/profile.cpp core/frame_cpu.cpp core/startup_diagnostics.cpp
  core/pipeline_stats.cpp core/invalidation_stats.cpp core/av_telemetry.cpp
  core/wait.cpp core/readiness.cpp core/sync_census.cpp core/diagnostic_trace.cpp core/signals.cpp core/syscall.cpp core/a32_unwind.cpp core/shutdown_trace.cpp
  core/elf.cpp core/elf_relocations.cpp core/linker.cpp core/process_image.cpp core/process.cpp core/device_identity.cpp core/java_script_loader.cpp
  cpu/dynarmic_backend.cpp cpu/dynarmic_a32_backend.cpp cpu/block_list.cpp cpu/stubs.cpp cpu/kuser.cpp)
target_include_directories(venture_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(venture_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(venture_runtime PUBLIC venture_common PRIVATE dynarmic miniz merry::mcl fmt::fmt)
target_include_directories(venture_runtime SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
target_compile_options(venture_runtime PRIVATE -Wall -Wextra)
target_compile_definitions(venture_runtime PRIVATE
  $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:VENTURE_DEVELOPER_CONTROL=1>)
if(WIN32)
  set_source_files_properties(platform/win32/memory.cpp PROPERTIES COMPILE_OPTIONS "-fms-extensions")
  target_sources(venture_runtime PRIVATE platform/win32/crash_report.cpp)
  target_sources(venture_runtime PRIVATE platform/win32/runtime_child.cpp)
  target_sources(venture_runtime PRIVATE platform/win32/runtime_stop.cpp)
  target_sources(venture_runtime PRIVATE platform/win32/file_activity.cpp)
  target_sources(venture_runtime PRIVATE platform/win32/audio_timer.cpp)
  target_sources(venture_runtime PRIVATE platform/win32/timed_wait.cpp)
  target_sources(venture_runtime PRIVATE platform/win32/profile.cpp)
  target_sources(venture_runtime PRIVATE platform/win32/runtime_qos.cpp)
  target_sources(venture_runtime PRIVATE platform/win32/cpu_affinity.cpp)
  target_sources(venture_runtime PRIVATE platform/win32/memory.cpp platform/win32/low_memory.cpp platform/win32/environment.cpp platform/win32/thread_priority.cpp)
  target_link_libraries(venture_runtime PRIVATE mincore)
else()
  target_sources(venture_runtime PRIVATE platform/posix/crash_report.cpp)
  target_link_libraries(venture_runtime PRIVATE ${CMAKE_DL_LIBS})
  target_sources(venture_runtime PRIVATE platform/posix/runtime_child.cpp)
  target_sources(venture_runtime PRIVATE platform/posix/runtime_stop.cpp)
  target_sources(venture_runtime PRIVATE platform/posix/file_activity.cpp)
  target_sources(venture_runtime PRIVATE platform/posix/audio_timer.cpp)
  target_sources(venture_runtime PRIVATE platform/posix/timed_wait.cpp)
  target_sources(venture_runtime PRIVATE platform/posix/profile.cpp)
  target_sources(venture_runtime PRIVATE platform/posix/runtime_qos.cpp)
  target_sources(venture_runtime PRIVATE platform/posix/cpu_affinity.cpp)
  target_sources(venture_runtime PRIVATE platform/posix/memory.cpp platform/posix/low_memory.cpp platform/posix/environment.cpp platform/posix/thread_priority.cpp)
endif()
add_subdirectory(hle)
add_subdirectory(hle/steam)
target_link_libraries(venture_runtime PRIVATE mimalloc-static)
add_executable(venture-runtime main.cpp)
if(TARGET venture_sqlite3)
  add_dependencies(venture-runtime venture_sqlite3)  # loaded at run time
endif()
if(WIN32)
  target_sources(venture-runtime PRIVATE platform/win32/runtime.rc)
  target_include_directories(venture-runtime PRIVATE platform/win32)
  target_sources(venture-runtime PRIVATE platform/win32/low_memory_startup.cpp)
  target_link_options(venture-runtime PRIVATE "LINKER:--entry=venture_low_memory_startup")
  add_custom_command(TARGET venture-runtime POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/profile_symbols.py"
      "${PROJECT_SOURCE_DIR}/.deps/llvm-mingw/bin/llvm-nm.exe"
      "$<TARGET_FILE:venture-runtime>" VERBATIM)
endif()
if(NOT WIN32)
  # Host symbol index for VENTURE_PROFILE attribution, as on Windows.
  add_custom_command(TARGET venture-runtime POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/profile_symbols.py"
      "${CMAKE_NM}" "$<TARGET_FILE:venture-runtime>" VERBATIM)
  set_property(TARGET venture-runtime PROPERTY POSITION_INDEPENDENT_CODE ON)
  target_link_options(venture-runtime PRIVATE -pie)
endif()
# The ordinary launch path loads ANGLE from the executable directory. Stage the
# provisioned, pinned runtime so a fresh build works without a developer env var.
set(VENTURE_ANGLE_ROOT "${PROJECT_SOURCE_DIR}/.deps/angle/d2e5e488da0c6215f8276e5b8d992a4a6ec20c32"
    CACHE PATH "Installed Venture ANGLE dependency")
if(WIN32)
  set(venture_angle_libraries libEGL.dll libGLESv2.dll)
else()
  set(venture_angle_libraries libEGL.so libGLESv2.so)
endif()
foreach(library IN LISTS venture_angle_libraries)
  if(EXISTS "${VENTURE_ANGLE_ROOT}/bin/${library}")
    add_custom_command(TARGET venture-runtime POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VENTURE_ANGLE_ROOT}/bin/${library}"
        "$<TARGET_FILE_DIR:venture-runtime>/${library}" VERBATIM)
  endif()
endforeach()
function(venture_stage_media target)
  foreach(component IN LISTS venture_ffmpeg_components)
    get_filename_component(name "${component}" NAME)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${VENTURE_FFMPEG_ROOT}/${component}")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VENTURE_FFMPEG_ROOT}/${component}"
        "$<TARGET_FILE_DIR:${target}>/${name}" VERBATIM)
  endforeach()
endfunction()
venture_stage_media(venture-runtime)
add_custom_command(TARGET venture-runtime POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:venture-runtime>/LICENSES"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/LICENSES/LGPL-2.1.txt"
    "$<TARGET_FILE_DIR:venture-runtime>/LICENSES/LGPL-2.1.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VENTURE_FFMPEG_ROOT}/venture-ffmpeg.json"
    "$<TARGET_FILE_DIR:venture-runtime>/venture-ffmpeg.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/.deps/ffmpeg/archive/ffmpeg-8.1.3.tar.xz"
    "$<TARGET_FILE_DIR:venture-runtime>/LICENSES/ffmpeg-8.1.3.tar.xz"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/.deps/ffmpeg/signature/release.asc"
    "$<TARGET_FILE_DIR:venture-runtime>/LICENSES/ffmpeg-8.1.3.tar.xz.asc" VERBATIM)
target_link_libraries(venture-runtime PRIVATE venture_common)
target_link_libraries(venture-runtime PRIVATE venture_steam)
target_link_libraries(venture-runtime PRIVATE venture_ovrplatform)

# Android NDK HLE shares the integrated core and guest VFS.
add_library(venture_android STATIC
  android/handles32.cpp
  android/package_assets.cpp
  android/native_window.cpp
  android/native_activity.cpp
  android/looper.cpp
  android/input.cpp
  android/hardware_buffer.cpp
  hle/libandroid/libandroid.cpp
  hle/libandroid/assets.cpp
  hle/libandroid/windows.cpp
  hle/libandroid/looper.cpp
  hle/libandroid/configuration.cpp
  hle/libandroid/input.cpp
  hle/libandroid/choreographer.cpp
  hle/libandroid/services.cpp
  hle/libandroid/hardware_buffer.cpp
  hle/libmediandk/image_reader.cpp
  hle/libmediandk/media.cpp
  hle/libcamera2ndk/camera.cpp
)
target_include_directories(venture_android PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(venture_android PUBLIC venture_common venture_runtime PRIVATE venture_hle venture_java zlibstatic)
target_link_libraries(venture-runtime PRIVATE venture_runtime venture_hle)
target_compile_options(venture-runtime PRIVATE -Wall -Wextra)

# Keep ownership of core tests here; the libc worktrees add their own test sources.
if(VENTURE_BUILD_TESTS)
  function(venture_add_core_tests)
    file(GLOB core_tests CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tests/runtime/core_*_test.cpp")
    list(REMOVE_ITEM core_tests "${PROJECT_SOURCE_DIR}/tests/runtime/core_a32_abi_test.cpp")
    target_sources(venture_tests PRIVATE ${core_tests})
    target_link_libraries(venture_tests PRIVATE venture_runtime miniz)
    target_compile_options(venture_tests PRIVATE -Wall -Wextra)
  endfunction()
  cmake_language(DEFER DIRECTORY ${PROJECT_SOURCE_DIR} CALL venture_add_core_tests)
endif()

add_subdirectory(audio)
target_link_libraries(venture_android PRIVATE venture_audio)
target_link_libraries(venture-runtime PRIVATE venture_audio)

add_subdirectory(java)
target_link_libraries(venture-runtime PRIVATE venture_java)

add_subdirectory(gfx)
target_link_libraries(venture-runtime PRIVATE venture_gfx venture_android)

add_subdirectory(hle/vulkan)
target_link_libraries(venture-runtime PRIVATE venture_vulkan)

# These configured paths are development fallbacks for the executable-relative
# layout documented in java/support_files.h. --java-home still overrides it.
set(VENTURE_GUEST_TEST_JAVA_CLASSES "${PROJECT_BINARY_DIR}/guest/java-classes")
add_custom_command(OUTPUT "${VENTURE_GUEST_TEST_JAVA_CLASSES}/org/venture/tests/VtestJni.class"
  COMMAND "${VENTURE_JAVA_HOME}/bin/javac" --release 21 -d "${VENTURE_GUEST_TEST_JAVA_CLASSES}"
    "${PROJECT_SOURCE_DIR}/tests/guest/java/VtestJni.java"
  DEPENDS "${PROJECT_SOURCE_DIR}/tests/guest/java/VtestJni.java" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/bin/guest/java-classes/org/venture/tests/VtestJni.class"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${VENTURE_GUEST_TEST_JAVA_CLASSES}"
    "${CMAKE_BINARY_DIR}/bin/guest/java-classes"
  DEPENDS "${VENTURE_GUEST_TEST_JAVA_CLASSES}/org/venture/tests/VtestJni.class")
add_custom_target(venture_guest_java_fixture DEPENDS "${CMAKE_BINARY_DIR}/bin/guest/java-classes/org/venture/tests/VtestJni.class")
add_dependencies(venture-runtime venture_guest_java_fixture)
add_subdirectory(java/framework)
target_link_libraries(venture-runtime PRIVATE venture_java_framework)

add_subdirectory(xr)
target_link_libraries(venture-runtime PRIVATE venture_xr)
target_sources(venture-runtime PRIVATE java/app_runtime.cpp java/support_files.cpp)

# The framework JAR is already built in bin/. Stage the bridge and the complete
# converter/JDK payload too; a copied bin/ never needs the source checkout.
if(NOT TARGET Python3::Interpreter)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
add_custom_target(venture_java_support ALL
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/dex/stage_runtime.py"
    --bin "$<TARGET_FILE_DIR:venture-runtime>" --jdk "${VENTURE_JAVA_HOME}" --bridge "${VENTURE_JAVA_BRIDGE_JAR}"
    --agent "$<TARGET_FILE:venture_jvmti_agent>"
  DEPENDS venture_java_bridge_jar venture-framework venture_jvmti_agent VERBATIM)
add_dependencies(venture-runtime venture_java_support)
if(WIN32)
  add_dependencies(venture-runtime venture-java)
endif()

if(WIN32)
  target_compile_definitions(venture-runtime PRIVATE VENTURE_JAVA_PATH_SEPARATOR=59)
else()
  target_compile_definitions(venture-runtime PRIVATE VENTURE_JAVA_PATH_SEPARATOR=58)
endif()
