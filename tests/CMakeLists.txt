add_subdirectory(unit)

if(VECTIS_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

if(TARGET vectis_bin)
  add_test(NAME vectis_cli_version COMMAND vectis_bin --version)
endif()
