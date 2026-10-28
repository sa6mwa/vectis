file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
execute_process(COMMAND "${PROBE}" "${WORK_DIR}" RESULT_VARIABLE result
  OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 5)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "C storage behavior failed: ${result}\n${out}${err}")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
