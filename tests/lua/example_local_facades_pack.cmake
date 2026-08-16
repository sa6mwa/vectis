function(vectis_pack_and_run_lua_example name script marker)
  set(output "${WORK_DIR}/vectis-example-${name}-pack")
  set(run_env ${ARGN})

  file(REMOVE "${output}")
  execute_process(COMMAND "${VECTIS_BIN}" -a pack
                          --script "${script}"
                          --output "${output}"
                  RESULT_VARIABLE pack_result
                  OUTPUT_VARIABLE pack_stdout
                  ERROR_VARIABLE pack_stderr)
  if(NOT pack_result EQUAL 0)
    message(FATAL_ERROR "packing ${name} Lua example failed: ${pack_stdout}${pack_stderr}")
  endif()

  if(run_env)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${run_env} "${output}"
                    RESULT_VARIABLE run_result
                    OUTPUT_VARIABLE run_stdout
                    ERROR_VARIABLE run_stderr)
  else()
    execute_process(COMMAND "${output}"
                    RESULT_VARIABLE run_result
                    OUTPUT_VARIABLE run_stdout
                    ERROR_VARIABLE run_stderr)
  endif()
  if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "packed ${name} Lua example failed: ${run_stdout}${run_stderr}")
  endif()
  if(NOT run_stdout MATCHES "${marker}")
    message(FATAL_ERROR "packed ${name} Lua example missed success marker: ${run_stdout}${run_stderr}")
  endif()
endfunction()

vectis_pack_and_run_lua_example(
  "data-formats"
  "${VECTIS_SOURCE_DIR}/examples/lua/data_formats.lua"
  "lua data formats example ok")

vectis_pack_and_run_lua_example(
  "crypto-certs"
  "${VECTIS_SOURCE_DIR}/examples/lua/crypto_certs.lua"
  "lua crypto certs example ok"
  "VECTIS_LUA_CRYPTO_EXAMPLE_DIR=${WORK_DIR}")

vectis_pack_and_run_lua_example(
  "curl-protocols"
  "${VECTIS_SOURCE_DIR}/examples/lua/curl_protocols.lua"
  "lua curl protocols example ok"
  "VECTIS_LUA_CURL_PROTOCOL_EXAMPLE_DIR=${WORK_DIR}")

vectis_pack_and_run_lua_example(
  "cai-local"
  "${VECTIS_SOURCE_DIR}/examples/lua/cai_local.lua"
  "lua cai local example ok"
  "VECTIS_LUA_CAI_EXAMPLE_DIR=${WORK_DIR}")

vectis_pack_and_run_lua_example(
  "audio-sus"
  "${VECTIS_SOURCE_DIR}/examples/lua/audio_sus.lua"
  "lua audio sus example ok")

vectis_pack_and_run_lua_example(
  "sus-loaded-model"
  "${VECTIS_SOURCE_DIR}/examples/lua/sus_loaded_model.lua"
  "lua sus loaded model example ok"
  "VECTIS_LUA_SUS_EXAMPLE_DIR=${WORK_DIR}")

vectis_pack_and_run_lua_example(
  "audio-devices"
  "${VECTIS_SOURCE_DIR}/examples/lua/audio_devices.lua"
  "lua audio devices example ok")

vectis_pack_and_run_lua_example(
  "mdf-render"
  "${VECTIS_SOURCE_DIR}/examples/lua/mdf_render.lua"
  "lua mdf render example ok")

vectis_pack_and_run_lua_example(
  "terminal-tools"
  "${VECTIS_SOURCE_DIR}/examples/lua/terminal_tools.lua"
  "lua terminal tools example ok")

vectis_pack_and_run_lua_example(
  "logging"
  "${VECTIS_SOURCE_DIR}/examples/lua/logging.lua"
  "lua logging example ok")

vectis_pack_and_run_lua_example(
  "protocol-clients"
  "${VECTIS_SOURCE_DIR}/examples/lua/protocol_clients.lua"
  "lua protocol clients example ok"
  "VECTIS_LUA_PROTOCOL_EXAMPLE_DIR=${WORK_DIR}")
