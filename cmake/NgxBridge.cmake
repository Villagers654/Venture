# Optional: venture_ngx.dll, the DLSS bridge the runtime loads for the game
# page's DLSS VR Optimization tab (src/runtime/xr/dlss.h). Off unless
# VENTURE_NGX_SDK_DIR names NVIDIA's DLSS SDK (github.com/NVIDIA/DLSS); without
# it Venture builds as before and DLSS reports the bridge as missing.
set(VENTURE_NGX_SDK_DIR "" CACHE PATH "NVIDIA DLSS SDK directory; builds venture_ngx.dll with MSVC (Windows)")
set(VENTURE_NGX_GENERATOR "Visual Studio 17 2022" CACHE STRING "MSVC generator for venture_ngx.dll")
if(WIN32 AND VENTURE_NGX_SDK_DIR)
  include(ExternalProject)
  include(FetchContent)
  FetchContent_GetProperties(venture_vulkan_headers)
  ExternalProject_Add(venture_ngx_bridge
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/tools/ngx-bridge
    BINARY_DIR ${CMAKE_BINARY_DIR}/ngx-bridge
    CMAKE_GENERATOR "${VENTURE_NGX_GENERATOR}"
    CMAKE_GENERATOR_PLATFORM x64
    CMAKE_ARGS -DNGX_SDK_DIR=${VENTURE_NGX_SDK_DIR}
               -DVULKAN_HEADERS_DIR=${venture_vulkan_headers_SOURCE_DIR}/include
               -DVENTURE_SOURCE_DIR=${PROJECT_SOURCE_DIR}
               -DOUTPUT_DIR=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/venture_ngx.dll)
  add_dependencies(venture-runtime venture_ngx_bridge)
  message(STATUS "venture_ngx.dll: built with ${VENTURE_NGX_GENERATOR} from ${VENTURE_NGX_SDK_DIR}")
endif()
