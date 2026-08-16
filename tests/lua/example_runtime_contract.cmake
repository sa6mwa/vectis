if(NOT DEFINED VECTIS_SOURCE_DIR)
  message(FATAL_ERROR "VECTIS_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE example_files
     "${VECTIS_SOURCE_DIR}/examples/*.c"
     "${VECTIS_SOURCE_DIR}/examples/*.lua")

foreach(example_file IN LISTS example_files)
  file(READ "${example_file}" example_text)
  string(REGEX MATCH "os[.]execute|io[.]popen|sleep[(]3600u?[)]"
         forbidden "${example_text}")
  if(forbidden)
    message(FATAL_ERROR
            "example runtime contract violation in ${example_file}: ${forbidden}")
  endif()
endforeach()
