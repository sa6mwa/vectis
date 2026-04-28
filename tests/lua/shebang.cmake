if(NOT DEFINED VECTIS_BIN)
  message(FATAL_ERROR "VECTIS_BIN is required")
endif()

if(NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "WORK_DIR is required")
endif()

set(script "${WORK_DIR}/vectis-shebang-smoke.lua")
file(WRITE "${script}" "#!${VECTIS_BIN}\nlocal vectis = require(\"vectis\")\nassert(vectis.status_string(vectis.OK) == \"ok\")\n")
file(CHMOD "${script}"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

execute_process(COMMAND "${script}"
                RESULT_VARIABLE result
                OUTPUT_VARIABLE output
                ERROR_VARIABLE error)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "shebang smoke failed (${result})\nstdout:\n${output}\nstderr:\n${error}")
endif()
