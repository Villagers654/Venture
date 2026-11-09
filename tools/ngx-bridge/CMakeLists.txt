# venture_ngx.dll: the NGX DLSS bridge (venture_ngx.cpp). NGX's static library
# is MSVC-only, so this is its own MSVC project; Venture's build runs it as an
# external project when VENTURE_NGX_SDK_DIR is set (cmake/NgxBridge.cmake).
cmake_minimum_required(VERSION 3.28)
project(venture_ngx LANGUAGES CXX)

set(NGX_SDK_DIR "" CACHE PATH "NVIDIA DLSS SDK (github.com/NVIDIA/DLSS): include/ and nvsdk_ngx_s.lib")
set(VULKAN_HEADERS_DIR "" CACHE PATH "Vulkan-Headers include directory")
set(VENTURE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Venture source tree")
set(OUTPUT_DIR "" CACHE PATH "Where venture_ngx.dll goes (Venture's bin directory)")

if(NOT MSVC)
  message(FATAL_ERROR "venture_ngx.dll needs MSVC: NGX's static library uses the MSVC ABI")
endif()
find_path(NGX_INCLUDE_DIR nvsdk_ngx_vk.h HINTS "${NGX_SDK_DIR}" PATH_SUFFIXES include NO_DEFAULT_PATH REQUIRED)
# /MT build: the SDK's static-runtime library (its root in some copies of the SDK).
find_library(NGX_LIBRARY nvsdk_ngx_s HINTS "${NGX_SDK_DIR}"
  PATH_SUFFIXES lib/Windows_x86_64/x64 lib/Windows_x86_64/x86_64 lib/Windows_x86_64 lib "" NO_DEFAULT_PATH REQUIRED)

add_library(venture_ngx SHARED venture_ngx.cpp)
set_property(TARGET venture_ngx PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_compile_features(venture_ngx PRIVATE cxx_std_17)
target_compile_definitions(venture_ngx PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(venture_ngx PRIVATE /W3 /EHsc)
target_include_directories(venture_ngx PRIVATE "${NGX_INCLUDE_DIR}" "${VULKAN_HEADERS_DIR}"
  "${VENTURE_SOURCE_DIR}/src/runtime")
target_link_libraries(venture_ngx PRIVATE "${NGX_LIBRARY}" advapi32 user32)
if(OUTPUT_DIR)
  set_target_properties(venture_ngx PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUTPUT_DIR}" RUNTIME_OUTPUT_DIRECTORY_DEBUG "${OUTPUT_DIR}")
endif()
