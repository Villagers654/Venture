# The Quest compatibility layer: an implicit OpenXR API layer, shipped in the
# Android APK (assets/openxr/1/api_layers/implicit.d) and tested on x64
# against the mock runtime's headset profiles. It answers the Quest
# extensions a runtime lacks and runs OpenXR 1.1 games on 1.0 runtimes
# (docs/design/native-headsets.md, section 5.4).
include(${PROJECT_SOURCE_DIR}/cmake/OpenXRSdk.cmake)

# The extensions the layer answers when the runtime lacks them, name:version.
# One list for the code (generated extensions.inc) and the manifests, whose
# instance_extensions the loader checks an application's request against.
set(VENTURE_QUEST_LAYER_EXTENSIONS
  XR_KHR_android_thread_settings:6
  XR_FB_display_refresh_rate:1
  XR_FB_color_space:3
  XR_KHR_locate_spaces:1)
set(venture_quest_layer_name XR_APILAYER_VENTURE_quest_compat)

set(venture_quest_layer_code "")
set(venture_quest_layer_json "")
foreach(entry IN LISTS VENTURE_QUEST_LAYER_EXTENSIONS)
  string(REPLACE ":" ";" parts "${entry}")
  list(GET parts 0 name)
  list(GET parts 1 version)
  string(APPEND venture_quest_layer_code "{\"${name}\", ${version}},\n")
  if(venture_quest_layer_json)
    string(APPEND venture_quest_layer_json ",")
  endif()
  string(APPEND venture_quest_layer_json "\n      {\"name\": \"${name}\", \"extension_version\": \"${version}\"}")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/quest_layer/extensions.inc"
  CONTENT "// Generated from VENTURE_QUEST_LAYER_EXTENSIONS (src/quest_layer/CMakeLists.txt).\n${venture_quest_layer_code}")

add_library(venture_quest_layer_core STATIC layer.cpp records.cpp frame_timing.cpp)
target_include_directories(venture_quest_layer_core PUBLIC ${PROJECT_SOURCE_DIR}/src
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(venture_quest_layer_core PUBLIC
  VENTURE_QUEST_LAYER_NAME="${venture_quest_layer_name}")
target_link_libraries(venture_quest_layer_core PUBLIC OpenXR::headers)
# The records (records.cpp) name the Vulkan functions' types where the headers
# are there; on Android the NDK has them, with EGL's.
if(TARGET Vulkan::Headers)
  target_link_libraries(venture_quest_layer_core PRIVATE Vulkan::Headers)
endif()
if(ANDROID)
  target_link_libraries(venture_quest_layer_core PUBLIC log)
endif()

add_library(venture_quest_layer SHARED export.cpp)
target_link_libraries(venture_quest_layer PRIVATE venture_quest_layer_core)
set_target_properties(venture_quest_layer PROPERTIES PREFIX "lib" CXX_VISIBILITY_PRESET hidden)
if(ANDROID)
  target_link_options(venture_quest_layer PRIVATE -Wl,--no-undefined)
  target_link_libraries(venture_quest_layer PRIVATE log)
endif()

# The implicit manifest as the APK ships it, next to the library. Android's
# loader finds the library by name; the desktop loader relative to the file.
if(ANDROID)
  set(venture_quest_layer_path "$<TARGET_FILE_NAME:venture_quest_layer>")
else()
  set(venture_quest_layer_path "./$<TARGET_FILE_NAME:venture_quest_layer>")
endif()
set(venture_quest_layer_manifest "{\n  \"file_format_version\": \"1.0.0\",\n  \"api_layer\": {\n    \"name\": \"${venture_quest_layer_name}\",\n    \"library_path\": \"@PATH@\",\n    \"api_version\": \"1.1\",\n    \"implementation_version\": \"1\",\n    \"description\": \"Venture: Quest extensions and OpenXR 1.1 on any runtime\",\n    @EXTRA@\"instance_extensions\": [${venture_quest_layer_json}\n    ]\n  }\n}\n")
string(REPLACE "@PATH@" "${venture_quest_layer_path}" implicit "${venture_quest_layer_manifest}")
string(REPLACE "@EXTRA@" "\"disable_environment\": \"DISABLE_VENTURE_QUEST_LAYER\",\n    " implicit "${implicit}")
file(GENERATE OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/venture_quest_layer.json" CONTENT "${implicit}")
# The same layer as an explicit one, for tests that enable it by name.
string(REPLACE "@PATH@" "../$<TARGET_FILE_NAME:venture_quest_layer>" explicit "${venture_quest_layer_manifest}")
string(REPLACE "@EXTRA@" "" explicit "${explicit}")
file(GENERATE OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/quest-layer-explicit/venture_quest_layer.json" CONTENT "${explicit}")
