# The real external passthrough source client against a controlled server
# process on the temporary SBN1 compatibility pipe (Windows only): the backend
# probe, admission, grant changes, cancellation, reconnect and teardown.
if(NOT WIN32)
  return()
endif()
add_executable(venture_passthrough_source_fixture server.cpp)
add_executable(venture_passthrough_source_transport_tests client.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/xr/passthrough_source.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/platform/win32/passthrough_source.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/xr/passthrough_source_sbn1.cpp
  ${PROJECT_SOURCE_DIR}/src/runtime/platform/win32/passthrough_source_sbn1.cpp)
foreach(target IN ITEMS venture_passthrough_source_fixture venture_passthrough_source_transport_tests)
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/runtime)
  target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
  if(MINGW)
    # Neither process depends on where the libc++ DLLs are.
    target_link_options(${target} PRIVATE -static)
  endif()
endforeach()
target_link_libraries(venture_passthrough_source_transport_tests PRIVATE bcrypt)
add_test(NAME passthrough_source_windows_transport
  COMMAND venture_passthrough_source_transport_tests "$<TARGET_FILE:venture_passthrough_source_fixture>")
set_tests_properties(passthrough_source_windows_transport PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
