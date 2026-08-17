set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(cpkt_repo_root "${CMAKE_CURRENT_LIST_DIR}/../..")
set(cpkt_bootlin_resolver "${cpkt_repo_root}/scripts/cpkt-toolchains.sh")
execute_process(
  COMMAND "${cpkt_bootlin_resolver}" ensure x86_64-linux-gnu
  RESULT_VARIABLE cpkt_bootlin_ensure_result
  OUTPUT_QUIET
  ERROR_VARIABLE cpkt_bootlin_ensure_error
)
if(NOT cpkt_bootlin_ensure_result EQUAL 0)
  message(FATAL_ERROR
    "unable to provision pinned Bootlin toolchain for fuzzing: ${cpkt_bootlin_ensure_error}")
endif()
execute_process(
  COMMAND "${cpkt_bootlin_resolver}" discover x86_64-linux-gnu
  RESULT_VARIABLE cpkt_bootlin_result
  OUTPUT_VARIABLE cpkt_bootlin_description
  ERROR_VARIABLE cpkt_bootlin_error
)
if(NOT cpkt_bootlin_result EQUAL 0)
  message(FATAL_ERROR
    "unable to inspect pinned Bootlin toolchain for fuzzing: ${cpkt_bootlin_error}")
endif()

foreach(key cc cxx ld ar ranlib strip nm objcopy objdump addr2line readelf sysroot root)
  string(REGEX MATCH "(^|\n)${key}=([^\r\n]+)" cpkt_bootlin_match
    "${cpkt_bootlin_description}")
  if(NOT cpkt_bootlin_match)
    message(FATAL_ERROR "Bootlin resolver did not report ${key} for fuzzing")
  endif()
  set(cpkt_bootlin_${key} "${CMAKE_MATCH_2}")
endforeach()

set(cpkt_aflpp_resolver "${CMAKE_CURRENT_LIST_DIR}/../../scripts/cpkt-aflpp.sh")
execute_process(
  COMMAND "${cpkt_aflpp_resolver}" discover
  RESULT_VARIABLE cpkt_aflpp_result
  OUTPUT_VARIABLE cpkt_aflpp_description
  ERROR_VARIABLE cpkt_aflpp_error
)
if(NOT cpkt_aflpp_result EQUAL 0)
  message(FATAL_ERROR "unable to provision pinned AFL++: ${cpkt_aflpp_error}")
endif()

foreach(key cc cxx helper)
  string(REGEX MATCH "(^|\n)${key}=([^\r\n]+)" cpkt_aflpp_match
    "${cpkt_aflpp_description}")
  if(NOT cpkt_aflpp_match)
    message(FATAL_ERROR "AFL++ resolver did not report ${key}")
  endif()
  set(cpkt_aflpp_${key} "${CMAKE_MATCH_2}")
endforeach()

set(ENV{AFL_PATH} "${cpkt_aflpp_helper}")
set(ENV{AFL_SKIP_CPUFREQ} "1")
set(ENV{AFL_QUIET} "1")
set(CMAKE_C_COMPILER "${cpkt_aflpp_cc}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${cpkt_aflpp_cxx}" CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER "${cpkt_bootlin_ld}" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${cpkt_bootlin_ar}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${cpkt_bootlin_ranlib}" CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP "${cpkt_bootlin_strip}" CACHE FILEPATH "" FORCE)
set(CMAKE_NM "${cpkt_bootlin_nm}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY "${cpkt_bootlin_objcopy}" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJDUMP "${cpkt_bootlin_objdump}" CACHE FILEPATH "" FORCE)
set(CMAKE_ADDR2LINE "${cpkt_bootlin_addr2line}" CACHE FILEPATH "" FORCE)
set(CMAKE_READELF "${cpkt_bootlin_readelf}" CACHE FILEPATH "" FORCE)
set(cpkt_find_root_path)
if(DEFINED VECTIS_EXTERNAL_ROOT AND NOT VECTIS_EXTERNAL_ROOT STREQUAL "")
  list(APPEND cpkt_find_root_path "${VECTIS_EXTERNAL_ROOT}")
endif()
list(APPEND cpkt_find_root_path "${cpkt_bootlin_sysroot}" "${cpkt_bootlin_root}")
set(CMAKE_SYSROOT "${cpkt_bootlin_sysroot}" CACHE PATH "" FORCE)
set(CMAKE_FIND_ROOT_PATH "${cpkt_find_root_path}" CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY CACHE STRING "" FORCE)
