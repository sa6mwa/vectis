set(script "${WORK_DIR}/vectis-pack-smoke.lua")
set(output "${WORK_DIR}/vectis-pack-smoke")

file(WRITE "${script}" "local vectis = require(\"vectis\")\nassert(vectis.status_string(vectis.OK) == \"ok\")\nassert(arg[0]:match(\"vectis%-pack%-smoke$\"))\nassert(arg[1] == \"first\")\nassert(arg[2] == \"second\")\n")

execute_process(COMMAND "${VECTIS_BIN}" pack --script "${script}" --output "${output}"
                RESULT_VARIABLE pack_result
                OUTPUT_VARIABLE pack_stdout
                ERROR_VARIABLE pack_stderr)
if(NOT pack_result EQUAL 0)
  message(FATAL_ERROR "vectis pack failed: ${pack_stdout}${pack_stderr}")
endif()

execute_process(COMMAND "${output}" first second
                RESULT_VARIABLE run_result
                OUTPUT_VARIABLE run_stdout
                ERROR_VARIABLE run_stderr)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "packed vectis failed: ${run_stdout}${run_stderr}")
endif()
