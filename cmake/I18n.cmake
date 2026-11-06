# Interface translations compiled into a target (src/launcher/i18n, tools/i18n_*.py).
#
#   i18n_add_library(<name> SOURCE_DIR <dir with i18n.h/i18n.cpp>)
#
# builds a static library <name> from i18n.cpp and a source generated from
# i18n/*.po at build time, so packages need no translation files beside the
# program. A new or removed .po is picked up when CMake reruns (CONFIGURE_DEPENDS).
if(NOT TARGET Python3::Interpreter)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()

function(i18n_add_library name)
  cmake_parse_arguments(I18N "" "SOURCE_DIR" "" ${ARGN})
  set(catalogs ${CMAKE_CURRENT_BINARY_DIR}/generated/i18n/${name}_catalogs.cpp)
  file(GLOB po_files CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/i18n/*.po)
  file(GLOB pot_files CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/i18n/*.pot)
  add_custom_command(OUTPUT ${catalogs}
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/i18n_compile.py"
      --root "${PROJECT_SOURCE_DIR}" --out "${catalogs}" --header i18n.h
    DEPENDS ${po_files} ${pot_files} ${PROJECT_SOURCE_DIR}/i18n/i18n.json
      ${PROJECT_SOURCE_DIR}/tools/i18n_compile.py ${PROJECT_SOURCE_DIR}/tools/i18n_po.py
    COMMENT "Compiling interface translations"
    VERBATIM)
  add_library(${name} STATIC ${I18N_SOURCE_DIR}/i18n.cpp ${catalogs})
  target_include_directories(${name} PUBLIC ${I18N_SOURCE_DIR})
  target_compile_features(${name} PUBLIC cxx_std_20)
endfunction()
