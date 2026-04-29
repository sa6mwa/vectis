function(vectis_add_unit_test target_name)
  add_executable(${target_name} ${ARGN})
  target_link_libraries(${target_name} PRIVATE ${vectis_test_link_target})
  target_include_directories(${target_name} PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src"
  )
  set_target_properties(${target_name} PROPERTIES
    C_STANDARD 90
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
  )
  if(VECTIS_ENABLE_COVERAGE)
    target_compile_options(${target_name} PRIVATE --coverage -O0 -g)
    target_link_options(${target_name} PRIVATE --coverage)
  endif()
  add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

vectis_add_unit_test(vectis_unit_config test_vectis_config.c)
vectis_add_unit_test(vectis_unit_certs test_vectis_certs.c)
vectis_add_unit_test(vectis_unit_https_runtime test_vectis_https_runtime.c)
vectis_add_unit_test(vectis_unit_https_mtls_runtime test_vectis_https_mtls_runtime.c)
vectis_add_unit_test(vectis_unit_runtime test_vectis_runtime.c)
vectis_add_unit_test(vectis_unit_sdk_surface test_vectis_sdk_surface.c)
