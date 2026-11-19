if(TARGET venture AND TARGET venture-runtime)
  if(NOT TARGET Python3::Interpreter)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
  endif()
  if(WIN32)
    set(venture_package_name Venture-win64)
    get_filename_component(venture_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(venture_import_tools --readobj "${venture_compiler_bin}/llvm-readobj.exe")
  else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
      set(venture_package_name Venture-linux-arm64)
    else()
      set(venture_package_name Venture-linux64)
    endif()
    include("${PROJECT_SOURCE_DIR}/cmake/AngleRoot.cmake")
  endif()
  set(venture_package_command "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/package_release.py"
    --build "${CMAKE_BINARY_DIR}" --output "${PROJECT_SOURCE_DIR}/dist/${venture_package_name}"
    --angle "${VENTURE_ANGLE_ROOT}" --strip "${CMAKE_STRIP}" ${venture_import_tools})
  add_custom_target(package-release COMMAND ${venture_package_command}
    DEPENDS venture venture-runtime VERBATIM)
  if(WIN32)
    # Venture-windows64.zip and -setup.exe from the layout (docs/WINDOWS_RELEASE.md).
    add_custom_target(package-windows COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tools/packaging/windows/build_installer.py"
      --layout "${PROJECT_SOURCE_DIR}/dist/${venture_package_name}" --output-dir "${PROJECT_SOURCE_DIR}/dist"
      ${venture_import_tools} VERBATIM)
    add_dependencies(package-windows package-release)
  endif()
  if(VENTURE_BUILD_TESTS)
    add_test(NAME release_layout COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/package_release_test.py"
      --build "${CMAKE_BINARY_DIR}" --angle "${VENTURE_ANGLE_ROOT}"
      --strip "${CMAKE_STRIP}" ${venture_import_tools})
    set_tests_properties(release_layout PROPERTIES TIMEOUT 600)
  endif()
endif()
