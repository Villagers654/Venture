# The installed ANGLE the build stages and the release layout ships
# (tools/angle/build_angle.py). Linux arm64 trees, native or cross-built, use
# its own slot (build_angle.py --target linux-arm64); every other target the
# host's.
include_guard(GLOBAL)
set(venture_angle_slot "d2e5e488da0c6215f8276e5b8d992a4a6ec20c32")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  string(APPEND venture_angle_slot "-linux-arm64")
endif()
set(VENTURE_ANGLE_ROOT "${PROJECT_SOURCE_DIR}/.deps/angle/${venture_angle_slot}" CACHE PATH "Installed Venture ANGLE dependency")
