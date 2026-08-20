if(NOT DEFINED VECTIS_BIN OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "VECTIS_BIN and WORK_DIR are required")
endif()

set(output "${WORK_DIR}/vectis-pack-must-not-exist")
file(REMOVE "${output}")
execute_process(
  COMMAND "${VECTIS_BIN}" -a pack --script missing.lua --output "${output}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr)
if(NOT result EQUAL 69)
  message(FATAL_ERROR
    "Darwin pack returned ${result}, expected 69: ${stdout}${stderr}")
endif()
if(EXISTS "${output}")
  message(FATAL_ERROR "Darwin pack unexpectedly created an output artifact")
endif()
if(NOT stderr MATCHES "pack is not supported on Darwin")
  message(FATAL_ERROR "Darwin pack failed with unexpected error: ${stdout}${stderr}")
endif()
