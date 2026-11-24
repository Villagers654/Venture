add_library(venture_common STATIC
  controls.cpp
  controls_file.cpp
  digest.cpp
  environment.cpp
  log.cpp
  paths.cpp
  saves_migration.cpp
  process.cpp
  steam.cpp
  steam_library.cpp
  steam_art.cpp
  search.cpp
  ipc.cpp
  launch_details.cpp
  openxr_layers.cpp
  injected_modules.cpp
  startup_progress.cpp
  stall_record.cpp
  system_fonts.cpp
  http/client.cpp
  meta/auth.cpp
  meta/accounts.cpp
  meta/store.cpp
  meta/commerce.cpp
  library/library.cpp
  library/display.cpp
  standalone.cpp
  library/eye_scale_db.cpp
  library/import.cpp
  library/save_import.cpp
  library/mods.cpp
  android/adb.cpp
  android/archive.cpp
  layoutlib_component.cpp
  android/manifest.cpp
  android/native_host.cpp
  android/elf.cpp
  android/dex.cpp
  android/apk_rewrite.cpp
  android/game_adapter.cpp
  android/headset_games.cpp
  android/axml.cpp
  download/manager.cpp
  download/asset_files.cpp
)
target_include_directories(venture_common PUBLIC ${PROJECT_SOURCE_DIR}/src)
# The eye scale database (third_party/venture-eye-scale, a copy of its own
# repository; tools/sync_eye_scale_db.py) is compiled in.
set(venture_eye_scale_db "${PROJECT_SOURCE_DIR}/third_party/venture-eye-scale/titles.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${venture_eye_scale_db}")
file(READ "${venture_eye_scale_db}" VENTURE_EYE_SCALE_TITLES)
configure_file(library/eye_scale_titles.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/eye_scale_titles.h" @ONLY)
# How Steam portraits frame known titles' store covers (common/steam_art.cpp).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/steam_portraits.json")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/steam_portraits.json" VENTURE_STEAM_PORTRAITS)
configure_file(steam_portraits.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/steam_portraits.h" @ONLY)
target_include_directories(venture_common PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(venture_common PUBLIC nlohmann_json::nlohmann_json CURL::libcurl PRIVATE miniz)
if(VENTURE_BUILD_LAUNCHER)
  target_compile_definitions(venture_common PRIVATE VENTURE_STEAM_ART)
  target_include_directories(venture_common SYSTEM PRIVATE ${stb_SOURCE_DIR} ${webp_SOURCE_DIR}/src)
  target_link_libraries(venture_common PRIVATE webpdecoder)
endif()
if(WIN32)
  target_sources(venture_common PRIVATE platform/win32/digest.cpp platform/win32/paths.cpp platform/win32/secret_store.cpp platform/win32/process.cpp platform/win32/file_ops.cpp platform/win32/pipe.cpp platform/win32/openxr_layers.cpp platform/win32/system_fonts.cpp
    platform/win32/injected_modules.cpp)
  target_link_libraries(venture_common PRIVATE crypt32 bcrypt shell32 ole32 uuid advapi32 version)
elseif(ANDROID)
  # One APK for every Android headset. The NDK has no stable crypto or keyring
  # API for native code: portable SHA-256, sessions through the app's Keystore.
  # HTTPS, the Keystore and the rest of the app's Java side through JNI
  # (platform/android/jni.h); fonts from /system/fonts.
  target_sources(venture_common PRIVATE sha256_portable.cpp platform/android/digest.cpp platform/android/secret_store.cpp
    platform/android/jni.cpp platform/android/http_transfer.cpp platform/android/system_fonts.cpp
    platform/posix/paths.cpp platform/posix/process.cpp platform/posix/file_ops.cpp platform/posix/pipe.cpp platform/posix/dbus.cpp platform/posix/openxr_layers.cpp
    platform/posix/injected_modules.cpp)
  target_link_libraries(venture_common PRIVATE ${CMAKE_DL_LIBS} log)
  add_library(venture_core SHARED platform/android/native_core.cpp)
  target_link_libraries(venture_core PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,venture_common>")
  target_link_options(venture_core PRIVATE -Wl,--no-undefined)
else()
  find_package(OpenSSL REQUIRED)
  target_link_libraries(venture_common PRIVATE OpenSSL::Crypto)
  target_sources(venture_common PRIVATE platform/posix/digest.cpp platform/posix/paths.cpp platform/posix/secret_store.cpp platform/posix/process.cpp platform/posix/file_ops.cpp platform/posix/pipe.cpp platform/posix/dbus.cpp platform/posix/openxr_layers.cpp platform/posix/system_fonts.cpp
    platform/posix/injected_modules.cpp)
  target_link_libraries(venture_common PRIVATE ${CMAKE_DL_LIBS})
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBSECRET QUIET IMPORTED_TARGET libsecret-1)
    if(TARGET PkgConfig::LIBSECRET)
      # Headers only: secret_store.cpp loads libsecret at run time and falls
      # back to a sealed file, so neither libsecret nor GLib is required.
      target_include_directories(venture_common SYSTEM PRIVATE ${LIBSECRET_INCLUDE_DIRS})
      target_compile_definitions(venture_common PRIVATE VENTURE_HAVE_LIBSECRET)
    endif()
  endif()
endif()
