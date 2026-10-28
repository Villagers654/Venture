include(Dependencies.cmake)
if(NOT TARGET Python3::Interpreter)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
set(generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(OUTPUT ${generated}/gl_dispatch.cpp ${generated}/egl_dispatch.inc ${generated}/egl_a32_adapters.inc
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/codegen/gl_thunks.py
    --gl ${venture_gl_registry_SOURCE_DIR}/xml/gl.xml
    --egl ${venture_egl_registry_SOURCE_DIR}/api/egl.xml --output ${generated}
  DEPENDS ${PROJECT_SOURCE_DIR}/tools/codegen/gl_thunks.py
    ${venture_gl_registry_SOURCE_DIR}/xml/gl.xml ${venture_egl_registry_SOURCE_DIR}/api/egl.xml
  VERBATIM)
add_library(venture_gfx STATIC
  angle.cpp blob_cache.cpp gpu_device.cpp pipeline_trace.cpp queue_probe.cpp queue_sync.cpp dispatch.cpp texture_decode.cpp xr_images.cpp a32_handles.cpp
  ../hle/egl/egl.cpp ../hle/gles/gles.cpp ../hle/gles/gles_a32.cpp ../hle/gles/buffers.cpp ../hle/gles/textures.cpp
  ${generated}/gl_dispatch.cpp ${generated}/egl_dispatch.inc)
if(WIN32)
  target_sources(venture_gfx PRIVATE ../platform/win32/gfx_library.cpp)
else()
  target_sources(venture_gfx PRIVATE ../platform/posix/gfx_library.cpp)
  target_link_libraries(venture_gfx PRIVATE ${CMAKE_DL_LIBS})
endif()
target_include_directories(venture_gfx PUBLIC ${PROJECT_SOURCE_DIR}/src/runtime)
target_include_directories(venture_gfx SYSTEM PUBLIC
  ${venture_vulkan_headers_SOURCE_DIR}/include ${venture_xr_headers_SOURCE_DIR}/include
  ${venture_gl_registry_SOURCE_DIR}/api ${venture_egl_registry_SOURCE_DIR}/api)
target_include_directories(venture_gfx PRIVATE ${generated} ${venture_astcenc_SOURCE_DIR}/Source)
target_compile_definitions(venture_gfx PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(venture_gfx PUBLIC venture_common venture_runtime venture_android venture_host_xr PRIVATE astcenc-sse2-static)
