if(NOT DEFINED VECTIS_BINARY_DIR)
  message(FATAL_ERROR "VECTIS_BINARY_DIR is required")
endif()
if(NOT DEFINED VECTIS_ROOT)
  message(FATAL_ERROR "VECTIS_ROOT is required")
endif()
get_filename_component(VECTIS_BINARY_DIR "${VECTIS_BINARY_DIR}" ABSOLUTE)
get_filename_component(VECTIS_ROOT "${VECTIS_ROOT}" ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/vectis_owned_paths.cmake")
vectis_require_project_root("${VECTIS_ROOT}")
vectis_require_owned_path("${VECTIS_BINARY_DIR}" "Package build directory")
if(DEFINED VECTIS_DIST_DIR AND NOT "${VECTIS_DIST_DIR}" STREQUAL "")
  set(vectis_dist_dir "${VECTIS_DIST_DIR}")
else()
  set(vectis_dist_dir "${VECTIS_ROOT}/dist")
endif()
vectis_require_owned_path("${vectis_dist_dir}" "Package output directory" "${VECTIS_ROOT}/dist")

function(vectis_import_cache_path var_name)
  if(DEFINED ${var_name} AND NOT "${${var_name}}" STREQUAL "")
    return()
  endif()
  file(STRINGS "${VECTIS_BINARY_DIR}/CMakeCache.txt" cache_line
       REGEX "^${var_name}(:[^=]+)?=" LIMIT_COUNT 1)
  if(cache_line)
    string(REGEX REPLACE "^[^=]*=" "" cache_value "${cache_line}")
    set(${var_name} "${cache_value}" PARENT_SCOPE)
  endif()
endfunction()

vectis_import_cache_path(VECTIS_EXTERNAL_ROOT)
vectis_import_cache_path(CMAKE_BUILD_TYPE)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "Release packaging requires a Release build; local development executables are not distributable")
endif()
vectis_import_cache_path(CMAKE_INSTALL_NAME_TOOL)
vectis_import_cache_path(VECTIS_OTOOL)
vectis_import_cache_path(CMAKE_OTOOL)

include("${VECTIS_BINARY_DIR}/package-metadata.cmake")

set(package_stage_root "${VECTIS_BINARY_DIR}/package")
set(package_prefix_name "vectis-${VECTIS_VERSION}-${VECTIS_TARGET_ID}")
set(package_root "${package_stage_root}/${package_prefix_name}")

file(REMOVE_RECURSE "${package_root}")
file(MAKE_DIRECTORY "${package_root}")

if(NOT EXISTS "${VECTIS_BINARY_DIR}/cmake_install.cmake")
  message(FATAL_ERROR
    "package generation requires a real install-enabled build tree; missing ${VECTIS_BINARY_DIR}/cmake_install.cmake")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${VECTIS_BINARY_DIR}" --prefix "${package_root}"
  RESULT_VARIABLE install_result
)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "failed to install vectis package payload")
endif()

set(vectis_sdk_names cpkt cpktdb cpktmisc liblockdc liblockdc-postgres
  lonejson pslog cai liblql libmdf softline)
file(MAKE_DIRECTORY "${package_root}/deps")
foreach(name IN LISTS vectis_sdk_names)
  if(NOT EXISTS "${VECTIS_EXTERNAL_ROOT}/${name}/lib")
    message(FATAL_ERROR "Missing independently prepared SDK: ${name}")
  endif()
  # Preserve each provider's public layout, validation inventory and notices.
  file(COPY "${VECTIS_EXTERNAL_ROOT}/${name}" DESTINATION "${package_root}/deps")
endforeach()

function(vectis_darwin_fixup_dylibs)
  if(NOT VECTIS_TARGET_ID MATCHES "apple-darwin$")
    return()
  endif()
  if(NOT CMAKE_INSTALL_NAME_TOOL OR NOT EXISTS "${CMAKE_INSTALL_NAME_TOOL}")
    message(FATAL_ERROR
      "Darwin package fixup requires target install_name_tool; got ${CMAKE_INSTALL_NAME_TOOL}")
  endif()
  if(NOT VECTIS_OTOOL AND CMAKE_OTOOL)
    set(VECTIS_OTOOL "${CMAKE_OTOOL}")
  endif()
  if(NOT VECTIS_OTOOL OR NOT EXISTS "${VECTIS_OTOOL}")
    message(FATAL_ERROR "Darwin package fixup requires target otool; got ${VECTIS_OTOOL}")
  endif()

  file(GLOB vectis_darwin_dylibs LIST_DIRECTORIES false
       "${package_root}/lib/*.dylib")
  set(vectis_darwin_dylib_names "")
  foreach(vectis_darwin_dylib IN LISTS vectis_darwin_dylibs)
    get_filename_component(vectis_darwin_dylib_name
                           "${vectis_darwin_dylib}" NAME)
    list(APPEND vectis_darwin_dylib_names "${vectis_darwin_dylib_name}")
  endforeach()

  foreach(vectis_darwin_dylib IN LISTS vectis_darwin_dylibs)
    get_filename_component(vectis_darwin_dylib_name
                           "${vectis_darwin_dylib}" NAME)
    execute_process(
      COMMAND "${VECTIS_OTOOL}" -D "${vectis_darwin_dylib}"
      RESULT_VARIABLE vectis_darwin_id_probe_result
      OUTPUT_VARIABLE vectis_darwin_id_probe_output
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(vectis_darwin_install_id_name "${vectis_darwin_dylib_name}")
    if(vectis_darwin_id_probe_result EQUAL 0)
      string(REPLACE "\n" ";" vectis_darwin_id_probe_lines
             "${vectis_darwin_id_probe_output}")
      list(LENGTH vectis_darwin_id_probe_lines
           vectis_darwin_id_probe_line_count)
      if(vectis_darwin_id_probe_line_count GREATER 1)
        list(GET vectis_darwin_id_probe_lines 1 vectis_darwin_install_id)
        string(STRIP "${vectis_darwin_install_id}" vectis_darwin_install_id)
        get_filename_component(vectis_darwin_existing_id_name
                               "${vectis_darwin_install_id}" NAME)
        list(FIND vectis_darwin_dylib_names
             "${vectis_darwin_existing_id_name}"
             vectis_darwin_existing_id_index)
        if(NOT vectis_darwin_existing_id_index EQUAL -1)
          set(vectis_darwin_install_id_name
              "${vectis_darwin_existing_id_name}")
        endif()
      endif()
    endif()
    execute_process(
      COMMAND "${CMAKE_INSTALL_NAME_TOOL}" -id
              "@rpath/${vectis_darwin_install_id_name}"
              "${vectis_darwin_dylib}"
      RESULT_VARIABLE vectis_darwin_id_result)
    if(NOT vectis_darwin_id_result EQUAL 0)
      message(FATAL_ERROR
        "failed to set Darwin install name for ${vectis_darwin_dylib}")
    endif()
  endforeach()

  file(GLOB vectis_darwin_loaders LIST_DIRECTORIES false
       "${package_root}/bin/*" "${package_root}/lib/*.dylib")
  foreach(vectis_darwin_loader IN LISTS vectis_darwin_loaders)
    execute_process(
      COMMAND "${VECTIS_OTOOL}" -L "${vectis_darwin_loader}"
      RESULT_VARIABLE vectis_darwin_otool_result
      OUTPUT_VARIABLE vectis_darwin_otool_output
      ERROR_QUIET)
    if(NOT vectis_darwin_otool_result EQUAL 0)
      continue()
    endif()
    string(REPLACE "\n" ";" vectis_darwin_otool_lines
           "${vectis_darwin_otool_output}")
    foreach(vectis_darwin_line IN LISTS vectis_darwin_otool_lines)
      string(STRIP "${vectis_darwin_line}" vectis_darwin_line)
      if(NOT vectis_darwin_line MATCHES "^/")
        continue()
      endif()
      string(REGEX REPLACE " .*" "" vectis_darwin_dep
             "${vectis_darwin_line}")
      get_filename_component(vectis_darwin_dep_name
                             "${vectis_darwin_dep}" NAME)
      list(FIND vectis_darwin_dylib_names "${vectis_darwin_dep_name}"
           vectis_darwin_dep_index)
      if(NOT vectis_darwin_dep_index EQUAL -1)
        execute_process(
          COMMAND "${CMAKE_INSTALL_NAME_TOOL}" -change
                  "${vectis_darwin_dep}"
                  "@rpath/${vectis_darwin_dep_name}"
                  "${vectis_darwin_loader}"
          RESULT_VARIABLE vectis_darwin_change_result)
        if(NOT vectis_darwin_change_result EQUAL 0)
          message(FATAL_ERROR
            "failed to rewrite Darwin dependency ${vectis_darwin_dep} "
            "in ${vectis_darwin_loader}")
        endif()
      endif()
      unset(vectis_darwin_dep_index)
    endforeach()
  endforeach()
endfunction()

vectis_darwin_fixup_dylibs()

file(GLOB_RECURSE vectis_package_pc_files LIST_DIRECTORIES false
     "${package_root}/lib/pkgconfig/*.pc")
foreach(vectis_package_pc_file IN LISTS vectis_package_pc_files)
  file(READ "${vectis_package_pc_file}" vectis_package_pc_contents)
  string(REPLACE "${VECTIS_EXTERNAL_ROOT}" "\${pcfiledir}/../.."
         vectis_package_pc_contents "${vectis_package_pc_contents}")
  string(REPLACE "${package_root}" "\${pcfiledir}/../.."
         vectis_package_pc_contents "${vectis_package_pc_contents}")
  file(WRITE "${vectis_package_pc_file}" "${vectis_package_pc_contents}")
endforeach()

# The canonical installed CMake config discovers the independent bundled prefixes.

file(MAKE_DIRECTORY "${vectis_dist_dir}")
set(archive_base "${vectis_dist_dir}/${package_prefix_name}.tar")
set(archive "${archive_base}.gz")
find_program(VECTIS_TAR_BIN NAMES tar)
find_program(VECTIS_GZIP_BIN NAMES gzip)
if(NOT VECTIS_TAR_BIN)
  message(FATAL_ERROR "failed to find tar for archive creation")
endif()
if(NOT VECTIS_GZIP_BIN)
  message(FATAL_ERROR "failed to find gzip for archive creation")
endif()
file(REMOVE "${archive_base}" "${archive}")
execute_process(
  COMMAND "${VECTIS_TAR_BIN}" -cf "${archive_base}" --format=gnu --owner 0 --group 0 "${package_prefix_name}"
  WORKING_DIRECTORY "${package_stage_root}"
  RESULT_VARIABLE tar_result
)
if(NOT tar_result EQUAL 0)
  message(FATAL_ERROR "failed to create package archive")
endif()
execute_process(
  COMMAND "${VECTIS_GZIP_BIN}" -9 -f "${archive_base}"
  RESULT_VARIABLE gzip_result
)
if(NOT gzip_result EQUAL 0)
  message(FATAL_ERROR "failed to gzip package archive")
endif()
