if(NOT DEFINED VECTIS_SOURCE_DIR)
  message(FATAL_ERROR "VECTIS_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE runtime_lua_files
     "${VECTIS_SOURCE_DIR}/examples/*.c"
     "${VECTIS_SOURCE_DIR}/examples/*.lua"
     "${VECTIS_SOURCE_DIR}/tests/lua/*.cmake"
     "${VECTIS_SOURCE_DIR}/tests/lua/*.lua")
list(APPEND runtime_lua_files "${VECTIS_SOURCE_DIR}/scripts/test-e2e.sh")

foreach(runtime_lua_file IN LISTS runtime_lua_files)
  file(READ "${runtime_lua_file}" example_text)
  string(REGEX MATCH "os[.]execute|io[.]popen|sleep[(]3600u?[)]"
         forbidden "${example_text}")
  if(forbidden)
    message(FATAL_ERROR
            "Lua runtime contract violation in ${runtime_lua_file}: ${forbidden}")
  endif()
endforeach()
