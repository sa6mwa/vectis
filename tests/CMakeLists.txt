if(VECTIS_BUILD_TESTS)
  add_subdirectory(unit)
endif()

if(VECTIS_BUILD_FUZZERS)
  add_subdirectory(fuzz)
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
