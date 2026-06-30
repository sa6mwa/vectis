function(vectis_add_fuzzer target_name)
  add_executable(${target_name} ${ARGN})
  target_link_libraries(${target_name} PRIVATE ${vectis_test_link_target})
  target_include_directories(${target_name} PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src"
  )
  target_compile_definitions(${target_name} PRIVATE VECTIS_BUILD_FUZZERS)
  target_compile_options(${target_name} PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
    -g
  )
  target_link_options(${target_name} PRIVATE
    -fsanitize=fuzzer,address,undefined
  )
endfunction()

vectis_add_fuzzer(vectis_fuzz_json_validate fuzz_vectis_json_validate.c)

if(VECTIS_WITH_KORE_RUNTIME)
  vectis_add_fuzzer(vectis_fuzz_kore_bridge fuzz_vectis_kore_bridge.c)
endif()
