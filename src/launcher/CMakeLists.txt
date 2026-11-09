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
# Interface translations: i18n/*.po compiled in at build time (cmake/I18n.cmake).
include(${PROJECT_SOURCE_DIR}/cmake/I18n.cmake)
i18n_add_library(venture_i18n SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/i18n)
# The launcher without its window (core.h): the GUI and venture-cli share it.
add_library(venture_launcher_core STATIC core.cpp debug_report.cpp diagnosis.cpp dlss_optimization.cpp drives.cpp import.cpp sign_in_link.cpp store_link.cpp updater.cpp ui/rich_text_parse.cpp)
target_link_libraries(venture_launcher_core PUBLIC venture_common venture_ovrplatform venture_i18n)
# Package -> app id pairs of the tested titles (data/compatibility.json) link
# imported APKs to their store listing without a lookup (store_link.h).
set(VENTURE_COMPATIBILITY_JSON ${PROJECT_SOURCE_DIR}/data/compatibility.json)
file(READ ${VENTURE_COMPATIBILITY_JSON} VENTURE_COMPATIBILITY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${VENTURE_COMPATIBILITY_JSON})
set(VENTURE_KNOWN_STORE_IDS "")
string(JSON VENTURE_TITLE_COUNT ERROR_VARIABLE VENTURE_TITLE_ERROR LENGTH "${VENTURE_COMPATIBILITY}" titles)
if(NOT VENTURE_TITLE_ERROR AND VENTURE_TITLE_COUNT GREATER 0)
  math(EXPR VENTURE_TITLE_LAST "${VENTURE_TITLE_COUNT} - 1")
  foreach(i RANGE ${VENTURE_TITLE_LAST})
    string(JSON package ERROR_VARIABLE package_error GET "${VENTURE_COMPATIBILITY}" titles ${i} package)
    string(JSON app_id ERROR_VARIABLE app_id_error GET "${VENTURE_COMPATIBILITY}" titles ${i} app_id)
    if(NOT package_error AND NOT app_id_error AND package MATCHES "^[A-Za-z0-9_.]+$"
       AND app_id MATCHES "^[0-9][0-9][0-9][0-9][0-9]+$")
      string(APPEND VENTURE_KNOWN_STORE_IDS "  {\"${package}\", \"${app_id}\"},\n")
    endif()
  endforeach()
endif()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/launcher/known_store_ids.h
  CONTENT "#pragma once
#include <string_view>
#include <utility>
inline constexpr std::pair<std::string_view, std::string_view> kKnownStoreIds[] = {
${VENTURE_KNOWN_STORE_IDS}  {\"\", \"\"}};
" @ONLY)
target_include_directories(venture_launcher_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
# Store links are looked up again once per version (core.cpp).
target_compile_definitions(venture_launcher_core PRIVATE VENTURE_VERSION="${PROJECT_VERSION}")
add_executable(venture WIN32 main.cpp app.cpp ui/style.cpp ui/views.cpp ui/chrome.cpp ui/logo.cpp ui/rich_text.cpp assets/texture_cache.cpp
  assets/controller_art.cpp
  assets/render_model.cpp)
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
# The icon font for systems without Segoe's (tools/make_icon_font.py builds it).
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/assets/venture-icons.ttf ICON_FONT_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ICON_FONT_BYTES "${ICON_FONT_HEX}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/venture-icons.ttf)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/launcher/icon_font.h
  CONTENT "#pragma once
inline constexpr unsigned char kIconFont[] = {${ICON_FONT_BYTES}};
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
  target_link_libraries(venture_launcher_core PUBLIC ole32 user32 advapi32)
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
