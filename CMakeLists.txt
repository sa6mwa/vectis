cmake_minimum_required(VERSION 3.21)

project(vectis VERSION 0.0.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakeDependentOption)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)

find_package(Threads REQUIRED)

option(VECTIS_BUILD_STATIC "Build the static vectis library." ON)
option(VECTIS_BUILD_SHARED "Build the shared vectis library." OFF)
option(VECTIS_BUILD_BINARY "Build the vectis executable." ON)
option(VECTIS_BUILD_TESTS "Build vectis unit tests." ON)
option(VECTIS_BUILD_EXAMPLES "Build vectis SDK examples." ON)
option(VECTIS_BUILD_FUZZERS "Build fuzz targets." OFF)
option(VECTIS_ENABLE_COVERAGE "Build with coverage instrumentation." OFF)
option(VECTIS_INSTALL "Enable install rules." ON)

set(VECTIS_EXTERNAL_ROOT "${CMAKE_SOURCE_DIR}/.cache/deps/host-debug"
    CACHE PATH "Provisioned third-party dependency root.")
set(VECTIS_DIST_DIR "${CMAKE_BINARY_DIR}/dist"
    CACHE PATH "Distribution output directory.")
set(VECTIS_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}"
    CACHE STRING "Release target architecture.")
set(VECTIS_TARGET_OS "${CMAKE_SYSTEM_NAME}"
    CACHE STRING "Release target operating system.")
set(VECTIS_TARGET_LIBC ""
    CACHE STRING "Release target libc when applicable.")

string(TOLOWER "${VECTIS_TARGET_ARCH}" VECTIS_TARGET_ARCH_LOWER)
string(TOLOWER "${VECTIS_TARGET_OS}" VECTIS_TARGET_OS_LOWER)
string(TOLOWER "${VECTIS_TARGET_LIBC}" VECTIS_TARGET_LIBC_LOWER)
if(VECTIS_TARGET_OS_LOWER STREQUAL "darwin")
  set(VECTIS_TARGET_ID "${VECTIS_TARGET_ARCH_LOWER}-apple-darwin")
elseif(VECTIS_TARGET_OS_LOWER STREQUAL "linux")
  if(NOT VECTIS_TARGET_LIBC_LOWER STREQUAL "")
    set(VECTIS_TARGET_ID "${VECTIS_TARGET_ARCH_LOWER}-linux-${VECTIS_TARGET_LIBC_LOWER}")
  else()
    set(VECTIS_TARGET_ID "${VECTIS_TARGET_ARCH_LOWER}-linux")
  endif()
else()
  set(VECTIS_TARGET_ID "${VECTIS_TARGET_ARCH_LOWER}-${VECTIS_TARGET_OS_LOWER}")
endif()

set(vectis_lockdc_config_dir "${VECTIS_EXTERNAL_ROOT}/lib/cmake/lockdc")
if(APPLE)
  set(vectis_lockdc_shared_library "${VECTIS_EXTERNAL_ROOT}/lib/liblockdc.dylib")
else()
  set(vectis_lockdc_shared_library "${VECTIS_EXTERNAL_ROOT}/lib/liblockdc.so")
endif()
if(EXISTS "${vectis_lockdc_config_dir}/lockdcConfig.cmake")
  find_package(lockdc CONFIG REQUIRED PATHS "${vectis_lockdc_config_dir}" NO_DEFAULT_PATH)
endif()

if(NOT VECTIS_BUILD_STATIC AND NOT VECTIS_BUILD_SHARED)
  message(FATAL_ERROR "Enable at least one of VECTIS_BUILD_STATIC or VECTIS_BUILD_SHARED.")
endif()

if(VECTIS_BUILD_SHARED AND NOT EXISTS "${vectis_lockdc_shared_library}")
  message(FATAL_ERROR
    "VECTIS_BUILD_SHARED=ON requires a usable ${vectis_lockdc_shared_library}. "
    "Provision the liblockdc SDK bundle before enabling shared vectis builds."
  )
endif()

if(VECTIS_BUILD_FUZZERS AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "VECTIS_BUILD_FUZZERS=ON requires Clang.")
endif()

function(vectis_configure_c_target target)
  get_target_property(target_type ${target} TYPE)
  set_target_properties(${target} PROPERTIES
    C_STANDARD 90
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
  )
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE
      -std=c89
      -Wall
      -Wextra
      -Werror
      -Wpedantic
      -Wstrict-prototypes
      -Wmissing-prototypes
      -Wshadow
      -Wpointer-arith
      -Wcast-qual
      -Wwrite-strings
      -Wdeclaration-after-statement
    )
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Werror
      -Wpedantic
      -Wstrict-prototypes
      -Wmissing-prototypes
      -Wshadow
      -Wpointer-arith
      -Wcast-qual
      -Wwrite-strings
    )
  endif()
  if(UNIX OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  if(VECTIS_ENABLE_COVERAGE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE --coverage -O0)
    if(NOT target_type STREQUAL "STATIC_LIBRARY")
      target_link_options(${target} PRIVATE --coverage)
    endif()
  endif()
endfunction()

set(vectis_external_include_dir "${VECTIS_EXTERNAL_ROOT}/include")
if(NOT EXISTS "${vectis_external_include_dir}/pslog.h")
  message(STATUS "vectis external headers not provisioned yet: ${vectis_external_include_dir}/pslog.h")
endif()

add_library(vectis_external_headers INTERFACE)
target_include_directories(vectis_external_headers INTERFACE "${vectis_external_include_dir}")

if(TARGET lockdc::static)
  set(vectis_lockdc_static_target lockdc::static)
endif()

if(TARGET lockdc::shared)
  set(vectis_lockdc_shared_target lockdc::shared)
elseif(TARGET lockdc::static)
  set(vectis_lockdc_shared_target lockdc::static)
endif()

if(EXISTS "${vectis_lockdc_shared_library}")
  set(vectis_lockdc_shared_link "${vectis_lockdc_shared_library}")
elseif(DEFINED vectis_lockdc_shared_target)
  set(vectis_lockdc_shared_link "${vectis_lockdc_shared_target}")
endif()

set(vectis_public_headers
  include/vectis/vectis.h
)

set(vectis_sources
  src/vectis.c
)

set(vectis_private_includes
  "${CMAKE_SOURCE_DIR}/src"
)

if(VECTIS_BUILD_STATIC)
  add_library(vectis_static STATIC ${vectis_sources} ${vectis_public_headers})
  add_library(vectis::vectis_static ALIAS vectis_static)
  target_include_directories(vectis_static
    PUBLIC
      "${CMAKE_SOURCE_DIR}/include"
    PRIVATE
      ${vectis_private_includes}
  )
  target_link_libraries(vectis_static
    PUBLIC
      Threads::Threads
      vectis_external_headers
      ${vectis_lockdc_static_target}
  )
  set_target_properties(vectis_static PROPERTIES OUTPUT_NAME vectis)
  vectis_configure_c_target(vectis_static)
endif()

if(VECTIS_BUILD_SHARED)
  add_library(vectis_shared SHARED ${vectis_sources} ${vectis_public_headers})
  add_library(vectis::vectis_shared ALIAS vectis_shared)
  target_include_directories(vectis_shared
    PUBLIC
      "${CMAKE_SOURCE_DIR}/include"
    PRIVATE
      ${vectis_private_includes}
  )
  target_link_libraries(vectis_shared
    PUBLIC
      Threads::Threads
      vectis_external_headers
      ${vectis_lockdc_shared_link}
  )
  set_target_properties(vectis_shared PROPERTIES
    OUTPUT_NAME vectis
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
  )
  vectis_configure_c_target(vectis_shared)
endif()

configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/package-metadata.cmake.in"
  "${CMAKE_BINARY_DIR}/package-metadata.cmake"
  @ONLY
)

if(VECTIS_BUILD_STATIC)
  set(vectis_test_link_target vectis_static)
elseif(VECTIS_BUILD_SHARED)
  set(vectis_test_link_target vectis_shared)
endif()

if(VECTIS_BUILD_BINARY)
  add_executable(vectis_bin
    src/vectis_cli.c
    src/vectis_main.c
  )
  target_include_directories(vectis_bin PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${vectis_external_include_dir}"
  )
  if(VECTIS_BUILD_STATIC)
    target_link_libraries(vectis_bin PRIVATE vectis_static)
  elseif(VECTIS_BUILD_SHARED)
    target_link_libraries(vectis_bin PRIVATE vectis_shared)
  endif()
  set_target_properties(vectis_bin PROPERTIES
    OUTPUT_NAME vectis
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
  )
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_bin PRIVATE
      -std=c99
      -Wall
      -Wextra
      -Werror
      -Wpedantic
      -Wstrict-prototypes
      -Wmissing-prototypes
      -Wshadow
      -Wpointer-arith
      -Wcast-qual
      -Wwrite-strings
    )
  endif()
  if(UNIX OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_compile_definitions(vectis_bin PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
endif()

if(VECTIS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(VECTIS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(VECTIS_INSTALL)
  if(VECTIS_BUILD_STATIC)
    install(TARGETS vectis_static ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  endif()
  if(VECTIS_BUILD_SHARED)
    install(TARGETS vectis_shared
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
  endif()
  if(VECTIS_BUILD_BINARY)
    install(TARGETS vectis_bin RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()
  install(FILES ${vectis_public_headers} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/vectis")
  install(DIRECTORY examples/
          DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/vectis/examples"
          FILES_MATCHING
          PATTERN "*.c"
          PATTERN "README.md"
          PATTERN "CMakeLists.txt")
endif()
