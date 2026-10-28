add_library(venture_audio STATIC
  miniaudio_impl.c
  audio.cpp
  ../media/media.cpp
  ../hle/aaudio/aaudio.cpp
  ../hle/opensles/opensles.cpp
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
