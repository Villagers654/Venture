# Use the same read-only resolver as bootstrap and every Python tool.
if(NOT TARGET Python3::Interpreter)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/venture_paths.py"
  OUTPUT_VARIABLE VENTURE_DATA_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY)
file(TO_CMAKE_PATH "${VENTURE_DATA_ROOT}/dev-cache" VENTURE_GFX_DOWNLOAD_CACHE)
