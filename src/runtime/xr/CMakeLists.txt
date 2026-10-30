# Headers, registry and loader deliberately come from the same pinned SDK.
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(openxr_sdk)
if(MINGW)
  # SDK 1.1.63 applies its ELF version script to every non-MSVC compiler,
  # including MinGW. PE exports already use XRAPI_DLL_EXPORT.
  set_target_properties(openxr_loader PROPERTIES LINK_FLAGS "" PREFIX "")
endif()

# Use the same Vulkan declarations as GLES and guest Vulkan.
FetchContent_GetProperties(venture_vulkan_headers)
if(NOT TARGET Vulkan::Headers)
  add_library(venture_xr_vulkan_headers INTERFACE)
  target_include_directories(venture_xr_vulkan_headers SYSTEM INTERFACE ${venture_vulkan_headers_SOURCE_DIR}/include)
  add_library(Vulkan::Headers ALIAS venture_xr_vulkan_headers)
endif()
add_library(venture_host_xr STATIC host_xr.cpp display_memory.cpp)
target_include_directories(venture_host_xr PUBLIC ${PROJECT_SOURCE_DIR}/src/runtime)
target_link_libraries(venture_host_xr PUBLIC venture_common OpenXR::openxr_loader Vulkan::Headers)
if(WIN32)
  target_sources(venture_host_xr PRIVATE ../platform/win32/xr_native.cpp)
else()
  target_sources(venture_host_xr PRIVATE ../platform/posix/xr_native.cpp)
  target_link_libraries(venture_host_xr PRIVATE ${CMAKE_DL_LIBS})
endif()

add_library(venture_xr_capture STATIC capture.cpp)
target_include_directories(venture_xr_capture PUBLIC ${PROJECT_SOURCE_DIR}/src/runtime)
target_link_libraries(venture_xr_capture PUBLIC Vulkan::Headers OpenXR::headers PRIVATE zlibstatic)

add_library(venture_xr STATIC steam_link.cpp color_space.cpp color_images.cpp guest_images.cpp dev_dump.cpp session.cpp clock.cpp menu_chord.cpp bindings.cpp hands.cpp angle_images.cpp haptic_output.cpp haptic_sequence.cpp
  ../hle/openxr/state.cpp ../hle/openxr/openxr.cpp ../hle/openxr/graphics.cpp
  ../hle/openxr/extensions.cpp ../hle/openxr/actions.cpp ../hle/openxr/register.cpp ../hle/openxr/hands.cpp
  ../hle/openxr/spaces.cpp ../hle/openxr/haptics.cpp
  ../hle/openxr/android_surface.cpp
  ../hle/openxr/passthrough.cpp ../hle/openxr/passthrough_images.cpp
  ../hle/vrapi/vrapi.cpp ../hle/vrapi/swapchains.cpp ../hle/vrapi/input.cpp
  ../hle/vrapi/layers.cpp ../hle/vrapi/a32.cpp
  ../hle/vrapi/haptics.cpp
  ../hle/vrapi/vulkan_images.cpp
  ../hle/vrapi/properties.cpp ../hle/vrapi/integration.cpp ../hle/vrapi/register.cpp)
if(WIN32)
  target_sources(venture_xr PRIVATE ../platform/win32/xr_clock.cpp ../platform/win32/steam_link.cpp)
  target_link_libraries(venture_xr PRIVATE bcrypt)
else()
  target_sources(venture_xr PRIVATE ../platform/posix/xr_clock.cpp)
endif()
target_include_directories(venture_xr PUBLIC ${PROJECT_SOURCE_DIR}/src/runtime)
target_link_libraries(venture_xr PUBLIC venture_host_xr venture_runtime venture_vulkan PRIVATE venture_java venture_xr_capture)
if(EXISTS "${PROJECT_SOURCE_DIR}/src/runtime/gfx/xr_images.h")
  target_compile_definitions(venture_xr PRIVATE VENTURE_XR_HAS_ANGLE)
  target_link_libraries(venture_xr PUBLIC venture_gfx)
endif()

add_executable(venture_xr_probe ${PROJECT_SOURCE_DIR}/tools/xr_probe/main.cpp)
target_link_libraries(venture_xr_probe PRIVATE venture_xr)

# The headless fallback ships with the runtime, independently of test builds.
add_subdirectory(${PROJECT_SOURCE_DIR}/tests/mock_openxr_runtime
                 ${CMAKE_CURRENT_BINARY_DIR}/mock_openxr_runtime)
add_dependencies(venture-runtime venture_mock_openxr_runtime)

if(NOT TARGET Python3::Interpreter)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
set(color_generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
foreach(color_shader IN ITEMS vert frag ms_frag)
  if(color_shader STREQUAL "vert")
    set(color_source shaders/color.vert)
    set(color_defines)
  else()
    set(color_source shaders/color.frag)
    if(color_shader STREQUAL "ms_frag")
      set(color_defines -DMULTISAMPLE=1)
    else()
      set(color_defines)
    endif()
  endif()
  add_custom_command(OUTPUT ${color_generated}/color_${color_shader}.inc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${color_generated}
    COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.0
      ${color_defines} ${CMAKE_CURRENT_SOURCE_DIR}/${color_source}
      -o ${color_generated}/color_${color_shader}.spv
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
      ${color_generated}/color_${color_shader}.spv ${color_generated}/color_${color_shader}.inc
    DEPENDS glslang-standalone ${color_source} ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
    VERBATIM)
  target_sources(venture_xr PRIVATE ${color_generated}/color_${color_shader}.inc)
endforeach()
target_include_directories(venture_xr PRIVATE ${color_generated})
