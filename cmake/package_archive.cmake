if(NOT DEFINED VECTIS_BINARY_DIR)
  message(FATAL_ERROR "VECTIS_BINARY_DIR is required")
endif()
if(NOT DEFINED VECTIS_ROOT)
  message(FATAL_ERROR "VECTIS_ROOT is required")
endif()
get_filename_component(VECTIS_BINARY_DIR "${VECTIS_BINARY_DIR}" ABSOLUTE)
get_filename_component(VECTIS_ROOT "${VECTIS_ROOT}" ABSOLUTE)
if(DEFINED VECTIS_DIST_DIR AND NOT "${VECTIS_DIST_DIR}" STREQUAL "")
  set(vectis_dist_dir "${VECTIS_DIST_DIR}")
else()
  set(vectis_dist_dir "${VECTIS_ROOT}/dist")
endif()

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
vectis_import_cache_path(CMAKE_INSTALL_NAME_TOOL)
vectis_import_cache_path(VECTIS_OTOOL)

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

if(NOT EXISTS "${VECTIS_EXTERNAL_ROOT}/include" OR NOT EXISTS "${VECTIS_EXTERNAL_ROOT}/lib")
  message(FATAL_ERROR "VECTIS_EXTERNAL_ROOT does not contain include/lib: ${VECTIS_EXTERNAL_ROOT}")
endif()

file(COPY "${VECTIS_EXTERNAL_ROOT}/include/" DESTINATION "${package_root}/include")
file(COPY "${VECTIS_EXTERNAL_ROOT}/lib/" DESTINATION "${package_root}/lib")
if(EXISTS "${VECTIS_EXTERNAL_ROOT}/share")
  file(COPY "${VECTIS_EXTERNAL_ROOT}/share/" DESTINATION "${package_root}/share")
endif()

file(MAKE_DIRECTORY "${package_root}/lib/cmake/vectis")
file(WRITE "${package_root}/lib/cmake/vectis/vectisConfig.cmake" [=[
get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

include(CMakeFindDependencyMacro)
find_dependency(Threads REQUIRED)
find_package(lockdc CONFIG REQUIRED PATHS "${PACKAGE_PREFIX_DIR}/lib/cmake/lockdc" NO_DEFAULT_PATH)

if(NOT TARGET vectis::static AND EXISTS "${PACKAGE_PREFIX_DIR}/lib/libvectis.a")
  add_library(vectis::static STATIC IMPORTED)
  set_target_properties(vectis::static PROPERTIES
    IMPORTED_LOCATION "${PACKAGE_PREFIX_DIR}/lib/libvectis.a"
    INTERFACE_INCLUDE_DIRECTORIES "${PACKAGE_PREFIX_DIR}/include"
    INTERFACE_LINK_LIBRARIES "lockdc::static;Threads::Threads"
  )
endif()

if(NOT TARGET vectis::shared)
  if(APPLE AND EXISTS "${PACKAGE_PREFIX_DIR}/lib/libvectis.dylib")
    add_library(vectis::shared SHARED IMPORTED)
    set_target_properties(vectis::shared PROPERTIES
      IMPORTED_LOCATION "${PACKAGE_PREFIX_DIR}/lib/libvectis.dylib"
      INTERFACE_INCLUDE_DIRECTORIES "${PACKAGE_PREFIX_DIR}/include"
      INTERFACE_LINK_LIBRARIES "lockdc::shared;Threads::Threads"
    )
  elseif(EXISTS "${PACKAGE_PREFIX_DIR}/lib/libvectis.so")
    add_library(vectis::shared SHARED IMPORTED)
    set_target_properties(vectis::shared PROPERTIES
      IMPORTED_LOCATION "${PACKAGE_PREFIX_DIR}/lib/libvectis.so"
      INTERFACE_INCLUDE_DIRECTORIES "${PACKAGE_PREFIX_DIR}/include"
      INTERFACE_LINK_LIBRARIES "lockdc::shared;Threads::Threads"
    )
  endif()
endif()

if(NOT TARGET vectis::vectis)
  if(TARGET vectis::static)
    add_library(vectis::vectis ALIAS vectis::static)
  elseif(TARGET vectis::shared)
    add_library(vectis::vectis ALIAS vectis::shared)
  endif()
endif()
]=])

file(WRITE "${package_root}/lib/cmake/vectis/vectisConfigVersion.cmake" "set(PACKAGE_VERSION \"${VECTIS_VERSION}\")\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n")

if(VECTIS_TARGET_ID MATCHES "apple-darwin$")
  if(NOT CMAKE_INSTALL_NAME_TOOL OR NOT EXISTS "${CMAKE_INSTALL_NAME_TOOL}")
    message(FATAL_ERROR "CMAKE_INSTALL_NAME_TOOL is required for Darwin package fixups")
  endif()
  file(GLOB vectis_dylibs LIST_DIRECTORIES false "${package_root}/lib/*.dylib")
  foreach(vectis_dylib IN LISTS vectis_dylibs)
    get_filename_component(vectis_dylib_name "${vectis_dylib}" NAME)
    execute_process(
      COMMAND "${CMAKE_INSTALL_NAME_TOOL}" -id "@rpath/${vectis_dylib_name}" "${vectis_dylib}"
      RESULT_VARIABLE id_result
      ERROR_VARIABLE id_error
    )
    if(NOT id_result EQUAL 0)
      message(FATAL_ERROR "failed to rewrite install name for ${vectis_dylib}\n${id_error}")
    endif()
  endforeach()
endif()

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
