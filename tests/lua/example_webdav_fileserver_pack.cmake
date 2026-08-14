set(output "${WORK_DIR}/vectis-example-lua-webdav-fileserver-pack")
set(cache_dir "${WORK_DIR}/lua-webdav-fileserver-pack-example")

include("${VECTIS_SOURCE_DIR}/tests/lua/port_retry.cmake")

file(REMOVE "${output}")

execute_process(COMMAND "${VECTIS_BIN}" -a pack
                        --script "${VECTIS_SOURCE_DIR}/examples/lua/webdav_fileserver.lua"
                        --output "${output}"
                RESULT_VARIABLE pack_result
                OUTPUT_VARIABLE pack_stdout
                ERROR_VARIABLE pack_stderr)
if(NOT pack_result EQUAL 0)
  message(FATAL_ERROR "packing Lua WebDAV fileserver example failed: ${pack_stdout}${pack_stderr}")
endif()

vectis_run_command_with_port(
  LABEL "packed Lua WebDAV fileserver example"
  PORT_ENV "VECTIS_LUA_WEBDAV_EXAMPLE_PORT"
  SUCCESS_MARKER "lua webdav fileserver example ok"
  EXTRA_ENV "VECTIS_LUA_WEBDAV_EXAMPLE_CACHE_DIR=${cache_dir}"
  COMMAND "${output}")
