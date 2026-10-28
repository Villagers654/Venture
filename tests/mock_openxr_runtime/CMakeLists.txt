add_library(venture_mock_openxr_runtime SHARED ../../src/common/environment.cpp runtime.cpp d3d11.cpp input_timeline.cpp ../../src/runtime/xr/hands.cpp)
target_include_directories(venture_mock_openxr_runtime PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(venture_mock_openxr_runtime PRIVATE venture_xr_capture Vulkan::Headers OpenXR::headers zlibstatic nlohmann_json::nlohmann_json)
if(WIN32)
  target_sources(venture_mock_openxr_runtime PRIVATE ../../src/runtime/platform/win32/xr_native.cpp)
  target_link_libraries(venture_mock_openxr_runtime PRIVATE d3d11 dxgi)
  target_compile_definitions(venture_mock_openxr_runtime PRIVATE VENTURE_MOCK_TIME_EXTENSION="XR_KHR_win32_convert_performance_counter_time")
else()
  target_sources(venture_mock_openxr_runtime PRIVATE ../../src/runtime/platform/posix/xr_native.cpp)
  target_compile_definitions(venture_mock_openxr_runtime PRIVATE VENTURE_MOCK_TIME_EXTENSION="XR_KHR_convert_timespec_time")
  target_link_libraries(venture_mock_openxr_runtime PRIVATE ${CMAKE_DL_LIBS})
endif()
# The loader resolves a bare library name through the system search path on
# both platforms; "./" makes it relative to this manifest wherever it is copied.
set(venture_mock_runtime_prefix "./")
file(GENERATE OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mock_openxr_runtime.json"
  CONTENT "{\n  \"file_format_version\": \"1.0.0\",\n  \"runtime\": {\"library_path\": \"${venture_mock_runtime_prefix}$<TARGET_FILE_NAME:venture_mock_openxr_runtime>\"}\n}\n")
