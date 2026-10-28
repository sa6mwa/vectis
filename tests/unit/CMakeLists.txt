function(vectis_add_unit_test target_name)
  add_executable(${target_name} ${ARGN})
  target_link_libraries(${target_name} PRIVATE ${vectis_test_link_target})
  target_include_directories(${target_name} PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src"
  )
  vectis_configure_c89_target(${target_name})
  if(VECTIS_ENABLE_COVERAGE)
    target_compile_options(${target_name} PRIVATE --coverage -O0 -g)
    target_link_options(${target_name} PRIVATE --coverage)
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target_name} PRIVATE -UNDEBUG)
  endif()
  if(target_name MATCHES "^vectis_unit_kore_(proxy_(curl_loop(_shared)?|curl_pool|direct_io|live|local_live|autoblock_live|overlap_live|unwritten_live|admission_live|h2_memory_live|ws_live|ws_admission_memory_live|go_ws_live|ws_rejection_live|wss_live)|prebody_handoff)$")
    set(kore_test_deadline 15)
    if(target_name STREQUAL "vectis_unit_kore_proxy_curl_loop_shared")
      set(kore_test_deadline 30)
    endif()
    add_test(NAME ${target_name} COMMAND "${CMAKE_COMMAND}"
      "-DEXECUTABLE=$<TARGET_FILE:${target_name}>"
      "-DTIMEOUT_SECONDS=${kore_test_deadline}" -DEXPECTED_EXIT=0
      -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
  else()
    add_test(NAME ${target_name} COMMAND ${target_name})
  endif()
  set_tests_properties(${target_name} PROPERTIES
    LABELS "unit;local"
    TIMEOUT 180)
endfunction()

function(vectis_add_cpp_header_test target_name)
  add_executable(${target_name} ${ARGN})
  target_link_libraries(${target_name} PRIVATE ${vectis_test_link_target})
  target_include_directories(${target_name} PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
  )
  set_target_properties(${target_name} PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target_name} PRIVATE
      -UNDEBUG
      -Wall
      -Wextra
      -Werror
      -Wpedantic
      -Wshadow
      -Wpointer-arith
      -Wcast-qual
      -Wwrite-strings
    )
  endif()
  add_test(NAME ${target_name} COMMAND ${target_name})
  set_tests_properties(${target_name} PROPERTIES
    LABELS "unit;local"
    TIMEOUT 180)
endfunction()

add_executable(vectis_unit_lql_worker test_lql_worker.c)
target_link_libraries(vectis_unit_lql_worker PRIVATE liblql::lql_static Threads::Threads)
vectis_configure_c89_target(vectis_unit_lql_worker)
add_test(NAME vectis_unit_lql_worker COMMAND vectis_unit_lql_worker)
add_test(NAME vectis_unit_lql_worker_small COMMAND vectis_unit_lql_worker small)
set_tests_properties(vectis_unit_lql_worker vectis_unit_lql_worker_small PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 5 RUN_SERIAL TRUE)

add_executable(vectis_unit_webdav_stack_alloc test_webdav_stack_alloc.c)
target_link_libraries(vectis_unit_webdav_stack_alloc PRIVATE ${vectis_test_link_target})
target_include_directories(vectis_unit_webdav_stack_alloc PRIVATE "${CMAKE_SOURCE_DIR}/src")
vectis_configure_c89_target(vectis_unit_webdav_stack_alloc)
add_test(NAME vectis_unit_webdav_stack_alloc COMMAND vectis_unit_webdav_stack_alloc "${CMAKE_CURRENT_BINARY_DIR}/webdav-stack-alloc")
set_tests_properties(vectis_unit_webdav_stack_alloc PROPERTIES LABELS "unit;local;failure-fast;stack" TIMEOUT 15 RUN_SERIAL TRUE)
vectis_add_unit_test(vectis_unit_supervisor test_vectis_supervisor.c)
vectis_add_unit_test(vectis_unit_lua_trace_observer test_lua_trace_observer.c)
target_link_libraries(vectis_unit_lua_trace_observer PRIVATE cpkt::lua_runtime)
set_tests_properties(vectis_unit_lua_trace_observer PROPERTIES
  LABELS "unit;local;failure-fast" TIMEOUT 5)
if(VECTIS_WITH_KORE_RUNTIME AND UNIX)
  vectis_add_unit_test(vectis_unit_fork_exit test_vectis_fork_exit.c)
  set_tests_properties(vectis_unit_fork_exit PROPERTIES LABELS "unit;local;failure-fast;lifecycle" TIMEOUT 10 RUN_SERIAL TRUE)
  add_test(NAME vectis_unit_fork_exit_failure COMMAND vectis_unit_fork_exit failure)
  set_tests_properties(vectis_unit_fork_exit_failure PROPERTIES LABELS "unit;local;failure-fast;lifecycle" TIMEOUT 10 RUN_SERIAL TRUE)
  add_test(NAME vectis_unit_fork_exit_crash COMMAND vectis_unit_fork_exit crash)
  set_tests_properties(vectis_unit_fork_exit_crash PROPERTIES LABELS "unit;local;failure-fast;lifecycle" TIMEOUT 10 RUN_SERIAL TRUE)
  add_test(NAME vectis_unit_fork_exit_respawn COMMAND vectis_unit_fork_exit respawn)
  set_tests_properties(vectis_unit_fork_exit_respawn PROPERTIES LABELS "unit;local;failure-fast;lifecycle" TIMEOUT 10 RUN_SERIAL TRUE)
endif()
vectis_add_unit_test(vectis_unit_pg test_vectis_pg.c)
target_compile_definitions(vectis_unit_pg PRIVATE _POSIX_C_SOURCE=200809L)
set_tests_properties(vectis_unit_pg PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 5)
add_executable(vectis_unit_storage test_vectis_storage.c)
target_link_libraries(vectis_unit_storage PRIVATE ${vectis_test_link_target})
vectis_configure_c89_target(vectis_unit_storage)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(vectis_unit_storage PRIVATE -UNDEBUG)
endif()
add_test(NAME vectis_storage_c_behavior COMMAND "${CMAKE_COMMAND}"
  "-DPROBE=$<TARGET_FILE:vectis_unit_storage>"
  "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/storage-data"
  -P "${CMAKE_SOURCE_DIR}/tests/storage.cmake")
set_tests_properties(vectis_storage_c_behavior PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 6)
if(VECTIS_BUILD_BINARY)
  add_executable(vectis_unit_stack_worker test_vectis_stack_worker.c
    "${CMAKE_SOURCE_DIR}/src/vectis_lua_compression.c"
    "${CMAKE_SOURCE_DIR}/src/vectis_lua_curl.c"
    "${CMAKE_SOURCE_DIR}/src/vectis_lua_buffer.c"
    "${CMAKE_SOURCE_DIR}/src/vectis_lua_zlib_raw.c"
    "${CMAKE_SOURCE_DIR}/src/vectis_lua_zlib_stream.c"
    "${CMAKE_SOURCE_DIR}/src/vectis_lua_gz.c"
    "${CMAKE_SOURCE_DIR}/src/vectis_lua_sqlite.c"
    "${CMAKE_SOURCE_DIR}/src/vectis_lua_pdf.c"
    "${CMAKE_SOURCE_DIR}/src/vectis_lua_png.c"
    "${CMAKE_SOURCE_DIR}/src/vectis_lua_crypto.c"
    "${CMAKE_SOURCE_DIR}/src/vectis_lua_xml_objects.c")
  target_include_directories(vectis_unit_stack_worker PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(vectis_unit_stack_worker PRIVATE ${vectis_test_link_target} cpkt::lua_runtime)
  vectis_configure_c89_target(vectis_unit_stack_worker)
  add_test(NAME vectis_unit_stack_worker COMMAND vectis_unit_stack_worker
    "${CMAKE_SOURCE_DIR}/tests/lua/stack_worker.lua" "${CMAKE_CURRENT_BINARY_DIR}/stack-worker")
  set_tests_properties(vectis_unit_stack_worker PROPERTIES LABELS "unit;lua;local;failure-fast;stack" TIMEOUT 30 RUN_SERIAL TRUE)
  add_executable(vectis_unit_lql_lua_oom test_lql_lua_oom.c $<TARGET_OBJECTS:vectis_liblql_lua>)
  target_link_libraries(vectis_unit_lql_lua_oom PRIVATE liblql::lql_static cpkt::lua_runtime)
  vectis_configure_c89_target(vectis_unit_lql_lua_oom)
  add_test(NAME vectis_unit_lql_lua_oom COMMAND vectis_unit_lql_lua_oom "${CMAKE_SOURCE_DIR}/tests/lua/lql_oom.lua")
  set_tests_properties(vectis_unit_lql_lua_oom PROPERTIES LABELS "unit;lua;local;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_executable(vectis_unit_xml_objects_lua_oom test_xml_objects_lua_oom.c "${CMAKE_SOURCE_DIR}/src/vectis_lua_xml_objects.c")
  target_include_directories(vectis_unit_xml_objects_lua_oom PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/include")
  target_link_libraries(vectis_unit_xml_objects_lua_oom PRIVATE ${vectis_test_link_target} cpkt::lua_runtime)
  vectis_configure_c89_target(vectis_unit_xml_objects_lua_oom)
  add_test(NAME vectis_unit_xml_objects_lua_oom COMMAND vectis_unit_xml_objects_lua_oom "${CMAKE_SOURCE_DIR}/tests/lua/xml_objects_oom.lua")
  set_tests_properties(vectis_unit_xml_objects_lua_oom PROPERTIES LABELS "unit;lua;local;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_executable(vectis_unit_curl_lua_oom test_curl_lua_oom.c)
  target_include_directories(vectis_unit_curl_lua_oom PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/include")
  target_link_libraries(vectis_unit_curl_lua_oom PRIVATE ${vectis_test_link_target} cpkt::lua_runtime)
  vectis_configure_c89_target(vectis_unit_curl_lua_oom)
  add_test(NAME vectis_unit_curl_lua_oom COMMAND vectis_unit_curl_lua_oom "${CMAKE_SOURCE_DIR}/tests/lua/curl_handles_oom.lua" "${CMAKE_SOURCE_DIR}/tests/lua/curl_completions_oom.lua" "${CMAKE_CURRENT_BINARY_DIR}/curl-oom-output.bin")
  set_tests_properties(vectis_unit_curl_lua_oom PROPERTIES LABELS "unit;lua;local;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_executable(vectis_unit_pdf_lua_oom test_pdf_lua_oom.c "${CMAKE_SOURCE_DIR}/src/vectis_lua_pdf.c")
  add_executable(vectis_unit_crypto_lua_oom test_crypto_lua_oom.c "${CMAKE_SOURCE_DIR}/src/vectis_lua_crypto.c")
  target_include_directories(vectis_unit_crypto_lua_oom PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/include")
  target_link_libraries(vectis_unit_crypto_lua_oom PRIVATE ${vectis_test_link_target} cpkt::lua_runtime)
  vectis_configure_c89_target(vectis_unit_crypto_lua_oom)
  add_test(NAME vectis_unit_crypto_lua_oom COMMAND vectis_unit_crypto_lua_oom "${CMAKE_SOURCE_DIR}/tests/lua/crypto_oom.lua")
  set_tests_properties(vectis_unit_crypto_lua_oom PROPERTIES LABELS "unit;lua;local;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_executable(vectis_test_pg_connection_lua_oom test_pg_connection_lua_oom.c)
  target_include_directories(vectis_test_pg_connection_lua_oom PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/include")
  target_link_libraries(vectis_test_pg_connection_lua_oom PRIVATE ${vectis_test_link_target} cpkt::lua_runtime)
  vectis_configure_c89_target(vectis_test_pg_connection_lua_oom)
  add_executable(vectis_unit_png_lua_oom test_png_lua_oom.c "${CMAKE_SOURCE_DIR}/src/vectis_lua_png.c")
  target_include_directories(vectis_unit_png_lua_oom PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/include")
  target_link_libraries(vectis_unit_png_lua_oom PRIVATE ${vectis_test_link_target} cpkt::lua_runtime)
  vectis_configure_c89_target(vectis_unit_png_lua_oom)
  add_test(NAME vectis_unit_png_lua_oom COMMAND vectis_unit_png_lua_oom "${CMAKE_SOURCE_DIR}/tests/lua/png_oom.lua")
  set_tests_properties(vectis_unit_png_lua_oom PROPERTIES LABELS "unit;lua;local;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_executable(vectis_unit_compression_lua_oom test_compression_lua_oom.c "${CMAKE_SOURCE_DIR}/src/vectis_lua_compression.c")
  target_include_directories(vectis_unit_compression_lua_oom PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/include")
  target_link_libraries(vectis_unit_compression_lua_oom PRIVATE ${vectis_test_link_target} cpkt::lua_runtime)
  vectis_configure_c89_target(vectis_unit_compression_lua_oom)
  add_test(NAME vectis_unit_compression_lua_oom COMMAND vectis_unit_compression_lua_oom "${CMAKE_SOURCE_DIR}/tests/lua/compression_oom.lua")
  set_tests_properties(vectis_unit_compression_lua_oom PROPERTIES LABELS "unit;lua;local;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  target_include_directories(vectis_unit_pdf_lua_oom PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/include")
  target_link_libraries(vectis_unit_pdf_lua_oom PRIVATE ${vectis_test_link_target} cpkt::lua_runtime)
  vectis_configure_c89_target(vectis_unit_pdf_lua_oom)
  add_test(NAME vectis_unit_pdf_lua_oom COMMAND vectis_unit_pdf_lua_oom
    "${CMAKE_SOURCE_DIR}/tests/lua/pdf_oom.lua" "${CMAKE_SOURCE_DIR}/tests/fixtures/pdf")
  set_tests_properties(vectis_unit_pdf_lua_oom PROPERTIES LABELS "unit;lua;local;failure-fast" TIMEOUT 10 RUN_SERIAL TRUE)
  add_executable(vectis_unit_sqlite_lua_oom test_sqlite_lua_oom.c)
  target_include_directories(vectis_unit_sqlite_lua_oom PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/include")
  target_link_libraries(vectis_unit_sqlite_lua_oom PRIVATE ${vectis_test_link_target} cpkt::lua_runtime)
  vectis_configure_c89_target(vectis_unit_sqlite_lua_oom)
  add_test(NAME vectis_unit_sqlite_lua_oom COMMAND vectis_unit_sqlite_lua_oom "${CMAKE_SOURCE_DIR}/tests/lua/sqlite_oom.lua")
  set_tests_properties(vectis_unit_sqlite_lua_oom PROPERTIES LABELS "unit;lua;local;failure-fast" TIMEOUT 15 RUN_SERIAL TRUE)
  add_executable(vectis_unit_pg_lua_gc test_pg_lua_gc.c "${CMAKE_SOURCE_DIR}/src/vectis_lua_pg.c")
  target_include_directories(vectis_unit_pg_lua_gc PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/include" "${vectis_lockdc_lua_source_dir}/src/lua")
  target_link_libraries(vectis_unit_pg_lua_gc PRIVATE vectis_external_headers ${vectis_test_link_target} cpkt::lua_runtime)
  vectis_configure_c89_target(vectis_unit_pg_lua_gc)
  target_compile_options(vectis_unit_pg_lua_gc PRIVATE -UNDEBUG)
  add_test(NAME vectis_unit_pg_lua_gc COMMAND vectis_unit_pg_lua_gc "${CMAKE_SOURCE_DIR}/tests/lua/postgres_gc.lua")
  set_tests_properties(vectis_unit_pg_lua_gc PROPERTIES LABELS "unit;local;lua;failure-fast" TIMEOUT 5)
endif()
vectis_add_unit_test(vectis_unit_config test_vectis_config.c)
vectis_add_unit_test(vectis_unit_digest test_vectis_digest.c)
vectis_add_unit_test(vectis_unit_sqlite test_vectis_sqlite.c)
vectis_add_unit_test(vectis_unit_pdf test_vectis_pdf.c)
vectis_add_unit_test(vectis_unit_curl test_vectis_curl.c)
target_compile_definitions(vectis_unit_curl PRIVATE VECTIS_CURL_FIXTURE_PATH="${CMAKE_CURRENT_BINARY_DIR}/curl-native-input.bin" VECTIS_CURL_OUTPUT_PATH="${CMAKE_CURRENT_BINARY_DIR}/curl-native-output.bin")
set_tests_properties(vectis_unit_curl PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 10 RUN_SERIAL TRUE)
vectis_add_unit_test(vectis_unit_xml_objects test_vectis_xml_objects.c)
set_tests_properties(vectis_unit_xml_objects PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 10)
vectis_add_unit_test(vectis_unit_crypto test_vectis_crypto.c)
target_compile_definitions(vectis_unit_crypto PRIVATE VECTIS_CRYPTO_KEY_PATH="${CMAKE_CURRENT_BINARY_DIR}/crypto-key-fixture.pem")
set_tests_properties(vectis_unit_crypto PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 10)
add_executable(vectis_test_pg_connection_live test_pg_connection_live.c)
target_include_directories(vectis_test_pg_connection_live PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(vectis_test_pg_connection_live PRIVATE ${vectis_test_link_target})
vectis_configure_c89_target(vectis_test_pg_connection_live)
vectis_add_unit_test(vectis_unit_png test_vectis_png.c)
target_compile_definitions(vectis_unit_png PRIVATE VECTIS_PNG_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}/png-native.png")
set_tests_properties(vectis_unit_png PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 5)
vectis_add_unit_test(vectis_unit_compression test_vectis_compression.c)
set_tests_properties(vectis_unit_compression PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 10)
target_compile_definitions(vectis_unit_pdf PRIVATE
  VECTIS_PDF_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/pdf"
  VECTIS_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}")
set_tests_properties(vectis_unit_pdf PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 5)
vectis_add_unit_test(vectis_unit_sqlite_failures test_sqlite_failures.c)
set_tests_properties(vectis_unit_sqlite_failures PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 5)
set_tests_properties(vectis_unit_sqlite PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 5)
vectis_add_unit_test(vectis_unit_buffer test_vectis_buffer.c)
set_tests_properties(vectis_unit_buffer PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 5)
vectis_add_unit_test(vectis_unit_zlib_stream test_vectis_zlib_stream.c
  "${CMAKE_SOURCE_DIR}/src/vectis_lua_zlib_stream.c")
vectis_add_unit_test(vectis_unit_zlib_raw test_vectis_zlib_raw.c)
target_link_libraries(vectis_unit_zlib_raw PRIVATE ZLIB::ZLIB)
set_tests_properties(vectis_unit_zlib_raw PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 5)
vectis_add_unit_test(vectis_unit_zlib_back test_vectis_zlib_back.c)
target_link_libraries(vectis_unit_zlib_back PRIVATE ZLIB::ZLIB)
set_tests_properties(vectis_unit_zlib_back PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 5)
vectis_add_unit_test(vectis_unit_zlib_raw_lua_oom test_zlib_raw_lua_oom.c
  "${CMAKE_SOURCE_DIR}/src/vectis_lua_zlib_raw.c"
  "${CMAKE_SOURCE_DIR}/src/vectis_lua_buffer.c")
target_link_libraries(vectis_unit_zlib_raw_lua_oom PRIVATE ZLIB::ZLIB cpkt::lua_runtime)
set_tests_properties(vectis_unit_zlib_raw_lua_oom PROPERTIES LABELS "unit;local;failure-fast" TIMEOUT 5)
target_link_libraries(vectis_unit_zlib_stream PRIVATE ZLIB::ZLIB cpkt::lua_runtime)
set_tests_properties(vectis_unit_zlib_stream PROPERTIES
  LABELS "unit;local;failure-fast" TIMEOUT 5)
target_compile_definitions(vectis_unit_digest PRIVATE
  VECTIS_DIGEST_TEST_ROOT="${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(vectis_unit_digest PROPERTIES
  LABELS "unit;local;failure-fast" TIMEOUT 5)
vectis_add_unit_test(vectis_unit_mdf_failure test_vectis_mdf_failure.c)
target_link_libraries(vectis_unit_mdf_failure PRIVATE libmdf::mdf_static)
set_tests_properties(vectis_unit_mdf_failure PROPERTIES
  LABELS "unit;local;failure-fast" TIMEOUT 5)
vectis_add_unit_test(vectis_unit_proxy_route_selection
  test_proxy_route_selection.c)
vectis_add_unit_test(vectis_unit_proxy_url test_proxy_url.c)
vectis_add_unit_test(vectis_unit_proxy_curl_capabilities
  test_proxy_curl_capabilities.c)
if(VECTIS_WITH_KORE_RUNTIME)
  if(APPLE)
    vectis_add_unit_test(vectis_unit_kore_quiescence_darwin
      test_kore_quiescence_darwin.c)
  endif()
  vectis_add_unit_test(vectis_unit_kore_response_write_timeout
    test_kore_response_write_timeout.c)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME vectis_unit_forked_kore_sanitizer
      COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/test_forked_kore_sanitizer.sh"
        "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_forked_kore_sanitizer PROPERTIES
      LABELS "unit;local" TIMEOUT 15)
  endif()
  vectis_add_unit_test(vectis_unit_proxy_framing test_proxy_framing.c)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Compile the BSD branch of the C89 adapter with a minimal event.h shim.
    # This checks interest translation; native kqueue execution is separate.
    add_executable(vectis_unit_proxy_events_kqueue_sim
      test_proxy_events_kqueue_sim.c
      "${CMAKE_SOURCE_DIR}/src/vectis_proxy_events.c")
    target_include_directories(vectis_unit_proxy_events_kqueue_sim PRIVATE
      "${CMAKE_SOURCE_DIR}/src"
      "${CMAKE_SOURCE_DIR}/tests/fixtures/kqueue")
    target_compile_options(vectis_unit_proxy_events_kqueue_sim PRIVATE
      -U__linux__ -UNDEBUG)
    vectis_configure_c89_target(vectis_unit_proxy_events_kqueue_sim)
    add_test(NAME vectis_unit_proxy_events_kqueue_sim
      COMMAND vectis_unit_proxy_events_kqueue_sim)
    set_tests_properties(vectis_unit_proxy_events_kqueue_sim PROPERTIES
      LABELS "unit;local;proxy" TIMEOUT 5)
  endif()
  vectis_add_unit_test(vectis_unit_proxy_headers test_proxy_headers.c)
  vectis_add_unit_test(vectis_unit_proxy_http test_proxy_http.c)
  vectis_add_unit_test(vectis_unit_proxy_http_upstream
    test_proxy_http_upstream.c)
  vectis_add_unit_test(vectis_unit_proxy_http_wire test_proxy_http_wire.c)
  vectis_add_unit_test(vectis_unit_proxy_upload test_proxy_upload.c)
  vectis_add_unit_test(vectis_unit_proxy_ws_handshake
    test_proxy_ws_handshake.c)
  vectis_add_unit_test(vectis_unit_proxy_ws_wire test_proxy_ws_wire.c)
  vectis_add_unit_test(vectis_unit_proxy_ws_rejection
    test_proxy_ws_rejection.c)
  vectis_add_unit_test(vectis_unit_kore_proxy_live test_kore_proxy_live.c)
  vectis_add_unit_test(vectis_unit_kore_proxy_autoblock_live
    test_kore_proxy_autoblock_live.c)
  vectis_add_unit_test(vectis_unit_kore_proxy_local_live
    test_kore_proxy_local_live.c)
  vectis_add_unit_test(vectis_unit_kore_proxy_overlap_live
    test_kore_proxy_overlap_live.c)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    vectis_add_unit_test(vectis_unit_kore_proxy_admission_live
      test_kore_proxy_admission_live.c)
    vectis_add_unit_test(vectis_unit_kore_proxy_h2_memory_live
      test_kore_proxy_h2_memory_live.c)
    vectis_add_unit_test(vectis_unit_kore_proxy_ws_admission_memory_live
      test_kore_proxy_ws_admission_memory_live.c)
    add_test(NAME vectis_unit_kore_proxy_ws_admission_memory_filled
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_WS_FILLED_BUFFER=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_ws_admission_memory_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_ws_admission_memory_filled
      PROPERTIES LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_ws_admission_memory_bidirectional
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_WS_BIDIRECTIONAL=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_ws_admission_memory_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_ws_admission_memory_bidirectional
      PROPERTIES LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_h2_memory_large
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_H2_LARGE_BUFFER=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_h2_memory_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_h2_memory_large PROPERTIES
      LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_h2_mtls_live
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_H2_MTLS=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_h2_memory_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_h2_mtls_live PROPERTIES
      LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_h2_max_config_memory_live
      COMMAND ${CMAKE_COMMAND} -E env
        VECTIS_PROXY_H2_MTLS_LARGE_IDENTITY=1
        VECTIS_PROXY_H2_LARGE_BUFFER=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_h2_memory_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_h2_max_config_memory_live
      PROPERTIES LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_idle_cache_memory_live
      COMMAND ${CMAKE_COMMAND} -E env
        VECTIS_PROXY_H2_MTLS_LARGE_IDENTITY=1
        VECTIS_PROXY_H2_LARGE_BUFFER=1
        VECTIS_PROXY_H2_IDLE_CACHE_MEMORY=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_h2_memory_live>"
        -DTIMEOUT_SECONDS=20 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_idle_cache_memory_live
      PROPERTIES LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_h2_upload_memory_live
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_H2_UPLOAD_MEMORY=1
        VECTIS_PROXY_H2_LARGE_BUFFER=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_h2_memory_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_h2_upload_memory_live
      PROPERTIES LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_h2_duplex_memory_live
      COMMAND ${CMAKE_COMMAND} -E env
        VECTIS_PROXY_H2_DUPLEX_MEMORY=1
        VECTIS_PROXY_H2_MTLS_LARGE_IDENTITY=1
        VECTIS_PROXY_H2_LARGE_BUFFER=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_h2_memory_live>"
        -DTIMEOUT_SECONDS=20 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_h2_duplex_memory_live
      PROPERTIES LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_mixed_upload_memory_live
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_MIXED_UPLOAD_MEMORY=1
        VECTIS_PROXY_H2_LARGE_BUFFER=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_h2_memory_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_mixed_upload_memory_live
      PROPERTIES LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_mixed_transfer_memory_live
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_MIXED_TRANSFER_MEMORY=1
        VECTIS_PROXY_H2_LARGE_BUFFER=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_h2_memory_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_mixed_transfer_memory_live
      PROPERTIES LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_mixed_max_config_memory_live
      COMMAND ${CMAKE_COMMAND} -E env
        VECTIS_PROXY_MIXED_TRANSFER_MEMORY=1
        VECTIS_PROXY_H2_MTLS_LARGE_IDENTITY=1
        VECTIS_PROXY_H2_LARGE_BUFFER=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_h2_memory_live>"
        -DTIMEOUT_SECONDS=20 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_mixed_max_config_memory_live
      PROPERTIES LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_mixed_memory_live
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_MIXED_MEMORY=1
        VECTIS_PROXY_H2_LARGE_BUFFER=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_h2_memory_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_mixed_memory_live PROPERTIES
      LABELS "unit;local" TIMEOUT 180)
    vectis_add_unit_test(vectis_unit_kore_proxy_unwritten_live
      test_kore_proxy_unwritten_live.c)
    target_include_directories(vectis_unit_kore_proxy_unwritten_live PRIVATE
      "${CMAKE_SOURCE_DIR}/vendor/kore/upstream/include")
    target_link_options(vectis_unit_kore_proxy_unwritten_live PRIVATE
      -Wl,--wrap=net_send_flush)
  endif()
  vectis_add_unit_test(vectis_unit_kore_proxy_unwritten
    test_kore_proxy_unwritten.c)
  target_include_directories(vectis_unit_kore_proxy_unwritten PRIVATE
    "${CMAKE_SOURCE_DIR}/vendor/kore/upstream/include")
  vectis_add_unit_test(vectis_unit_kore_proxy_ws_live
    test_kore_proxy_ws_live.c)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(vectis_test_go_executable NAMES go)
    if(vectis_test_go_executable)
      set(vectis_test_go_ws_origin
        "${CMAKE_CURRENT_BINARY_DIR}/proxy_go_ws_origin")
      add_custom_command(OUTPUT "${vectis_test_go_ws_origin}"
        COMMAND ${CMAKE_COMMAND} -E env
          "GOCACHE=${CMAKE_CURRENT_BINARY_DIR}/go-cache"
          CGO_ENABLED=0 GOTOOLCHAIN=local
          "${vectis_test_go_executable}" build -trimpath
          -o "${vectis_test_go_ws_origin}"
          "${CMAKE_SOURCE_DIR}/tests/fixtures/proxy_go_ws_origin.go"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/fixtures/proxy_go_ws_origin.go"
        VERBATIM)
      add_custom_target(vectis_test_go_ws_origin
        DEPENDS "${vectis_test_go_ws_origin}")
      vectis_add_unit_test(vectis_unit_kore_proxy_go_ws_live
        test_kore_proxy_go_ws_live.c)
      add_dependencies(vectis_unit_kore_proxy_go_ws_live
        vectis_test_go_ws_origin)
      target_compile_definitions(vectis_unit_kore_proxy_go_ws_live PRIVATE
        VECTIS_TEST_GO_WS_ORIGIN="${vectis_test_go_ws_origin}")
    endif()
  endif()
  vectis_add_unit_test(vectis_unit_kore_proxy_ws_rejection_live
    test_kore_proxy_ws_rejection_live.c)
  vectis_add_unit_test(vectis_unit_kore_proxy_wss_live
    test_kore_proxy_wss_live.c)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(vectis_unit_kore_proxy_wss_live PRIVATE
      -Wl,--wrap=curl_easy_recv -Wl,--wrap=curl_easy_send)
    add_test(NAME vectis_unit_kore_proxy_wss_mtls_live
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_WSS_MTLS=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_wss_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_wss_mtls_live PROPERTIES
      LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_wss_retry_live
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_WSS_MTLS=1
        VECTIS_PROXY_WSS_RETRY=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_wss_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_wss_retry_live PROPERTIES
      LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_wss_slow_live
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_WSS_MTLS=1
        VECTIS_PROXY_WSS_SLOW=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_wss_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_wss_slow_live PROPERTIES
      LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_wss_cancel_live
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_WSS_MTLS=1
        VECTIS_PROXY_WSS_SLOW=1 VECTIS_PROXY_WSS_CANCEL=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_wss_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_wss_cancel_live PROPERTIES
      LABELS "unit;local" TIMEOUT 180)
    add_test(NAME vectis_unit_kore_proxy_wss_shutdown_live
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_PROXY_WSS_MTLS=1
        VECTIS_PROXY_WSS_SLOW=1 VECTIS_PROXY_WSS_CANCEL=1
        VECTIS_PROXY_WSS_SHUTDOWN=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_wss_live>"
        -DTIMEOUT_SECONDS=15 -DEXPECTED_EXIT=0
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_wss_shutdown_live PROPERTIES
      LABELS "unit;local" TIMEOUT 180)
  endif()
  vectis_add_unit_test(vectis_unit_proxy_route_registration
    test_proxy_route_registration.c)
  vectis_add_unit_test(vectis_unit_proxy_director
    test_proxy_director.c)
  vectis_add_unit_test(vectis_unit_proxy_response
    test_proxy_response.c)
  vectis_add_unit_test(vectis_unit_proxy_local test_proxy_local.c)
  vectis_add_unit_test(vectis_unit_proxy_select test_proxy_select.c)
  vectis_add_unit_test(vectis_unit_proxy_telemetry test_proxy_telemetry.c)
else()
  vectis_add_unit_test(vectis_unit_proxy_no_kore test_proxy_no_kore.c)
endif()
vectis_add_unit_test(vectis_unit_acme_state test_vectis_acme_state.c)
vectis_add_unit_test(vectis_unit_cai test_vectis_cai.c)
vectis_add_unit_test(vectis_unit_smith test_vectis_smith.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(vectis_unit_smith PRIVATE VECTIS_TEST_SMITH_FAULTS=1)
  target_link_options(vectis_unit_smith PRIVATE -Wl,--wrap=lc_acquire -Wl,--wrap=malloc -Wl,--wrap=free)
endif()
vectis_add_unit_test(vectis_unit_audio_worker test_vectis_audio_worker.c)
vectis_add_unit_test(vectis_unit_sus_worker test_vectis_sus_worker.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(vectis_unit_sus_worker PRIVATE
    VECTIS_TEST_WRAP_SUS=1)
  target_link_options(vectis_unit_sus_worker PRIVATE
    -Wl,--wrap=cpkt_sus_open_path
    -Wl,--wrap=cpkt_sus_open_cached
    -Wl,--wrap=cpkt_sus_string_free)
endif()
vectis_add_unit_test(vectis_unit_certs test_vectis_certs.c)
vectis_add_unit_test(vectis_unit_auth test_vectis_auth.c)
vectis_add_unit_test(vectis_unit_totp_qr test_vectis_totp_qr.c)
vectis_add_unit_test(vectis_unit_embedded_fs test_vectis_embedded_fs.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(vectis_unit_embedded_fs PRIVATE VECTIS_TEST_WRAP_FCHMOD=1)
  target_link_options(vectis_unit_embedded_fs PRIVATE -Wl,--wrap=fchmod)
endif()
vectis_add_unit_test(vectis_unit_mailbox test_vectis_mailbox.c)
vectis_add_unit_test(vectis_unit_webdav test_vectis_webdav.c)
vectis_add_unit_test(vectis_unit_webdav_atomic test_vectis_webdav_atomic.c)
if(VECTIS_WITH_KORE_RUNTIME)
  vectis_add_unit_test(vectis_unit_proxy_curl_duplex test_proxy_curl_duplex.c)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    vectis_add_unit_test(vectis_unit_kore_proxy_direct_io
      test_kore_proxy_direct_io.c)
    target_include_directories(vectis_unit_kore_proxy_direct_io PRIVATE
      "${CMAKE_SOURCE_DIR}/vendor/kore/upstream/include")
    target_compile_options(vectis_unit_kore_proxy_direct_io PRIVATE -std=c99)
    vectis_add_unit_test(vectis_unit_proxy_curl_socket_handoff
      test_proxy_curl_socket_handoff.c)
    vectis_add_unit_test(vectis_unit_proxy_curl_tls_floor
      test_proxy_curl_tls_floor.c)
    set_tests_properties(vectis_unit_proxy_curl_tls_floor PROPERTIES
      TIMEOUT 10)
    vectis_add_unit_test(vectis_unit_proxy_curl_http2_pause
      test_proxy_curl_http2_pause.c)
    vectis_add_unit_test(vectis_unit_proxy_curl_http2_pause_many
      test_proxy_curl_http2_pause.c)
    target_compile_definitions(vectis_unit_proxy_curl_http2_pause_many PRIVATE
      VECTIS_HTTP2_SCALE_PROBE=1)
    add_test(NAME vectis_unit_proxy_curl_http2_pause_cancel
      COMMAND $<TARGET_FILE:vectis_unit_proxy_curl_http2_pause_many>)
    set_tests_properties(vectis_unit_proxy_curl_http2_pause_cancel PROPERTIES
      ENVIRONMENT "VECTIS_H2_CANCEL_AFTER_PAUSE=1"
      LABELS "unit;local"
      TIMEOUT 180)
    add_test(NAME vectis_unit_proxy_curl_http2_pause_isolated
      COMMAND $<TARGET_FILE:vectis_unit_proxy_curl_http2_pause_many>)
    set_tests_properties(vectis_unit_proxy_curl_http2_pause_isolated PROPERTIES
      ENVIRONMENT "VECTIS_H2_ISOLATED_SERVER=1"
      LABELS "unit;local"
      TIMEOUT 180)
    add_test(NAME vectis_unit_proxy_curl_http2_pause_isolated_cancel
      COMMAND $<TARGET_FILE:vectis_unit_proxy_curl_http2_pause_many>)
    set_tests_properties(vectis_unit_proxy_curl_http2_pause_isolated_cancel
      PROPERTIES
      ENVIRONMENT "VECTIS_H2_ISOLATED_SERVER=1;VECTIS_H2_CANCEL_AFTER_PAUSE=1"
      LABELS "unit;local"
      TIMEOUT 180)
    vectis_add_unit_test(vectis_unit_proxy_curl_http2_duplex
      test_proxy_curl_http2_duplex.c)
    vectis_add_unit_test(vectis_unit_proxy_curl_http2_duplex_unknown
      test_proxy_curl_http2_duplex.c)
    target_compile_definitions(vectis_unit_proxy_curl_http2_duplex_unknown
      PRIVATE VECTIS_HTTP2_UNKNOWN_LENGTH=1)
    vectis_add_unit_test(vectis_unit_proxy_curl_http2_duplex_get
      test_proxy_curl_http2_duplex.c)
    target_compile_definitions(vectis_unit_proxy_curl_http2_duplex_get PRIVATE
      VECTIS_HTTP2_FRAMED_GET=1)
    vectis_add_unit_test(vectis_unit_proxy_curl_http2_duplex_get_unknown
      test_proxy_curl_http2_duplex.c)
    target_compile_definitions(
      vectis_unit_proxy_curl_http2_duplex_get_unknown PRIVATE
      VECTIS_HTTP2_FRAMED_GET=1 VECTIS_HTTP2_UNKNOWN_LENGTH=1)
    vectis_add_unit_test(vectis_unit_proxy_curl_http2_head_upload
      test_proxy_curl_http2_duplex.c)
    target_compile_definitions(vectis_unit_proxy_curl_http2_head_upload PRIVATE
      VECTIS_HTTP2_FRAMED_HEAD=1)
    vectis_add_unit_test(vectis_unit_kore_proxy_curl_loop
      test_kore_proxy_curl_loop.c)
    vectis_add_unit_test(vectis_unit_kore_proxy_curl_pool
      test_proxy_curl_pool.c)
    target_include_directories(vectis_unit_kore_proxy_curl_pool PRIVATE
      "${CMAKE_SOURCE_DIR}/vendor/kore/upstream/include")
    target_compile_options(vectis_unit_kore_proxy_curl_pool PRIVATE -std=c99)
    target_include_directories(vectis_unit_kore_proxy_curl_loop PRIVATE
      "${CMAKE_SOURCE_DIR}/vendor/kore/upstream/include")
    target_compile_options(vectis_unit_kore_proxy_curl_loop PRIVATE -std=c99)
    vectis_add_unit_test(vectis_unit_kore_proxy_curl_loop_shared
      test_kore_proxy_curl_loop.c)
    target_compile_definitions(vectis_unit_kore_proxy_curl_loop_shared PRIVATE
      VECTIS_PROXY_SHARED_MULTI=1)
    target_include_directories(vectis_unit_kore_proxy_curl_loop_shared PRIVATE
      "${CMAKE_SOURCE_DIR}/vendor/kore/upstream/include")
    target_compile_options(vectis_unit_kore_proxy_curl_loop_shared PRIVATE -std=c99)
    add_test(NAME vectis_unit_kore_proxy_curl_loop_failure_cleanup
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_KORE_PROBE_EXIT_AFTER_START=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_curl_loop>"
        -DTIMEOUT_SECONDS=10 -DEXPECTED_EXIT=71
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_curl_loop_failure_cleanup
      PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 20)
    add_test(NAME vectis_unit_kore_proxy_curl_loop_shared_failure_cleanup
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_KORE_PROBE_EXIT_AFTER_START=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_proxy_curl_loop_shared>"
        -DTIMEOUT_SECONDS=10 -DEXPECTED_EXIT=71
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_proxy_curl_loop_shared_failure_cleanup
      PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 20)
  endif()
  vectis_add_unit_test(vectis_unit_kore_prebody_handoff
    test_kore_prebody_handoff.c)
  target_compile_definitions(vectis_unit_kore_prebody_handoff PRIVATE
    KORE_USE_CURL)
  target_include_directories(vectis_unit_kore_prebody_handoff PRIVATE
    "${CMAKE_SOURCE_DIR}/vendor/kore/upstream/include")
  target_compile_options(vectis_unit_kore_prebody_handoff PRIVATE -std=c99)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME vectis_unit_kore_prebody_handoff_failure_cleanup
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_KORE_PREBODY_EXIT_AFTER_START=1
        "${CMAKE_COMMAND}" "-DEXECUTABLE=$<TARGET_FILE:vectis_unit_kore_prebody_handoff>"
        -DTIMEOUT_SECONDS=10 -DEXPECTED_EXIT=71
        -P "${CMAKE_SOURCE_DIR}/tests/run_kore.cmake")
    set_tests_properties(vectis_unit_kore_prebody_handoff_failure_cleanup
      PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 20)
  endif()
  vectis_add_unit_test(vectis_unit_proxy_curl_connect_only
    test_proxy_curl_connect_only.c)
  vectis_add_unit_test(vectis_unit_proxy_curl_connect_only_tls
    test_proxy_curl_connect_only_tls.c)
  vectis_add_unit_test(vectis_unit_proxy_curl_ws_raw
    test_proxy_curl_ws_raw.c)
  vectis_add_unit_test(vectis_unit_proxy_ws_rejection_framing
    test_proxy_ws_rejection_framing.c)
  vectis_add_unit_test(vectis_unit_proxy_curl_chunked_trailers
    test_proxy_curl_chunked_trailers.c)
  vectis_add_unit_test(vectis_unit_kore_pipelining test_kore_pipelining.c)
  vectis_add_unit_test(vectis_unit_https_runtime test_vectis_https_runtime.c)
  vectis_add_unit_test(vectis_unit_https_mtls_runtime test_vectis_https_mtls_runtime.c)
  vectis_add_unit_test(vectis_unit_runtime test_vectis_runtime.c)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.27")
    set_tests_properties(vectis_unit_runtime PROPERTIES
      TIMEOUT_SIGNAL_NAME SIGTERM
      TIMEOUT_SIGNAL_GRACE_PERIOD 5)
  endif()
  vectis_add_unit_test(vectis_unit_runtime_header_limit
    test_vectis_runtime.c)
  target_compile_definitions(vectis_unit_runtime_header_limit PRIVATE
    VECTIS_RUNTIME_HEADER_LIMIT_ONLY)
  # This one-case executable shares the runtime test source; the other static
  # cases are intentionally not linked from its reduced main function.
  target_compile_options(vectis_unit_runtime_header_limit PRIVATE
    -Wno-unused-function
    -Wno-unused-variable)
  vectis_add_unit_test(vectis_unit_sdk_surface test_vectis_sdk_surface.c)
else()
  message(STATUS "Kore runtime disabled; skipping Kore runtime unit tests")
  message(STATUS "Kore runtime disabled; skipping SDK surface runtime integration unit test")
endif()

include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
  enable_language(CXX)
  vectis_add_cpp_header_test(vectis_unit_header_cpp test_vectis_header_cpp.cpp)
else()
  message(STATUS "C++ compiler not available; skipping vectis C++ header smoke test")
endif()

if(VECTIS_TARGET_ID STREQUAL "x86_64-linux-gnu" AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
    AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64")
  add_test(NAME vectis_runtime_contract
    COMMAND "${CMAKE_COMMAND}" "-DBUILD=${CMAKE_BINARY_DIR}"
            -P "${CMAKE_SOURCE_DIR}/scripts/test_runtime_contract.cmake")
  set_tests_properties(vectis_runtime_contract PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 30)
  add_test(NAME vectis_runtime_reconfigure
    COMMAND "${CMAKE_COMMAND}" "-DBUILD=${CMAKE_BINARY_DIR}"
            -P "${CMAKE_SOURCE_DIR}/scripts/test_runtime_reconfigure.cmake")
  set_tests_properties(vectis_runtime_reconfigure PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 30)
  # This harness configures native tools and must retain the host's declared
  # global cache identity instead of treating the product's sandbox as XDG.
  set_tests_properties(vectis_runtime_reconfigure PROPERTIES
    ENVIRONMENT "XDG_CACHE_HOME=$ENV{XDG_CACHE_HOME}")
  add_test(NAME vectis_runtime_mapping
    COMMAND "${CMAKE_COMMAND}" "-DBUILD=${CMAKE_BINARY_DIR}"
            -P "${CMAKE_SOURCE_DIR}/scripts/test_runtime_mapping.cmake")
  set_tests_properties(vectis_runtime_mapping PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 30)
endif()

if(TARGET vectis_unit_supervisor)
  set_property(TEST vectis_unit_supervisor APPEND PROPERTY LABELS failure-fast)
endif()

vectis_add_unit_test(vectis_unit_gz test_vectis_gz.c "${CMAKE_SOURCE_DIR}/src/vectis_lua_gz.c")
target_link_libraries(vectis_unit_gz PRIVATE cpkt::lua_runtime)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tests/gz-unit")
set_tests_properties(vectis_unit_gz PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests/gz-unit")
add_test(NAME vectis_unit_gz_lua_oom COMMAND vectis_unit_gz --oom)
set_tests_properties(vectis_unit_gz_lua_oom PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests/gz-unit" LABELS "unit;lua;local;failure-fast;stack" TIMEOUT 5 RUN_SERIAL TRUE)
