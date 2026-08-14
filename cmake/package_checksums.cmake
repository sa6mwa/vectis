if(DEFINED VECTIS_DIST_DIR AND NOT "${VECTIS_DIST_DIR}" STREQUAL "")
  set(dist_dir "${VECTIS_DIST_DIR}")
else()
  set(dist_dir "${VECTIS_ROOT}/dist")
endif()
if((NOT DEFINED VECTIS_VERSION OR "${VECTIS_VERSION}" STREQUAL "")
   AND DEFINED VECTIS_BINARY_DIR
   AND EXISTS "${VECTIS_BINARY_DIR}/package-metadata.cmake")
  include("${VECTIS_BINARY_DIR}/package-metadata.cmake")
endif()
if((NOT DEFINED VECTIS_VERSION OR "${VECTIS_VERSION}" STREQUAL "")
   AND DEFINED VECTIS_ROOT
   AND EXISTS "${VECTIS_ROOT}/scripts/release_version.sh")
  execute_process(
    COMMAND "${VECTIS_ROOT}/scripts/release_version.sh"
    WORKING_DIRECTORY "${VECTIS_ROOT}"
    OUTPUT_VARIABLE VECTIS_VERSION
    RESULT_VARIABLE vectis_version_result
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT vectis_version_result EQUAL 0)
    message(FATAL_ERROR "failed to resolve VECTIS_VERSION")
  endif()
endif()
set(checksums_name "vectis-${VECTIS_VERSION}-CHECKSUMS")
set(checksums_path "${dist_dir}/${checksums_name}")

file(MAKE_DIRECTORY "${dist_dir}")
file(GLOB checksum_inputs RELATIVE "${dist_dir}" LIST_DIRECTORIES false
  "${dist_dir}/vectis-${VECTIS_VERSION}.tar.gz"
  "${dist_dir}/vectis-${VECTIS_VERSION}-*.tar.gz"
  "${dist_dir}/vectis-${VECTIS_VERSION}-*.zip"
  "${dist_dir}/vectis-lua-${VECTIS_VERSION}.tar.gz"
  "${dist_dir}/vectis-${VECTIS_VERSION}-1.rockspec"
  "${dist_dir}/vectis-${VECTIS_VERSION}-1.src.rock"
)
list(REMOVE_DUPLICATES checksum_inputs)
list(FILTER checksum_inputs EXCLUDE REGEX "^${checksums_name}$")
list(LENGTH checksum_inputs checksum_input_count)
if(checksum_input_count EQUAL 0)
  message(FATAL_ERROR "no release artifacts found in ${dist_dir} for version ${VECTIS_VERSION}")
endif()
list(SORT checksum_inputs)

execute_process(
  COMMAND sha256sum ${checksum_inputs}
  WORKING_DIRECTORY "${dist_dir}"
  OUTPUT_FILE "${checksums_path}"
  RESULT_VARIABLE checksum_result
)
if(NOT checksum_result EQUAL 0)
  message(FATAL_ERROR "failed to create ${checksums_name}")
endif()
