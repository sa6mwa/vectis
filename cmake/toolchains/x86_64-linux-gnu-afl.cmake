set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

include("${CMAKE_CURRENT_LIST_DIR}/../cpkt_bootlin_toolchain.cmake")
cpkt_configure_bootlin_toolchain(x86_64-linux-gnu)
set(cpkt_aflpp_resolver "${CPKT_LIFECYCLE_SCRIPTS_DIR}/cpkt-aflpp.sh")
execute_process(
  COMMAND "${cpkt_aflpp_resolver}" discover
  RESULT_VARIABLE cpkt_aflpp_result
  OUTPUT_VARIABLE cpkt_aflpp_description
  ERROR_VARIABLE cpkt_aflpp_error
)
if(NOT cpkt_aflpp_result EQUAL 0)
  message(FATAL_ERROR "unable to inspect pinned AFL++: ${cpkt_aflpp_error}")
endif()
if(NOT cpkt_aflpp_description MATCHES "(^|[\r\n])status=ready([\r\n]|$)")
  message(FATAL_ERROR "Prepare AFL++ before configure: scripts/cpkt-aflpp.sh ensure")
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
