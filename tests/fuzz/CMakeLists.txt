add_executable(vectis_fuzz_json_validate fuzz_vectis_json_validate.c)
target_link_libraries(vectis_fuzz_json_validate PRIVATE ${vectis_test_link_target})
target_compile_options(vectis_fuzz_json_validate PRIVATE
  -fsanitize=fuzzer,address,undefined
  -fno-omit-frame-pointer
  -g
)
target_link_options(vectis_fuzz_json_validate PRIVATE
  -fsanitize=fuzzer,address,undefined
)

