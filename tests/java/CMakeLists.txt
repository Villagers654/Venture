if(TARGET venture-framework)

  find_program(VENTURE_JAVA java HINTS "${VENTURE_JAVA_HOME}/bin" REQUIRED)
  set(framework_test_classes "${CMAKE_CURRENT_BINARY_DIR}/classes")
  add_custom_command(OUTPUT "${framework_test_classes}/FrameworkTest.class"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${framework_test_classes}"
    COMMAND "${VENTURE_JAVAC}" -encoding UTF-8 --release 21 -cp "${VENTURE_FRAMEWORK_JAR}"
      -d "${framework_test_classes}" "${CMAKE_CURRENT_SOURCE_DIR}/FrameworkTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/AbiProfileTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/VMStackTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/ProcessExitTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/ProcessCommandsTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/ColdRestartTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/LifecycleTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/LocalServicesTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/PreferencesTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/AssetReadTraceTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/RecordingHost.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/TestNatives.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/GraphicsTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/ThermalTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/MenuQuitTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/SQLiteTest.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/InstrumentationProbe.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/FragmentPermissionsTest.java"
    DEPENDS "${VENTURE_FRAMEWORK_JAR}" FrameworkTest.java AbiProfileTest.java VMStackTest.java ProcessExitTest.java ProcessCommandsTest.java
      ColdRestartTest.java LifecycleTest.java LocalServicesTest.java PreferencesTest.java AssetReadTraceTest.java RecordingHost.java TestNatives.java GraphicsTest.java ThermalTest.java MenuQuitTest.java SQLiteTest.java InstrumentationProbe.java FragmentPermissionsTest.java VERBATIM)
  add_custom_target(venture_framework_tests ALL DEPENDS "${framework_test_classes}/FrameworkTest.class")
  if(WIN32)
    set(framework_classpath "${VENTURE_FRAMEWORK_JAR}\;${framework_test_classes}")
  else()
    set(framework_classpath "${VENTURE_FRAMEWORK_JAR}:${framework_test_classes}")
  endif()

  add_test(NAME framework_java COMMAND "${VENTURE_JAVA}" -Xverify:all -cp "${framework_classpath}" FrameworkTest)
  add_test(NAME framework_fragment_permissions COMMAND "${VENTURE_JAVA}" -Xverify:all -cp "${framework_classpath}" FragmentPermissionsTest)
  set_tests_properties(framework_fragment_permissions PROPERTIES TIMEOUT 15)

  add_test(NAME framework_preferences COMMAND "${VENTURE_JAVA}" -Xverify:all -cp "${framework_classpath}" PreferencesTest)
  set_tests_properties(framework_preferences PROPERTIES TIMEOUT 30)

  if(NOT TARGET Python3::Interpreter)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
  endif()

  add_test(NAME framework_virtual_files COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/virtual_files_rewrite_test.py" "${VENTURE_FRAMEWORK_JAR}")

  # Several JVM fixtures take ~16 s even on an idle host. Assertions in the
  # fixtures test behavior; this bound only catches a stalled suite.
  set_tests_properties(framework_java PROPERTIES TIMEOUT 120)
  foreach(abi IN ITEMS arm64-v8a armeabi-v7a)
    add_test(NAME framework_abi_${abi} COMMAND "${VENTURE_JAVA}" -Xverify:all
      -Dventure.guest.abi=${abi} -cp "${framework_classpath}" AbiProfileTest ${abi})
  endforeach()
endif()
