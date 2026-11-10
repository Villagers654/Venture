# Mixed reality fixtures: the production passthrough, frame and image code on
# real Vulkan (lavapipe in CI) against a scripted OpenXR host, external
# passthrough source and OpenVR camera. The end-to-end backend tests through the mock runtime are in
# runtime/xr_passthrough_test.cpp (venture_xr_tests).
set(passthrough_color_generated "$<TARGET_PROPERTY:venture_xr,BINARY_DIR>/generated")
set(passthrough_vk_generated "$<TARGET_PROPERTY:venture_vulkan,BINARY_DIR>/generated")
# Production sources shared by both fixtures, compiled once.
add_library(venture_passthrough_fixture OBJECT
  ${PROJECT_SOURCE_DIR}/src/common/environment.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/hle/openxr/passthrough.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/hle/openxr/passthrough_images.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/hle/openxr/passthrough_camera.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/hle/openxr/image_upload.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/hle/openxr/state.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/xr/color_images.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/xr/color_space.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/xr/guest_images.cpp
  # Venture NR's hook in the host copy (off: VENTURE_NR is unset).
  ${PROJECT_SOURCE_DIR}/src/runtime/xr/ngx_bridge.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/xr/venture_nr.cpp)
add_executable(venture_passthrough_tests tests.cpp support.cpp ../test_main.cpp)
add_executable(venture_passthrough_lifecycle_tests lifecycle.cpp lifecycle_host.cpp support.cpp ../test_main.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/hle/openxr/openxr.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/hle/openxr/graphics.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/hle/openxr/sphere_layers.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/xr/sphere_images.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/xr/session.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/xr/display_override.cpp)
# The lifecycle fixture runs the real xr::Session.
target_compile_definitions(venture_passthrough_lifecycle_tests PRIVATE VENTURE_PASSTHROUGH_REAL_SESSION=1)
foreach(target IN ITEMS venture_passthrough_fixture venture_passthrough_tests venture_passthrough_lifecycle_tests)
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/runtime
    ${passthrough_color_generated} ${passthrough_vk_generated})
  target_link_libraries(${target} PRIVATE OpenXR::headers Vulkan::Headers nlohmann_json::nlohmann_json doctest::doctest)
  target_compile_definitions(${target} PRIVATE VK_NO_PROTOTYPES)
  if(WIN32)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_definitions(${target} PRIVATE DOCTEST_CONFIG_NO_POSIX_SIGNALS)
  endif()
  # Production code the fixtures do not reach stays out of the link (see -Wl,--gc-sections).
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
  endif()
  # The generated shaders and Vulkan member tables.
  add_dependencies(${target} venture_xr venture_vulkan)
endforeach()
foreach(target IN ITEMS venture_passthrough_tests venture_passthrough_lifecycle_tests)
  target_link_libraries(${target} PRIVATE venture_passthrough_fixture ${CMAKE_DL_LIBS})
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(${target} PRIVATE -Wl,--gc-sections)
  endif()
  if(MINGW)
    # The PE linker reports undefined symbols before dropping unreached sections;
    # as with the ELF link, runtime services the fixtures never reach stay unresolved.
    target_link_options(${target} PRIVATE -Wl,--Xlink=-force:unresolved)
  endif()
endforeach()
add_test(NAME passthrough COMMAND venture_passthrough_tests)
add_test(NAME passthrough_production_lifecycle COMMAND venture_passthrough_lifecycle_tests)

# The ordinary VR colour shaders stay byte-identical to the retained upstream copy.
add_test(NAME passthrough_ordinary_shader_unchanged COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/shader_variant_test.py" "$<TARGET_FILE:glslang-standalone>" "${passthrough_color_generated}")

# The production host initializer's extension requests; one process each, as
# host_xr() keeps a process-lifetime instance.
add_executable(venture_passthrough_host_tests host_extensions.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/xr/host_xr.cpp
  ${PROJECT_SOURCE_DIR}/src/common/environment.cpp)
target_include_directories(venture_passthrough_host_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/runtime)
target_link_libraries(venture_passthrough_host_tests PRIVATE OpenXR::headers Vulkan::Headers)
if(WIN32)
  target_compile_definitions(venture_passthrough_host_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
function(add_passthrough_host_test name setting expected profile)
  if(setting STREQUAL "<unset>")
    set(environment --unset=VENTURE_PASSTHROUGH)
  else()
    set(environment "VENTURE_PASSTHROUGH=${setting}")
  endif()
  add_test(NAME "passthrough_host_${name}" COMMAND ${CMAKE_COMMAND} -E env ${environment}
    $<TARGET_FILE:venture_passthrough_host_tests> ${expected} ${profile})
endfunction()
# On unless exactly "0".
add_passthrough_host_test(default "<unset>" 1 both)
add_passthrough_host_test(off "0" 0 both)
add_passthrough_host_test(on "1" 1 both)
add_passthrough_host_test(word "true" 1 both)
add_passthrough_host_test(padded_zero " 0" 1 both)
foreach(profile IN ITEMS none passthrough mesh htc)
  add_passthrough_host_test("available_${profile}" "<unset>" 1 ${profile})
endforeach()
