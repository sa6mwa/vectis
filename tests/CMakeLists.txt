add_subdirectory(unit)

if(VECTIS_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

