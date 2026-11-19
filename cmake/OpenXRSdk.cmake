# The pinned OpenXR SDK (cmake/Dependencies.cmake declares it): headers,
# registry and the Khronos loader from the same source. The runtime's host XR
# and the Quest compatibility layer both include this.
include_guard(GLOBAL)
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
if(CMAKE_HOST_WIN32 AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Cross-building for Linux on Windows: the loader's own libopenxr_loader.so.N.M
  # symlink targets need symlink rights Windows does not grant by default. Nothing
  # here loads that name; CMake's soname links are still made.
  get_property(_venture_loader_targets DIRECTORY "${openxr_sdk_SOURCE_DIR}/src/loader" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_venture_target IN LISTS _venture_loader_targets)
    if(_venture_target MATCHES "^libopenxr_loader[.]so[.]")
      set_property(TARGET ${_venture_target} PROPERTY EXCLUDE_FROM_ALL TRUE)
    endif()
  endforeach()
endif()
