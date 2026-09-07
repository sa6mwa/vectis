if(NOT VECTIS_BUILD_BINARY)
  message(FATAL_ERROR "Metrics benchmarks require VECTIS_BUILD_BINARY")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(VECTIS_BUILD_TESTS)
  add_test(NAME vectis_metrics_storage_smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/metrics_storage.py"
      --binary "$<TARGET_FILE:vectis_bin>" --root "${CMAKE_CURRENT_BINARY_DIR}" --smoke)
  set_tests_properties(vectis_metrics_storage_smoke PROPERTIES
    LABELS "local;integration;benchmark" TIMEOUT 120)
endif()
