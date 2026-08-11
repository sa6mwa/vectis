set(output "${WORK_DIR}/vectis-example-lua-api-server-pack")
set(auth_path "${WORK_DIR}/vectis-example-lua-api-server-pack-auth.json")

file(REMOVE "${output}" "${auth_path}" "${auth_path}.lock")

execute_process(COMMAND "${VECTIS_BIN}" -a pack
                        --script "${VECTIS_SOURCE_DIR}/examples/lua/api_server.lua"
                        --output "${output}"
                RESULT_VARIABLE pack_result
                OUTPUT_VARIABLE pack_stdout
                ERROR_VARIABLE pack_stderr)
if(NOT pack_result EQUAL 0)
  message(FATAL_ERROR "packing Lua API server example failed: ${pack_stdout}${pack_stderr}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -E env
                        "VECTIS_LUA_API_EXAMPLE_PORT=28587"
                        "VECTIS_LUA_API_EXAMPLE_AUTH_PATH=${auth_path}"
                        "${output}"
                RESULT_VARIABLE run_result
                OUTPUT_VARIABLE run_stdout
                ERROR_VARIABLE run_stderr)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "packed Lua API server example failed: ${run_stdout}${run_stderr}")
endif()
if(NOT run_stdout MATCHES "lua api server example ok")
  message(FATAL_ERROR "packed Lua API server example missed success marker: ${run_stdout}${run_stderr}")
endif()
