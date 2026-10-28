# Integrated tests use real core, graphics and native-window implementations.
if(NOT TARGET Python3::Interpreter)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
target_sources(venture_tests PRIVATE runtime/vulkan_test.cpp runtime/vulkan_shader_test.cpp runtime/vulkan_subpass_sync_test.cpp runtime/vulkan_callbacks_test_support.cpp)
target_sources(venture_tests PRIVATE runtime/deferred_queues_test.cpp runtime/vulkan_fastpath_test.cpp)

target_link_libraries(venture_tests PRIVATE venture_vulkan)

target_sources(venture_tests PRIVATE runtime/vulkan_texture_test.cpp)
target_sources(venture_tests PRIVATE runtime/vulkan_multiview_test.cpp)
target_include_directories(venture_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
foreach(stage vert frag)
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vulkan_multiview.${stage}.inc
    COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.1
      ${PROJECT_SOURCE_DIR}/tests/runtime/vulkan_multiview.${stage}
      -o ${CMAKE_CURRENT_BINARY_DIR}/vulkan_multiview.${stage}.spv
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
      ${CMAKE_CURRENT_BINARY_DIR}/vulkan_multiview.${stage}.spv
      ${CMAKE_CURRENT_BINARY_DIR}/vulkan_multiview.${stage}.inc
    DEPENDS glslang-standalone runtime/vulkan_multiview.${stage} ${PROJECT_SOURCE_DIR}/tools/codegen/vk_spirv.py
    VERBATIM)
  target_sources(venture_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vulkan_multiview.${stage}.inc)
endforeach()
target_link_libraries(venture_tests PRIVATE astcenc-sse2-static)
if(UNIX)
  target_link_libraries(venture_tests PRIVATE ${CMAKE_DL_LIBS})
endif()

# Terminal allocator cases leave irreversible quit state: one process each.
target_sources(venture_tests PRIVATE runtime/terminal_allocator_test.cpp runtime/compatible_allocator_test.cpp)
add_test(NAME vulkan_terminal_allocator COMMAND venture_tests
  "--test-case=Terminal allocator bridge drains a real native device without guest reentry" --no-skip=true)
add_test(NAME vulkan_compatible_allocators COMMAND venture_tests
  "--test-case=Compatible allocator records preserve cross-record ownership" --no-skip=true)
add_test(NAME vulkan_throwing_allocator COMMAND venture_tests
  "--test-case=Throwing allocator callback cannot report clean terminal ownership" --no-skip=true)
add_test(NAME vulkan_stopped_allocator COMMAND venture_tests
  "--test-case=Stopped guest allocator serves native calls before the terminal bridge" --no-skip=true)
