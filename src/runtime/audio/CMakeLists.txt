add_library(venture_audio STATIC
  miniaudio_impl.c
  audio.cpp
  output_mirror.cpp
  ../media/media.cpp
  ../media/vulkan_video.cpp
  ../hle/aaudio/aaudio.cpp
  ../hle/opensles/opensles.cpp
  ../hle/ovrplatform/microphone.cpp
)
target_include_directories(venture_audio PUBLIC ${PROJECT_SOURCE_DIR}/src/runtime)
target_include_directories(venture_audio SYSTEM PUBLIC ${miniaudio_SOURCE_DIR})
target_compile_definitions(venture_audio PUBLIC MA_NO_DECODING MA_NO_ENCODING
  MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE MA_NO_GENERATION)
target_link_libraries(venture_audio PUBLIC venture_common venture_runtime venture_hle)
if(WIN32)
  set(media_install install)
else()
  set(media_install install-posix)
endif()
set(VENTURE_FFMPEG_ROOT "${PROJECT_SOURCE_DIR}/.deps/ffmpeg/${media_install}" CACHE PATH "Verified LGPL FFmpeg build")
if(NOT EXISTS "${VENTURE_FFMPEG_ROOT}/include/libavformat/avformat.h")
  message(FATAL_ERROR "Missing FFmpeg headers: run python tools/bootstrap.py --ffmpeg")
endif()
target_include_directories(venture_audio SYSTEM PRIVATE "${VENTURE_FFMPEG_ROOT}/include")
# Vulkan Video pictures are converted by a compute shader (media/vulkan_video.cpp).
# Vulkan::Headers and glslang-standalone are defined by later subdirectories;
# both resolve at generation time.
target_link_libraries(venture_audio PRIVATE Vulkan::Headers)
if(NOT TARGET Python3::Interpreter)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
set(media_generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(OUTPUT ${media_generated}/yuv_rgba_comp.inc
  COMMAND ${CMAKE_COMMAND} -E make_directory ${media_generated}
  COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.3
    ${PROJECT_SOURCE_DIR}/src/runtime/media/shaders/yuv_rgba.comp -o ${media_generated}/yuv_rgba_comp.spv
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
    ${media_generated}/yuv_rgba_comp.spv ${media_generated}/yuv_rgba_comp.inc
  DEPENDS glslang-standalone ${PROJECT_SOURCE_DIR}/src/runtime/media/shaders/yuv_rgba.comp
    ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
  VERBATIM)
target_sources(venture_audio PRIVATE ${media_generated}/yuv_rgba_comp.inc)
target_include_directories(venture_audio PRIVATE ${media_generated})
if(WIN32)
  target_compile_definitions(venture_audio PUBLIC MA_ENABLE_WASAPI MA_ENABLE_NULL)
  target_sources(venture_audio PRIVATE ../platform/win32/audio_endpoints.cpp)
  target_link_libraries(venture_audio PRIVATE ole32)
else()
  # SDL3 audio is the only device backend (platform/posix/audio_sdl.cpp); null
  # serves the test executables.
  target_compile_definitions(venture_audio PUBLIC MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_CUSTOM MA_ENABLE_NULL)
  target_sources(venture_audio PRIVATE ../platform/posix/audio_endpoints.cpp ../platform/posix/audio_sdl.cpp)
  find_package(Threads REQUIRED)
  target_link_libraries(venture_audio PRIVATE SDL3::SDL3-static Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
