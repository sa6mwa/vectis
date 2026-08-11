set(output "${WORK_DIR}/vectis-example-lua-downstream-api-pack")

include("${VECTIS_SOURCE_DIR}/tests/lua/port_retry.cmake")

file(REMOVE "${output}")

execute_process(COMMAND "${VECTIS_BIN}" -a pack
                        --script "${VECTIS_SOURCE_DIR}/examples/lua/downstream_api.lua"
                        --output "${output}"
                RESULT_VARIABLE pack_result
                OUTPUT_VARIABLE pack_stdout
                ERROR_VARIABLE pack_stderr)
if(NOT pack_result EQUAL 0)
  message(FATAL_ERROR "packing Lua downstream API example failed: ${pack_stdout}${pack_stderr}")
endif()

vectis_run_command_with_port(
  LABEL "packed Lua downstream API example"
  PORT_ENV "VECTIS_LUA_DOWNSTREAM_EXAMPLE_PORT"
  SUCCESS_MARKER "lua downstream API example ok"
  COMMAND "${output}")
