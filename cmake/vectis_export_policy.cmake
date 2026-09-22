# cmake/vectis.exports is the canonical, source-controlled dynamic ABI.  Keep
# it deliberately smaller than the public-header token set: headers alone do
# not prevent implementation or dependency symbols from becoming linkable.
if(NOT DEFINED VECTIS_EXPORT_LIST OR NOT EXISTS "${VECTIS_EXPORT_LIST}")
  message(FATAL_ERROR "VECTIS_EXPORT_LIST must name the canonical export allowlist")
endif()
file(STRINGS "${VECTIS_EXPORT_LIST}" vectis_public_exports
     REGEX "^vectis_[A-Za-z0-9_]+$")
list(REMOVE_DUPLICATES vectis_public_exports)
list(SORT vectis_public_exports)
if(vectis_public_exports STREQUAL "")
  message(FATAL_ERROR "Vectis shared-library export allowlist is empty")
endif()

if(DEFINED VECTIS_EXPORT_ELF_MAP)
  set(vectis_export_map "VECTIS_0 {\n  global:\n")
  foreach(vectis_export_symbol IN LISTS vectis_public_exports)
    string(APPEND vectis_export_map "    ${vectis_export_symbol};\n")
  endforeach()
  string(APPEND vectis_export_map "  local:\n    *;\n};\n")
  file(WRITE "${VECTIS_EXPORT_ELF_MAP}" "${vectis_export_map}")
endif()

if(DEFINED VECTIS_EXPORT_DARWIN_LIST)
  set(vectis_export_darwin "")
  foreach(vectis_export_symbol IN LISTS vectis_public_exports)
    string(APPEND vectis_export_darwin "_${vectis_export_symbol}\n")
  endforeach()
  file(WRITE "${VECTIS_EXPORT_DARWIN_LIST}" "${vectis_export_darwin}")
endif()

if(DEFINED VECTIS_EXPORT_EXPECTED_LIST)
  string(JOIN "\n" vectis_export_expected ${vectis_public_exports})
  file(WRITE "${VECTIS_EXPORT_EXPECTED_LIST}" "${vectis_export_expected}\n")
endif()

if(DEFINED VECTIS_EXPORT_LIBRARY)
  if(NOT EXISTS "${VECTIS_EXPORT_LIBRARY}")
    message(FATAL_ERROR "shared library does not exist: ${VECTIS_EXPORT_LIBRARY}")
  endif()
  if((NOT DEFINED VECTIS_EXPORT_NM OR VECTIS_EXPORT_NM STREQUAL "") AND
     DEFINED VECTIS_EXPORT_BUILD_DIR AND
     EXISTS "${VECTIS_EXPORT_BUILD_DIR}/CMakeCache.txt")
    file(STRINGS "${VECTIS_EXPORT_BUILD_DIR}/CMakeCache.txt"
         vectis_export_nm_cache REGEX "^CMAKE_NM:[^=]*=")
    if(NOT vectis_export_nm_cache STREQUAL "")
      list(GET vectis_export_nm_cache 0 vectis_export_nm_line)
      string(REGEX REPLACE "^CMAKE_NM:[^=]*=" "" VECTIS_EXPORT_NM
             "${vectis_export_nm_line}")
    endif()
  endif()
  if(NOT DEFINED VECTIS_EXPORT_NM OR NOT EXISTS "${VECTIS_EXPORT_NM}")
    message(FATAL_ERROR "VECTIS_EXPORT_NM must name the target nm tool")
  endif()
  if(DEFINED VECTIS_EXPORT_DARWIN AND VECTIS_EXPORT_DARWIN)
    execute_process(
      COMMAND "${VECTIS_EXPORT_NM}" -gU "${VECTIS_EXPORT_LIBRARY}"
      RESULT_VARIABLE vectis_nm_result
      OUTPUT_VARIABLE vectis_nm_output
      ERROR_VARIABLE vectis_nm_error)
    string(REPLACE "\n" ";" vectis_nm_lines "${vectis_nm_output}")
    set(vectis_actual_exports)
    foreach(vectis_nm_line IN LISTS vectis_nm_lines)
      string(REGEX MATCH "[^ \t]+$" vectis_nm_symbol "${vectis_nm_line}")
      string(REGEX REPLACE "^_" "" vectis_nm_symbol "${vectis_nm_symbol}")
      if(vectis_nm_symbol STREQUAL "")
        continue()
      endif()
      list(APPEND vectis_actual_exports "${vectis_nm_symbol}")
    endforeach()
  else()
    execute_process(
      COMMAND "${VECTIS_EXPORT_NM}" -D --defined-only --format=posix
              "${VECTIS_EXPORT_LIBRARY}"
      RESULT_VARIABLE vectis_nm_result
      OUTPUT_VARIABLE vectis_nm_output
      ERROR_VARIABLE vectis_nm_error)
    string(REPLACE "\n" ";" vectis_nm_lines "${vectis_nm_output}")
    set(vectis_actual_exports)
    foreach(vectis_nm_line IN LISTS vectis_nm_lines)
      string(REGEX MATCH "^[^ \t]+" vectis_nm_symbol "${vectis_nm_line}")
      string(REGEX REPLACE "@@.*$" "" vectis_nm_symbol "${vectis_nm_symbol}")
      # The linker-generated version node is not a callable ABI symbol.
      if(vectis_nm_symbol STREQUAL "VECTIS_0")
        continue()
      endif()
      if(vectis_nm_symbol STREQUAL "")
        continue()
      endif()
      list(APPEND vectis_actual_exports "${vectis_nm_symbol}")
    endforeach()
  endif()
  if(NOT vectis_nm_result EQUAL 0)
    message(FATAL_ERROR "target nm failed for ${VECTIS_EXPORT_LIBRARY}: ${vectis_nm_error}")
  endif()
  list(REMOVE_DUPLICATES vectis_actual_exports)
  list(SORT vectis_actual_exports)
  if(NOT vectis_actual_exports STREQUAL vectis_public_exports)
    string(JOIN "\n" vectis_expected_text ${vectis_public_exports})
    string(JOIN "\n" vectis_actual_text ${vectis_actual_exports})
    message(FATAL_ERROR
      "Vectis shared-library dynamic exports differ from the canonical allowlist.\n"
      "expected:\n${vectis_expected_text}\nactual:\n${vectis_actual_text}")
  endif()
endif()
