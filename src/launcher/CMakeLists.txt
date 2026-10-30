add_library(venture_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
  ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp)
target_include_directories(venture_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp ${imgui_SOURCE_DIR}/misc/freetype)
target_link_libraries(venture_imgui PUBLIC SDL3::SDL3-static freetype)
target_compile_definitions(venture_imgui PUBLIC IMGUI_ENABLE_FREETYPE IMGUI_USE_WCHAR32)
# The launcher without its window (core.h): the GUI and venture-cli share it.
add_library(venture_launcher_core STATIC core.cpp debug_report.cpp drives.cpp import.cpp updater.cpp ui/rich_text_parse.cpp)
target_link_libraries(venture_launcher_core PUBLIC venture_common venture_ovrplatform)
add_executable(venture WIN32 main.cpp app.cpp ui/style.cpp ui/views.cpp ui/chrome.cpp ui/logo.cpp ui/rich_text.cpp assets/texture_cache.cpp)
# The logos ship inside the executable; assets/venture.svg, assets/steam.svg and
# assets/discord.svg are their only sources.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/assets/venture.svg VENTURE_LOGO_SVG)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/assets/steam.svg STEAM_LOGO_SVG)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/assets/discord.svg DISCORD_LOGO_SVG)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/venture.svg
  ${CMAKE_CURRENT_SOURCE_DIR}/assets/steam.svg ${CMAKE_CURRENT_SOURCE_DIR}/assets/discord.svg)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/launcher/logo_svg.h
  CONTENT "#pragma once
inline constexpr char kVentureLogoSvg[] = R\"svg(${VENTURE_LOGO_SVG})svg\";
inline constexpr char kSteamLogoSvg[] = R\"svg(${STEAM_LOGO_SVG})svg\";
inline constexpr char kDiscordLogoSvg[] = R\"svg(${DISCORD_LOGO_SVG})svg\";
" @ONLY)
target_include_directories(venture PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(venture PRIVATE VENTURE_VERSION="${PROJECT_VERSION}")
target_link_libraries(venture PRIVATE venture_launcher_core venture_imgui webpdecoder)
target_include_directories(venture SYSTEM PRIVATE ${stb_SOURCE_DIR} ${webp_SOURCE_DIR}/src ${nanosvg_SOURCE_DIR}/src)
if(WIN32)
  target_sources(venture_launcher_core PRIVATE platform/win32/auth_browser.cpp)
  target_include_directories(venture_launcher_core PRIVATE platform/win32)
  target_include_directories(venture_launcher_core SYSTEM PRIVATE ${webview2_SOURCE_DIR}/build/native/include)
  target_compile_options(venture_launcher_core PRIVATE -fms-extensions)
  target_link_libraries(venture_launcher_core PUBLIC ole32 user32)
  target_sources(venture PRIVATE platform/win32/window_style.cpp)
  target_include_directories(venture PRIVATE platform/win32)
  target_compile_options(venture PRIVATE -fms-extensions)
  target_link_libraries(venture PRIVATE dwmapi)
  # The executable's icon (taskbar, Start menu, shortcuts) is rasterized from assets/venture.svg at build time.
  add_executable(venture-make-icon platform/win32/make_icon.cpp)
  set_target_properties(venture-make-icon PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  target_include_directories(venture-make-icon SYSTEM PRIVATE ${stb_SOURCE_DIR} ${nanosvg_SOURCE_DIR}/src)
  set(VENTURE_ICON ${CMAKE_CURRENT_BINARY_DIR}/generated/launcher/venture.ico)
  add_custom_command(OUTPUT ${VENTURE_ICON}
    COMMAND venture-make-icon ${CMAKE_CURRENT_SOURCE_DIR}/assets/venture.svg ${VENTURE_ICON}
    DEPENDS venture-make-icon ${CMAKE_CURRENT_SOURCE_DIR}/assets/venture.svg ${CMAKE_CURRENT_SOURCE_DIR}/ui/app_icon.h VERBATIM)
  file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/launcher/launcher.rc
    CONTENT "1 ICON \"${VENTURE_ICON}\"\n" @ONLY)
  set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/generated/launcher/launcher.rc PROPERTIES OBJECT_DEPENDS ${VENTURE_ICON})
  target_sources(venture PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/launcher/launcher.rc ${VENTURE_ICON})
  add_custom_command(TARGET venture POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${webview2_SOURCE_DIR}/build/native/x64/WebView2Loader.dll $<TARGET_FILE_DIR:venture>/WebView2Loader.dll)
else()
  target_sources(venture_launcher_core PRIVATE platform/posix/auth_browser.cpp)
  target_sources(venture PRIVATE platform/posix/window_style.cpp platform/posix/system_fonts.cpp)
endif()
if(MINGW)
  target_link_options(venture PRIVATE -municode)
endif()
# venture-cli: the launcher's commands in a terminal, on the same core.
add_executable(venture-cli cli.cpp)
target_compile_definitions(venture-cli PRIVATE VENTURE_VERSION="${PROJECT_VERSION}")
target_link_libraries(venture-cli PRIVATE venture_launcher_core)
if(WIN32)
  add_custom_command(TARGET venture-cli POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${webview2_SOURCE_DIR}/build/native/x64/WebView2Loader.dll $<TARGET_FILE_DIR:venture-cli>/WebView2Loader.dll)
endif()
if(MINGW)
  target_link_options(venture-cli PRIVATE -municode)
endif()

# Focused checks can be built without the game runtime. Excluded from normal
# builds/packages; the probe drives the real App, SDL renderer and ImGui UI.
add_executable(venture_launcher_pacing_tests EXCLUDE_FROM_ALL
  ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
  ${PROJECT_SOURCE_DIR}/tests/launcher_frame_pacing_test.cpp)
target_include_directories(venture_launcher_pacing_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(venture_launcher_pacing_tests PRIVATE doctest::doctest)
if(NOT WIN32)
  get_target_property(venture_probe_sources venture SOURCES)
  list(REMOVE_ITEM venture_probe_sources main.cpp)
  add_executable(venture_launcher_idle_probe EXCLUDE_FROM_ALL
    ${PROJECT_SOURCE_DIR}/tests/launcher_idle_probe.cpp ${venture_probe_sources})
  foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_LIBRARIES)
    get_target_property(value venture ${property})
    set_property(TARGET venture_launcher_idle_probe PROPERTY ${property} "${value}")
  endforeach()
endif()

# Settings persistence checks need no game runtime or headset.
add_executable(venture_launcher_settings_tests EXCLUDE_FROM_ALL
  ${PROJECT_SOURCE_DIR}/tests/test_main.cpp
  ${PROJECT_SOURCE_DIR}/tests/launcher_settings_test.cpp)
target_link_libraries(venture_launcher_settings_tests PRIVATE venture_common doctest::doctest)
if(WIN32)
  target_sources(venture_launcher_settings_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/runtime/xr/steam_link.cpp
    ${PROJECT_SOURCE_DIR}/src/runtime/core/device_identity.cpp)
  target_include_directories(venture_launcher_settings_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/runtime)
endif()
