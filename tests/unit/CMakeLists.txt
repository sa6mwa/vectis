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
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target_name} PRIVATE -UNDEBUG)
  endif()
  add_test(NAME ${target_name} COMMAND ${target_name})
  set_tests_properties(${target_name} PROPERTIES
    LABELS "unit;local"
    TIMEOUT 180)
endfunction()

function(vectis_add_cpp_header_test target_name)
  add_executable(${target_name} ${ARGN})
  target_link_libraries(${target_name} PRIVATE ${vectis_test_link_target})
  target_include_directories(${target_name} PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
  )
  set_target_properties(${target_name} PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target_name} PRIVATE
      -UNDEBUG
      -Wall
      -Wextra
      -Werror
      -Wpedantic
      -Wshadow
      -Wpointer-arith
      -Wcast-qual
      -Wwrite-strings
    )
  endif()
  add_test(NAME ${target_name} COMMAND ${target_name})
  set_tests_properties(${target_name} PROPERTIES
    LABELS "unit;local"
    TIMEOUT 180)
endfunction()

vectis_add_unit_test(vectis_unit_config test_vectis_config.c)
vectis_add_unit_test(vectis_unit_cai test_vectis_cai.c)
vectis_add_unit_test(vectis_unit_audio_worker test_vectis_audio_worker.c)
vectis_add_unit_test(vectis_unit_sus_worker test_vectis_sus_worker.c)
vectis_add_unit_test(vectis_unit_certs test_vectis_certs.c)
vectis_add_unit_test(vectis_unit_auth test_vectis_auth.c)
vectis_add_unit_test(vectis_unit_totp_qr test_vectis_totp_qr.c)
vectis_add_unit_test(vectis_unit_embedded_fs test_vectis_embedded_fs.c)
vectis_add_unit_test(vectis_unit_mailbox test_vectis_mailbox.c)
vectis_add_unit_test(vectis_unit_webdav test_vectis_webdav.c)
if(VECTIS_WITH_KORE_RUNTIME)
  vectis_add_unit_test(vectis_unit_https_runtime test_vectis_https_runtime.c)
  vectis_add_unit_test(vectis_unit_https_mtls_runtime test_vectis_https_mtls_runtime.c)
  vectis_add_unit_test(vectis_unit_runtime test_vectis_runtime.c)
  vectis_add_unit_test(vectis_unit_runtime_header_limit
    test_vectis_runtime.c)
  target_compile_definitions(vectis_unit_runtime_header_limit PRIVATE
    VECTIS_RUNTIME_HEADER_LIMIT_ONLY)
  vectis_add_unit_test(vectis_unit_sdk_surface test_vectis_sdk_surface.c)
else()
  message(STATUS "Kore runtime disabled; skipping Kore runtime unit tests")
  message(STATUS "Kore runtime disabled; skipping SDK surface runtime integration unit test")
endif()

include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
  enable_language(CXX)
  vectis_add_cpp_header_test(vectis_unit_header_cpp test_vectis_header_cpp.cpp)
else()
  message(STATUS "C++ compiler not available; skipping vectis C++ header smoke test")
endif()
