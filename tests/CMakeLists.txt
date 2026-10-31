# Source names are remapped for release builds; fixtures are executable-relative.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/data/required_rule_catalog.json"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test-data/required_rule_catalog.json" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/data/steam-shortcuts.vdf"
  "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test-data/steam-shortcuts.vdf" COPYONLY)
add_executable(venture_tests
  test_main.cpp
  test_launcher.cpp
  steam_library_test.cpp
  paths_test.cpp
  digest_test.cpp
  environment_test.cpp
  log_test.cpp
  dlc_test.cpp
  import_test.cpp
  accounts_test.cpp
  launcher_accounts_test.cpp

  ${PROJECT_SOURCE_DIR}/src/launcher/ui/rich_text_parse.cpp
  ${PROJECT_SOURCE_DIR}/src/launcher/debug_report.cpp
  ${PROJECT_SOURCE_DIR}/src/launcher/drives.cpp
  ${PROJECT_SOURCE_DIR}/src/launcher/updater.cpp
  broker_test.cpp
  broker_http_test.cpp
  launch_channel_test.cpp
  runtime/hle_marshal_test.cpp
  runtime/process_paths_test.cpp
  runtime/invalidation_gate_test.cpp

)
target_include_directories(venture_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
target_link_libraries(venture_tests PRIVATE venture_common doctest::doctest miniz)
if(VENTURE_BUILD_LAUNCHER)
  target_compile_definitions(venture_tests PRIVATE VENTURE_TEST_STEAM_ART)
  foreach(format png webp)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/data/steam-art.${format}"
      "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test-data/steam-art.${format}" COPYONLY)
  endforeach()
endif()
if(VENTURE_BUILD_LAUNCHER AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(venture_launcher_video_tests launcher_video_test.cpp
    ${PROJECT_SOURCE_DIR}/src/launcher/platform/posix/window_style.cpp
    ${PROJECT_SOURCE_DIR}/src/launcher/ui/chrome.cpp)
  target_include_directories(venture_launcher_video_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(venture_launcher_video_tests PRIVATE SDL3::SDL3-static doctest::doctest)
  add_test(NAME launcher_video_driver COMMAND venture_launcher_video_tests)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_library(VENTURE_SYSTEMD_LIBRARY systemd)
  find_path(VENTURE_SD_BUS_INCLUDE systemd/sd-bus.h)
  if(VENTURE_SYSTEMD_LIBRARY AND VENTURE_SD_BUS_INCLUDE)
    # The fake portal links sd-bus directly; the clients under test load it at run time.
    add_executable(venture_dbus_portal_tests dbus_portal_test.cpp)
    target_include_directories(venture_dbus_portal_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${VENTURE_SD_BUS_INCLUDE})
    target_link_libraries(venture_dbus_portal_tests PRIVATE venture_common doctest::doctest ${VENTURE_SYSTEMD_LIBRARY})
    add_test(NAME dbus_portal_clients COMMAND venture_dbus_portal_tests)
  endif()
  # The runtime's SDL3 audio backend under a private PipeWire.
  add_executable(venture_audio_pipewire_tests audio_pipewire_test.cpp
    ${PROJECT_SOURCE_DIR}/src/runtime/audio/miniaudio_impl.c
    ${PROJECT_SOURCE_DIR}/src/runtime/platform/posix/audio_sdl.cpp)
  target_include_directories(venture_audio_pipewire_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)
  target_include_directories(venture_audio_pipewire_tests SYSTEM PRIVATE ${miniaudio_SOURCE_DIR})
  target_compile_definitions(venture_audio_pipewire_tests PRIVATE MA_NO_DECODING MA_NO_ENCODING
    MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE MA_NO_GENERATION
    MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_CUSTOM MA_ENABLE_NULL)
  target_link_libraries(venture_audio_pipewire_tests PRIVATE SDL3::SDL3-static nlohmann_json::nlohmann_json
    doctest::doctest ${CMAKE_DL_LIBS} m)
  add_test(NAME audio_pipewire_routing COMMAND venture_audio_pipewire_tests)
endif()
add_executable(venture_broker_child broker_child.cpp)
target_link_libraries(venture_broker_child PRIVATE venture_common)
add_dependencies(venture_tests venture_broker_child)
target_compile_definitions(venture_tests PRIVATE VENTURE_BROKER_CHILD="$<TARGET_FILE:venture_broker_child>")
add_executable(venture_launch_channel_child launch_channel_child.cpp)
target_link_libraries(venture_launch_channel_child PRIVATE venture_common)
add_dependencies(venture_tests venture_launch_channel_child)
target_compile_definitions(venture_tests PRIVATE VENTURE_LAUNCH_CHANNEL_CHILD="$<TARGET_FILE:venture_launch_channel_child>")
if(TARGET venture_hle)
  file(GLOB LIBC_A_TEST_SOURCES CONFIGURE_DEPENDS runtime/libc_a_*_test.cpp runtime/libc_b_*_test.cpp)
  target_sources(venture_tests PRIVATE ${LIBC_A_TEST_SOURCES})
  target_link_libraries(venture_tests PRIVATE venture_hle)
  target_compile_definitions(venture_tests PRIVATE VENTURE_TEST_CRASH_REPORT)
  target_compile_definitions(venture_tests PRIVATE VENTURE_LIBC_B_TEST_ROOT="${CMAKE_CURRENT_BINARY_DIR}/libc-b-fixtures")
endif()
target_sources(venture_tests PRIVATE runtime/loader_lifecycle_test.cpp)
# Every source partition runs once; supervisor deadlines overlap in isolated roots.
set(venture_test_partitions venture_tests)
set(venture_test_split_filters)
foreach(source core_restart core_thread_failure jit_cache vulkan vulkan_texture vulkan_fastpath)
  set(filter "*${source}_test.cpp")
  add_test(NAME venture_tests_${source} COMMAND venture_tests "--source-file=${filter}" --duration=true)
  list(APPEND venture_test_partitions venture_tests_${source})
  list(APPEND venture_test_split_filters "${filter}")
endforeach()
list(JOIN venture_test_split_filters "," venture_test_split_filter)
add_test(NAME venture_tests COMMAND venture_tests "--source-file-exclude=${venture_test_split_filter}" --duration=true)
set_tests_properties(venture_tests_core_restart PROPERTIES COST 1500)
# The JIT cache partition again with the register allocator's full-slot checks on (environment appended below).
add_test(NAME venture_tests_jit_cache_alloc_checks COMMAND venture_tests "--source-file=*jit_cache_test.cpp" --duration=true)
set_tests_properties(venture_tests_jit_cache_alloc_checks PROPERTIES COST 40)
list(APPEND venture_test_partitions venture_tests_jit_cache_alloc_checks)
add_test(NAME libc_numeric_null_input COMMAND venture_tests
  "--test-case=libc A null integer input delivers a recoverable guest SIGSEGV")
list(APPEND venture_test_partitions libc_numeric_null_input)
set_tests_properties(venture_tests_vulkan_fastpath PROPERTIES COST 20)

if(NOT TARGET Python3::Interpreter)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()

# Windows release import allowlist; synthetic PE files, so it runs on any host.
get_filename_component(venture_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(VENTURE_LLVM_READOBJ NAMES llvm-readobj HINTS "${venture_compiler_dir}")
add_test(NAME package_imports COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/package_imports_test.py"
  $<$<BOOL:${VENTURE_LLVM_READOBJ}>:${VENTURE_LLVM_READOBJ}>)
set_tests_properties(package_imports PROPERTIES SKIP_RETURN_CODE 77)
# Windows zip/installer tooling; the installer is built when makensis is found.
find_program(VENTURE_MAKENSIS NAMES makensis HINTS "${PROJECT_SOURCE_DIR}/.deps/nsis")
add_test(NAME windows_installer COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/windows_installer_test.py"
  $<$<BOOL:${VENTURE_MAKENSIS}>:${VENTURE_MAKENSIS}>)
if(TARGET venture_runtime)
  add_executable(venture_engine_defaults_tests test_main.cpp runtime/engine_defaults_test.cpp)
  target_link_libraries(venture_engine_defaults_tests PRIVATE venture_runtime doctest::doctest)
  add_test(NAME engine_renderer_defaults COMMAND venture_engine_defaults_tests)
  add_executable(venture_java_script_loader_tests test_main.cpp runtime/java_script_loader_test.cpp)
  target_link_libraries(venture_java_script_loader_tests PRIVATE venture_runtime doctest::doctest)
  add_test(NAME java_script_loader COMMAND venture_java_script_loader_tests)
  add_executable(venture_module_iteration_tests test_main.cpp runtime/module_iteration_test.cpp)
  target_link_libraries(venture_module_iteration_tests PRIVATE venture_hle venture_java doctest::doctest)
  add_test(NAME guest_module_iteration COMMAND venture_module_iteration_tests)
  # Freestanding, synthetic C API fixtures: no SDK, engine or guest game code.
  set(engine_policy_fixture "${CMAKE_CURRENT_BINARY_DIR}/engine-policy/libCrySystem.so")
  add_custom_command(OUTPUT "${engine_policy_fixture}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/engine-policy"
    COMMAND "${CMAKE_CXX_COMPILER}" --target=aarch64-none-linux-gnu
      -nostdlib -fuse-ld=lld -shared -fPIC -fno-builtin -fno-exceptions -fno-rtti -O1
      -Wl,-soname,libCrySystem.so "${CMAKE_CURRENT_SOURCE_DIR}/runtime/engine_policy_guest.cpp"
      -o "${engine_policy_fixture}"
    DEPENDS runtime/engine_policy_guest.cpp VERBATIM)
  add_custom_target(venture_engine_policy_guest DEPENDS "${engine_policy_fixture}")
  add_executable(venture_engine_policy_tests test_main.cpp runtime/engine_policy_test.cpp)
  target_link_libraries(venture_engine_policy_tests PRIVATE venture_runtime doctest::doctest)
  target_compile_definitions(venture_engine_policy_tests PRIVATE VENTURE_ENGINE_POLICY_FIXTURE="${engine_policy_fixture}")
  add_dependencies(venture_engine_policy_tests venture_engine_policy_guest)
  add_test(NAME engine_frame_pacing COMMAND venture_engine_policy_tests)
  add_executable(venture_terminal_exit_child runtime/terminal_exit_child.cpp)
  target_link_libraries(venture_terminal_exit_child PRIVATE venture_runtime venture_vulkan)
  add_test(NAME terminal_guest_exit COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/terminal_guest_exit_test.py" "$<TARGET_FILE:venture_terminal_exit_child>")
  # Loaded baseline maxima: terminal 1.60 s, media 2.76 s, diagnostics 2.40 s;
  # fleet failures exhausted 25-45 s before completion. Allow startup/worker
  # scheduling and the inner hang watchdog to report a useful failure.
  set_tests_properties(terminal_guest_exit PROPERTIES TIMEOUT 300)

endif()
foreach(abi IN ITEMS a64 a32)
  add_executable(venture_replacement_exports_${abi} test_main.cpp runtime/replacement_exports_test.cpp)
  target_link_libraries(venture_replacement_exports_${abi} PRIVATE venture_hle doctest::doctest)
  if(abi STREQUAL "a32")
    target_compile_definitions(venture_replacement_exports_${abi} PRIVATE VENTURE_REPLACEMENT_FIXTURE_A32=1)
  endif()
  if(WIN32)
    target_sources(venture_replacement_exports_${abi} PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime/platform/win32/low_memory_startup.cpp")
    target_link_options(venture_replacement_exports_${abi} PRIVATE "LINKER:--entry=venture_low_memory_startup")
  else()
    set_property(TARGET venture_replacement_exports_${abi} PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_link_options(venture_replacement_exports_${abi} PRIVATE -pie)
  endif()
  add_test(NAME replacement_exports_${abi} COMMAND venture_replacement_exports_${abi} "--test-case=replacement exports*")
endforeach()
add_executable(venture_guest_unwind_runner runtime/guest_unwind_runner.cpp)
target_link_libraries(venture_guest_unwind_runner PRIVATE venture_hle)
if(WIN32)
  target_sources(venture_guest_unwind_runner PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime/platform/win32/low_memory_startup.cpp")
  target_link_options(venture_guest_unwind_runner PRIVATE "LINKER:--entry=venture_low_memory_startup")
else()
  set_property(TARGET venture_guest_unwind_runner PROPERTY POSITION_INDEPENDENT_CODE ON)
  target_link_options(venture_guest_unwind_runner PRIVATE -pie)
endif()
if(WIN32)
  set(unwind_clang "${PROJECT_SOURCE_DIR}/.deps/android-ndk/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe")
else()
  set(unwind_clang "${PROJECT_SOURCE_DIR}/.deps/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++")
endif()
if(EXISTS "${unwind_clang}")
  foreach(abi IN ITEMS a64 a32)
    if(abi STREQUAL "a64")
      set(unwind_target aarch64-linux-android23)
    else()
      set(unwind_target armv7a-linux-androideabi23)
    endif()
    set(unwind_fixture "${CMAKE_CURRENT_BINARY_DIR}/signal-unwind/${abi}/libsignal_unwind.so")
    add_custom_command(OUTPUT "${unwind_fixture}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/signal-unwind/${abi}"
      COMMAND "${unwind_clang}" --target=${unwind_target} -shared -fPIC -O1 -g
        -fexceptions -funwind-tables -fno-omit-frame-pointer -static-libstdc++ -Wl,--no-undefined
        "${CMAKE_CURRENT_SOURCE_DIR}/guest/src/signal_unwind.cpp" -o "${unwind_fixture}"
      DEPENDS guest/src/signal_unwind.cpp VERBATIM)
    add_custom_target(signal_unwind_${abi}_fixture DEPENDS "${unwind_fixture}")
    add_dependencies(venture_guest_unwind_runner signal_unwind_${abi}_fixture)
    set(ack_fixture "${CMAKE_CURRENT_BINARY_DIR}/signal-unwind/${abi}/libcond_signal_ack.so")
    add_custom_command(OUTPUT "${ack_fixture}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/signal-unwind/${abi}"
      COMMAND "${unwind_clang}" --target=${unwind_target} -x c -nostdlib++ -shared -fPIC -O1
        -Wl,--no-undefined "${CMAKE_CURRENT_SOURCE_DIR}/guest/src/cond_signal_ack.c" -o "${ack_fixture}"
      DEPENDS guest/src/cond_signal_ack.c VERBATIM)
    add_custom_target(cond_signal_ack_${abi}_fixture DEPENDS "${ack_fixture}")
    add_dependencies(venture_guest_unwind_runner cond_signal_ack_${abi}_fixture)
  endforeach()
endif()
add_test(NAME guest_signal_unwind COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime/signal_unwind_test.py"
  "$<TARGET_FILE:venture_guest_unwind_runner>" "${CMAKE_CURRENT_BINARY_DIR}/signal-unwind")
set_tests_properties(guest_signal_unwind PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)


add_test(NAME ffmpeg_cache COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_cache_test.py")
add_executable(venture_paths_migration_child paths_migration_child.cpp)
target_link_libraries(venture_paths_migration_child PRIVATE venture_common)
add_test(NAME venture_data_migration COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/data_migration_test.py" "$<TARGET_FILE:venture_paths_migration_child>")
set_tests_properties(venture_data_migration PROPERTIES TIMEOUT 300)
add_executable(venture_environment_child environment_child.cpp)
target_link_libraries(venture_environment_child PRIVATE venture_common)
if(VENTURE_JAVA_HOME)
  add_test(NAME venture_host_environment COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/host_environment_test.py" "${VENTURE_JAVA_HOME}" "$<TARGET_FILE:venture_environment_child>")
endif()
if(TARGET venture_runtime)

  add_executable(venture_crash_report_child runtime/crash_report_child.cpp)
  target_link_libraries(venture_crash_report_child PRIVATE venture_runtime)
  if(WIN32)
    target_sources(venture_crash_report_child PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime/platform/win32/low_memory_startup.cpp")
    target_link_options(venture_crash_report_child PRIVATE "LINKER:--entry=venture_low_memory_startup")
  endif()
  add_test(NAME venture_crash_report COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/crash_report_test.py"
    "$<TARGET_FILE:venture_crash_report_child>" "${CMAKE_CURRENT_BINARY_DIR}/crash-report-evidence")
  set_tests_properties(venture_crash_report PROPERTIES TIMEOUT 240)
endif()
# Fatal-signal cases end their process, so each runs alone.
add_test(NAME debuggerd_chained_fault COMMAND venture_tests
  "--test-case=debuggerd ends a fault whose crash reporter chains and returns" --no-skip=true)
add_test(NAME debuggerd_chained_abort COMMAND venture_tests
  "--test-case=debuggerd re-raises SIGABRT for a crash reporter that chains and returns" --no-skip=true)
add_test(NAME unresolved_fault_limit COMMAND venture_tests
  "--test-case=a returning handler whose chain ends in SIG_DFL cannot spin on one fault" --no-skip=true)
add_test(NAME debuggerd_oneshot_siginfo COMMAND venture_tests
  "--test-case=debuggerd reports the fault for a one-shot SA_SIGINFO crash reporter" --no-skip=true)
add_test(NAME debuggerd_unhandled_summary COMMAND venture_tests
  "--test-case=a fault delivered straight to debuggerd keeps the sentinel's summary line" --no-skip=true)

if(TARGET dynarmic)
  add_executable(venture_fp_round_tests runtime/fp_round_test.cpp)
  target_link_libraries(venture_fp_round_tests PRIVATE dynarmic doctest::doctest)
  add_test(NAME fp_round_semantics COMMAND venture_fp_round_tests)
  add_test(NAME dynarmic COMMAND venture_dynarmic_tests)
  target_sources(venture_tests PRIVATE runtime/jit_memory_test.cpp runtime/jit_cache_test.cpp runtime/jit_block_list_test.cpp)
  add_test(NAME jit_block_list COMMAND venture_tests "--test-case=JIT block list*")
  set_tests_properties(jit_block_list PROPERTIES ENVIRONMENT "VENTURE_JIT_PRETRANSLATE=1")
  target_link_libraries(venture_tests PRIVATE dynarmic merry::mcl xbyak::xbyak tsl::robin_map)

  if(WIN32)

    add_executable(venture_rcpc_reference runtime/rcpc_reference.cpp)
    target_link_libraries(venture_rcpc_reference PRIVATE dynarmic nlohmann_json::nlohmann_json)
    add_test(NAME rcpc_reference COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/rcpc_reference_test.py"
      --clang "${CMAKE_C_COMPILER}" --objcopy "${CMAKE_OBJCOPY}" --driver "$<TARGET_FILE:venture_rcpc_reference>")
    set_tests_properties(rcpc_reference PROPERTIES TIMEOUT 90)
  endif()

  add_executable(venture_jit_ir_equivalence runtime/jit_ir_equivalence.cpp)
  target_link_libraries(venture_jit_ir_equivalence PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_ir_equivalence SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_executable(venture_jit_nzcv_lea_test runtime/jit_nzcv_lea_test.cpp)
  target_link_libraries(venture_jit_nzcv_lea_test PRIVATE dynarmic)
  add_test(NAME jit_nzcv_lea COMMAND venture_jit_nzcv_lea_test)
  # Half of each seed runs the shipped configuration (check_halt_on_memory_access,
  # default passes), half without it; both memory modes are cross-checked.
  foreach(seed IN ITEMS 0x495246555a5a)
    add_test(NAME jit_ir_equivalence_${seed} COMMAND venture_jit_ir_equivalence --seed ${seed} --count 2048
      --halt-on-memory both --cross-memory-mode --output "${PROJECT_BINARY_DIR}/jit-ir-equivalence/${seed}")
    set_tests_properties(jit_ir_equivalence_${seed} PROPERTIES TIMEOUT 120 LABELS "cpu;jit")
  endforeach()
  # The register allocator's full-slot consistency checks (single definition, released untouched slots) run only
  # with VENTURE_JIT_CHECK_ALLOC_SCOPE=1, so one seed and the JIT cache partition run again with them on.
  add_test(NAME jit_ir_equivalence_alloc_checks COMMAND venture_jit_ir_equivalence --seed 0x495246555a5a --count 2048
    --halt-on-memory both --cross-memory-mode --output "${PROJECT_BINARY_DIR}/jit-ir-equivalence/alloc-checks")
  set_tests_properties(jit_ir_equivalence_alloc_checks PROPERTIES TIMEOUT 120 LABELS "cpu;jit"
    ENVIRONMENT "VENTURE_JIT_CHECK_ALLOC_SCOPE=1")
endif()

# One immutable guest ISA per process; never mix the A32 arena with A64 fixtures.
if(TARGET venture_runtime AND TARGET venture_hle)

  if(WIN32)

  else()

  endif()

  add_executable(venture_a32_abi_tests runtime/a32_memory_main.cpp runtime/core_a32_abi_test.cpp runtime/libc_a32_test.cpp
    runtime/zlib_a32_test.cpp runtime/a32_signal_test.cpp)
  target_link_libraries(venture_a32_abi_tests PRIVATE venture_runtime venture_hle doctest::doctest)
  if(EXISTS "${unwind_clang}")
    set(personality_fixture "${CMAKE_CURRENT_BINARY_DIR}/signal-unwind/a32/libcondition_personality.so")
    add_custom_command(OUTPUT "${personality_fixture}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/signal-unwind/a32"
      COMMAND "${unwind_clang}" --target=armv7a-linux-androideabi23 -x c -std=c11 -O1 -shared -fPIC -nostdlib
        "${CMAKE_CURRENT_SOURCE_DIR}/guest/src/condition_personality.c" -lc -ldl -o "${personality_fixture}"
      DEPENDS guest/src/condition_personality.c VERBATIM)
    add_custom_target(a32_condition_personality_fixture DEPENDS "${personality_fixture}")
    add_dependencies(venture_a32_abi_tests a32_condition_personality_fixture)
    target_compile_definitions(venture_a32_abi_tests PRIVATE VENTURE_A32_PERSONALITY_FIXTURE="${personality_fixture}")
  endif()
  target_compile_options(venture_a32_abi_tests PRIVATE -Wall -Wextra)
  add_executable(venture_a32_memory_tests runtime/a32_memory_main.cpp runtime/a32_memory_test.cpp)
  target_link_libraries(venture_a32_memory_tests PRIVATE venture_runtime venture_hle doctest::doctest)
  target_compile_options(venture_a32_memory_tests PRIVATE -Wall -Wextra)
  add_executable(venture_thread_heap_cleanup_tests runtime/a32_memory_main.cpp runtime/thread_heap_cleanup_test.cpp)
  target_link_libraries(venture_thread_heap_cleanup_tests PRIVATE venture_runtime mimalloc-static doctest::doctest)
  add_test(NAME thread_heap_cleanup COMMAND venture_thread_heap_cleanup_tests)
  set_tests_properties(thread_heap_cleanup PROPERTIES TIMEOUT 90)
  add_test(NAME guest_heap_check COMMAND venture_thread_heap_cleanup_tests "-tc=a block freed again*")
  set_tests_properties(guest_heap_check PROPERTIES TIMEOUT 60 ENVIRONMENT VENTURE_HEAP_CHECK=1)
  add_executable(venture_guest_heap64_tests runtime/guest_heap64_test.cpp)
  target_link_libraries(venture_guest_heap64_tests PRIVATE venture_runtime mimalloc-static doctest::doctest)
  add_test(NAME guest_heap64 COMMAND venture_guest_heap64_tests)
  if(NOT WIN32)
    set_property(TARGET venture_a32_abi_tests PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_link_options(venture_a32_abi_tests PRIVATE -pie)
    set_property(TARGET venture_a32_memory_tests PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_link_options(venture_a32_memory_tests PRIVATE -pie)
    set_property(TARGET venture_thread_heap_cleanup_tests PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_link_options(venture_thread_heap_cleanup_tests PRIVATE -pie)
  endif()
  add_test(NAME a32_abi COMMAND venture_a32_abi_tests)
  # Fatal-signal cases end their process, so each runs alone.
  add_test(NAME a32_debuggerd_fault COMMAND venture_a32_abi_tests
    "--test-case=A32 sigaction reports debuggerd from startup and a fault takes the default action" --no-skip=true)
  add_test(NAME a32_debuggerd_chain COMMAND venture_a32_abi_tests
    "--test-case=A32 crash reporter chaining to debuggerd ends with the default action" --no-skip=true)
  if(TARGET venture_vulkan)
    add_executable(venture_a32_vulkan_feature_tests runtime/a32_memory_main.cpp runtime/vulkan_a32_feature_test.cpp)
    target_link_libraries(venture_a32_vulkan_feature_tests PRIVATE venture_runtime venture_vulkan doctest::doctest)
    if(WIN32)
      target_sources(venture_a32_vulkan_feature_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime/platform/win32/low_memory_startup.cpp")
      target_link_options(venture_a32_vulkan_feature_tests PRIVATE "LINKER:--entry=venture_low_memory_startup")
    else()
      set_property(TARGET venture_a32_vulkan_feature_tests PROPERTY POSITION_INDEPENDENT_CODE ON)
      target_link_options(venture_a32_vulkan_feature_tests PRIVATE -pie)
    endif()
    add_test(NAME a32_vulkan_features COMMAND venture_a32_vulkan_feature_tests)
    # A64 discovery and guest device properties against a simulated host device
    # (gpu_device() replaced).
    add_executable(venture_vulkan_feature_tests test_main.cpp runtime/vulkan_feature_test.cpp)
    target_link_libraries(venture_vulkan_feature_tests PRIVATE venture_runtime venture_vulkan doctest::doctest)
    add_test(NAME vulkan_features COMMAND venture_vulkan_feature_tests)
    add_executable(venture_vulkan_texture_scratch_tests test_main.cpp runtime/vulkan_texture_scratch_test.cpp)
    target_link_libraries(venture_vulkan_texture_scratch_tests PRIVATE venture_runtime venture_vulkan doctest::doctest)
    add_test(NAME vulkan_texture_scratch COMMAND venture_vulkan_texture_scratch_tests)
    add_executable(venture_vulkan_lazy_tests test_main.cpp runtime/vulkan_lazy_test.cpp)
    target_link_libraries(venture_vulkan_lazy_tests PRIVATE venture_runtime venture_vulkan doctest::doctest)
    add_test(NAME vulkan_lazy COMMAND venture_vulkan_lazy_tests)
    set_tests_properties(vulkan_lazy PROPERTIES TIMEOUT 90)
  endif()
  add_test(NAME a32_memory COMMAND venture_a32_memory_tests)
  add_test(NAME a32_memory_reject_host COMMAND venture_a32_memory_tests --reject-host-writable)
  add_executable(venture_a32_guard_tests test_main.cpp runtime/a32_guard_test.cpp)
  target_link_libraries(venture_a32_guard_tests PRIVATE venture_runtime doctest::doctest)
  if(WIN32)
    target_sources(venture_a32_guard_tests PRIVATE runtime/a32_guard_startup.cpp)
    target_link_libraries(venture_a32_guard_tests PRIVATE psapi)
    target_link_options(venture_a32_guard_tests PRIVATE "LINKER:--entry=venture_guard_test_startup")
  else()
    set_property(TARGET venture_a32_guard_tests PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_link_options(venture_a32_guard_tests PRIVATE -pie)
  endif()
  add_test(NAME a32_memory_occupied_guard COMMAND venture_a32_guard_tests)
  add_executable(venture_a64_arena_release_tests test_main.cpp runtime/a64_arena_release_test.cpp)
  target_link_libraries(venture_a64_arena_release_tests PRIVATE venture_runtime doctest::doctest)
  if(NOT WIN32)
    set_property(TARGET venture_a64_arena_release_tests PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_link_options(venture_a64_arena_release_tests PRIVATE -pie)
  endif()
  add_test(NAME a64_early_arena_release COMMAND venture_a64_arena_release_tests)
endif()

if(TARGET venture_runtime)
  if(WIN32)
    add_library(venture_exit_detach_fixture SHARED runtime/exit_detach_fixture.cpp)
    add_dependencies(venture_tests venture_exit_detach_fixture)
    target_compile_definitions(venture_tests PRIVATE VENTURE_EXIT_DETACH_FIXTURE="$<TARGET_FILE:venture_exit_detach_fixture>")
  endif()
  add_executable(venture_runtime_restart_child runtime/restart_child.cpp)
  target_link_libraries(venture_runtime_restart_child PRIVATE venture_runtime venture_hle venture_audio venture_vulkan)
  add_dependencies(venture_tests venture_runtime_restart_child)
  target_compile_definitions(venture_tests PRIVATE VENTURE_RESTART_CHILD="$<TARGET_FILE:venture_runtime_restart_child>")
  if(WIN32)

    # Two harnesses each retain a 30-second child cap, plus the 10-second
    # acknowledgement probe; the outer guard must allow all three stages.

  endif()

  add_executable(venture_jit_low_lane_test runtime/jit_low_lane_test.cpp)
  target_link_libraries(venture_jit_low_lane_test PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_low_lane_test SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_test(NAME jit_low_lane COMMAND venture_jit_low_lane_test)
  set_tests_properties(jit_low_lane PROPERTIES TIMEOUT 120 COST 2 LABELS "cpu;jit")
  add_executable(venture_jit_fp_to_int_test runtime/jit_fp_to_int_test.cpp)
  target_link_libraries(venture_jit_fp_to_int_test PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_fp_to_int_test SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_test(NAME jit_fp_to_int COMMAND venture_jit_fp_to_int_test)
  set_tests_properties(jit_fp_to_int PROPERTIES TIMEOUT 120 LABELS "cpu;jit")
  add_executable(venture_jit_exclusive_aba_test runtime/jit_exclusive_aba_test.cpp)
  target_link_libraries(venture_jit_exclusive_aba_test PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_exclusive_aba_test SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_test(NAME jit_exclusive_aba COMMAND venture_jit_exclusive_aba_test)
  set_tests_properties(jit_exclusive_aba PROPERTIES TIMEOUT 60 LABELS "cpu;jit")
  add_executable(venture_jit_barrier_test runtime/jit_barrier_test.cpp)
  target_link_libraries(venture_jit_barrier_test PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_barrier_test SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_test(NAME jit_barrier COMMAND venture_jit_barrier_test)
  set_tests_properties(jit_barrier PROPERTIES TIMEOUT 120 LABELS "cpu;jit")
  add_executable(venture_jit_flags_select_test runtime/jit_flags_select_test.cpp)
  target_link_libraries(venture_jit_flags_select_test PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_flags_select_test SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_test(NAME jit_flags_select COMMAND venture_jit_flags_select_test)
  set_tests_properties(jit_flags_select PROPERTIES TIMEOUT 60 LABELS "cpu;jit")
  add_executable(venture_jit_bitfield_test runtime/jit_bitfield_test.cpp)
  target_link_libraries(venture_jit_bitfield_test PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_bitfield_test SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_test(NAME jit_bitfield COMMAND venture_jit_bitfield_test cache)
  add_test(NAME jit_bitfield_uncached COMMAND venture_jit_bitfield_test uncached)
  set_tests_properties(jit_bitfield jit_bitfield_uncached PROPERTIES TIMEOUT 120 COST 1 LABELS "cpu;jit")
  add_executable(venture_jit_scalar_fp_test runtime/jit_scalar_fp_test.cpp)
  target_link_libraries(venture_jit_scalar_fp_test PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_scalar_fp_test SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_test(NAME jit_scalar_fp COMMAND venture_jit_scalar_fp_test)
  set_tests_properties(jit_scalar_fp PROPERTIES TIMEOUT 120 LABELS "cpu;jit")
  add_executable(venture_jit_round_int_test runtime/jit_round_int_test.cpp)
  target_link_libraries(venture_jit_round_int_test PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_round_int_test SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_test(NAME jit_round_int COMMAND venture_jit_round_int_test)
  set_tests_properties(jit_round_int PROPERTIES TIMEOUT 120 LABELS "cpu;jit")
  add_executable(venture_jit_cached_rbx_test runtime/jit_cached_rbx_test.cpp)
  target_link_libraries(venture_jit_cached_rbx_test PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_cached_rbx_test SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_test(NAME jit_cached_rbx COMMAND venture_jit_cached_rbx_test)
  set_tests_properties(jit_cached_rbx PROPERTIES TIMEOUT 60 LABELS "cpu;jit")
  add_executable(venture_jit_cached_volatile_test runtime/jit_cached_volatile_test.cpp)
  target_link_libraries(venture_jit_cached_volatile_test PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_cached_volatile_test SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_test(NAME jit_cached_volatile COMMAND venture_jit_cached_volatile_test)
  set_tests_properties(jit_cached_volatile PROPERTIES TIMEOUT 60 LABELS "cpu;jit")
  add_executable(venture_jit_outlined_atomics_test runtime/jit_outlined_atomics_test.cpp)
  target_link_libraries(venture_jit_outlined_atomics_test PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_outlined_atomics_test SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_test(NAME jit_outlined_atomics COMMAND venture_jit_outlined_atomics_test)
  set_tests_properties(jit_outlined_atomics PROPERTIES TIMEOUT 60 LABELS "cpu;jit")
  if(WIN32)
    # Developer codegen inspection tool; not a test.

  endif()
  add_executable(venture_jit_halt_poll_test runtime/jit_halt_poll_test.cpp)
  target_link_libraries(venture_jit_halt_poll_test PRIVATE dynarmic merry::mcl)
  target_include_directories(venture_jit_halt_poll_test SYSTEM PRIVATE "${Boost_INCLUDE_DIR}")
  add_test(NAME jit_halt_poll COMMAND venture_jit_halt_poll_test)
  set_tests_properties(jit_halt_poll PROPERTIES TIMEOUT 60 LABELS "cpu;jit")

  if(NOT TARGET Python3::Interpreter)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
  endif()

  # These scripts already isolate their queues, homes and child jobs in private
  # temporary directories. Register them directly so their waits can overlap.

  foreach(fixture quiet backfill build)

    if(fixture STREQUAL "quiet")

    elseif(fixture STREQUAL "backfill")

    else()

    endif()
  endforeach()

  add_test(NAME jre_modules COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/jre_modules_test.py")

  # Enrollment and wake completion each have a 120-second watchdog; interrupt
  # cleanup has 30 seconds. Keep outer headroom for all three failure stages.

endif()

if(TARGET venture_runtime AND TARGET venture_java)
  # ELF32 inspection has its own process with the production low arena.
  add_executable(venture_a32_loader_tests test_main.cpp runtime/elf32_loader_test.cpp runtime/loader_lifecycle_test.cpp)
  target_compile_definitions(venture_a32_loader_tests PRIVATE VENTURE_TEST_LOADER_A32)
  if(WIN32)
    target_sources(venture_a32_loader_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime/platform/win32/low_memory_startup.cpp")
    target_link_options(venture_a32_loader_tests PRIVATE "LINKER:--entry=venture_low_memory_startup")
  else()
    target_sources(venture_a32_loader_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime/platform/posix/low_memory_startup.cpp")
  endif()
  target_link_libraries(venture_a32_loader_tests PRIVATE venture_runtime venture_java venture_hle miniz doctest::doctest)
  add_test(NAME venture_a32_loader_tests COMMAND venture_a32_loader_tests)
  # The owned input is read-only. Configure/cache/report output stays in this build.
  set_tests_properties(venture_a32_loader_tests PROPERTIES
    ENVIRONMENT "VENTURE_DATA_DIR=${PROJECT_BINARY_DIR}/a32-inspection-data")
endif()

add_executable(venture_dlc_tests EXCLUDE_FROM_ALL test_main.cpp dlc_test.cpp)
target_link_libraries(venture_dlc_tests PRIVATE venture_common doctest::doctest)
if(WIN32)
  target_link_libraries(venture_dlc_tests PRIVATE ws2_32)
endif()

if(TARGET venture_android)
    if(NOT TARGET Python3::Interpreter)
      find_package(Python3 COMPONENTS Interpreter REQUIRED)
    endif()
    set(VENTURE_ANDROID_FIXTURES "${PROJECT_BINARY_DIR}/android-fixtures")
    add_custom_command(OUTPUT "${VENTURE_ANDROID_FIXTURES}/synthetic.json" "${VENTURE_ANDROID_FIXTURES}/real.json"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/android_make_fixtures.py" "${VENTURE_ANDROID_FIXTURES}"
      DEPENDS runtime/android_make_fixtures.py
      VERBATIM)
    add_custom_target(venture_android_fixtures DEPENDS "${VENTURE_ANDROID_FIXTURES}/synthetic.json" "${VENTURE_ANDROID_FIXTURES}/real.json")
    add_dependencies(venture_tests venture_android_fixtures)
    target_sources(venture_tests PRIVATE
      runtime/android_test_support.cpp
      runtime/android_assets_test.cpp
      runtime/android_runtime_test.cpp)
    target_compile_definitions(venture_tests PRIVATE
      VENTURE_ANDROID_FIXTURES="${VENTURE_ANDROID_FIXTURES}"
      VENTURE_ANDROID_IMPORTS="${CMAKE_CURRENT_SOURCE_DIR}/runtime/android_imports.json")
    target_link_libraries(venture_tests PRIVATE venture_android)
endif()

target_sources(venture_tests PRIVATE runtime/ovrplatform_test.cpp runtime/ovrplatform_http_test.cpp
  runtime/ovrplatform_presence_test.cpp runtime/assetfile_test.cpp runtime/horizon_test.cpp)
target_link_libraries(venture_tests PRIVATE venture_ovrplatform)

target_compile_definitions(venture_tests PRIVATE VENTURE_OVR_PLAN_FIXTURE="${CMAKE_CURRENT_BINARY_DIR}/ovrplatform-plan-test.json")
if(WIN32)
  target_link_libraries(venture_tests PRIVATE ws2_32)
endif()

if(TARGET venture_audio)
  add_executable(venture_media_decode_probe runtime/media_decode_probe.cpp)
  target_link_libraries(venture_media_decode_probe PRIVATE venture_audio)
  venture_stage_media(venture_media_decode_probe)

  add_executable(venture_media_tests test_main.cpp runtime/media_test.cpp runtime/android_test_support.cpp runtime/audio_test_support.cpp)
  target_include_directories(venture_media_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
  target_link_libraries(venture_media_tests PRIVATE venture_audio venture_android doctest::doctest)
  target_compile_definitions(venture_media_tests PRIVATE VENTURE_MEDIA_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/media/tone-aac.mp4")
  target_compile_definitions(venture_media_tests PRIVATE VENTURE_VIDEO_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/media/pattern-h264.mp4")
  target_compile_definitions(venture_media_tests PRIVATE VENTURE_AV_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/media/pattern-av.mp4")
  target_compile_definitions(venture_media_tests PRIVATE VENTURE_VP9_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/media/pattern-vp9.webm")
  venture_stage_media(venture_media_tests)
  add_test(NAME media_audio COMMAND venture_media_tests)
  add_test(NAME media_missing_library COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/media_missing_library_test.py" "$<TARGET_FILE:venture_media_tests>")
  add_test(NAME media_capture_source COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/media_capture_test.py" "$<TARGET_FILE:venture_media_tests>")
  set_tests_properties(media_audio PROPERTIES TIMEOUT 300)
  foreach(kind IN ITEMS audio audio_render)
    add_executable(venture_${kind}_tests test_main.cpp runtime/audio_test_support.cpp runtime/${kind}_test.cpp)
    target_include_directories(venture_${kind}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
    target_link_libraries(venture_${kind}_tests PRIVATE venture_audio venture_runtime doctest::doctest)
    add_test(NAME venture_${kind}_tests COMMAND venture_${kind}_tests)
  endforeach()
  add_executable(venture_audio_teardown_child runtime/audio_teardown_child.cpp)
  target_link_libraries(venture_audio_teardown_child PRIVATE venture_audio venture_runtime)
  add_test(NAME audio_static_teardown COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/audio_teardown_test.py" "$<TARGET_FILE:venture_audio_teardown_child>")
  set_tests_properties(audio_static_teardown PROPERTIES TIMEOUT 65)
endif()

if(TARGET venture_java)
  set(media_java_classes "${CMAKE_CURRENT_BINARY_DIR}/media-java-classes")
  add_custom_command(OUTPUT "${media_java_classes}/MediaFlow.class"
    COMMAND "${VENTURE_JAVA_HOME}/bin/javac" -encoding UTF-8 --release 21 -cp "${VENTURE_FRAMEWORK_JAR}"
      -d "${media_java_classes}" "${CMAKE_CURRENT_SOURCE_DIR}/java/MediaFlow.java"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/java/MediaFlow.java" venture-framework VERBATIM)
  add_custom_target(venture_media_java_fixture DEPENDS "${media_java_classes}/MediaFlow.class")
  add_executable(venture_media_java_tests test_main.cpp runtime/media_java_test.cpp runtime/android_test_support.cpp)
  target_include_directories(venture_media_java_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
  target_link_libraries(venture_media_java_tests PRIVATE venture_java_framework doctest::doctest)
  target_compile_definitions(venture_media_java_tests PRIVATE
    VENTURE_TEST_JAVA_HOME="${VENTURE_JAVA_HOME}" VENTURE_TEST_FRAMEWORK_JAR="${VENTURE_FRAMEWORK_JAR}"
    VENTURE_MEDIA_JAVA_CLASSES="${media_java_classes}" VENTURE_MEDIA_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/media/tone-aac.mp4")
  target_compile_definitions(venture_media_java_tests PRIVATE VENTURE_VIDEO_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/media/pattern-h264.mp4")
  add_dependencies(venture_media_java_tests venture_media_java_fixture)
  venture_stage_media(venture_media_java_tests)
  add_test(NAME media_java COMMAND venture_media_java_tests)
  set_tests_properties(media_java PROPERTIES TIMEOUT 300)
  target_sources(venture_tests PRIVATE runtime/java_abi_test.cpp runtime/java_elf_test.cpp)
  target_compile_definitions(venture_tests PRIVATE VENTURE_TEST_NATIVE_FIXTURES="${PROJECT_SOURCE_DIR}/build/java-native-fixtures")
  target_link_libraries(venture_tests PRIVATE venture_java)
  add_executable(venture_java_tests test_main.cpp runtime/java_test_support.cpp runtime/java_jni_test.cpp)
  target_include_directories(venture_java_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
  target_link_libraries(venture_java_tests PRIVATE venture_java venture_hle doctest::doctest)
  target_compile_definitions(venture_java_tests PRIVATE
    VENTURE_TEST_JAVA_HOME="${VENTURE_JAVA_HOME}" VENTURE_TEST_BRIDGE_JAR="${VENTURE_JAVA_BRIDGE_JAR}"
    VENTURE_TEST_JVM_AGENT="$<TARGET_FILE:venture_jvmti_agent>"
    VENTURE_TEST_JAVA_CLASSES="${CMAKE_CURRENT_BINARY_DIR}/java-classes")
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/java-classes/BridgeFixture.class"
    COMMAND "${VENTURE_JAVA_HOME}/bin/javac" -encoding UTF-8 -d "${CMAKE_CURRENT_BINARY_DIR}/java-classes"
      "${CMAKE_CURRENT_SOURCE_DIR}/runtime/java_fixture/BridgeFixture.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/runtime/java_fixture/ExportFixture.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/runtime/java_fixture/DependencyFixture.java"
      "${CMAKE_CURRENT_SOURCE_DIR}/runtime/java_fixture/A32Fields.java"
    DEPENDS runtime/java_fixture/BridgeFixture.java runtime/java_fixture/ExportFixture.java runtime/java_fixture/DependencyFixture.java runtime/java_fixture/A32Fields.java VERBATIM)
  add_custom_target(venture_java_fixture DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/java-classes/BridgeFixture.class")
  add_dependencies(venture_java_tests venture_java_fixture)
  add_dependencies(venture_java_tests venture_jvmti_agent)
  add_test(NAME venture_java_tests COMMAND venture_java_tests)
  add_executable(venture_java_framework_startup_tests runtime/java_framework_startup_test.cpp)
  target_link_libraries(venture_java_framework_startup_tests PRIVATE venture_java venture_hle)
  target_compile_definitions(venture_java_framework_startup_tests PRIVATE
    VENTURE_TEST_JAVA_HOME="${VENTURE_JAVA_HOME}" VENTURE_TEST_BRIDGE_JAR="${VENTURE_JAVA_BRIDGE_JAR}"
    VENTURE_TEST_FRAMEWORK_JAR="${VENTURE_FRAMEWORK_JAR}"
    VENTURE_TEST_JVM_AGENT="$<TARGET_FILE:venture_jvmti_agent>")
  add_dependencies(venture_java_framework_startup_tests venture-framework venture_jvmti_agent)
  add_test(NAME java_framework_startup COMMAND venture_java_framework_startup_tests)
  set_tests_properties(java_framework_startup PROPERTIES TIMEOUT 60)
  set_tests_properties(venture_java_tests PROPERTIES ENVIRONMENT "VENTURE_JVM_DIAGNOSTICS=1")
  add_executable(venture_a32_java_tests runtime/a32_memory_main.cpp runtime/java_a32_test.cpp)
  target_include_directories(venture_a32_java_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
  target_link_libraries(venture_a32_java_tests PRIVATE venture_java venture_android venture_hle doctest::doctest)
  target_compile_definitions(venture_a32_java_tests PRIVATE
    VENTURE_TEST_JAVA_HOME="${VENTURE_JAVA_HOME}" VENTURE_TEST_BRIDGE_JAR="${VENTURE_JAVA_BRIDGE_JAR}"
    VENTURE_TEST_JVM_AGENT="$<TARGET_FILE:venture_jvmti_agent>"
    VENTURE_TEST_JAVA_CLASSES="${CMAKE_CURRENT_BINARY_DIR}/java-classes"
    VENTURE_ANDROID_FIXTURES="${VENTURE_ANDROID_FIXTURES}")
  add_dependencies(venture_a32_java_tests venture_java_fixture venture_android_fixtures)
  add_dependencies(venture_a32_java_tests venture_jvmti_agent)
  if(NOT WIN32)
    set_property(TARGET venture_a32_java_tests PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_link_options(venture_a32_java_tests PRIVATE -pie)
  endif()
  add_test(NAME venture_a32_java_tests COMMAND venture_a32_java_tests)
  # Real freestanding guest code exercises dlsym and the returned VM tables.
  # Clang can produce these small ELF fixtures without an Android sysroot.
  find_program(VENTURE_JNI_FIXTURE_CLANG NAMES clang
    HINTS "${PROJECT_SOURCE_DIR}/.deps/llvm-mingw/bin")
  find_program(VENTURE_JNI_FIXTURE_LLD NAMES ld.lld
    HINTS "${PROJECT_SOURCE_DIR}/.deps/llvm-mingw/bin")
  if(VENTURE_JNI_FIXTURE_CLANG AND VENTURE_JNI_FIXTURE_LLD)
    foreach(abi IN ITEMS a64 a32)
      if(abi STREQUAL "a64")
        set(triple aarch64-linux-android23)
        set(test_target venture_java_tests)
      else()
        set(triple armv7a-linux-androideabi23)
        set(test_target venture_a32_java_tests)
      endif()
      set(fixture "${CMAKE_CURRENT_BINARY_DIR}/libvtest_jni_invocation_${abi}.so")
      add_custom_command(OUTPUT "${fixture}"
        COMMAND "${VENTURE_JNI_FIXTURE_CLANG}" --target=${triple} -O2 -fPIC
          -fno-stack-protector -nostdlib -shared -fuse-ld=${VENTURE_JNI_FIXTURE_LLD} -Wl,--hash-style=sysv
          "${CMAKE_CURRENT_SOURCE_DIR}/guest/jni_invocation.c" -o "${fixture}"
        DEPENDS guest/jni_invocation.c VERBATIM)
      add_custom_target(venture_jni_invocation_${abi} DEPENDS "${fixture}")
      add_dependencies(${test_target} venture_jni_invocation_${abi})
      target_compile_definitions(${test_target} PRIVATE VENTURE_TEST_JNI_INVOCATION_FIXTURE="${fixture}")
    endforeach()
  endif()
  add_executable(venture_java_conversion_probe runtime/java_conversion_probe.cpp)
  target_link_libraries(venture_java_conversion_probe PRIVATE venture_java)
  if(NOT TARGET Python3::Interpreter)
    find_package(Python3 COMPONENTS Interpreter QUIET)
  endif()
  if(Python3_Interpreter_FOUND)
    add_test(NAME venture_unicode_paths COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/unicode_paths_test.py")
    set_tests_properties(venture_unicode_paths PROPERTIES ENVIRONMENT "VENTURE_CONVERSION_PROBE=$<TARGET_FILE:venture_java_conversion_probe>" TIMEOUT 300)
    add_test(NAME venture_java_conversion COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/java_conversion_test.py")
    set_tests_properties(venture_java_conversion PROPERTIES ENVIRONMENT "VENTURE_CONVERSION_PROBE=$<TARGET_FILE:venture_java_conversion_probe>")
    # Locks the build JARs exclusively to prove the copy never reads them.
  endif()
endif()

if(TARGET venture_gfx)

  add_executable(venture_a32_gfx_tests runtime/a32_memory_main.cpp runtime/gfx_a32_test.cpp)
  target_include_directories(venture_a32_gfx_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
  target_link_libraries(venture_a32_gfx_tests PRIVATE venture_gfx venture_hle doctest::doctest)
  if(NOT WIN32)
    set_property(TARGET venture_a32_gfx_tests PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_link_options(venture_a32_gfx_tests PRIVATE -pie)
  endif()
  add_test(NAME a32_gfx COMMAND venture_a32_gfx_tests)
target_sources(venture_tests PRIVATE runtime/gfx_test_support.cpp runtime/gfx_test.cpp runtime/blob_cache_test.cpp )
  target_link_libraries(venture_tests PRIVATE venture_gfx venture_android zlibstatic)
  target_include_directories(venture_tests PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
endif()

if(TARGET venture_vulkan)
  include(${PROJECT_SOURCE_DIR}/src/runtime/hle/vulkan/tests.cmake)
endif()
add_subdirectory(java)
if(TARGET venture_java_framework)
  if(TARGET venture_gfx AND TARGET venture_vulkan)
    add_executable(venture_java_graphics_tests test_main.cpp runtime/java_graphics_test.cpp)
    target_link_libraries(venture_java_graphics_tests PRIVATE venture_java_framework venture_gfx venture_vulkan venture_hle doctest::doctest)
    target_compile_definitions(venture_java_graphics_tests PRIVATE
      VENTURE_TEST_JAVA_HOME="${VENTURE_JAVA_HOME}" VENTURE_TEST_FRAMEWORK_JAR="${VENTURE_FRAMEWORK_JAR}"
      VENTURE_TEST_JAVA_CLASSES="${CMAKE_CURRENT_BINARY_DIR}/java/classes"
      VENTURE_TEST_MOCK_XR="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mock_openxr_runtime.json")
    add_dependencies(venture_java_graphics_tests venture_framework_tests venture_mock_openxr_runtime)
    add_test(NAME java_graphics COMMAND venture_java_graphics_tests)
    # The mock host remembers its display rate in the data dir: keep it out of the user profile.
    set_tests_properties(java_graphics PROPERTIES TIMEOUT 30 ENVIRONMENT "VENTURE_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/xr-test-data")
    add_test(NAME java_graphics_inputs COMMAND venture_java_graphics_tests)
    set_tests_properties(java_graphics_inputs PROPERTIES TIMEOUT 15 ENVIRONMENT "VENTURE_TEST_EGL_INPUTS_ONLY=1;VENTURE_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/xr-test-data")
  endif()
  add_executable(venture_framework_native_tests test_main.cpp runtime/framework_native_test.cpp
    ${PROJECT_SOURCE_DIR}/src/runtime/core/device_identity.cpp
    ${PROJECT_SOURCE_DIR}/src/runtime/java/natives/framework_natives.cpp
    ${PROJECT_SOURCE_DIR}/src/runtime/java/natives/path_bridge.cpp)
  target_include_directories(venture_framework_native_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime ${VENTURE_JAVA_HOME}/include)
  if(WIN32)
    target_include_directories(venture_framework_native_tests PRIVATE ${VENTURE_JAVA_HOME}/include/win32)
  else()
    target_include_directories(venture_framework_native_tests PRIVATE ${VENTURE_JAVA_HOME}/include/linux)
  endif()
  target_link_libraries(venture_framework_native_tests PRIVATE venture_common doctest::doctest)
  add_test(NAME framework_native COMMAND venture_framework_native_tests)
  add_executable(venture_framework_runtime_tests test_main.cpp runtime/framework_runtime_test.cpp)
  target_link_libraries(venture_framework_runtime_tests PRIVATE venture_java_framework venture_hle doctest::doctest)
  target_compile_definitions(venture_framework_runtime_tests PRIVATE
    VENTURE_TEST_JAVA_HOME="${VENTURE_JAVA_HOME}" VENTURE_TEST_FRAMEWORK_JAR="${VENTURE_FRAMEWORK_JAR}")
  if(TARGET venture_android_fixtures)
    target_compile_definitions(venture_framework_runtime_tests PRIVATE VENTURE_ANDROID_FIXTURES="${VENTURE_ANDROID_FIXTURES}")
    add_dependencies(venture_framework_runtime_tests venture_android_fixtures)
  endif()
  add_test(NAME framework_runtime COMMAND venture_framework_runtime_tests)
  add_executable(venture_menu_quit_tests test_main.cpp runtime/menu_quit_test.cpp)
  target_link_libraries(venture_menu_quit_tests PRIVATE venture_java_framework venture_hle zlibstatic doctest::doctest)
  target_include_directories(venture_menu_quit_tests PRIVATE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
  target_compile_definitions(venture_menu_quit_tests PRIVATE
    VENTURE_TEST_JAVA_HOME="${VENTURE_JAVA_HOME}" VENTURE_TEST_FRAMEWORK_JAR="${VENTURE_FRAMEWORK_JAR}"
    VENTURE_TEST_JAVA_CLASSES="${CMAKE_CURRENT_BINARY_DIR}/java/classes")
  add_dependencies(venture_menu_quit_tests venture_framework_tests)

  # Existing data-safety quit guard; developer menu suites remain removed.
  add_test(NAME menu_quit_barriers COMMAND venture_menu_quit_tests)
  set_tests_properties(menu_quit_barriers PROPERTIES TIMEOUT 90)
  add_executable(venture_framework_sqlite_tests test_main.cpp runtime/framework_sqlite_test.cpp)
  target_link_libraries(venture_framework_sqlite_tests PRIVATE venture_java_framework venture_hle doctest::doctest)
  target_compile_definitions(venture_framework_sqlite_tests PRIVATE VENTURE_TEST_JAVA_HOME="${VENTURE_JAVA_HOME}"
    VENTURE_TEST_FRAMEWORK_JAR="${VENTURE_FRAMEWORK_JAR}" VENTURE_TEST_JAVA_CLASSES="${CMAKE_CURRENT_BINARY_DIR}/java/classes")
  add_dependencies(venture_framework_sqlite_tests venture_framework_tests)
  add_test(NAME framework_sqlite COMMAND venture_framework_sqlite_tests)
endif()
if(TARGET venture_xr)
  add_executable(venture_a32_xr_tests runtime/a32_memory_main.cpp runtime/a32_xr_test.cpp)
  target_link_libraries(venture_a32_xr_tests PRIVATE venture_xr venture_audio venture_ovrplatform venture_mock_openxr_runtime doctest::doctest)
  if(NOT WIN32)
    set_property(TARGET venture_a32_xr_tests PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_link_options(venture_a32_xr_tests PRIVATE -pie)
  endif()
  add_test(NAME a32_xr COMMAND venture_a32_xr_tests)
  set_tests_properties(a32_xr PROPERTIES ENVIRONMENT "XR_RUNTIME_JSON=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mock_openxr_runtime.json;VENTURE_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/xr-test-data")
  target_link_libraries(venture_tests PRIVATE venture_xr)
add_executable(venture_xr_tests test_main.cpp  runtime/xr_test.cpp runtime/xr_vrapi_test.cpp
     mock_openxr_runtime/input_timeline.cpp runtime/xr_android_surface_test.cpp
     runtime/xr_color_space_test.cpp runtime/xr_call_trace_test.cpp)
  target_link_libraries(venture_xr_tests PRIVATE venture_java_framework)
  target_compile_definitions(venture_xr_tests PRIVATE
    VENTURE_TEST_JAVA_HOME="${VENTURE_JAVA_HOME}" VENTURE_TEST_FRAMEWORK_JAR="${VENTURE_FRAMEWORK_JAR}")
  target_include_directories(venture_xr_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
  add_test(NAME xr_call_observer COMMAND venture_xr_tests "--test-case=XR call*")
  set_tests_properties(xr_call_observer PROPERTIES ENVIRONMENT "VENTURE_TRACE_XR_FRAMES=1" TIMEOUT 30)
  add_test(NAME venture_xr_tests COMMAND venture_xr_tests)
  # Run wall-clock pacing assertions before long CPU/compile work accumulates.
  set_tests_properties(venture_xr_tests PROPERTIES COST 2000)

  add_test(NAME vrapi_vulkan_process_exit COMMAND venture_xr_tests
    "--test-case=VrApi Vulkan process exit with live images" --no-skip=true)
  add_test(NAME vrapi_vulkan_destroyed_device_exit COMMAND venture_xr_tests
    "--test-case=VrApi Vulkan process exit after the game destroyed its device" --no-skip=true)
  target_link_libraries(venture_xr_tests PRIVATE venture_xr venture_mock_openxr_runtime venture_xr_capture zlibstatic doctest::doctest nlohmann_json::nlohmann_json)
  set_tests_properties(${venture_test_partitions} venture_xr_tests vrapi_vulkan_process_exit vrapi_vulkan_destroyed_device_exit PROPERTIES ENVIRONMENT "XR_RUNTIME_JSON=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mock_openxr_runtime.json;VENTURE_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/xr-test-data")
  # Real-device Vulkan tests pick their GPU through the XR runtime: use the mock, not
  # whatever runtime (and headset driver) this machine has active.
  foreach(test vulkan_lazy vulkan_terminal_allocator)
    if(TEST ${test})
      set_tests_properties(${test} PROPERTIES ENVIRONMENT "XR_RUNTIME_JSON=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mock_openxr_runtime.json")
    endif()
  endforeach()
  # A runtime that numbers each handle type from 1 (VDXR): the instance and session share a value.
  add_test(NAME xr_typed_handles COMMAND venture_xr_tests
    "--test-case=XR guest lifecycle and real Vulkan swapchain through mock loader")
  set_tests_properties(xr_typed_handles PROPERTIES ENVIRONMENT "XR_RUNTIME_JSON=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mock_openxr_runtime.json;VENTURE_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/xr-typed-handles-data;VENTURE_MOCK_TYPED_HANDLES=1")
  # A runtime that leaves untracked velocities undefined (NaN): the guest must see zeros.
  add_test(NAME xr_no_velocity COMMAND venture_xr_tests
    "--test-case=XR guest lifecycle and real Vulkan swapchain through mock loader")
  set_tests_properties(xr_no_velocity PROPERTIES ENVIRONMENT "XR_RUNTIME_JSON=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mock_openxr_runtime.json;VENTURE_DATA_DIR=${CMAKE_CURRENT_BINARY_DIR}/xr-no-velocity-data;VENTURE_MOCK_NO_VELOCITY=1")
  add_test(NAME xr_no_hmd COMMAND venture_xr_probe)
  set_tests_properties(xr_no_hmd PROPERTIES
    ENVIRONMENT "XR_RUNTIME_JSON=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mock_openxr_runtime.json;VENTURE_MOCK_NO_HMD=1"
    PASS_REGULAR_EXPRESSION "HMD available: 0 .*Error: Venture Mock OpenXR reports no headset")
  add_test(NAME xr_missing_runtime COMMAND venture_xr_probe)

  set_tests_properties(xr_missing_runtime PROPERTIES
    ENVIRONMENT "XR_RUNTIME_JSON=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/nonexistent_runtime.json"
    PASS_REGULAR_EXPRESSION "HMD available: 0 .*Error: Your configured OpenXR runtime is unavailable\\."
    # Below the 30 s headset wait, so it still proves a missing runtime fails without
    # waiting, but above the 5 s a loaded machine can take just to start the probe.
    TIMEOUT 20)
  if(NOT TARGET Python3::Interpreter)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
  endif()
  FetchContent_GetProperties(openxr_sdk SOURCE_DIR venture_xr_sdk_root)
  add_test(NAME xr_codegen COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/tools/codegen/xr_generate.py
    --registry ${venture_xr_sdk_root}/specification/registry/xr.xml --check)
endif()

if(TARGET venture-runtime)
  if(WIN32)

target_sources(venture_tests PRIVATE )

  endif()
endif()

set_tests_properties(venture_tests PROPERTIES RESOURCE_LOCK venture_libc_fixtures)
set_tests_properties(venture_tests_vulkan PROPERTIES RESOURCE_LOCK venture_vulkan_captures)

# Each process gets its own data and OS temp roots, including inherited children.
# Keep OS temp roots short: path tests first create ordinary directory prefixes
# before exercising the long-path-aware publication functions.
if(WIN32)
  file(TO_CMAKE_PATH "$ENV{TEMP}" venture_test_temp_base)
else()
  set(venture_test_temp_base "$ENV{TMPDIR}")
  if(NOT venture_test_temp_base)
    set(venture_test_temp_base /tmp)
  endif()
endif()
string(SHA256 venture_test_build_key "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${venture_test_build_key}" 0 6 venture_test_build_key)
set(venture_test_partition_index 0)
foreach(test IN LISTS venture_test_partitions)
  math(EXPR venture_test_partition_index "${venture_test_partition_index} + 1")
  set(root "${CMAKE_CURRENT_BINARY_DIR}/partition-data/${test}")
  set(temp "${venture_test_temp_base}/vct-${venture_test_build_key}-${venture_test_partition_index}")
  file(MAKE_DIRECTORY "${temp}" "${root}/local")
  set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT
    "VENTURE_DATA_DIR=${root}/data" "LOCALAPPDATA=${root}/local"
    "TEMP=${temp}" "TMP=${temp}" "TMPDIR=${temp}")
endforeach()
set_property(TEST venture_tests_jit_cache_alloc_checks APPEND PROPERTY ENVIRONMENT "VENTURE_JIT_CHECK_ALLOC_SCOPE=1")

# Public Android API 32 constants: pure JVM reflection, no game or OpenXR calls.
if(TARGET venture-framework)
  set(manifest_api_classes "${CMAKE_CURRENT_BINARY_DIR}/manifest-api-classes")
  add_custom_command(OUTPUT "${manifest_api_classes}/ManifestApiFixture.class"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${manifest_api_classes}"
    COMMAND "${VENTURE_JAVA_HOME}/bin/javac" -encoding UTF-8 -d "${manifest_api_classes}"
      "${CMAKE_CURRENT_SOURCE_DIR}/java/ManifestApiFixture.java"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/java/ManifestApiFixture.java"
    VERBATIM)
  add_custom_target(venture_manifest_api_fixture ALL DEPENDS "${manifest_api_classes}/ManifestApiFixture.class")
  add_dependencies(venture_manifest_api_fixture venture-framework)
  add_test(NAME framework_manifest_api COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/framework_manifest_api_test.py" "${VENTURE_JAVA_HOME}"
    "${manifest_api_classes}" "${VENTURE_FRAMEWORK_JAR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/java/android32-manifest-api.txt")
  set_tests_properties(framework_manifest_api PROPERTIES TIMEOUT 35)
endif()

if(TARGET dynarmic)
  add_executable(venture_jit_pc_observer_test runtime/jit_pc_observer_test.cpp)
  target_link_libraries(venture_jit_pc_observer_test PRIVATE dynarmic merry::mcl)
  add_test(NAME jit_pc_observer COMMAND venture_jit_pc_observer_test)
endif()


# Isolate allocation-failure injection from the broad test suite and runtime.
if(TARGET venture_vulkan)
  add_executable(venture_deferred_queue_error_tests runtime/deferred_queue_error_test.cpp)
  target_include_directories(venture_deferred_queue_error_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)
  target_link_libraries(venture_deferred_queue_error_tests PRIVATE venture_vulkan doctest::doctest)
  add_test(NAME vulkan_deferred_error_contract COMMAND venture_deferred_queue_error_tests)
endif()
