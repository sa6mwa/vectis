if(NOT DEFINED VECTIS_BIN)
  message(FATAL_ERROR "VECTIS_BIN is required")
endif()

if(NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "WORK_DIR is required")
endif()

set(script "${WORK_DIR}/vectis-shebang-smoke.lua")
set(sibling_module "${WORK_DIR}/shebang_sibling.lua")
set(nested_module_dir "${WORK_DIR}/shebang_nested")

file(WRITE "${sibling_module}" "return { value = 'sibling-loaded' }\n")
file(MAKE_DIRECTORY "${nested_module_dir}")
file(WRITE "${nested_module_dir}/init.lua" "return { value = 'nested-loaded' }\n")
file(WRITE "${script}" "#!${VECTIS_BIN}\nlocal vectis = require(\"vectis\")\nlocal sibling = require(\"shebang_sibling\")\nlocal nested = require(\"shebang_nested\")\nassert(vectis.status_string(vectis.OK) == \"ok\")\nassert(sibling.value == \"sibling-loaded\")\nassert(nested.value == \"nested-loaded\")\n")
file(CHMOD "${script}"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

execute_process(COMMAND "${script}"
                RESULT_VARIABLE result
                OUTPUT_VARIABLE output
                ERROR_VARIABLE error)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "shebang smoke failed (${result})\nstdout:\n${output}\nstderr:\n${error}")
endif()
