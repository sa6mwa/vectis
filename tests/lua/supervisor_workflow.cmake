if(NOT DEFINED SCRIPT)
  set(SCRIPT "${VECTIS_SOURCE_DIR}/tests/lua/supervisor_workflow.lua")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}/supervisor-workflow")
set(root "${WORK_DIR}/supervisor-workflow/pouch")
file(REMOVE_RECURSE "${root}")
file(REMOVE_RECURSE "${root}-state" "${root}-config")
file(MAKE_DIRECTORY "${root}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "VECTIS_WORKFLOW_TEST_ROOT=${root}"
  "XDG_STATE_HOME=${root}-state" "XDG_CONFIG_HOME=${root}-config"
  "${VECTIS_BIN}" "${SCRIPT}"
  RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 20)
if(NOT result EQUAL 0 OR NOT out MATCHES "supervisor workflow (behavior|example) ok")
  message(FATAL_ERROR "workflow behavior failed: ${result}\n${out}\n${err}")
endif()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  set(packed "${WORK_DIR}/supervisor-workflow/packed")
  execute_process(COMMAND "${VECTIS_BIN}" -a pack --script
    "${SCRIPT}" --output "${packed}"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 20)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "workflow pack failed: ${out}\n${err}")
  endif()
  file(REMOVE_RECURSE "${root}")
  file(REMOVE_RECURSE "${root}-state" "${root}-config")
  file(MAKE_DIRECTORY "${root}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "VECTIS_WORKFLOW_TEST_ROOT=${root}"
    "XDG_STATE_HOME=${root}-state" "XDG_CONFIG_HOME=${root}-config" "${packed}"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 20)
  if(NOT result EQUAL 0 OR NOT out MATCHES "supervisor workflow (behavior|example) ok")
    message(FATAL_ERROR "packed workflow behavior failed: ${result}\n${out}\n${err}")
  endif()
endif()
