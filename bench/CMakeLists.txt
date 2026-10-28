if(NOT VECTIS_BUILD_BINARY)
  message(FATAL_ERROR "Metrics benchmarks require VECTIS_BUILD_BINARY")
endif()
include("${CMAKE_SOURCE_DIR}/cmake/vectis_bench_fixture.cmake")
vectis_add_bench_fixture(bench_fixture)
if(VECTIS_BUILD_TESTS)
  add_test(NAME vectis_metrics_storage_smoke
    COMMAND "${bench_fixture}" metrics-storage --binary "$<TARGET_FILE:vectis_bin>"
      --script "${CMAKE_CURRENT_SOURCE_DIR}/metrics_storage.lua"
      --root "${CMAKE_CURRENT_BINARY_DIR}/metrics-storage" --smoke)
  set_tests_properties(vectis_metrics_storage_smoke PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" LABELS "local;integration;benchmark" TIMEOUT 120)
endif()
