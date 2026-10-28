# Registries and decoder sources are immutable, shared by all graphics targets.
include(FetchContent)
include("${PROJECT_SOURCE_DIR}/cmake/DataPaths.cmake")
FetchContent_Declare(venture_gl_registry
  URL https://github.com/KhronosGroup/OpenGL-Registry/archive/6af574a14089ccfee87efe230ebcdd8742859813.tar.gz
  URL_HASH SHA256=fa80b86e5c2d91c3088b67a128d4070f2520dac6bb823f0538f4870ec9e50942
  DOWNLOAD_DIR "${VENTURE_GFX_DOWNLOAD_CACHE}"
  DOWNLOAD_NAME OpenGL-Registry-6af574a14089ccfee87efe230ebcdd8742859813.tar.gz
  SOURCE_SUBDIR venture-no-build)
FetchContent_Declare(venture_egl_registry
  URL https://github.com/KhronosGroup/EGL-Registry/archive/db3425b8246136faccb5e2782b5694960bd6edf1.tar.gz
  URL_HASH SHA256=3d6b42b23da8030f5b29ade07d7384c5f885e713f17f04650f6cd4b8329b88e8
  DOWNLOAD_DIR "${VENTURE_GFX_DOWNLOAD_CACHE}"
  DOWNLOAD_NAME EGL-Registry-db3425b8246136faccb5e2782b5694960bd6edf1.tar.gz
  SOURCE_SUBDIR venture-no-build)
FetchContent_Declare(venture_vulkan_headers
  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/c46850864f4661461b0f6cb9922c058ffea4915e.tar.gz
  URL_HASH SHA256=539ff8eaa4abf22fddbd98225c68a0c5ddb940075a4048c8b7f5d04449070a4e
  DOWNLOAD_DIR "${VENTURE_GFX_DOWNLOAD_CACHE}"
  DOWNLOAD_NAME Vulkan-Headers-c46850864f4661461b0f6cb9922c058ffea4915e.tar.gz
  SOURCE_SUBDIR venture-no-build)
FetchContent_Declare(venture_xr_headers
  URL https://github.com/KhronosGroup/OpenXR-SDK/archive/97dcbd94ed2e006329e383a4de456e080338d2bb.tar.gz
  URL_HASH SHA256=10f806c9b920672b4caba21500e84cf12a52aad88621daf6f20fd0da95765720
  DOWNLOAD_DIR "${VENTURE_GFX_DOWNLOAD_CACHE}"
  DOWNLOAD_NAME OpenXR-SDK-97dcbd94ed2e006329e383a4de456e080338d2bb.tar.gz
  SOURCE_SUBDIR venture-no-build)
set(ASTCENC_DECOMPRESSOR OFF CACHE BOOL "" FORCE)
set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNITTEST OFF CACHE BOOL "" FORCE)
set(ASTCENC_ISA_SSE2 ON CACHE BOOL "" FORCE)
FetchContent_Declare(venture_astcenc
  URL https://github.com/ARM-software/astc-encoder/archive/30aabb3f42406df45a910d8496f9bee17eeba9bb.tar.gz
  URL_HASH SHA256=4ae89d0d16435c761a65de3e96642a46b9a2e459152c72bcf1edcaf70ef7c2e8
  DOWNLOAD_DIR "${VENTURE_GFX_DOWNLOAD_CACHE}"
  DOWNLOAD_NAME astc-encoder-30aabb3f42406df45a910d8496f9bee17eeba9bb.tar.gz)
FetchContent_MakeAvailable(venture_gl_registry venture_egl_registry venture_vulkan_headers venture_xr_headers venture_astcenc)
