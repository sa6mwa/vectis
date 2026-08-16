if(VECTIS_BUILD_TESTS)
  add_subdirectory(unit)
endif()

if(VECTIS_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

add_executable(vectis_pack_smtp_harness
  helpers/vectis_pack_smtp_harness.c)
target_link_libraries(vectis_pack_smtp_harness PRIVATE Threads::Threads)
set_target_properties(vectis_pack_smtp_harness PROPERTIES
  C_STANDARD 90
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(vectis_pack_smtp_harness PRIVATE -UNDEBUG)
endif()

add_executable(vectis_acme_mock_provider
  helpers/vectis_acme_mock_provider.c)
target_link_libraries(vectis_acme_mock_provider PRIVATE OpenSSL::Crypto)
set_target_properties(vectis_acme_mock_provider PROPERTIES
  C_STANDARD 90
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(vectis_acme_mock_provider PRIVATE -UNDEBUG)
endif()

add_executable(vectis_pick_port
  helpers/vectis_pick_port.c)
set_target_properties(vectis_pick_port PROPERTIES
  C_STANDARD 90
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)

if(TARGET vectis_bin)
  find_program(VECTIS_HOST_LUA NAMES lua5.5 lua)

  add_executable(vectis_opcua_lua_e2e
    opcua_lua_e2e.c
    $<TARGET_OBJECTS:vectis_opcua_lua>)
  target_include_directories(vectis_opcua_lua_e2e PRIVATE
    "${VECTIS_EXTERNAL_ROOT}/include")
  target_compile_definitions(vectis_opcua_lua_e2e PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_link_libraries(vectis_opcua_lua_e2e PRIVATE
    cpkt::lua_runtime
    cpkt::opcua
    Threads::Threads)
  if(UNIX AND NOT APPLE)
    target_link_libraries(vectis_opcua_lua_e2e PRIVATE m)
  endif()
  set_target_properties(vectis_opcua_lua_e2e PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
  )
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_opcua_lua_e2e PRIVATE
      -std=c99
      -Wall
      -Wextra
      -Werror
      -Wpedantic
      -Wstrict-prototypes
      -Wmissing-prototypes
      -Wshadow
      -Wpointer-arith
      -Wcast-qual
      -Wwrite-strings
    )
  endif()
  add_test(NAME vectis_lua_opcua_e2e
           COMMAND vectis_opcua_lua_e2e
                   "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/opcua_client.lua")
  set_tests_properties(vectis_lua_opcua_e2e PROPERTIES
    LABELS "lua;smoke;local;integration"
    TIMEOUT 30)

  add_test(NAME vectis_cli_version COMMAND vectis_bin --version)
  set_tests_properties(vectis_cli_version PROPERTIES
    LABELS "smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_smoke
           COMMAND vectis_bin "${CMAKE_SOURCE_DIR}/tests/lua/smoke.lua" first second)
  set_tests_properties(vectis_lua_smoke PROPERTIES
    ENVIRONMENT "VECTIS_EXPECTED_VERSION=${PROJECT_VERSION}"
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_preload_artifact_contract
           COMMAND bash
                   "${CMAKE_SOURCE_DIR}/scripts/verify_vectis_lua_preloads.sh"
                   "$<TARGET_FILE:vectis_bin>"
                   "${PROJECT_VERSION}")
  set_tests_properties(vectis_lua_preload_artifact_contract PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  if(VECTIS_HOST_LUA)
    add_test(NAME vectis_lua_top_level_module
             COMMAND "${CMAKE_COMMAND}"
                     "-DVECTIS_HOST_LUA=${VECTIS_HOST_LUA}"
                     "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                     "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                     -P "${CMAKE_SOURCE_DIR}/tests/lua/top_level_module.cmake")
    set_tests_properties(vectis_lua_top_level_module PROPERTIES
      LABELS "lua;smoke;local"
      TIMEOUT 30)
  endif()
  add_test(NAME vectis_lua_curl
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/curl.cmake")
  set_tests_properties(vectis_lua_curl PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_curl_contract
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/curl_contract.cmake")
  set_tests_properties(vectis_lua_curl_contract PROPERTIES
    LABELS "lua;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_openssl
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/openssl.cmake")
  set_tests_properties(vectis_lua_openssl PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_dsv
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/dsv.cmake")
  set_tests_properties(vectis_lua_dsv PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_facade_contracts
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/facade_contracts.cmake")
  set_tests_properties(vectis_lua_facade_contracts PROPERTIES
    LABELS "lua;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_lockd_helpers
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/lockd_helpers.cmake")
  set_tests_properties(vectis_lua_lockd_helpers PROPERTIES
    LABELS "lua;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_lockd_examples
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/lockd_examples.cmake")
  set_tests_properties(vectis_lua_lockd_examples PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_http
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/http.cmake")
  set_tests_properties(vectis_lua_http PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_mqtt
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/mqtt.cmake")
  set_tests_properties(vectis_lua_mqtt PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_webdav
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/webdav.cmake")
  set_tests_properties(vectis_lua_webdav PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_webdav_server
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/webdav_server.cmake")
  set_tests_properties(vectis_lua_webdav_server PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_certs
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/certs.cmake")
  set_tests_properties(vectis_lua_certs PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_shebang
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/shebang.cmake")
  set_tests_properties(vectis_lua_shebang PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_ssh_sftp
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/ssh_sftp.cmake")
  set_tests_properties(vectis_lua_ssh_sftp PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_audio_sus
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/audio_sus.cmake")
  set_tests_properties(vectis_lua_audio_sus PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_sus_audio_live
           COMMAND "${CMAKE_COMMAND}" -E env
                   "VECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   bash "${CMAKE_SOURCE_DIR}/scripts/test-live-sus-audio.sh")
  set_tests_properties(vectis_lua_sus_audio_live PROPERTIES
    LABELS "lua;live"
    TIMEOUT 300)
  add_test(NAME vectis_lua_pack
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PACK_SMTP_HARNESS=$<TARGET_FILE:vectis_pack_smtp_harness>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/pack.cmake")
  set_tests_properties(vectis_lua_pack PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_cli_admin_credentials
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/admin_credentials.cmake")
  set_tests_properties(vectis_cli_admin_credentials PROPERTIES
    LABELS "cli;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_mdf_render
           COMMAND vectis_bin "${CMAKE_SOURCE_DIR}/examples/lua/mdf_render.lua")
  set_tests_properties(vectis_example_lua_mdf_render PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_terminal_tools
           COMMAND vectis_bin "${CMAKE_SOURCE_DIR}/examples/lua/terminal_tools.lua")
  set_tests_properties(vectis_example_lua_terminal_tools PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_logging
           COMMAND vectis_bin "${CMAKE_SOURCE_DIR}/examples/lua/logging.lua")
  set_tests_properties(vectis_example_lua_logging PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_data_formats
           COMMAND vectis_bin "${CMAKE_SOURCE_DIR}/examples/lua/data_formats.lua")
  set_tests_properties(vectis_example_lua_data_formats PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_crypto_certs
           COMMAND "${CMAKE_COMMAND}" -E env
                   "VECTIS_LUA_CRYPTO_EXAMPLE_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/crypto_certs.lua")
  set_tests_properties(vectis_example_lua_crypto_certs PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_protocol_clients
           COMMAND "${CMAKE_COMMAND}" -E env
                   "VECTIS_LUA_PROTOCOL_EXAMPLE_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/protocol_clients.lua")
  set_tests_properties(vectis_example_lua_protocol_clients PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_curl_protocols
           COMMAND "${CMAKE_COMMAND}" -E env
                   "VECTIS_LUA_CURL_PROTOCOL_EXAMPLE_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/curl_protocols.lua")
  set_tests_properties(vectis_example_lua_curl_protocols PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_cai_local
           COMMAND "${CMAKE_COMMAND}" -E env
                   "VECTIS_LUA_CAI_EXAMPLE_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/cai_local.lua")
  set_tests_properties(vectis_example_lua_cai_local PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_audio_sus
           COMMAND "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/audio_sus.lua")
  set_tests_properties(vectis_example_lua_audio_sus PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_sus_loaded_model
           COMMAND "${CMAKE_COMMAND}" -E env
                   "VECTIS_LUA_SUS_EXAMPLE_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/sus_loaded_model.lua")
  set_tests_properties(vectis_example_lua_sus_loaded_model PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_audio_devices
           COMMAND "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/audio_devices.lua")
  set_tests_properties(vectis_example_lua_audio_devices PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_local_data_pipeline
           COMMAND "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/local_data_pipeline.lua")
  set_tests_properties(vectis_example_lua_local_data_pipeline PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_mailbox_pipeline
           COMMAND "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/mailbox_pipeline.lua")
  set_tests_properties(vectis_example_lua_mailbox_pipeline PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_local_data_pipeline_pack
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/example_local_data_pipeline_pack.cmake")
  set_tests_properties(vectis_example_lua_local_data_pipeline_pack PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_local_facades_pack
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/example_local_facades_pack.cmake")
  set_tests_properties(vectis_example_lua_local_facades_pack PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_webdav_fileserver
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_SCRIPT=${CMAKE_SOURCE_DIR}/examples/lua/webdav_fileserver.lua"
                   "-DVECTIS_PORT_ENV=VECTIS_LUA_WEBDAV_EXAMPLE_PORT"
                   "-DVECTIS_SUCCESS_MARKER=lua webdav fileserver example ok"
                   "-DVECTIS_LABEL=Lua WebDAV fileserver example"
                   "-DVECTIS_EXTRA_ENV=VECTIS_LUA_WEBDAV_EXAMPLE_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/lua-webdav-fileserver-example"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/run_example_with_port.cmake")
  set_tests_properties(vectis_example_lua_webdav_fileserver PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_webdav_fileserver_pack
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/example_webdav_fileserver_pack.cmake")
  set_tests_properties(vectis_example_lua_webdav_fileserver_pack PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_api_server
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_SCRIPT=${CMAKE_SOURCE_DIR}/examples/lua/api_server.lua"
                   "-DVECTIS_PORT_ENV=VECTIS_LUA_API_EXAMPLE_PORT"
                   "-DVECTIS_SUCCESS_MARKER=lua api server example ok"
                   "-DVECTIS_LABEL=Lua API server example"
                   "-DVECTIS_EXTRA_ENV=VECTIS_LUA_API_EXAMPLE_AUTH_PATH=${CMAKE_CURRENT_BINARY_DIR}/vectis-lua-api-example-auth.json"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/run_example_with_port.cmake")
  set_tests_properties(vectis_example_lua_api_server PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_api_server_pack
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/example_api_pack.cmake")
  set_tests_properties(vectis_example_lua_api_server_pack PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_metrics_persistent
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_SCRIPT=${CMAKE_SOURCE_DIR}/examples/lua/metrics_persistent.lua"
                   "-DVECTIS_PORT_ENV=VECTIS_LUA_METRICS_PERSISTENT_EXAMPLE_PORT"
                   "-DVECTIS_SUCCESS_MARKER=lua metrics persistent example ok"
                   "-DVECTIS_LABEL=Lua metrics persistent example"
                   "-DVECTIS_EXTRA_ENV=VECTIS_LUA_METRICS_PERSISTENT_EXAMPLE_STORAGE=${CMAKE_CURRENT_BINARY_DIR}/lua-metrics-persistent-pouch"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/run_example_with_port.cmake")
  set_tests_properties(vectis_example_lua_metrics_persistent PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_metrics_authenticated
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_SCRIPT=${CMAKE_SOURCE_DIR}/examples/lua/metrics_authenticated.lua"
                   "-DVECTIS_PORT_ENV=VECTIS_LUA_METRICS_AUTH_EXAMPLE_PORT"
                   "-DVECTIS_SUCCESS_MARKER=lua metrics authenticated example ok"
                   "-DVECTIS_LABEL=Lua metrics authenticated example"
                   "-DVECTIS_EXTRA_ENV=VECTIS_LUA_METRICS_AUTH_EXAMPLE_AUTH_PATH=${CMAKE_CURRENT_BINARY_DIR}/lua-metrics-auth-example-credentials.json;VECTIS_LUA_METRICS_AUTH_EXAMPLE_STATE_PATH=${CMAKE_CURRENT_BINARY_DIR}/lua-metrics-auth-example-state.json;VECTIS_LUA_METRICS_AUTH_EXAMPLE_STORAGE=${CMAKE_CURRENT_BINARY_DIR}/lua-metrics-auth-pouch"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/run_example_with_port.cmake")
  set_tests_properties(vectis_example_lua_metrics_authenticated PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_metrics_ephemeral
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_SCRIPT=${CMAKE_SOURCE_DIR}/examples/lua/metrics_ephemeral.lua"
                   "-DVECTIS_PORT_ENV=VECTIS_LUA_METRICS_EPHEMERAL_EXAMPLE_PORT"
                   "-DVECTIS_SUCCESS_MARKER=lua metrics ephemeral example ok"
                   "-DVECTIS_LABEL=Lua metrics ephemeral example"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/run_example_with_port.cmake")
  set_tests_properties(vectis_example_lua_metrics_ephemeral PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_downstream_api
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_SCRIPT=${CMAKE_SOURCE_DIR}/examples/lua/downstream_api.lua"
                   "-DVECTIS_PORT_ENV=VECTIS_LUA_DOWNSTREAM_EXAMPLE_PORT"
                   "-DVECTIS_SUCCESS_MARKER=lua downstream API example ok"
                   "-DVECTIS_LABEL=Lua downstream API example"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/run_example_with_port.cmake")
  set_tests_properties(vectis_example_lua_downstream_api PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_downstream_api_pack
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/example_downstream_api_pack.cmake")
  set_tests_properties(vectis_example_lua_downstream_api_pack PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
endif()
