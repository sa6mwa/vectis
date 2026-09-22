# Local executables select the complete configured Bootlin runtime at link time.
# This file is never installed/exported with the SDK.
if(NOT VECTIS_TARGET_ID STREQUAL "x86_64-linux-gnu" OR
   NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64")
  return()
endif()

set(vectis_test_loader "${CMAKE_SYSROOT}/lib/ld-linux-x86-64.so.2")
if(NOT EXISTS "${vectis_test_loader}" OR NOT EXISTS "${CMAKE_SYSROOT}/lib/libc.so.6")
  message(FATAL_ERROR "Pinned Bootlin runtime is missing under ${CMAKE_SYSROOT}; reconfigure with the pinned toolchain")
endif()
set(vectis_test_runtime_dirs "${CMAKE_SYSROOT}/lib;${CMAKE_SYSROOT}/usr/lib")
get_filename_component(vectis_compiler_runtime_dir "${CMAKE_SYSROOT}/../lib64" ABSOLUTE)
if(IS_DIRECTORY "${vectis_compiler_runtime_dir}")
  # Bootlin's sanitizer DSOs live beside the sysroot, not inside it.
  list(APPEND vectis_test_runtime_dirs "${vectis_compiler_runtime_dir}")
endif()
# SDK consumers may select an install tree plus a separate target dependency
# prefix. Keep both inside CMake's target-only package search boundary.
list(APPEND CMAKE_FIND_ROOT_PATH ${CMAKE_PREFIX_PATH})

function(vectis_pin_local_executables directory)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(kind ${target} TYPE)
    # Release CLI is static and must retain its normal shipping metadata.
    if(kind STREQUAL "EXECUTABLE" AND NOT
       (target STREQUAL "vectis_bin" AND CMAKE_BUILD_TYPE STREQUAL "Release"))
      target_link_options(${target} PRIVATE
        "LINKER:--dynamic-linker=${vectis_test_loader}"
        "LINKER:--disable-new-dtags")
      # RPATH covers transitive dependencies. The runtime contract verifies the
      # actual resolved paths, including dependencies found in the loader cache.
      set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH ${vectis_test_runtime_dirs})
      set_property(GLOBAL APPEND PROPERTY VECTIS_RUNTIME_TARGET_FILES "$<TARGET_FILE:${target}>")
    endif()
  endforeach()
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    vectis_pin_local_executables("${child}")
  endforeach()
endfunction()
function(vectis_finalize_local_runtime)
  vectis_pin_local_executables("${CMAKE_SOURCE_DIR}")
  get_property(files GLOBAL PROPERTY VECTIS_RUNTIME_TARGET_FILES)
  list(JOIN files "\n" manifest)
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/test-runtime/targets.txt"
    CONTENT "${manifest}\n")
endfunction()
cmake_language(DEFER CALL vectis_finalize_local_runtime)

if(VECTIS_BUILD_TESTS OR VECTIS_BUILD_FUZZERS)
  get_filename_component(vectis_toolchain_cache "${CMAKE_SYSROOT}/../../../.." ABSOLUTE)
  if(NOT DEFINED CPKT_DEPENDENCY_CACHE OR CPKT_DEPENDENCY_CACHE STREQUAL "")
    message(FATAL_ERROR "CPKT_DEPENDENCY_CACHE must be configured before test runtime setup")
  endif()
  set(vectis_dependency_cache "${CPKT_DEPENDENCY_CACHE}")
  configure_file("${CMAKE_SOURCE_DIR}/cmake/test_environment.cmake.in"
    "${CMAKE_BINARY_DIR}/test_environment.cmake" @ONLY)
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_BINARY_DIR}/test_environment.cmake")
endif()
