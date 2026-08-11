set(output "${WORK_DIR}/vectis-example-lua-downstream-api-pack")

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

execute_process(COMMAND "${CMAKE_COMMAND}" -E env
                        "VECTIS_LUA_DOWNSTREAM_EXAMPLE_PORT=28589"
                        "${output}"
                RESULT_VARIABLE run_result
                OUTPUT_VARIABLE run_stdout
                ERROR_VARIABLE run_stderr)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "packed Lua downstream API example failed: ${run_stdout}${run_stderr}")
endif()
if(NOT run_stdout MATCHES "lua downstream API example ok")
  message(FATAL_ERROR "packed Lua downstream API example missed success marker: ${run_stdout}${run_stderr}")
endif()
