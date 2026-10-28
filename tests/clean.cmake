cmake_minimum_required(VERSION 3.24)
file(REMOVE_RECURSE "${WORK_DIR}")
set(root "${WORK_DIR}/checkout")
file(MAKE_DIRECTORY "${root}/scripts")
file(COPY "${SOURCE_DIR}/scripts/clean.sh" DESTINATION "${root}/scripts")
foreach(directory IN ITEMS build dist .cache vendor/kore/upstream)
  file(MAKE_DIRECTORY "${root}/${directory}")
  file(WRITE "${root}/${directory}/.hidden" "owned generated input")
endforeach()
file(MAKE_DIRECTORY "${WORK_DIR}/global/cpkt/deps" "${WORK_DIR}/global/cpkt/toolchains" "${WORK_DIR}/apple-sdk")
foreach(directory IN ITEMS global/cpkt/deps global/cpkt/toolchains apple-sdk)
  file(WRITE "${WORK_DIR}/${directory}/preserve" "borrowed prerequisite")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "XDG_CACHE_HOME=${WORK_DIR}/global"
  bash "${root}/scripts/clean.sh" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Owned clean failed: ${out}${err}")
endif()
foreach(directory IN ITEMS build dist .cache vendor/kore/upstream)
  if(EXISTS "${root}/${directory}")
    message(FATAL_ERROR "Full clean retained ${directory}")
  endif()
endforeach()
foreach(directory IN ITEMS global/cpkt/deps global/cpkt/toolchains apple-sdk)
  if(NOT EXISTS "${WORK_DIR}/${directory}/preserve")
    message(FATAL_ERROR "Clean removed borrowed ${directory}")
  endif()
endforeach()

# Low-level package entrypoints enforce the same physical ownership boundary
# as their shell callers, before creating outputs or deleting an old stage.
file(MAKE_DIRECTORY "${root}/cmake" "${root}/build" "${WORK_DIR}/borrowed")
file(WRITE "${WORK_DIR}/borrowed/preserve" "foreign generated state")
foreach(name vectis_owned_paths package_clean_dist package_checksums
    package_archive package_darwin_smoke_bundle)
  file(COPY "${SOURCE_DIR}/cmake/${name}.cmake" DESTINATION "${root}/cmake")
endforeach()
function(reject name expected)
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DVECTIS_ROOT=${root}"
    ${ARGN} -P "${root}/cmake/${name}.cmake"
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(status EQUAL 0 OR NOT err MATCHES "${expected}")
    message(FATAL_ERROR "Package ownership rejection failed: ${name}\n${out}${err}")
  endif()
endfunction()
reject(package_clean_dist "cannot borrow another checkout"
  "-DVECTIS_DIST_DIR=${WORK_DIR}/borrowed")
reject(package_checksums "cannot borrow another checkout"
  "-DVECTIS_BINARY_DIR=${WORK_DIR}/borrowed"
  "-DVECTIS_DIST_DIR=${root}/build/checksums")
foreach(name package_archive package_darwin_smoke_bundle)
  reject(${name} "cannot borrow another checkout"
    "-DVECTIS_BINARY_DIR=${WORK_DIR}/borrowed"
    "-DVECTIS_DIST_DIR=${root}/build/package")
endforeach()
reject(package_clean_dist "Project root must identify this checkout"
  "-DVECTIS_ROOT=${WORK_DIR}/borrowed" "-DVECTIS_DIST_DIR=${root}/build/package")
file(CREATE_LINK "${WORK_DIR}/borrowed" "${root}/build/escape" SYMBOLIC)
reject(package_clean_dist "cannot borrow another checkout"
  "-DVECTIS_DIST_DIR=${root}/build/escape/absent")
if(NOT EXISTS "${WORK_DIR}/borrowed/preserve" OR
   EXISTS "${WORK_DIR}/borrowed/absent" OR EXISTS "${root}/build/checksums")
  message(FATAL_ERROR "Rejected packaging changed foreign state or wrote outputs")
endif()
file(MAKE_DIRECTORY "${root}/build/selected")
file(WRITE "${root}/build/selected/.hidden" "owned old payload")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DVECTIS_ROOT=${root}"
  "-DVECTIS_DIST_DIR=${root}/build/selected"
  -P "${root}/cmake/package_clean_dist.cmake" RESULT_VARIABLE status)
if(NOT status EQUAL 0 OR EXISTS "${root}/build/selected/.hidden" OR
   NOT EXISTS "${WORK_DIR}/borrowed/preserve")
  message(FATAL_ERROR "Scoped package cleanup did not preserve its boundary")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
