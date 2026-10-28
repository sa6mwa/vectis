if(VECTIS_BUILD_TESTS AND VECTIS_BUILD_BINARY)
  add_test(NAME vectis_lua_lql_upgrade
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/lql-upgrade"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/lql_upgrade.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_lql_upgrade PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_pg_connection_local
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/pg-connection-local"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/pg_connection_local.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_pg_connection_local PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_xml_objects
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/xml-objects"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/xml_objects.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_xml_objects PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  find_program(vectis_curl_test_go NAMES go)
  if(vectis_curl_test_go)
    set(curl_http_fixture "${CMAKE_CURRENT_BINARY_DIR}/curl_http_fixture")
    add_custom_command(OUTPUT "${curl_http_fixture}"
      COMMAND ${CMAKE_COMMAND} -E env "GOCACHE=${CMAKE_CURRENT_BINARY_DIR}/go-cache"
        CGO_ENABLED=0 GOTOOLCHAIN=local "${vectis_curl_test_go}" build -trimpath
        -o "${curl_http_fixture}" "${CMAKE_SOURCE_DIR}/tests/fixtures/curl_http.go"
      DEPENDS "${CMAKE_SOURCE_DIR}/tests/fixtures/curl_http.go" VERBATIM)
    add_custom_target(vectis_curl_http_fixture ALL DEPENDS "${curl_http_fixture}")
    add_test(NAME vectis_lua_curl_handles COMMAND "${CMAKE_COMMAND}"
      "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>" "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
      "-DCURL_HTTP_FIXTURE=${curl_http_fixture}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/curl-handles"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/curl_handles.cmake")
    set_tests_properties(vectis_lua_curl_handles PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 30 RUN_SERIAL TRUE)
  endif()
  add_test(NAME vectis_lua_crypto
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/crypto"
      "-DCRYPTO_FIXTURE=$<TARGET_FILE:vectis_unit_crypto>"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/crypto.cmake")
  set_tests_properties(vectis_lua_crypto PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 20 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_png
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/png"
      "-DEXTRA_ENV=VECTIS_PNG_FIXTURES=${CMAKE_SOURCE_DIR}/tests/fixtures/png"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/png.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_png PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_compression
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/compression"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/compression.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_compression PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_pdf
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/pdf"
      "-DEXTRA_ENV=VECTIS_PDF_FIXTURES=${CMAKE_SOURCE_DIR}/tests/fixtures/pdf"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/pdf.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_pdf PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_sqlite
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/sqlite"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/sqlite.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_sqlite PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_upstream_facades
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/upstream-facades"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/upstream_facades.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_upstream_facades PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_upstream_sources
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DCOMPILATION_DB=${CMAKE_BINARY_DIR}/compile_commands.json"
      "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/upstream-sources"
      "-Dpslog_core=${vectis_pslog_lua_source}" "-Dpslog_module=${vectis_pslog_lua_source_dir}/lua/pslog/init.lua"
      "-Dlql_core=${vectis_liblql_lua_source}" "-Dlql_module=${vectis_liblql_lua_source_dir}/lua/lql/init.lua"
      "-Dmdf_core=${vectis_libmdf_lua_source}" "-Dmdf_module=${vectis_libmdf_lua_source_dir}/lua/libmdf.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upstream_sources.cmake")
  set_tests_properties(vectis_lua_upstream_sources PROPERTIES LABELS "lua;local;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
endif()

if(VECTIS_BUILD_TESTS)
  add_test(NAME vectis_clean_ownership
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
      "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/clean-ownership" -P "${CMAKE_SOURCE_DIR}/tests/clean.cmake")
  set_tests_properties(vectis_clean_ownership PROPERTIES LABELS "unit;local;lifecycle;failure-fast" TIMEOUT 5)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME vectis_dependency_cache
      COMMAND bash "${CMAKE_SOURCE_DIR}/tests/dependency_cache.sh")
    set_tests_properties(vectis_dependency_cache PROPERTIES
      LABELS "unit;local;lifecycle;failure-fast" TIMEOUT 20 RUN_SERIAL TRUE)
  endif()
  add_test(NAME vectis_byte_array_generator
    COMMAND "${CMAKE_COMMAND}"
            "-DGENERATOR=${CMAKE_SOURCE_DIR}/cmake/vectis_generate_byte_array.cmake"
            "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/byte-array-generator"
            -P "${CMAKE_SOURCE_DIR}/tests/byte_array_generator.cmake")
  set_tests_properties(vectis_byte_array_generator PROPERTIES
    LABELS "unit;local"
    TIMEOUT 30)
  if(VECTIS_TARGET_ID STREQUAL "x86_64-linux-gnu" AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
      AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64")
    add_executable(vectis_runtime_probe helpers/vectis_runtime_probe.c)
    vectis_configure_c89_target(vectis_runtime_probe)
    target_compile_options(vectis_runtime_probe PRIVATE -Wall -Wextra -Werror -Wpedantic)
  endif()
  add_subdirectory(unit)
endif()

if(VECTIS_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

if(NOT VECTIS_BUILD_TESTS)
  return()
endif()

if(VECTIS_BUILD_BINARY)
  add_test(NAME vectis_cli_source_integrity COMMAND "${CMAKE_COMMAND}"
    "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/source-integrity"
    -P "${CMAKE_SOURCE_DIR}/tests/lua/source_integrity.cmake")
  set_tests_properties(vectis_cli_source_integrity PROPERTIES LABELS "cli;local;failure-fast" TIMEOUT 5)
  if(VECTIS_TEST_POSTGRES)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
       NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR
       NOT VECTIS_TARGET_ARCH STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
      message(FATAL_ERROR "Live PostgreSQL tests require a native Linux configuration")
    endif()
    add_test(NAME vectis_postgres_xa_live COMMAND bash
      "${CMAKE_SOURCE_DIR}/scripts/test_postgres_xa.sh" "$<TARGET_FILE:vectis_bin>")
    add_test(NAME vectis_postgres_connection_live COMMAND "${CMAKE_COMMAND}" -E env
      "VECTIS_POSTGRES_TEST_SCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/pg_connection.lua"
      "VECTIS_POSTGRES_NATIVE_TEST=$<TARGET_FILE:vectis_test_pg_connection_live>"
      "VECTIS_POSTGRES_OOM_TEST=$<TARGET_FILE:vectis_test_pg_connection_lua_oom>"
      bash "${CMAKE_SOURCE_DIR}/scripts/test_postgres_xa.sh" "$<TARGET_FILE:vectis_bin>")
    set_tests_properties(vectis_postgres_connection_live PROPERTIES
      LABELS "service;postgres;lua;packed" TIMEOUT 90 RUN_SERIAL TRUE
      ENVIRONMENT "XDG_RUNTIME_DIR=$ENV{XDG_RUNTIME_DIR};XDG_CONFIG_HOME=$ENV{XDG_CONFIG_HOME};XDG_CACHE_HOME=$ENV{XDG_CACHE_HOME}")
    set_tests_properties(vectis_postgres_xa_live PROPERTIES
      LABELS "service;postgres;xa;packed" TIMEOUT 90 RUN_SERIAL TRUE
      # Rootless Podman is a workstation prerequisite. Its runtime/network state
      # must use the host session, while database/test payloads remain owned.
      ENVIRONMENT "XDG_RUNTIME_DIR=$ENV{XDG_RUNTIME_DIR};XDG_CONFIG_HOME=$ENV{XDG_CONFIG_HOME};XDG_CACHE_HOME=$ENV{XDG_CACHE_HOME}")
  endif()
  add_test(NAME vectis_lua_storage
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/storage"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/storage.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_storage PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_softline_upgrade
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/softline-upgrade"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/softline_upgrade.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_softline_upgrade PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_zlib_raw
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/zlib-raw"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/zlib_raw.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_zlib_raw PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_zlib_raw_io
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/zlib-raw-io"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/zlib_raw_io.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_zlib_raw_io PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_zlib_headers
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/zlib-headers"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/zlib_headers.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_zlib_headers PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_zlib_back
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/zlib-back"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/zlib_back.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_zlib_back PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_zlib_allocator
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/zlib-allocator"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/zlib_allocator.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_zlib_allocator PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_zlib_buffers
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/zlib-buffers"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/zlib_buffers.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_zlib_buffers PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_buffer
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/buffer"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/buffer.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_buffer PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_gz
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/gz"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/gz.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_gz PROPERTIES LABELS "lua;local;packed" TIMEOUT 30 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_zlib_streaming
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/zlib-streaming"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/zlib_streaming.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_zlib_streaming PROPERTIES
    LABELS "lua;local;packed;failure-fast" TIMEOUT 30 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_pouch_typed_settings
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/pouch-typed-settings"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/pouch_typed_settings.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_pouch_typed_settings PROPERTIES
    LABELS "lua;local;packed;failure-fast" TIMEOUT 30 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_digest_streaming
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/digest-streaming"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/digest_streaming.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_digest_streaming PROPERTIES
    LABELS "lua;local;packed;failure-fast" TIMEOUT 30 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_queue_seams
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/queue-seams"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/queue_seams.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_queue_seams PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 30 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_upgrade_seams
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/upgrade-seams"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_upgrade_seams PROPERTIES LABELS "lua;local;packed;failure-fast" TIMEOUT 30 RUN_SERIAL TRUE)
endif()
if(NOT APPLE)
  add_executable(vectis_pack_smtp_harness
    helpers/vectis_pack_smtp_harness.c)
  target_link_libraries(vectis_pack_smtp_harness PRIVATE Threads::Threads)
  vectis_configure_c89_target(vectis_pack_smtp_harness)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_pack_smtp_harness PRIVATE -UNDEBUG)
  endif()
endif()

add_executable(vectis_acme_mock_provider
  helpers/vectis_acme_mock_provider.c)
target_link_libraries(vectis_acme_mock_provider PRIVATE OpenSSL::Crypto)
vectis_configure_c89_target(vectis_acme_mock_provider)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(vectis_acme_mock_provider PRIVATE -UNDEBUG)
endif()

add_executable(vectis_pick_port
  helpers/vectis_pick_port.c)
vectis_configure_c89_target(vectis_pick_port)

add_executable(vectis_wait_signal_harness
  helpers/vectis_wait_signal_harness.c)
target_link_libraries(vectis_wait_signal_harness PRIVATE ${vectis_test_link_target})
vectis_configure_c89_target(vectis_wait_signal_harness)

add_executable(vectis_ws_client
  helpers/vectis_ws_client.c)
set_target_properties(vectis_ws_client PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)

if(TARGET vectis_bin)
  if(VECTIS_WITH_KORE_RUNTIME AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
      AND VECTIS_TARGET_ARCH STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
    include("${CMAKE_SOURCE_DIR}/cmake/vectis_bench_fixture.cmake")
    vectis_add_bench_fixture(proxy_bench)
    add_test(NAME vectis_proxy_stream_smoke_http
      COMMAND "${proxy_bench}" benchmark --vectis "$<TARGET_FILE:vectis_bin>"
        --root "${CMAKE_BINARY_DIR}/bench/proxy" --smoke)
    add_test(NAME vectis_proxy_stream_smoke_tls
      COMMAND "${proxy_bench}" benchmark --vectis "$<TARGET_FILE:vectis_bin>"
        --root "${CMAKE_BINARY_DIR}/bench/proxy" --smoke --tls)
    add_test(NAME vectis_proxy_stream_smoke_h2
      COMMAND "${proxy_bench}" benchmark --vectis "$<TARGET_FILE:vectis_bin>"
        --root "${CMAKE_BINARY_DIR}/bench/proxy" --smoke --http2)
    set_tests_properties(vectis_proxy_stream_smoke_http
      vectis_proxy_stream_smoke_tls vectis_proxy_stream_smoke_h2 PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      LABELS "proxy;smoke;local;integration" TIMEOUT 25 RUN_SERIAL TRUE)
    add_test(NAME vectis_metrics_recovery_fast
      COMMAND "${proxy_bench}" metrics-recovery --binary "$<TARGET_FILE:vectis_bin>"
        --script "${CMAKE_SOURCE_DIR}/tests/lua/metrics_startup.lua"
        --root "${CMAKE_BINARY_DIR}/bench/metrics-recovery-fast" --updates 16)
    set_tests_properties(vectis_metrics_recovery_fast PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" LABELS "local;integration;metrics;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
    # One native producer shares identical expensive recovery evidence between
    # CTest, perf-gate and service e2e. It is not a per-test scheduler.
    set(metrics_recovery_args)
    set(metrics_recovery_test vectis_metrics_recovery_performance)
    set(metrics_recovery_label performance)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" metrics_configuration)
    if(VECTIS_ENABLE_COVERAGE OR
       "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${metrics_configuration}}" MATCHES "fsanitize=")
      # Instrumentation retains the full workload/readiness/cleanup assertions.
      # Its CPU measurement is not the normal build's performance contract.
      set(metrics_recovery_args --cpu-seconds 0)
      set(metrics_recovery_test vectis_metrics_recovery_instrumented)
      set(metrics_recovery_label instrumentation)
    endif()
    set(metrics_recovery_stamp "${CMAKE_BINARY_DIR}/test-results/metrics-recovery.passed")
    add_custom_command(OUTPUT "${metrics_recovery_stamp}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/test-results"
      COMMAND "${CMAKE_COMMAND}" -E rm -f "${metrics_recovery_stamp}"
      COMMAND "${proxy_bench}" metrics-recovery --binary "$<TARGET_FILE:vectis_bin>"
        --script "${CMAKE_SOURCE_DIR}/tests/lua/metrics_startup.lua"
        --root "${CMAKE_BINARY_DIR}/bench/metrics-recovery" ${metrics_recovery_args}
      COMMAND "${CMAKE_COMMAND}" -E touch "${metrics_recovery_stamp}"
      DEPENDS vectis_bin vectis_bench_fixture "${proxy_bench}" "${CMAKE_SOURCE_DIR}/tests/lua/metrics_startup.lua"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" VERBATIM)
    add_custom_target(vectis_metrics_recovery_gate DEPENDS "${metrics_recovery_stamp}")
    add_test(NAME ${metrics_recovery_test}
      COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target vectis_metrics_recovery_gate)
    set_tests_properties(${metrics_recovery_test} PROPERTIES
      LABELS "local;integration;metrics;${metrics_recovery_label}" TIMEOUT 300 RUN_SERIAL TRUE
      # This test invokes CMake's declared producer, not just a product child.
      # Preserve configure-time host prerequisite/cache discovery on regeneration.
      ENVIRONMENT "XDG_CACHE_HOME=$ENV{XDG_CACHE_HOME};XDG_CONFIG_HOME=$ENV{XDG_CONFIG_HOME};XDG_STATE_HOME=$ENV{XDG_STATE_HOME};XDG_RUNTIME_DIR=$ENV{XDG_RUNTIME_DIR}")
  endif()
  find_program(VECTIS_HOST_LUA NAMES lua5.5 lua)

  add_executable(vectis_opcua_lua_e2e
    opcua_lua_e2e.c
    $<TARGET_OBJECTS:vectis_opcua_lua>)
  add_executable(vectis_opcua_lua_cleanup unit/test_vectis_opcua_lua_cleanup.c)
  target_include_directories(vectis_opcua_lua_cleanup PRIVATE
    "${vectis_pslog_lua_source_dir}/include"
    "${CMAKE_SOURCE_DIR}/src")
  target_compile_definitions(vectis_opcua_lua_cleanup PRIVATE
    VECTIS_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(vectis_opcua_lua_cleanup PRIVATE
    ${vectis_test_link_target} cpkt::lua_runtime cpkt::opcua
    $<TARGET_OBJECTS:vectis_pslog_lua>)
  set_target_properties(vectis_opcua_lua_cleanup PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_opcua_lua_cleanup PRIVATE
      -UNDEBUG -Wall -Wextra -Werror -Wpedantic)
  endif()
  add_test(NAME vectis_opcua_lua_cleanup COMMAND vectis_opcua_lua_cleanup)
  set_tests_properties(vectis_opcua_lua_cleanup PROPERTIES
    LABELS "unit;lua;local;failure-fast" TIMEOUT 30)
  target_include_directories(vectis_opcua_lua_e2e PRIVATE
    "${vectis_pslog_lua_source_dir}/include")
  target_compile_definitions(vectis_opcua_lua_e2e PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_link_libraries(vectis_opcua_lua_e2e PRIVATE
    cpkt::lua_runtime
    cpkt::opcua pslog::pslog_static
    $<TARGET_OBJECTS:vectis_pslog_lua>
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

  add_test(NAME vectis_opcua_lua_reentry
           COMMAND vectis_opcua_lua_e2e
                   "${CMAKE_SOURCE_DIR}/tests/lua/opcua_reentry.lua")
  set_tests_properties(vectis_opcua_lua_reentry PROPERTIES
    LABELS "lua;local;integration" TIMEOUT 15)

  add_test(NAME vectis_cli_version COMMAND vectis_bin --version)
  set_tests_properties(vectis_cli_version PROPERTIES
    LABELS "smoke;local"
    TIMEOUT 30)
  add_executable(vectis_audio_lua_validation
    unit/test_vectis_audio_lua_validation.c)
  target_include_directories(vectis_audio_lua_validation PRIVATE
    "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(vectis_audio_lua_validation PRIVATE
    ${vectis_test_link_target} cpkt::lua_runtime cpkt::audio)
  set_target_properties(vectis_audio_lua_validation PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_audio_lua_validation PRIVATE
      -UNDEBUG -Wall -Wextra -Werror -Wpedantic)
  endif()
  add_test(NAME vectis_audio_lua_validation
    COMMAND vectis_audio_lua_validation)
  set_tests_properties(vectis_audio_lua_validation PROPERTIES
    LABELS "unit;lua;local;failure-fast" TIMEOUT 30)
  add_executable(vectis_sus_lua_lifetime
    unit/test_vectis_sus_lua_lifetime.c
    ../src/vectis_audio_lua.c)
  target_include_directories(vectis_sus_lua_lifetime PRIVATE
    "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(vectis_sus_lua_lifetime PRIVATE
    ${vectis_test_link_target} cpkt::lua_runtime cpkt::audio cpkt::sus)
  set_target_properties(vectis_sus_lua_lifetime PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_sus_lua_lifetime PRIVATE
      -UNDEBUG -Wall -Wextra -Werror -Wpedantic)
  endif()
  add_test(NAME vectis_sus_lua_lifetime COMMAND vectis_sus_lua_lifetime)
  set_tests_properties(vectis_sus_lua_lifetime PROPERTIES
    LABELS "unit;lua;local;failure-fast" TIMEOUT 30)
  add_executable(vectis_smith_cli_stream
    unit/test_vectis_smith_cli_stream.c)
  target_include_directories(vectis_smith_cli_stream PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src")
  target_compile_definitions(vectis_smith_cli_stream PRIVATE
    _POSIX_C_SOURCE=200809L)
  target_link_libraries(vectis_smith_cli_stream PRIVATE
    ${vectis_test_link_target}
    cai::cai_static
    libmdf::mdf_static
    softline::softline_static)
  if(UNIX AND NOT APPLE)
    target_link_libraries(vectis_smith_cli_stream PRIVATE m util)
  endif()
  set_target_properties(vectis_smith_cli_stream PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_smith_cli_stream PRIVATE
      -std=c99
      -UNDEBUG
      -Wall
      -Wextra
      -Werror
      -Wpedantic
      -Wstrict-prototypes
      -Wmissing-prototypes
      -Wshadow
      -Wpointer-arith
      -Wcast-qual
      -Wwrite-strings)
  endif()
  add_test(NAME vectis_smith_cli_stream COMMAND vectis_smith_cli_stream)
  set_tests_properties(vectis_smith_cli_stream PROPERTIES
    LABELS "cli;unit;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_smoke
           COMMAND vectis_bin "${CMAKE_SOURCE_DIR}/tests/lua/smoke.lua" first second)
  set_tests_properties(vectis_lua_smoke PROPERTIES
    ENVIRONMENT "VECTIS_EXPECTED_VERSION=${PROJECT_VERSION}"
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_mdf_output_policy
           COMMAND vectis_bin "${CMAKE_SOURCE_DIR}/tests/lua/mdf_output_policy.lua")
  set_tests_properties(vectis_lua_mdf_output_policy PROPERTIES
    LABELS "unit;lua;local" TIMEOUT 15)
  add_test(NAME vectis_lua_mdf_regressions
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/mdf-regressions"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/mdf_regressions.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/upgrade_seams.cmake")
  set_tests_properties(vectis_lua_mdf_regressions PROPERTIES
    LABELS "unit;lua;local;packed;failure-fast" TIMEOUT 30 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_logger_native_sink
           COMMAND vectis_bin "${CMAKE_SOURCE_DIR}/tests/lua/logger_native_sink.lua")
  set_tests_properties(vectis_lua_logger_native_sink PROPERTIES
    LABELS "unit;lua;local" TIMEOUT 15)
  foreach(log_policy access lockdc cai http all)
    add_test(NAME vectis_lua_app_log_cleanup_${log_policy}
             COMMAND vectis_bin
                     "${CMAKE_SOURCE_DIR}/tests/lua/app_log_cleanup.lua"
                     "${log_policy}")
    set_tests_properties(vectis_lua_app_log_cleanup_${log_policy} PROPERTIES
      LABELS "unit;lua;local;failure-fast" TIMEOUT 15)
  endforeach()
  if(VECTIS_WITH_KORE_RUNTIME)
    foreach(log_mask RANGE 0 15)
      add_test(NAME vectis_lua_app_failure_modes_${log_mask}
        COMMAND vectis_bin "${CMAKE_SOURCE_DIR}/tests/lua/app_failure_modes.lua"
          "${log_mask}")
      set_tests_properties(vectis_lua_app_failure_modes_${log_mask} PROPERTIES
        LABELS "unit;lua;local;failure-fast" TIMEOUT 5)
    endforeach()
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
      add_test(NAME vectis_lua_mcp_failure_modes
        COMMAND "${CMAKE_COMMAND}" "-DHTTP_TEST=$<TARGET_FILE:vectis_lua_http_behavior>"
          "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
          "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/failure-modes"
          -P "${CMAKE_SOURCE_DIR}/tests/lua/mcp_failures.cmake")
      set_tests_properties(vectis_lua_mcp_failure_modes PROPERTIES
        LABELS "integration;lua;local;failure-fast" TIMEOUT 20 RUN_SERIAL TRUE)
    endif()
  endif()
  add_test(NAME vectis_lua_json_encoder_boundaries
    COMMAND vectis_bin "${CMAKE_SOURCE_DIR}/tests/lua/json_encoder_boundaries.lua")
  set_tests_properties(vectis_lua_json_encoder_boundaries PROPERTIES
    LABELS "unit;lua;local;failure-fast;dependency" TIMEOUT 5)
  add_test(NAME vectis_lua_zlib
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/zlib.cmake")
  set_tests_properties(vectis_lua_zlib PROPERTIES
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
      LABELS "lua;smoke;local;failure-fast"
      TIMEOUT 30)
  endif()
  add_test(NAME vectis_lua_curl
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   "-DUPLOAD_FIXTURE=${proxy_bench}"
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
  add_test(NAME vectis_example_runtime_contract
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/example_runtime_contract.cmake")
  set_tests_properties(vectis_example_runtime_contract PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_server_signal_shutdown
           COMMAND bash
                   "${CMAKE_SOURCE_DIR}/tests/lua/server_signal_shutdown.sh"
                   "$<TARGET_FILE:vectis_bin>"
                   "$<TARGET_FILE:vectis_pick_port>"
                   "$<TARGET_FILE:vectis_wait_signal_harness>")
  set_tests_properties(vectis_server_signal_shutdown PROPERTIES
    LABELS "lua;smoke;local;integration"
    TIMEOUT 45)
  add_test(NAME vectis_lua_websocket
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DHTTP_TEST=$<TARGET_FILE:vectis_lua_http_behavior>"
                   "-DWS_CLIENT=$<TARGET_FILE:vectis_ws_client>"
                   "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/server_websocket.lua"
                   "-DMODE=websocket-enabled"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/lua-websocket"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/http_behavior.cmake")
  set_tests_properties(vectis_lua_websocket PROPERTIES
    LABELS "lua;smoke;local;integration"
    TIMEOUT 30)
  add_test(NAME vectis_lua_app_restart
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/server_restart.cmake")
  set_tests_properties(vectis_lua_app_restart PROPERTIES
    LABELS "lua;smoke;local;integration"
    TIMEOUT 30)
  add_test(NAME vectis_lua_http_redirect
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/http_redirect.cmake")
  set_tests_properties(vectis_lua_http_redirect PROPERTIES
    LABELS "lua;smoke;local;integration"
    TIMEOUT 45)
  add_test(NAME vectis_lua_openssl
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/openssl.cmake")
  set_tests_properties(vectis_lua_openssl PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  if((CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin") AND VECTIS_WITH_KORE_RUNTIME)
    add_executable(vectis_lua_http_behavior helpers/lua_http_behavior.c)
    target_link_libraries(vectis_lua_http_behavior PRIVATE ${vectis_test_link_target})
    vectis_configure_c89_target(vectis_lua_http_behavior)
    add_test(NAME vectis_lua_app_source_lifetime
      COMMAND "${CMAKE_COMMAND}" "-DHTTP_TEST=$<TARGET_FILE:vectis_lua_http_behavior>"
        "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>" "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/app_source_lifetime.lua"
        -DMODE=source "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/app-source-lifetime"
        -P "${CMAKE_SOURCE_DIR}/tests/lua/http_behavior.cmake")
    set_tests_properties(vectis_lua_app_source_lifetime PROPERTIES
      LABELS "lua;local;integration;packed;failure-fast" TIMEOUT 45 RUN_SERIAL TRUE)
    add_test(NAME vectis_lua_fork_exit_failure
      COMMAND "${CMAKE_COMMAND}" "-DHTTP_TEST=$<TARGET_FILE:vectis_lua_http_behavior>"
        "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>" "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/fork_exit_failure.lua"
        -DMODE=fork-failure "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/fork-exit-failure"
        -P "${CMAKE_SOURCE_DIR}/tests/lua/http_behavior.cmake")
    set_tests_properties(vectis_lua_fork_exit_failure PROPERTIES
      LABELS "lua;local;integration;packed;failure-fast" TIMEOUT 45 RUN_SERIAL TRUE)
    add_test(NAME vectis_lua_supervisor_route
      COMMAND "${CMAKE_COMMAND}" "-DHTTP_TEST=$<TARGET_FILE:vectis_lua_http_behavior>"
        "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>" "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/supervisor_route.lua"
        -DMODE=workflow-direct "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/supervisor-route"
        -P "${CMAKE_SOURCE_DIR}/tests/lua/http_behavior.cmake")
    set_tests_properties(vectis_lua_supervisor_route PROPERTIES
      LABELS "lua;local;integration;packed" TIMEOUT 45 RUN_SERIAL TRUE)
    add_test(NAME vectis_lua_xa_worker_wake
      COMMAND "${CMAKE_COMMAND}" "-DHTTP_TEST=$<TARGET_FILE:vectis_lua_http_behavior>"
        "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>" "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/supervisor_route.lua"
        -DMODE=workflow-xa "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/xa-worker-wake"
        -P "${CMAKE_SOURCE_DIR}/tests/lua/http_behavior.cmake")
    set_tests_properties(vectis_lua_xa_worker_wake PROPERTIES
      LABELS "lua;local;integration;packed;failure-fast" TIMEOUT 30 RUN_SERIAL TRUE)
  endif()
  add_test(NAME vectis_lua_supervisor_workflow
    COMMAND "${CMAKE_COMMAND}"
      "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/supervisor_workflow.cmake")
  set_tests_properties(vectis_lua_supervisor_workflow PROPERTIES
    LABELS "lua;local;integration;packed" TIMEOUT 60 RUN_SERIAL TRUE)
  add_test(NAME vectis_example_lua_supervisor_workflow
    COMMAND "${CMAKE_COMMAND}"
      "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/supervisor-example"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/examples/lua/supervisor_workflow.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/supervisor_workflow.cmake")
  set_tests_properties(vectis_example_lua_supervisor_workflow PROPERTIES
    LABELS "lua;local;integration;packed;example" TIMEOUT 60 RUN_SERIAL TRUE)
  add_test(NAME vectis_lua_supervisor_upstream_stateful
    COMMAND "${CMAKE_COMMAND}"
      "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/supervisor-upstream"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/supervisor_upstream_stateful.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/supervisor_workflow.cmake")
  set_tests_properties(vectis_lua_supervisor_upstream_stateful PROPERTIES
    LABELS "lua;local;integration;packed;upstream-contract;failure-fast" TIMEOUT 60 RUN_SERIAL TRUE)
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
  add_test(NAME vectis_lua_browser_sessions
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/browser_sessions.cmake")
  set_tests_properties(vectis_lua_browser_sessions PROPERTIES
    LABELS "lua;local;integration"
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
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/http.cmake")
  set_tests_properties(vectis_lua_http PROPERTIES
    LABELS "lua;smoke;local"
    TIMEOUT 30)
  add_executable(vectis_mqtt_wire helpers/mqtt_wire.c)
  vectis_configure_c89_target(vectis_mqtt_wire)
  add_test(NAME vectis_lua_mqtt
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DMQTT_WIRE=$<TARGET_FILE:vectis_mqtt_wire>"
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
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
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
  add_test(NAME vectis_cli_self_describe
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_UPSTREAM_LONEJSON=${vectis_lonejson_lua_source_dir}/lua/lonejson/init.lua"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/self_describe.cmake")
  set_tests_properties(vectis_cli_self_describe PROPERTIES
    LABELS "cli;smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_lua_sus_audio_live
           COMMAND "${CMAKE_COMMAND}" -E env
                   "VECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   bash "${CMAKE_SOURCE_DIR}/scripts/test-live-sus-audio.sh")
  set_tests_properties(vectis_lua_sus_audio_live PROPERTIES
    LABELS "lua;live"
    TIMEOUT 300)
  if(APPLE)
    add_test(NAME vectis_cli_pack_unsupported
             COMMAND "${CMAKE_COMMAND}"
                     "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                     "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                     -P "${CMAKE_SOURCE_DIR}/tests/lua/pack_unsupported.cmake")
    set_tests_properties(vectis_cli_pack_unsupported PROPERTIES
      LABELS "cli;local"
      TIMEOUT 30)
  else()
    add_test(NAME vectis_cli_unpack
             COMMAND "${CMAKE_COMMAND}"
                     "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                     "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                     -P "${CMAKE_SOURCE_DIR}/tests/lua/unpack.cmake")
    set_tests_properties(vectis_cli_unpack PROPERTIES
      LABELS "cli;smoke;local"
      TIMEOUT 30)
    add_test(NAME vectis_lua_cli
             COMMAND "${CMAKE_COMMAND}"
                     "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                     "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                     -P "${CMAKE_SOURCE_DIR}/tests/lua/cli.cmake")
    set_tests_properties(vectis_lua_cli PROPERTIES
      LABELS "cli;lua;smoke;local"
      TIMEOUT 30)
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
  endif()
  add_test(NAME vectis_cli_admin_credentials
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/admin_credentials.cmake")
  set_tests_properties(vectis_cli_admin_credentials PROPERTIES
    LABELS "cli;local"
    TIMEOUT 30)
  add_test(NAME vectis_cli_trace COMMAND "${CMAKE_COMMAND}"
    "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/trace"
    -P "${CMAKE_SOURCE_DIR}/tests/lua/trace.cmake")
  set_tests_properties(vectis_cli_trace PROPERTIES
    LABELS "cli;lua;local;failure-fast" TIMEOUT 5)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME vectis_cli_trace_packed COMMAND "${CMAKE_COMMAND}"
      "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>" -DPACKED=ON
      "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/trace-packed"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/trace.cmake")
    set_tests_properties(vectis_cli_trace_packed PROPERTIES
      LABELS "cli;lua;local;packed;failure-fast" TIMEOUT 5)
  endif()
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
  add_test(NAME vectis_example_lua_curl_worker_service
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_SCRIPT=${CMAKE_SOURCE_DIR}/examples/lua/curl_worker_service.lua"
                   "-DVECTIS_PORT_ENV=VECTIS_LUA_CURL_WORKER_EXAMPLE_PORT"
                   "-DVECTIS_SUCCESS_MARKER=lua curl worker service example ok"
                   "-DVECTIS_LABEL=Lua curl worker service example"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/run_example_with_port.cmake")
  set_tests_properties(vectis_example_lua_curl_worker_service PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_cai_worker_service
           COMMAND "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/cai_worker_service.lua")
  set_tests_properties(vectis_example_lua_cai_worker_service PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_cai_live_service_adapter
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_SCRIPT=${CMAKE_SOURCE_DIR}/examples/lua/cai_live_service_adapter.lua"
                   "-DVECTIS_PORT_ENV=VECTIS_LUA_CAI_ADAPTER_EXAMPLE_PORT"
                   "-DVECTIS_SUCCESS_MARKER=lua cai live service adapter example ok"
                   "-DVECTIS_LABEL=Lua cai live service adapter example"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/run_example_with_port.cmake")
  set_tests_properties(vectis_example_lua_cai_live_service_adapter PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_audio_worker_service
           COMMAND "${CMAKE_COMMAND}" -E env
                   "VECTIS_LUA_AUDIO_WORKER_EXAMPLE_PATH=${CMAKE_CURRENT_BINARY_DIR}/audio-worker-example.wav"
                   "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/audio_worker_service.lua")
  set_tests_properties(vectis_example_lua_audio_worker_service PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_sus_worker_service
           COMMAND "$<TARGET_FILE:vectis_bin>"
                   "${CMAKE_SOURCE_DIR}/examples/lua/sus_worker_service.lua")
  set_tests_properties(vectis_example_lua_sus_worker_service PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  add_test(NAME vectis_example_lua_cai_worker_service_supervised
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_SCRIPT=${CMAKE_SOURCE_DIR}/examples/lua/cai_worker_service.lua"
                   "-DVECTIS_PORT_ENV=VECTIS_LUA_CAI_WORKER_EXAMPLE_PORT"
                   "-DVECTIS_SUCCESS_MARKER=lua cai worker service example ok"
                   "-DVECTIS_LABEL=Lua cai worker service example"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/run_example_with_port.cmake")
  set_tests_properties(vectis_example_lua_cai_worker_service_supervised
    PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  if(NOT APPLE)
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
                     "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                     "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                     "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                     -P "${CMAKE_SOURCE_DIR}/tests/lua/example_local_facades_pack.cmake")
    set_tests_properties(vectis_example_lua_local_facades_pack PROPERTIES
      LABELS "lua;example-smoke;local"
      TIMEOUT 30)
  endif()
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
  if(NOT APPLE)
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
  endif()
  add_test(NAME vectis_example_lua_api_server
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PORT_HELPER=$<TARGET_FILE:vectis_pick_port>"
                   "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                   "-DVECTIS_SCRIPT=${CMAKE_SOURCE_DIR}/examples/lua/api_server.lua"
                   "-DVECTIS_PORT_ENV=VECTIS_LUA_API_EXAMPLE_PORT"
                   "-DVECTIS_SUCCESS_MARKER=lua api app example ok"
                   "-DVECTIS_LABEL=Lua API server example"
                   "-DVECTIS_EXTRA_ENV=VECTIS_LUA_API_EXAMPLE_LOCKD_PATH=${CMAKE_CURRENT_BINARY_DIR}/vectis-lua-api-example-lockd"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/run_example_with_port.cmake")
  set_tests_properties(vectis_example_lua_api_server PROPERTIES
    LABELS "lua;example-smoke;local"
    TIMEOUT 30)
  if(NOT APPLE)
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
  endif()
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
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/example_metrics_authenticated.cmake")
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
  if(NOT APPLE)
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
endif()

if(VECTIS_BUILD_BINARY)
  set_property(TEST vectis_lua_supervisor_workflow
    APPEND PROPERTY LABELS failure-fast)
  if(VECTIS_WITH_KORE_RUNTIME)
    set_property(TEST vectis_lua_supervisor_route APPEND PROPERTY LABELS failure-fast)
  endif()
  # Include the actual CLI source and reuse its declared closure for deterministic
  # native-destruction instrumentation; no alternate callback implementation.
  set(app_source_unit_sources ${vectis_binary_runner_sources})
  list(REMOVE_ITEM app_source_unit_sources src/vectis_cli.c src/vectis_main.c)
  list(TRANSFORM app_source_unit_sources PREPEND "${CMAKE_SOURCE_DIR}/"
    REGEX "^src/")
  add_executable(vectis_app_source_lifetime
    unit/test_vectis_app_source_lifetime.c ${app_source_unit_sources})
  foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES LINK_OPTIONS)
    get_target_property(app_source_unit_property vectis_bin ${property})
    if(app_source_unit_property)
      set_property(TARGET vectis_app_source_lifetime PROPERTY ${property} "${app_source_unit_property}")
    endif()
  endforeach()
  set_target_properties(vectis_app_source_lifetime PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_app_source_lifetime PRIVATE -UNDEBUG)
  endif()
  add_test(NAME vectis_app_source_lifetime COMMAND vectis_app_source_lifetime)
  set_tests_properties(vectis_app_source_lifetime PROPERTIES
    LABELS "unit;lua;local;failure-fast" TIMEOUT 15)
  add_custom_target(vectis_failures_fast DEPENDS vectis_bin vectis_unit_supervisor
    vectis_unit_lua_trace_observer vectis_unit_pg vectis_unit_storage
    vectis_unit_pg_lua_gc vectis_unit_digest vectis_unit_buffer
    vectis_unit_zlib_stream vectis_unit_zlib_raw vectis_unit_zlib_back
    vectis_unit_zlib_raw_lua_oom vectis_unit_mdf_failure
    vectis_smith_cli_stream
    vectis_opcua_lua_cleanup vectis_sus_lua_lifetime vectis_app_source_lifetime
    vectis_audio_lua_validation)
  foreach(helper vectis_unit_fork_exit vectis_lua_http_behavior vectis_bench_fixture
      vectis_unit_lql_worker vectis_unit_lql_lua_oom
      vectis_unit_sqlite vectis_unit_sqlite_failures vectis_unit_sqlite_lua_oom
      vectis_unit_pdf vectis_unit_pdf_lua_oom vectis_unit_png vectis_unit_png_lua_oom
      vectis_unit_compression vectis_unit_compression_lua_oom
      vectis_unit_crypto vectis_unit_crypto_lua_oom
      vectis_unit_xml_objects vectis_unit_xml_objects_lua_oom
      vectis_unit_curl vectis_unit_curl_lua_oom vectis_curl_http_fixture
      vectis_test_pg_connection_live vectis_test_pg_connection_lua_oom)
    if(TARGET ${helper})
      add_dependencies(vectis_failures_fast ${helper})
    endif()
  endforeach()
endif()

if(VECTIS_BUILD_BINARY)
  add_test(NAME vectis_lua_supervisor_client_options
    COMMAND "${CMAKE_COMMAND}" "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
      "-DVECTIS_SOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DWORK_DIR=${CMAKE_BINARY_DIR}/tests/client-options"
      "-DSCRIPT=${CMAKE_SOURCE_DIR}/tests/lua/supervisor_client_options.lua"
      -P "${CMAKE_SOURCE_DIR}/tests/lua/supervisor_workflow.cmake")
  set_tests_properties(vectis_lua_supervisor_client_options PROPERTIES
    LABELS "lua;local;packed;failure-fast" TIMEOUT 30 RUN_SERIAL TRUE)
endif()
