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

vectis_import_cache_path(CMAKE_C_COMPILER)
vectis_import_cache_path(CMAKE_TOOLCHAIN_FILE)
vectis_import_cache_path(CMAKE_BUILD_TYPE)
vectis_import_cache_path(VECTIS_OSXCROSS_BIN_DIR)
vectis_import_cache_path(VECTIS_OTOOL)

include("${VECTIS_BINARY_DIR}/package-metadata.cmake")
if(NOT VECTIS_TARGET_ID STREQUAL "arm64-apple-darwin")
  message(FATAL_ERROR "Darwin smoke bundle requires arm64-apple-darwin, got ${VECTIS_TARGET_ID}")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(bundle_root "${VECTIS_BINARY_DIR}/darwin-smoke-bundle")
set(bundle_dist "${bundle_root}/dist")
set(extract_root "${bundle_root}/release")
set(consumer_src_dir "${bundle_root}/consumer")
set(consumer_bin_dir "${bundle_root}/consumer-build")
set(stage_name "vectis-${VECTIS_VERSION}-${VECTIS_TARGET_ID}-smoke")
set(stage_root "${bundle_root}/${stage_name}")
set(release_archive "${bundle_dist}/vectis-${VECTIS_VERSION}-${VECTIS_TARGET_ID}.tar.gz")
set(release_prefix "${extract_root}/vectis-${VECTIS_VERSION}-${VECTIS_TARGET_ID}")
set(smoke_archive "${vectis_dist_dir}/${stage_name}.zip")

file(REMOVE_RECURSE "${bundle_root}")
file(MAKE_DIRECTORY "${bundle_dist}" "${extract_root}" "${consumer_src_dir}" "${consumer_bin_dir}")

execute_process(
  COMMAND "${CMAKE_COMMAND}"
    -DVECTIS_BINARY_DIR=${VECTIS_BINARY_DIR}
    -DVECTIS_ROOT=${VECTIS_ROOT}
    -DVECTIS_DIST_DIR=${bundle_dist}
    -P "${VECTIS_ROOT}/cmake/package_archive.cmake"
  RESULT_VARIABLE package_result
  OUTPUT_VARIABLE package_stdout
  ERROR_VARIABLE package_stderr
)
if(NOT package_result EQUAL 0)
  message(FATAL_ERROR
    "failed to build Darwin release archive for smoke bundle\n"
    "stdout:\n${package_stdout}\n"
    "stderr:\n${package_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar xf "${release_archive}"
  WORKING_DIRECTORY "${extract_root}"
  RESULT_VARIABLE extract_result
  OUTPUT_VARIABLE extract_stdout
  ERROR_VARIABLE extract_stderr
)
if(NOT extract_result EQUAL 0)
  message(FATAL_ERROR
    "failed to extract Darwin release archive for smoke bundle\n"
    "stdout:\n${extract_stdout}\n"
    "stderr:\n${extract_stderr}")
endif()

file(WRITE "${consumer_src_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.21)
project(vectis_darwin_smoke C)

find_package(vectis CONFIG REQUIRED)

add_executable(vectis_static_smoke smoke.c)
target_link_libraries(vectis_static_smoke PRIVATE vectis::static)
set_target_properties(vectis_static_smoke PROPERTIES
  BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "@executable_path/../lib")
]=])

file(WRITE "${consumer_src_dir}/smoke.c" [=[
#include <vectis/vectis.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    vectis_app_config config;
    vectis_error error;

    vectis_app_config_init(&config);
    if (config.tls.port != 8443u) {
        return 10;
    }
    if (vectis_json_validate_cstr("{\"ok\":true}", &error) != VECTIS_OK) {
        return 11;
    }
    if (vectis_json_validate_cstr("{\"ok\":", &error) != VECTIS_ERR_INVALID) {
        return 12;
    }

    puts("vectis darwin smoke ok");
    return 0;
}
]=])

set(configure_args
  -S "${consumer_src_dir}"
  -B "${consumer_bin_dir}"
  "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
  "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
  "-DVECTIS_EXTERNAL_ROOT=${release_prefix}"
  "-DCMAKE_PREFIX_PATH=${release_prefix}"
  "-Dvectis_DIR=${release_prefix}/lib/cmake/vectis"
  "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF"
  "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"
  "-DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON"
)
if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND configure_args "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "PATH=${VECTIS_OSXCROSS_BIN_DIR}:$ENV{PATH}"
          "${CMAKE_COMMAND}" ${configure_args}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_stdout
  ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "failed to configure Darwin smoke bundle consumer\n"
    "stdout:\n${configure_stdout}\n"
    "stderr:\n${configure_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "PATH=${VECTIS_OSXCROSS_BIN_DIR}:$ENV{PATH}"
          "${CMAKE_COMMAND}" --build "${consumer_bin_dir}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
    "failed to build Darwin smoke bundle consumer\n"
    "stdout:\n${build_stdout}\n"
    "stderr:\n${build_stderr}")
endif()

file(MAKE_DIRECTORY "${stage_root}/bin" "${stage_root}/lib")
file(COPY "${consumer_bin_dir}/vectis_static_smoke" DESTINATION "${stage_root}/bin")
file(GLOB smoke_dylibs LIST_DIRECTORIES false "${release_prefix}/lib/*.dylib")
if(smoke_dylibs)
  file(COPY ${smoke_dylibs} DESTINATION "${stage_root}/lib")
endif()
file(WRITE "${stage_root}/run-smoke.sh" [=[
#!/bin/sh
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DYLD_LIBRARY_PATH="$DIR/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" "$DIR/bin/vectis_static_smoke"
]=])
file(CHMOD "${stage_root}/run-smoke.sh"
  PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE
)

if(VECTIS_OTOOL AND EXISTS "${VECTIS_OTOOL}")
  execute_process(
    COMMAND "${VECTIS_OTOOL}" -hv "${stage_root}/bin/vectis_static_smoke"
    RESULT_VARIABLE otool_result
    OUTPUT_VARIABLE otool_output
    ERROR_VARIABLE otool_error
  )
  if(NOT otool_result EQUAL 0 OR NOT otool_output MATCHES "ARM64")
    message(FATAL_ERROR
      "Darwin smoke binary is not an arm64 Mach-O executable\n"
      "${otool_output}${otool_error}")
  endif()
endif()

file(MAKE_DIRECTORY "${vectis_dist_dir}")
file(REMOVE "${smoke_archive}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar cf "${smoke_archive}" --format=zip "${stage_name}"
  WORKING_DIRECTORY "${bundle_root}"
  RESULT_VARIABLE zip_result
  OUTPUT_VARIABLE zip_stdout
  ERROR_VARIABLE zip_stderr
)
if(NOT zip_result EQUAL 0)
  message(FATAL_ERROR
    "failed to create Darwin smoke bundle zip\n"
    "stdout:\n${zip_stdout}\n"
    "stderr:\n${zip_stderr}")
endif()
