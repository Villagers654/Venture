include(FetchContent)
if(NOT TARGET Python3::Interpreter)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

function(venture_vulkan_archive output name revision url)
  # bootstrap/research downloads are shared across worktrees. FetchContent still
  # checks URL_HASH for a local archive, so cache contents are never trusted.
  set(cached "${VENTURE_GFX_DOWNLOAD_CACHE}/${name}-${revision}.tar.gz")
  file(TO_CMAKE_PATH "${cached}" cached)
  if(WIN32 AND EXISTS "${cached}")
    set(${output} "${cached}" PARENT_SCOPE)
  else()
    set(${output} "${url}" PARENT_SCOPE)
  endif()
endfunction()

# The graphics target provisions the shared pinned Vulkan registry and headers.
FetchContent_GetProperties(venture_vulkan_headers)
FetchContent_Declare(venture_vma
  URL https://codeload.github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tar.gz/refs/tags/v3.4.0
  URL_HASH SHA256=822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1
  SYSTEM)
FetchContent_MakeAvailable(venture_vma)
configure_file(${venture_vma_SOURCE_DIR}/LICENSE.txt
  ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/LICENSES/VMA-MIT.txt COPYONLY)
install(FILES ${venture_vma_SOURCE_DIR}/LICENSE.txt DESTINATION LICENSES RENAME VMA-MIT.txt)

set(vk_generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(OUTPUT ${vk_generated}/vk_commands.inc ${vk_generated}/vk_structs.inc ${vk_generated}/vk_members.inc
  ${vk_generated}/vk_borrow_traits.inc ${vk_generated}/vk_borrow_members.inc
  ${vk_generated}/vk_plain_members.inc ${vk_generated}/vk_plain_borrow.inc ${vk_generated}/vk_structure_extensions.inc
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/codegen/vk_generate.py
          --registry ${venture_vulkan_headers_SOURCE_DIR}/registry/vk.xml --out ${vk_generated}
  DEPENDS ${PROJECT_SOURCE_DIR}/tools/codegen/vk_generate.py
          ${venture_vulkan_headers_SOURCE_DIR}/registry/vk.xml
  VERBATIM)

add_library(venture_vulkan STATIC
  vulkan.cpp multiview.cpp multiview_pipeline.cpp callbacks.cpp chain.cpp hardware_buffer.cpp sync_fd.cpp timing.cpp window_readback.cpp wsi.cpp address_log.cpp breadcrumbs.cpp shader.cpp full_rate_image.cpp
  ${vk_generated}/vk_commands.inc ${vk_generated}/vk_structs.inc ${vk_generated}/vk_members.inc
  ${vk_generated}/vk_borrow_traits.inc ${vk_generated}/vk_borrow_members.inc
  ${vk_generated}/vk_plain_members.inc ${vk_generated}/vk_plain_borrow.inc ${vk_generated}/vk_structure_extensions.inc)
target_include_directories(venture_vulkan PUBLIC ${PROJECT_SOURCE_DIR}/src/runtime
  ${venture_vulkan_headers_SOURCE_DIR}/include ${vk_generated})
target_link_libraries(venture_vulkan PUBLIC venture_common venture_runtime venture_gfx venture_android)
target_link_libraries(venture_vulkan PUBLIC venture_hle)
target_link_libraries(venture_vulkan PRIVATE GPUOpen::VulkanMemoryAllocator)
target_sources(venture_vulkan PRIVATE texture_memory.cpp serialized_queues.cpp deferred_queue_submissions.cpp)
if(WIN32)
  target_sources(venture_vulkan PRIVATE ../../platform/win32/vulkan_external.cpp)
else()
  target_sources(venture_vulkan PRIVATE ../../platform/posix/vulkan_external.cpp)
endif()
target_compile_definitions(venture_vulkan PUBLIC VK_NO_PROTOTYPES)

# Build the shader compiler with the same llvm-mingw toolchain. No SDK install
# and no prebuilt compiler binary is required.
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
venture_vulkan_archive(glslang_archive glslang 9a1dc7519823c28fe46a4e7092ed512c040937fd
  https://codeload.github.com/KhronosGroup/glslang/tar.gz/9a1dc7519823c28fe46a4e7092ed512c040937fd)
FetchContent_Declare(venture_glslang
  URL "${glslang_archive}"
  URL_HASH SHA256=14d017c1adb04f893b32d86c3f5df50313a752a4d27ef4084e545514ca0eee90
  SYSTEM)
FetchContent_MakeAvailable(venture_glslang)
add_custom_command(OUTPUT ${vk_generated}/texture_decode.inc
  COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.0
    -I${CMAKE_CURRENT_SOURCE_DIR}/shaders ${CMAKE_CURRENT_SOURCE_DIR}/shaders/decode.comp
    -o ${vk_generated}/texture_decode.spv
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
    ${vk_generated}/texture_decode.spv ${vk_generated}/texture_decode.inc
  DEPENDS glslang-standalone shaders/decode.comp shaders/astc_decompressor.glsl
    shaders/astc_lookup_tables.glsl shaders/etc2.glsl ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
  VERBATIM)
add_custom_command(OUTPUT ${vk_generated}/texture_depth.inc
  COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.0
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/depth_convert.comp -o ${vk_generated}/texture_depth.spv
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
    ${vk_generated}/texture_depth.spv ${vk_generated}/texture_depth.inc
  DEPENDS glslang-standalone shaders/depth_convert.comp ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
  VERBATIM)
add_custom_command(OUTPUT ${vk_generated}/texture_bc7.inc
  COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.0
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/bc7_encode.comp -o ${vk_generated}/texture_bc7.spv
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
    ${vk_generated}/texture_bc7.spv ${vk_generated}/texture_bc7.inc
  DEPENDS glslang-standalone shaders/bc7_encode.comp ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
  VERBATIM)
target_sources(venture_vulkan PRIVATE texture.cpp render_area.cpp render_checks.cpp subpass_sync.cpp deferred_free.cpp ${vk_generated}/texture_decode.inc ${vk_generated}/texture_depth.inc ${vk_generated}/texture_bc7.inc)
foreach(mode float_array srgb_array float_volume srgb_volume unorm_array unorm_volume)
  set(image_defines -DIMAGE_OUTPUT=1)
  if(mode MATCHES "srgb|unorm")
    list(APPEND image_defines -DIMAGE_OUTPUT_8BIT=1)
  else()
    list(APPEND image_defines -DIMAGE_FLOAT=1)
  endif()
  if(mode MATCHES "volume")
    list(APPEND image_defines -DIMAGE_VOLUME=1)
  endif()
  add_custom_command(OUTPUT ${vk_generated}/texture_decode_${mode}.inc
    COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.0 ${image_defines}
      -I${CMAKE_CURRENT_SOURCE_DIR}/shaders ${CMAKE_CURRENT_SOURCE_DIR}/shaders/decode.comp
      -o ${vk_generated}/texture_decode_${mode}.spv
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
      ${vk_generated}/texture_decode_${mode}.spv ${vk_generated}/texture_decode_${mode}.inc
    DEPENDS glslang-standalone shaders/decode.comp shaders/astc_decompressor.glsl
      shaders/astc_lookup_tables.glsl shaders/etc2.glsl ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
    VERBATIM)
  target_sources(venture_vulkan PRIVATE ${vk_generated}/texture_decode_${mode}.inc)
endforeach()
