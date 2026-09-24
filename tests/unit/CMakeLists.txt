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
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
      target_name MATCHES "^vectis_unit_kore_(proxy_(curl_loop(_shared)?|direct_io)|prebody_handoff)$")
    set(kore_test_deadline 15)
    if(target_name STREQUAL "vectis_unit_kore_proxy_curl_loop_shared")
      set(kore_test_deadline 30)
    endif()
    add_test(NAME ${target_name} COMMAND bash
      "${CMAKE_SOURCE_DIR}/scripts/run_forked_kore_test.sh"
      ${kore_test_deadline} 0 $<TARGET_FILE:${target_name}>)
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

vectis_add_unit_test(vectis_unit_config test_vectis_config.c)
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
    vectis_add_unit_test(vectis_unit_kore_proxy_curl_loop
      test_kore_proxy_curl_loop.c)
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
        bash "${CMAKE_SOURCE_DIR}/scripts/run_forked_kore_test.sh"
        10 71 $<TARGET_FILE:vectis_unit_kore_proxy_curl_loop>)
    set_tests_properties(vectis_unit_kore_proxy_curl_loop_failure_cleanup
      PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 20)
    add_test(NAME vectis_unit_kore_proxy_curl_loop_timeout_cleanup
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_KORE_PROBE_HANG_AFTER_START=1
        bash "${CMAKE_SOURCE_DIR}/scripts/run_forked_kore_test.sh"
        1 124 $<TARGET_FILE:vectis_unit_kore_proxy_curl_loop>)
    set_tests_properties(vectis_unit_kore_proxy_curl_loop_timeout_cleanup
      PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 10)
    add_test(NAME vectis_unit_kore_proxy_curl_loop_shared_failure_cleanup
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_KORE_PROBE_EXIT_AFTER_START=1
        bash "${CMAKE_SOURCE_DIR}/scripts/run_forked_kore_test.sh"
        10 71 $<TARGET_FILE:vectis_unit_kore_proxy_curl_loop_shared>)
    set_tests_properties(vectis_unit_kore_proxy_curl_loop_shared_failure_cleanup
      PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 20)
    add_test(NAME vectis_unit_kore_proxy_curl_loop_shared_timeout_cleanup
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_KORE_PROBE_HANG_AFTER_START=1
        bash "${CMAKE_SOURCE_DIR}/scripts/run_forked_kore_test.sh"
        1 124 $<TARGET_FILE:vectis_unit_kore_proxy_curl_loop_shared>)
    set_tests_properties(vectis_unit_kore_proxy_curl_loop_shared_timeout_cleanup
      PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 10)
  endif()
  vectis_add_unit_test(vectis_unit_kore_prebody_handoff
    test_kore_prebody_handoff.c)
  target_include_directories(vectis_unit_kore_prebody_handoff PRIVATE
    "${CMAKE_SOURCE_DIR}/vendor/kore/upstream/include")
  target_compile_options(vectis_unit_kore_prebody_handoff PRIVATE -std=c99)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME vectis_unit_kore_prebody_handoff_failure_cleanup
      COMMAND ${CMAKE_COMMAND} -E env VECTIS_KORE_PREBODY_EXIT_AFTER_START=1
        bash "${CMAKE_SOURCE_DIR}/scripts/run_forked_kore_test.sh"
        10 71 $<TARGET_FILE:vectis_unit_kore_prebody_handoff>)
    set_tests_properties(vectis_unit_kore_prebody_handoff_failure_cleanup
      PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 20)
  endif()
  vectis_add_unit_test(vectis_unit_proxy_curl_connect_only
    test_proxy_curl_connect_only.c)
  vectis_add_unit_test(vectis_unit_proxy_curl_connect_only_tls
    test_proxy_curl_connect_only_tls.c)
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
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/test_runtime_contract.py"
            "${CMAKE_BINARY_DIR}")
  set_tests_properties(vectis_runtime_contract PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 30)
  add_test(NAME vectis_runtime_reconfigure
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/test_runtime_reconfigure.py"
            "${CMAKE_BINARY_DIR}")
  set_tests_properties(vectis_runtime_reconfigure PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 30)
  add_test(NAME vectis_runtime_mapping
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/test_runtime_mapping.py"
            "${CMAKE_BINARY_DIR}")
  set_tests_properties(vectis_runtime_mapping PROPERTIES LABELS "unit;local;lifecycle" TIMEOUT 30)
endif()
