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

if(TARGET vectis_bin)
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
  add_test(NAME vectis_lua_http
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                   -P "${CMAKE_SOURCE_DIR}/tests/lua/http.cmake")
  set_tests_properties(vectis_lua_http PROPERTIES
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
  add_test(NAME vectis_lua_pack
           COMMAND "${CMAKE_COMMAND}"
                   "-DVECTIS_BIN=$<TARGET_FILE:vectis_bin>"
                   "-DVECTIS_PACK_SMTP_HARNESS=$<TARGET_FILE:vectis_pack_smtp_harness>"
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
endif()
