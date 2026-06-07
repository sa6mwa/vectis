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
option(VECTIS_WITH_KORE_RUNTIME "Build the embedded Kore runtime adapter." ON)
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

set(VECTIS_EXPECTED_SYSTEM_VERSION "0.1.0")
set(VECTIS_EXPECTED_LOCKDC_VERSION "0.10.0")
set(VECTIS_EXPECTED_LONEJSON_VERSION "0.31.0")
set(VECTIS_EXPECTED_PSLOG_VERSION "0.4.1")
set(VECTIS_EXPECTED_LUA_VERSION "5.5.0")
set(VECTIS_EXPECTED_LUA_SHA256 "57ccc32bbbd005cab75bcc52444052535af691789dba2b9016d5c50640d68b3d")
set(VECTIS_EXPECTED_LIBPID0_VERSION "0.3.0")
set(VECTIS_EXPECTED_LIBPID0_SHA256 "29591e058ab5ad9d0bbe0cd4c0d783ace2dee4e2ae6c590a0ba1d9f146f025f9")
set(VECTIS_EXPECTED_LIBXML2_VERSION "2.15.3")
set(VECTIS_EXPECTED_LIBXML2_SHA256 "5c6060277173270356c3f1c321a640ab629bdabc5e5ba9095b99e00759ba0c39")

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

function(vectis_manifest_value out_var key)
  file(STRINGS "${vectis_dependency_manifest}" manifest_line
       REGEX "^${key}=" LIMIT_COUNT 1)
  if(NOT manifest_line)
    message(FATAL_ERROR
      "dependency manifest ${vectis_dependency_manifest} is missing required key ${key}")
  endif()
  string(REGEX REPLACE "^[^=]*=" "" manifest_value "${manifest_line}")
  set(${out_var} "${manifest_value}" PARENT_SCOPE)
endfunction()

function(vectis_require_manifest key expected)
  vectis_manifest_value(actual "${key}")
  if(NOT actual STREQUAL "${expected}")
    message(FATAL_ERROR
      "dependency manifest ${key} mismatch: expected ${expected}, got ${actual}")
  endif()
endfunction()

function(vectis_require_dependency_file path)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR
      "dependency root ${VECTIS_EXTERNAL_ROOT} is incomplete; missing ${path}")
  endif()
endfunction()

set(vectis_lockdc_config_dir "${VECTIS_EXTERNAL_ROOT}/lib/cmake/lockdc")
set(vectis_dependency_manifest "${VECTIS_EXTERNAL_ROOT}/manifest.txt")
if(EXISTS "${vectis_dependency_manifest}")
  vectis_require_manifest(system_version "${VECTIS_EXPECTED_SYSTEM_VERSION}")
  vectis_require_manifest(liblockdc_version "${VECTIS_EXPECTED_LOCKDC_VERSION}")
  vectis_require_manifest(lonejson_version "${VECTIS_EXPECTED_LONEJSON_VERSION}")
  vectis_require_manifest(pslog_version "${VECTIS_EXPECTED_PSLOG_VERSION}")
  vectis_require_manifest(lua_version "${VECTIS_EXPECTED_LUA_VERSION}")
  vectis_require_manifest(lua_sha256 "${VECTIS_EXPECTED_LUA_SHA256}")
  vectis_require_manifest(libpid0_version "${VECTIS_EXPECTED_LIBPID0_VERSION}")
  vectis_require_manifest(libpid0_sha256 "${VECTIS_EXPECTED_LIBPID0_SHA256}")
  vectis_require_manifest(libxml2_version "${VECTIS_EXPECTED_LIBXML2_VERSION}")
  vectis_require_manifest(libxml2_sha256 "${VECTIS_EXPECTED_LIBXML2_SHA256}")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/lc/lc.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/lonejson.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/pslog.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/curl/curl.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/openssl/ssl.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/libssh2.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/nghttp2/nghttp2.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/zlib.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/libpid0-${VECTIS_EXPECTED_LIBPID0_VERSION}.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/lua.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/libxml2/libxml/parser.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/share/lonejson-source/src/lua/lonejson_lua.c")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/share/lonejson-source/lua/lonejson/init.lua")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/liblockdc.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/liblonejson.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libpslog.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcurl.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libssh2.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libssl.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcrypto.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libnghttp2.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libz.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/liblua.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libxml2.a")
  if(APPLE)
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/liblockdc.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/liblonejson.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libpslog.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcurl.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libssh2.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libssl.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcrypto.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libnghttp2.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libz.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libxml2.dylib")
  else()
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/liblockdc.so")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/liblonejson.so")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libpslog.so")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcurl.so")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libssh2.so")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libssl.so")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcrypto.so")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libnghttp2.so")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libz.so")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libxml2.so")
  endif()
else()
  message(STATUS "vectis dependency manifest not found yet: ${vectis_dependency_manifest}")
endif()

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
      -Wno-long-long
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
set(vectis_kore_include_dir "${CMAKE_SOURCE_DIR}/vendor/kore/upstream/include")
if(NOT EXISTS "${vectis_external_include_dir}/pslog.h")
  message(STATUS "vectis external headers not provisioned yet: ${vectis_external_include_dir}/pslog.h")
endif()
set(vectis_lua_static_library "${VECTIS_EXTERNAL_ROOT}/lib/liblua.a")
set(vectis_lonejson_lua_source_dir "${VECTIS_EXTERNAL_ROOT}/share/lonejson-source")
set(vectis_libxml2_static_library "${VECTIS_EXTERNAL_ROOT}/lib/libxml2.a")
if(VECTIS_BUILD_BINARY AND NOT EXISTS "${vectis_lua_static_library}")
  message(FATAL_ERROR
    "VECTIS_BUILD_BINARY=ON requires Lua in the provisioned dependency root: "
    "${vectis_lua_static_library}. Run scripts/deps.sh for this target first."
  )
endif()

add_library(vectis_external_headers INTERFACE)
target_include_directories(vectis_external_headers INTERFACE "${vectis_external_include_dir}")
if(EXISTS "${vectis_external_include_dir}/libxml2/libxml/parser.h")
  target_include_directories(vectis_external_headers INTERFACE "${vectis_external_include_dir}/libxml2")
endif()
if(EXISTS "${vectis_kore_include_dir}/kore/kore.h")
  target_include_directories(vectis_external_headers INTERFACE "${vectis_kore_include_dir}")
endif()

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
set(vectis_static_dependency_links
  "${VECTIS_EXTERNAL_ROOT}/lib/libpslog.a"
  "${VECTIS_EXTERNAL_ROOT}/lib/libcurl.a"
  "${VECTIS_EXTERNAL_ROOT}/lib/libssh2.a"
  "${VECTIS_EXTERNAL_ROOT}/lib/libssl.a"
  "${VECTIS_EXTERNAL_ROOT}/lib/libcrypto.a"
  "${VECTIS_EXTERNAL_ROOT}/lib/liblonejson.a"
  "${VECTIS_EXTERNAL_ROOT}/lib/libxml2.a"
  "${VECTIS_EXTERNAL_ROOT}/lib/libnghttp2.a"
  "${VECTIS_EXTERNAL_ROOT}/lib/libz.a"
)
set(vectis_shared_dependency_links)
foreach(vectis_shared_dep IN ITEMS pslog curl ssh2 ssl crypto lonejson xml2 nghttp2 z)
  if(APPLE)
    set(vectis_shared_dep_library "${VECTIS_EXTERNAL_ROOT}/lib/lib${vectis_shared_dep}.dylib")
  else()
    set(vectis_shared_dep_library "${VECTIS_EXTERNAL_ROOT}/lib/lib${vectis_shared_dep}.so")
  endif()
  if(EXISTS "${vectis_shared_dep_library}")
    list(APPEND vectis_shared_dependency_links "${vectis_shared_dep_library}")
  else()
    list(APPEND vectis_shared_dependency_links "${vectis_shared_dep}")
  endif()
endforeach()

set(vectis_public_headers
  include/vectis/vectis.h
)

set(vectis_sources
  src/vectis.c
)

set(vectis_kore_objects)
if(VECTIS_WITH_KORE_RUNTIME)
  set(vectis_kore_root "${CMAKE_SOURCE_DIR}/vendor/kore/upstream")
  if(NOT EXISTS "${vectis_kore_root}/src/kore.c")
    message(FATAL_ERROR "VECTIS_WITH_KORE_RUNTIME=ON requires vendor/kore/upstream.")
  endif()
  set(vectis_kore_generated_dir "${CMAKE_BINARY_DIR}/kore-generated")
  file(MAKE_DIRECTORY "${vectis_kore_generated_dir}")
  file(WRITE "${vectis_kore_generated_dir}/version.c"
    "const char *kore_version = \"vectis-embedded\";\n"
    "const char *kore_build_date = \"${CMAKE_PROJECT_VERSION}\";\n"
  )
  file(WRITE "${vectis_kore_generated_dir}/assets.c"
    "#include <sys/types.h>\n"
    "u_int8_t asset_builtin_kore_conf[] = {0};\n"
    "u_int32_t asset_len_builtin_kore_conf = 0;\n"
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    execute_process(
      COMMAND "${vectis_kore_root}/misc/linux-platform.sh"
      WORKING_DIRECTORY "${vectis_kore_root}"
      OUTPUT_FILE "${vectis_kore_generated_dir}/platform.h"
      RESULT_VARIABLE vectis_kore_platform_result
    )
    if(NOT vectis_kore_platform_result EQUAL 0)
      message(FATAL_ERROR "failed to generate Kore Linux platform header")
    endif()
    set(vectis_kore_platform_sources
      "${vectis_kore_root}/src/linux.c"
      "${vectis_kore_root}/src/seccomp.c"
    )
  else()
    file(WRITE "${vectis_kore_generated_dir}/platform.h" "")
    set(vectis_kore_platform_sources "${vectis_kore_root}/src/bsd.c")
  endif()
  set(vectis_kore_sources
    "${vectis_kore_root}/src/kore.c"
    "${vectis_kore_root}/src/buf.c"
    "${vectis_kore_root}/src/config.c"
    "${vectis_kore_root}/src/connection.c"
    "${vectis_kore_root}/src/domain.c"
    "${vectis_kore_root}/src/filemap.c"
    "${vectis_kore_root}/src/fileref.c"
    "${vectis_kore_root}/src/json.c"
    "${vectis_kore_root}/src/log.c"
    "${vectis_kore_root}/src/mem.c"
    "${vectis_kore_root}/src/msg.c"
    "${vectis_kore_root}/src/module.c"
    "${vectis_kore_root}/src/net.c"
    "${vectis_kore_root}/src/pool.c"
    "${vectis_kore_root}/src/runtime.c"
    "${vectis_kore_root}/src/sha1.c"
    "${vectis_kore_root}/src/sha2.c"
    "${vectis_kore_root}/src/timer.c"
    "${vectis_kore_root}/src/utils.c"
    "${vectis_kore_root}/src/worker.c"
    "${vectis_kore_root}/src/tls_openssl.c"
    "${vectis_kore_root}/src/keymgr_openssl.c"
    "${vectis_kore_root}/src/auth.c"
    "${vectis_kore_root}/src/accesslog.c"
    "${vectis_kore_root}/src/http.c"
    "${vectis_kore_root}/src/route.c"
    "${vectis_kore_root}/src/validator.c"
    "${vectis_kore_root}/src/websocket.c"
    "${vectis_kore_root}/src/acme.c"
    "${vectis_kore_root}/src/curl.c"
    "${vectis_kore_root}/src/ljson.c"
    "${vectis_kore_root}/src/vectis_pslog_impl.c"
    "${vectis_kore_generated_dir}/version.c"
    "${vectis_kore_generated_dir}/assets.c"
    ${vectis_kore_platform_sources}
  )
  add_library(vectis_kore_runtime OBJECT ${vectis_kore_sources})
  target_include_directories(vectis_kore_runtime PRIVATE
    "${vectis_kore_root}/include/kore"
    "${vectis_kore_generated_dir}"
    "${vectis_external_include_dir}"
  )
  target_compile_definitions(vectis_kore_runtime PRIVATE
    KORE_SINGLE_BINARY
    KORE_USE_ACME
    KORE_USE_CURL
    KORE_USE_LONEJSON
    KORE_USE_PSLOG
    KORE_VECTIS_DIRECT_TLS
    KORE_TMPDIR="/tmp"
    TLS_BACKEND_OPENSSL
    main=vectis_kore_main
    PREFIX="/usr/local"
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(vectis_kore_runtime PRIVATE
      _GNU_SOURCE=1
      _FORTIFY_SOURCE=2
    )
  endif()
  set_target_properties(vectis_kore_runtime PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS ON
    POSITION_INDEPENDENT_CODE ON
  )
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_kore_runtime PRIVATE -std=gnu99 -Wall -Wextra)
  endif()
  list(APPEND vectis_sources src/vectis_kore_bridge.c $<TARGET_OBJECTS:vectis_kore_runtime>)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(src/vectis_kore_bridge.c PROPERTIES
      COMPILE_OPTIONS "-std=gnu99;-Wno-pedantic;-Wno-long-long;-Wno-cast-qual;-Wno-missing-prototypes"
      COMPILE_DEFINITIONS "KORE_USE_ACME;KORE_USE_CURL;KORE_USE_LONEJSON;KORE_USE_PSLOG;KORE_VECTIS_DIRECT_TLS;TLS_BACKEND_OPENSSL"
    )
  endif()
else()
  list(APPEND vectis_sources src/vectis_kore_stub.c)
endif()

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
  target_link_directories(vectis_static
    PUBLIC
      "${VECTIS_EXTERNAL_ROOT}/lib"
  )
  target_link_libraries(vectis_static
    PUBLIC
      Threads::Threads
      vectis_external_headers
      ${vectis_lockdc_static_target}
      ${vectis_static_dependency_links}
      ${CMAKE_DL_LIBS}
  )
  if(VECTIS_WITH_KORE_RUNTIME AND UNIX AND NOT APPLE)
    target_link_options(vectis_static INTERFACE -Wl,--export-dynamic)
  endif()
  if(UNIX AND NOT APPLE)
    target_link_libraries(vectis_static PUBLIC m)
  endif()
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
  target_link_directories(vectis_shared
    PUBLIC
      "${VECTIS_EXTERNAL_ROOT}/lib"
  )
  target_link_libraries(vectis_shared
    PUBLIC
      Threads::Threads
      vectis_external_headers
      ${vectis_lockdc_shared_link}
      ${vectis_shared_dependency_links}
      ${CMAKE_DL_LIBS}
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
  set(vectis_lonejson_lua_source
    "${vectis_lonejson_lua_source_dir}/src/lua/lonejson_lua.c")
  add_library(vectis_lonejson_lua OBJECT "${vectis_lonejson_lua_source}")
  target_include_directories(vectis_lonejson_lua PRIVATE
    "${vectis_external_include_dir}"
    "${vectis_lonejson_lua_source_dir}/src"
    "${vectis_lonejson_lua_source_dir}/src/lua"
  )
  target_compile_definitions(vectis_lonejson_lua PRIVATE
    _POSIX_C_SOURCE=200809L)
  set_target_properties(vectis_lonejson_lua PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )

  add_executable(vectis_bin
    src/vectis_cli.c
    src/vectis_main.c
    $<TARGET_OBJECTS:vectis_lonejson_lua>
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
  target_link_libraries(vectis_bin PRIVATE "${vectis_lua_static_library}")
  if(UNIX AND NOT APPLE)
    target_link_libraries(vectis_bin PRIVATE m)
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
  if(EXISTS "${vectis_kore_include_dir}/kore/kore.h")
    install(DIRECTORY "${vectis_kore_include_dir}/kore"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  endif()
  install(DIRECTORY examples/
          DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/vectis/examples"
          FILES_MATCHING
          PATTERN "*.c"
          PATTERN "README.md"
          PATTERN "CMakeLists.txt")
endif()
