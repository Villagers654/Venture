# The one Venture APK for every Android headset (docs/design/native-headsets.md,
# section 5): the launcher (libventure.so, SDL3's Android activity), the app's
# Java side (HTTPS, Keystore, sign-in) and the Quest compatibility layer that
# adapted games receive. Built with Google's command-line build tools only
# (cmake/AndroidSdk.cmake, tools/android/build_apk.py); no Gradle.
include(${PROJECT_SOURCE_DIR}/cmake/AndroidSdk.cmake)

file(GLOB_RECURSE venture_app_java CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/java/*.java
  ${CMAKE_CURRENT_SOURCE_DIR}/shared/*.java)
# Classes the APK adds to the games it installs (assets/game/classes.dex):
# Venture's Platform glue, the shared HTTPS helper and the Meta OS classes a
# Quest supplies that other headsets lack (the plain ones of the PC framework).
file(GLOB_RECURSE venture_game_java CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/game/*.java)
set(venture_framework ${PROJECT_SOURCE_DIR}/src/runtime/java/framework)
set(venture_game_meta_java
  ${venture_framework}/com/oculus/os/AnalyticsEvent.java
  ${venture_framework}/com/oculus/os/UnifiedTelemetryLogger.java
  ${venture_framework}/horizonos/os/Build.java
  ${venture_framework}/metavr/os/Build.java)
# One dex per Java package of Meta classes: the adapter adds each only to
# games that define none of its classes themselves (common/android/game_adapter.cpp).
set(venture_game_meta_packages)
foreach(source ${venture_game_meta_java})
  get_filename_component(folder ${source} DIRECTORY)
  list(APPEND venture_game_meta_packages ${folder})
endforeach()
list(REMOVE_DUPLICATES venture_game_meta_packages)
set(venture_game_meta_dex)
# Each package compiles against the others (horizonos.os.Build uses metavr.os.Build).
set(venture_game_meta_sourcepath)
foreach(source ${venture_game_meta_java})
  list(APPEND venture_game_meta_sourcepath --asset-dex-sourcepath ${source})
endforeach()
foreach(folder ${venture_game_meta_packages})
  set(members)
  foreach(source ${venture_game_meta_java})
    get_filename_component(source_folder ${source} DIRECTORY)
    if(source_folder STREQUAL folder)
      list(APPEND members ${source})
    endif()
  endforeach()
  file(RELATIVE_PATH relative ${venture_framework} ${folder})
  string(REPLACE "/" "." name ${relative})
  list(JOIN members "," member_list)
  list(APPEND venture_game_meta_dex --asset-dex game/meta/${name}.dex=${member_list})
endforeach()
file(GLOB_RECURSE venture_app_res CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/*)
set(venture_sdl_java ${SDL3_SOURCE_DIR}/android-project/app/src/main/java)
file(GLOB_RECURSE venture_sdl_java_files CONFIGURE_DEPENDS ${venture_sdl_java}/*.java)

# The 32-bit game libraries (armeabi-v7a): ARM32-only games run natively on
# CPUs that still execute AArch32 (docs/design/native-headsets.md, 5.5), with
# these in place of Meta's 32-bit ones, as on a Quest that ran them. The same
# sources, built by a sub-build of this tree with the 32-bit NDK toolchain.
include(ExternalProject)
set(venture_a32_dir ${CMAKE_BINARY_DIR}/armeabi-v7a)
set(venture_a32_libraries ovrplatformloader vrapi venture_platform_alias openxr_loader venture_quest_layer)
set(venture_a32_outputs)
foreach(name ${venture_a32_libraries})
  list(APPEND venture_a32_outputs ${venture_a32_dir}/bin/lib${name}.so)
endforeach()
# The 32-bit test game (tools/android/headset_play_test.py), a few KiB.
set(venture_a32_test_game)
foreach(name testgame vrapi ovrplatformloader)
  list(APPEND venture_a32_test_game ${venture_a32_dir}/test_game/lib/lib${name}.so)
endforeach()
set(venture_a32_cache
  -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}
  -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}
  -DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}
  -DVENTURE_ANDROID_ABI:STRING=armeabi-v7a
  -DVENTURE_ANDROID_API:STRING=${VENTURE_ANDROID_API}
  -DVENTURE_ANDROID_NDK:PATH=${VENTURE_ANDROID_NDK}
  -DVENTURE_FETCH_CACHE_DIR:PATH=${VENTURE_FETCH_CACHE_DIR}
  -DFETCHCONTENT_UPDATES_DISCONNECTED:BOOL=ON
  -DVENTURE_AGENT_CACHE:BOOL=${VENTURE_AGENT_CACHE}
  -DVENTURE_ANDROID_GAME_LIBRARIES:BOOL=ON
  -DVENTURE_BUILD_LAUNCHER:BOOL=OFF
  -DVENTURE_BUILD_RUNTIME:BOOL=OFF
  -DVENTURE_BUILD_TESTS:BOOL=OFF)
if(CMAKE_PROJECT_TOP_LEVEL_INCLUDES)
  list(APPEND venture_a32_cache -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES:FILEPATH=${CMAKE_PROJECT_TOP_LEVEL_INCLUDES})
endif()
ExternalProject_Add(venture_game_libraries_a32
  SOURCE_DIR ${PROJECT_SOURCE_DIR}
  BINARY_DIR ${venture_a32_dir}
  CMAKE_GENERATOR Ninja
  CMAKE_CACHE_ARGS ${venture_a32_cache}
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target venture_platform_loader venture_vrapi
    venture_platform_alias venture_openxr_loader venture_quest_layer
    venture_test_game venture_test_game_vrapi venture_test_game_ovrplatformloader
  INSTALL_COMMAND ""
  BUILD_ALWAYS ON
  BUILD_BYPRODUCTS ${venture_a32_outputs} ${venture_a32_test_game}
  USES_TERMINAL_BUILD ON)
set(venture_a32_assets)
foreach(name ovrplatformloader vrapi openxr_loader venture_quest_layer)
  list(APPEND venture_a32_assets --asset game/lib/armeabi-v7a/lib${name}.so=${venture_a32_dir}/bin/lib${name}.so)
endforeach()
list(APPEND venture_a32_assets
  --asset game/lib/armeabi-v7a/platform_alias.so=${venture_a32_dir}/bin/libventure_platform_alias.so)

set(venture_apk ${CMAKE_BINARY_DIR}/apk/venture-${PROJECT_VERSION}-android-arm64.apk)
add_custom_command(OUTPUT ${venture_apk}
  COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/android/build_apk.py
    --build-tools ${VENTURE_ANDROID_BUILD_TOOLS}
    --android-jar ${VENTURE_ANDROID_JAR}
    --java-home ${VENTURE_ANDROID_JAVA_HOME}
    --manifest ${CMAKE_CURRENT_SOURCE_DIR}/AndroidManifest.xml
    --res ${CMAKE_CURRENT_SOURCE_DIR}/res
    --icon-svg ${PROJECT_SOURCE_DIR}/src/launcher/assets/venture.svg
    --java ${CMAKE_CURRENT_SOURCE_DIR}/java
    --java ${CMAKE_CURRENT_SOURCE_DIR}/shared
    --java ${venture_sdl_java}
    --jar-classes ${VENTURE_ANDROID_BUILD_TOOLS}/lib/apksigner.jar=com/android/apksig/
    --asset-dex game/classes.dex=${CMAKE_CURRENT_SOURCE_DIR}/game,${CMAKE_CURRENT_SOURCE_DIR}/shared
    ${venture_game_meta_sourcepath}
    ${venture_game_meta_dex}
    --asset game/lib/arm64-v8a/$<TARGET_FILE_NAME:venture_platform_loader>=$<TARGET_FILE:venture_platform_loader>
    --asset game/lib/arm64-v8a/$<TARGET_FILE_NAME:venture_vrapi>=$<TARGET_FILE:venture_vrapi>
    --asset game/platform_alias.so=$<TARGET_FILE:venture_platform_alias>
    ${venture_a32_assets}
    --strip ${CMAKE_STRIP}
    --lib $<TARGET_FILE:venture>
    --lib $<TARGET_FILE:venture_openxr_loader>
    --asset quest_layer/$<TARGET_FILE_NAME:venture_quest_layer>=$<TARGET_FILE:venture_quest_layer>
    --asset quest_layer/venture_quest_layer.json=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/venture_quest_layer.json
    --min-sdk ${VENTURE_ANDROID_API}
    --target-sdk ${VENTURE_ANDROID_TARGET_SDK}
    --version ${PROJECT_VERSION}
    --work ${CMAKE_CURRENT_BINARY_DIR}/apk-work
    --out ${venture_apk}
  DEPENDS venture venture_quest_layer venture_platform_loader venture_vrapi venture_openxr_loader
    venture_platform_alias venture_game_libraries_a32 ${venture_a32_outputs} ${CMAKE_CURRENT_SOURCE_DIR}/AndroidManifest.xml
    ${venture_app_java} ${venture_app_res} ${venture_sdl_java_files} ${venture_game_java} ${venture_game_meta_java}
    ${PROJECT_SOURCE_DIR}/tools/android/build_apk.py ${PROJECT_SOURCE_DIR}/src/launcher/assets/venture.svg
  COMMENT "Packaging the Venture APK"
  VERBATIM)
add_custom_target(venture_apk ALL DEPENDS ${venture_apk})

# The test games of tools/android/headset_play_test.py: a Quest-style
# NativeActivity for each ABI, which Venture imports, adapts and installs on
# the emulator. Not part of the default build.
set(venture_test_game_dir ${CMAKE_BINARY_DIR}/test_game)
file(MAKE_DIRECTORY ${venture_test_game_dir}/res)
set(venture_test_game_apks)
foreach(abi arm64-v8a armeabi-v7a)
  if(abi STREQUAL "arm64-v8a")
    set(suffix a64)
    set(libs --lib $<TARGET_FILE:venture_test_game> --lib $<TARGET_FILE:venture_test_game_vrapi>
      --lib $<TARGET_FILE:venture_test_game_ovrplatformloader>)
    set(depends venture_test_game venture_test_game_vrapi venture_test_game_ovrplatformloader)
  else()
    set(suffix a32)
    set(libs)
    foreach(lib ${venture_a32_test_game})
      list(APPEND libs --lib ${lib})
    endforeach()
    set(depends venture_game_libraries_a32 ${venture_a32_test_game})
  endif()
  set(SUFFIX ${suffix})
  configure_file(${PROJECT_SOURCE_DIR}/tests/android/test_game/AndroidManifest.xml
    ${venture_test_game_dir}/${abi}/AndroidManifest.xml @ONLY)
  set(apk ${venture_test_game_dir}/testgame-${abi}.apk)
  add_custom_command(OUTPUT ${apk}
    COMMAND Python3::Interpreter ${PROJECT_SOURCE_DIR}/tools/android/build_apk.py
      --build-tools ${VENTURE_ANDROID_BUILD_TOOLS}
      --android-jar ${VENTURE_ANDROID_JAR}
      --java-home ${VENTURE_ANDROID_JAVA_HOME}
      --manifest ${venture_test_game_dir}/${abi}/AndroidManifest.xml
      --res ${venture_test_game_dir}/res
      --abi ${abi} ${libs}
      --strip ${CMAKE_STRIP}
      --min-sdk 29 --target-sdk 32 --version 1.0
      --work ${venture_test_game_dir}/${abi}/work
      --out ${apk}
    DEPENDS ${depends} ${PROJECT_SOURCE_DIR}/tests/android/test_game/AndroidManifest.xml
      ${PROJECT_SOURCE_DIR}/tools/android/build_apk.py
    COMMENT "Packaging the ${abi} test game"
    VERBATIM)
  list(APPEND venture_test_game_apks ${apk})
endforeach()
add_custom_target(venture_test_game_apks DEPENDS ${venture_test_game_apks})
# The tools that sign them, for headset_play_test.py --direct, which signs a
# game adapted on the PC.
file(GENERATE OUTPUT ${venture_test_game_dir}/tools.json CONTENT
  "{\"build_tools\": \"${VENTURE_ANDROID_BUILD_TOOLS}\", \"java_home\": \"${VENTURE_ANDROID_JAVA_HOME}\"}\n")
