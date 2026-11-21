# Libraries the Venture APK adds to the games it installs on Android headsets
# (docs/design/native-headsets.md, section 5): carried in the APK as assets and
# written into each adapted game.
enable_language(ASM)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# libovrplatformloader.so: the Meta Platform SDK served by Venture's Platform
# HLE, with credentials from the Venture app's broker.
set(platform_generated ${CMAKE_CURRENT_BINARY_DIR}/platform)
file(GLOB platform_hle_sources CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/runtime/hle/ovrplatform/*.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/hle/ovrplatform/*.inc)
# The 64-bit library's exports are native entry image slots; the 32-bit one's
# (armeabi-v7a, for ARM32 games) are ARM slots converted by signature
# (platform/a32_call.h).
if(ANDROID_ABI STREQUAL "armeabi-v7a")
  set(platform_isa --isa arm --signatures ${platform_generated}/export_signatures.inc)
  set(platform_outputs ${platform_generated}/export_signatures.inc)
  set(platform_isa_sources platform/a32_entry.S platform/a32_platform.cpp platform/a32_call.cpp)
else()
  set(platform_isa)
  set(platform_outputs)
  set(platform_isa_sources ${PROJECT_SOURCE_DIR}/src/runtime/cpu/native_entry_a64.S)
endif()
add_custom_command(
  OUTPUT ${platform_generated}/exports.S ${platform_generated}/export_names.inc ${platform_outputs}
  COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/android/platform_exports.py
    --repo ${PROJECT_SOURCE_DIR} --asm ${platform_generated}/exports.S --names ${platform_generated}/export_names.inc
    ${platform_isa}
  DEPENDS ${PROJECT_SOURCE_DIR}/tools/android/platform_exports.py
    ${PROJECT_SOURCE_DIR}/tests/runtime/ovrplatform_coverage.txt ${platform_hle_sources}
    ${PROJECT_SOURCE_DIR}/src/runtime/core/generated/a32_vendor.json
  COMMENT "Platform loader exports"
  VERBATIM)
add_library(venture_platform_loader SHARED
  game_log.cpp
  platform/loader.cpp
  platform/native_platform.cpp
  platform/microphone.cpp
  platform/memory.cpp
  ${platform_generated}/exports.S
  ${platform_isa_sources}
  # The handlers and their table, without the guest runtime's parts.
  ${PROJECT_SOURCE_DIR}/src/runtime/hle/ovrplatform/hle.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/cpu/native_entry.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/cpu/stubs.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/cpu/guest_entry.cpp)
set_target_properties(venture_platform_loader PROPERTIES OUTPUT_NAME ovrplatformloader)
target_compile_definitions(venture_platform_loader PRIVATE VENTURE_PLATFORM_NATIVE_HOST)
target_include_directories(venture_platform_loader PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/runtime
  ${platform_generated})
target_link_libraries(venture_platform_loader PRIVATE venture_ovrplatform venture_common log aaudio)
# Only the Platform SDK's functions and the JNI entry points are exported
# (platform/exports.map).
target_link_options(venture_platform_loader PRIVATE -Wl,--no-undefined -Wl,--exclude-libs,ALL
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/platform/exports.map)
set_property(TARGET venture_platform_loader APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/exports.map)

# libvrapi.so: the VrApi core (src/runtime/hle/vrapi, shared with the PC
# runtime) on the headset's OpenXR runtime, reached through the Khronos Android
# loader linked in. The game renders with its own GLES context into the
# runtime's swapchain images (vrapi/swapchains.cpp).
include(${PROJECT_SOURCE_DIR}/cmake/OpenXRSdk.cmake)
set(rt ${PROJECT_SOURCE_DIR}/src/runtime)
add_library(venture_vrapi SHARED
  game_log.cpp
  vrapi/exports.cpp
  vrapi/host.cpp
  vrapi/host_xr.cpp
  vrapi/runtime.cpp
  vrapi/swapchains.cpp
  vrapi/vulkan.cpp
  ${rt}/hle/vrapi/vrapi.cpp ${rt}/hle/vrapi/swapchain_common.cpp ${rt}/hle/vrapi/input.cpp ${rt}/hle/vrapi/haptics.cpp
  ${rt}/hle/vrapi/properties.cpp ${rt}/hle/vrapi/layers.cpp
  ${rt}/xr/session.cpp ${rt}/xr/sanitize.cpp ${rt}/xr/clock.cpp ${rt}/xr/reference_spaces.cpp ${rt}/xr/bindings.cpp ${rt}/xr/openvr_devices.cpp
  ${rt}/xr/hands.cpp ${rt}/xr/haptic_output.cpp ${rt}/xr/haptic_sequence.cpp ${rt}/xr/menu_chord.cpp
  ${rt}/xr/grip_toggle.cpp ${rt}/xr/display_override.cpp ${rt}/xr/display_memory.cpp ${rt}/xr/eye_scale.cpp
  ${rt}/platform/posix/xr_clock.cpp
  ${rt}/core/device_identity.cpp
  # The XR call trace marks each call for the stall finder (core/stall_watch.h).
  ${rt}/core/stall_watch.cpp ${rt}/core/thread_lifetime.cpp)
# The 32-bit library converts the records that hold pointers (ILP32 layouts).
if(ANDROID_ABI STREQUAL "armeabi-v7a")
  target_sources(venture_vrapi PRIVATE vrapi/exports_ilp32.cpp)
endif()
set_target_properties(venture_vrapi PROPERTIES OUTPUT_NAME vrapi)
target_include_directories(venture_vrapi PRIVATE ${PROJECT_SOURCE_DIR}/src ${rt})
target_link_libraries(venture_vrapi PRIVATE venture_common openxr_loader OpenXR::headers GLESv3 EGL log ${CMAKE_DL_LIBS})
# Only VrApi's functions are exported (vrapi/exports.map).
target_link_options(venture_vrapi PRIVATE -Wl,--no-undefined -Wl,--exclude-libs,ALL -Wl,--error-limit=0
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/vrapi/exports.map)
set_property(TARGET venture_vrapi APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vrapi/exports.map)

# The Platform loader under a renamed copy's name (platform/alias.c): the
# adapter patches the placeholder soname to that name.
add_library(venture_platform_alias SHARED platform/alias.c)
set_target_properties(venture_platform_alias PROPERTIES OUTPUT_NAME venture_platform_alias)
target_link_libraries(venture_platform_alias PRIVATE venture_platform_loader)
target_link_options(venture_platform_alias PRIVATE -Wl,--no-as-needed
  -Wl,-soname,libventure_platform_alias_placeholder_soname_long_enough_for_any_game_library_name_0123456789abcdef0123456789abcdef0123456789.so
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/platform/alias.map)
set_property(TARGET venture_platform_alias APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/alias.map)

# libopenxr_loader.so: the pinned Khronos Android loader as the shared library
# games load, for those whose bundled loader finds only Meta's runtime.
add_library(venture_openxr_loader SHARED vrapi/openxr_loader.c)
set_target_properties(venture_openxr_loader PROPERTIES OUTPUT_NAME openxr_loader)
target_link_libraries(venture_openxr_loader PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,openxr_loader>" log android)
target_link_options(venture_openxr_loader PRIVATE -Wl,--no-undefined
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/vrapi/openxr_loader.map)
set_property(TARGET venture_openxr_loader APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vrapi/openxr_loader.map)

# The test game of tools/android/headset_play_test.py (tests/android/test_game):
# a NativeActivity library and stand-ins for Meta's libvrapi.so and
# libovrplatformloader.so, in test_game/lib, apart from Venture's own.
set(venture_test_game ${PROJECT_SOURCE_DIR}/tests/android/test_game)
add_library(venture_test_game SHARED EXCLUDE_FROM_ALL ${venture_test_game}/game.c)
target_link_libraries(venture_test_game PRIVATE android log)
set_target_properties(venture_test_game PROPERTIES OUTPUT_NAME testgame)
foreach(name vrapi ovrplatformloader)
  add_library(venture_test_game_${name} SHARED EXCLUDE_FROM_ALL ${venture_test_game}/stub.c)
  set_target_properties(venture_test_game_${name} PROPERTIES OUTPUT_NAME ${name})
endforeach()
set_target_properties(venture_test_game venture_test_game_vrapi venture_test_game_ovrplatformloader PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test_game/lib)
