# libcurl (CURL::libcurl) comes from cmake/Dependencies.cmake.

add_library(venture_ovrplatform STATIC
  provider.cpp
  http.cpp
  session.cpp
  hle.cpp
  library.cpp
  diagnostics.cpp
  account_store.cpp
  request_args.cpp
  model.cpp
  identity_iap.cpp
  broker.cpp
  social.cpp
  horizon.cpp
  asset_service.cpp
  progression.cpp
)
if(WIN32)
  target_sources(venture_ovrplatform PRIVATE ../../platform/win32/ovrplatform_locale.cpp)
else()
  target_sources(venture_ovrplatform PRIVATE ../../platform/posix/ovrplatform_locale.cpp)
endif()
target_include_directories(venture_ovrplatform PUBLIC ${PROJECT_SOURCE_DIR}/src/runtime)
target_link_libraries(venture_ovrplatform PUBLIC venture_common PRIVATE CURL::libcurl)

if(VENTURE_BUILD_RUNTIME)
  add_executable(venture-runtime-platform-check probe.cpp)
  target_link_libraries(venture-runtime-platform-check PRIVATE venture_ovrplatform)
endif()
