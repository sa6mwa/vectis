set(output "${WORK_DIR}/vectis-example-lua-api-server-pack")
set(lockd_path "${WORK_DIR}/vectis-example-lua-api-server-pack-lockd")

include("${VECTIS_SOURCE_DIR}/tests/lua/port_retry.cmake")

file(REMOVE "${output}")
file(REMOVE_RECURSE "${lockd_path}")

execute_process(COMMAND "${VECTIS_BIN}" -a pack
                        --script "${VECTIS_SOURCE_DIR}/examples/lua/api_server.lua"
                        --output "${output}"
                RESULT_VARIABLE pack_result
                OUTPUT_VARIABLE pack_stdout
                ERROR_VARIABLE pack_stderr)
if(NOT pack_result EQUAL 0)
  message(FATAL_ERROR "packing Lua API server example failed: ${pack_stdout}${pack_stderr}")
endif()

vectis_run_command_with_port(
  LABEL "packed Lua API server example"
  PORT_ENV "VECTIS_LUA_API_EXAMPLE_PORT"
  SUCCESS_MARKER "lua api app example ok"
  EXTRA_ENV "VECTIS_LUA_API_EXAMPLE_LOCKD_PATH=${lockd_path}"
  COMMAND "${output}")
