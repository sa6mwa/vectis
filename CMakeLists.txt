cmake_minimum_required(VERSION 3.21)

set(VECTIS_VERSION_OVERRIDE "" CACHE STRING "Deliberate release-candidate version override.")
set(vectis_version_args)
if(NOT VECTIS_VERSION_OVERRIDE STREQUAL "")
  list(APPEND vectis_version_args --override "${VECTIS_VERSION_OVERRIDE}")
endif()
execute_process(
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/release_version.sh" ${vectis_version_args}
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  OUTPUT_VARIABLE vectis_detected_version
  ERROR_VARIABLE vectis_version_error
  RESULT_VARIABLE vectis_version_result
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT vectis_version_result EQUAL 0)
  message(FATAL_ERROR "failed to resolve vectis version: ${vectis_version_error}")
endif()

project(vectis VERSION "${vectis_detected_version}" LANGUAGES C)

include(GNUInstallDirs)
include(CMakeDependentOption)
include(CMakePackageConfigHelpers)

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
set(VECTIS_TARGET_ID ""
    CACHE STRING "Release target identifier.")

set(VECTIS_EXPECTED_SYSTEM_VERSION "0.4.0")
set(VECTIS_EXPECTED_LOCKDC_VERSION "0.12.1")
set(VECTIS_EXPECTED_LONEJSON_VERSION "0.32.1")
set(VECTIS_EXPECTED_PSLOG_VERSION "0.4.1")
set(VECTIS_EXPECTED_CAI_VERSION "0.1.2")
set(VECTIS_EXPECTED_LIBMDF_VERSION "0.1.0")
set(VECTIS_EXPECTED_LUA_VERSION "5.5.0")
set(VECTIS_EXPECTED_LIBPID0_VERSION "0.3.0")
set(VECTIS_EXPECTED_LIBPID0_SHA256 "29591e058ab5ad9d0bbe0cd4c0d783ace2dee4e2ae6c590a0ba1d9f146f025f9")
set(VECTIS_EXPECTED_LIBXML2_VERSION "2.15.3")

if(VECTIS_TARGET_ID STREQUAL "")
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
set(CURL_DIR "${VECTIS_EXTERNAL_ROOT}/lib/cmake/CURL")
set(OpenSSL_DIR "${VECTIS_EXTERNAL_ROOT}/lib/cmake/OpenSSL")
set(Libssh2_DIR "${VECTIS_EXTERNAL_ROOT}/lib/cmake/libssh2")
set(nghttp2_DIR "${VECTIS_EXTERNAL_ROOT}/lib/cmake/nghttp2")
set(ZLIB_DIR "${VECTIS_EXTERNAL_ROOT}/lib/cmake/zlib")
set(Lua_DIR "${VECTIS_EXTERNAL_ROOT}/lib/cmake/Lua")
set(CpktLuaRuntime_DIR "${VECTIS_EXTERNAL_ROOT}/lib/cmake/CpktLuaRuntime")
set(LibXml2_DIR "${VECTIS_EXTERNAL_ROOT}/lib/cmake/libxml2")
set(lonejson_DIR "${VECTIS_EXTERNAL_ROOT}/lib/cmake/lonejson")
set(pslog_DIR "${VECTIS_EXTERNAL_ROOT}/lib/cmake/pslog")
set(cai_DIR "${VECTIS_EXTERNAL_ROOT}/lib/cmake/cai")
set(libmdf_DIR "${VECTIS_EXTERNAL_ROOT}/lib/cmake/libmdf")
set(vectis_dependency_manifest "${VECTIS_EXTERNAL_ROOT}/manifest.txt")
if(EXISTS "${vectis_dependency_manifest}")
  vectis_require_manifest(system_version "${VECTIS_EXPECTED_SYSTEM_VERSION}")
  vectis_require_manifest(liblockdc_version "${VECTIS_EXPECTED_LOCKDC_VERSION}")
  vectis_require_manifest(lonejson_version "${VECTIS_EXPECTED_LONEJSON_VERSION}")
  vectis_require_manifest(pslog_version "${VECTIS_EXPECTED_PSLOG_VERSION}")
  vectis_require_manifest(cai_version "${VECTIS_EXPECTED_CAI_VERSION}")
  vectis_require_manifest(libmdf_version "${VECTIS_EXPECTED_LIBMDF_VERSION}")
  vectis_require_manifest(lua_version "${VECTIS_EXPECTED_LUA_VERSION}")
  vectis_require_manifest(lua_runtime_facade_abi "0")
  vectis_require_manifest(libpid0_version "${VECTIS_EXPECTED_LIBPID0_VERSION}")
  vectis_require_manifest(libpid0_sha256 "${VECTIS_EXPECTED_LIBPID0_SHA256}")
  vectis_require_manifest(libxml2_version "${VECTIS_EXPECTED_LIBXML2_VERSION}")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/lonejson.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/pslog.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/cai/cai.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/cai/mcp.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/cai/auth.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/libmdf.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/libmdf/mdf.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/curl/curl.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/openssl/ssl.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/libssh2.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/nghttp2/nghttp2.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/zlib.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/libpid0-${VECTIS_EXPECTED_LIBPID0_VERSION}.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/lua.h")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/include/libxml2/libxml/parser.h")
  vectis_require_dependency_file("${CURL_DIR}/CURLConfig.cmake")
  vectis_require_dependency_file("${OpenSSL_DIR}/OpenSSLConfig.cmake")
  vectis_require_dependency_file("${Libssh2_DIR}/libssh2-config.cmake")
  vectis_require_dependency_file("${nghttp2_DIR}/nghttp2Config.cmake")
  vectis_require_dependency_file("${ZLIB_DIR}/ZLIBConfig.cmake")
  vectis_require_dependency_file("${Lua_DIR}/LuaConfig.cmake")
  vectis_require_dependency_file("${CpktLuaRuntime_DIR}/CpktLuaRuntimeConfig.cmake")
  vectis_require_dependency_file("${LibXml2_DIR}/libxml2-config.cmake")
  vectis_require_dependency_file("${libmdf_DIR}/libmdfConfig.cmake")
  vectis_require_dependency_file("${vectis_lockdc_config_dir}/lockdcConfig.cmake")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/share/lockdc-source/src/lua/lockdc_lua.c")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/share/lockdc-source/lua/lockdc/init.lua")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/share/lonejson-source/src/lua/lonejson_lua.c")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/share/lonejson-source/lua/lonejson/init.lua")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/share/cai-lua-source/lua/cai_lua.c")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/share/libmdf-lua-source/lua/libmdf.lua")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/share/libmdf-lua-source/lua/libmdf_core.c")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/liblonejson.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libpslog.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcai.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libmdf.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcurl.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libssh2.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libssl.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcrypto.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libnghttp2.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libz.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/liblua.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcpkt_lua_runtime.a")
  vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libxml2.a")
  if(APPLE)
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/liblonejson.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libpslog.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcai.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libmdf.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcurl.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libssh2.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libssl.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcrypto.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libnghttp2.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libz.dylib")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libxml2.dylib")
  else()
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/liblonejson.so")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libpslog.so")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libcai.so")
    vectis_require_dependency_file("${VECTIS_EXTERNAL_ROOT}/lib/libmdf.so")
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

find_package(lockdc CONFIG REQUIRED PATHS "${vectis_lockdc_config_dir}" NO_DEFAULT_PATH)
find_package(CURL CONFIG REQUIRED PATHS "${CURL_DIR}" NO_DEFAULT_PATH)
find_package(OpenSSL CONFIG REQUIRED PATHS "${OpenSSL_DIR}" NO_DEFAULT_PATH)
find_package(Libssh2 CONFIG REQUIRED PATHS "${Libssh2_DIR}" NO_DEFAULT_PATH)
find_package(nghttp2 CONFIG REQUIRED PATHS "${nghttp2_DIR}" NO_DEFAULT_PATH)
find_package(ZLIB CONFIG REQUIRED PATHS "${ZLIB_DIR}" NO_DEFAULT_PATH)
find_package(CpktLuaRuntime CONFIG REQUIRED PATHS "${CpktLuaRuntime_DIR}" NO_DEFAULT_PATH)
find_package(LibXml2 CONFIG REQUIRED PATHS "${LibXml2_DIR}" NO_DEFAULT_PATH)
find_package(lonejson CONFIG REQUIRED PATHS "${lonejson_DIR}" NO_DEFAULT_PATH)
find_package(pslog CONFIG REQUIRED PATHS "${pslog_DIR}" NO_DEFAULT_PATH)
find_package(cai CONFIG REQUIRED PATHS "${cai_DIR}" NO_DEFAULT_PATH)
find_package(libmdf CONFIG REQUIRED PATHS "${libmdf_DIR}" NO_DEFAULT_PATH)

if(TARGET cai::cai_static AND TARGET lonejson::lonejson_static)
  set_property(TARGET cai::cai_static PROPERTY
    INTERFACE_LINK_LIBRARIES "CURL::libcurl;Threads::Threads;OpenSSL::Crypto;lonejson::lonejson_static")
endif()

if(NOT VECTIS_BUILD_STATIC AND NOT VECTIS_BUILD_SHARED)
  message(FATAL_ERROR "Enable at least one of VECTIS_BUILD_STATIC or VECTIS_BUILD_SHARED.")
endif()

if(VECTIS_BUILD_STATIC AND NOT TARGET lockdc::static)
  message(FATAL_ERROR
    "VECTIS_BUILD_STATIC=ON requires lockdc::static from the provisioned "
    "lockdc CMake package metadata. Provision a complete liblockdc SDK bundle."
  )
endif()

if(VECTIS_BUILD_SHARED AND NOT TARGET lockdc::shared)
  message(FATAL_ERROR
    "VECTIS_BUILD_SHARED=ON requires lockdc::shared from the provisioned "
    "lockdc CMake package metadata. Provision a complete liblockdc SDK bundle."
  )
endif()

if(VECTIS_BUILD_FUZZERS AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "VECTIS_BUILD_FUZZERS=ON requires Clang.")
endif()

function(vectis_apply_relocatable_paths target)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE
      "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
      "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build"
      "-ffile-prefix-map=${VECTIS_EXTERNAL_ROOT}=deps"
    )
  endif()
endfunction()

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
  vectis_apply_relocatable_paths(${target})
  if(VECTIS_ENABLE_COVERAGE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE --coverage -O0)
    if(target_type STREQUAL "STATIC_LIBRARY")
      target_link_options(${target} INTERFACE --coverage)
    else()
      target_link_options(${target} PRIVATE --coverage)
    endif()
  endif()
endfunction()

set(vectis_external_include_dir "${VECTIS_EXTERNAL_ROOT}/include")
set(vectis_kore_include_dir "${CMAKE_SOURCE_DIR}/vendor/kore/upstream/include")
if(NOT EXISTS "${vectis_external_include_dir}/pslog.h")
  message(STATUS "vectis external headers not provisioned yet: ${vectis_external_include_dir}/pslog.h")
endif()
set(vectis_lockdc_lua_source_dir "${VECTIS_EXTERNAL_ROOT}/share/lockdc-source")
set(vectis_lonejson_lua_source_dir "${VECTIS_EXTERNAL_ROOT}/share/lonejson-source")
set(vectis_cai_lua_source_dir "${VECTIS_EXTERNAL_ROOT}/share/cai-lua-source")
set(vectis_libmdf_lua_source_dir "${VECTIS_EXTERNAL_ROOT}/share/libmdf-lua-source")

function(vectis_generate_lua_init_header source_path symbol output_path)
  file(READ "${source_path}" vectis_lua_init_hex HEX)
  string(REGEX MATCHALL ".." vectis_lua_init_bytes "${vectis_lua_init_hex}")
  set(vectis_lua_init_c "")
  set(vectis_lua_init_column 0)
  foreach(vectis_lua_init_byte IN LISTS vectis_lua_init_bytes)
    if(vectis_lua_init_column EQUAL 0)
      string(APPEND vectis_lua_init_c "    ")
    endif()
    string(APPEND vectis_lua_init_c "0x${vectis_lua_init_byte},")
    math(EXPR vectis_lua_init_column "${vectis_lua_init_column} + 1")
    if(vectis_lua_init_column EQUAL 16)
      string(APPEND vectis_lua_init_c "\n")
      set(vectis_lua_init_column 0)
    else()
      string(APPEND vectis_lua_init_c " ")
    endif()
  endforeach()
  if(NOT vectis_lua_init_column EQUAL 0)
    string(APPEND vectis_lua_init_c "\n")
  endif()
  set(vectis_lua_init_symbol "${symbol}")
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/vectis_lua_init.h.in"
    "${output_path}"
    @ONLY
  )
endfunction()

add_library(vectis_external_headers INTERFACE)
target_include_directories(vectis_external_headers INTERFACE "${vectis_external_include_dir}")
if(EXISTS "${vectis_kore_include_dir}/kore/kore.h")
  target_include_directories(vectis_external_headers INTERFACE "${vectis_kore_include_dir}")
endif()

set(vectis_lockdc_static_target lockdc::static)
set(vectis_lockdc_shared_target lockdc::shared)

set(vectis_static_dependency_links
  cai::cai_static
  libmdf::mdf_static
  pslog::pslog_static
  CURL::libcurl
  Libssh2::libssh2
  OpenSSL::SSL
  OpenSSL::Crypto
  lonejson::lonejson_static
  LibXml2::LibXml2
  nghttp2::nghttp2
  ZLIB::ZLIB
)
set(vectis_shared_dependency_links
  cai::cai_shared
  libmdf::mdf_shared
  pslog::pslog_shared
  cpkt::curl_shared
  cpkt::libssh2_shared
  cpkt::openssl_ssl_shared
  cpkt::openssl_crypto_shared
  lonejson::lonejson
  cpkt::libxml2_shared
  cpkt::nghttp2_shared
  cpkt::zlib_shared
)

set(vectis_public_headers
  include/vectis/vectis.h
  "${CMAKE_BINARY_DIR}/generated/include/vectis/vectis_version.h"
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
      COMMAND "${CMAKE_COMMAND}" -E env "TARGET_PLATFORM=${VECTIS_TARGET_ARCH}"
              "${vectis_kore_root}/misc/linux-platform.sh"
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
  vectis_apply_relocatable_paths(vectis_kore_runtime)
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

configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/vectis_version.h.in"
  "${CMAKE_BINARY_DIR}/generated/include/vectis/vectis_version.h"
  @ONLY
)

if(VECTIS_BUILD_STATIC)
  add_library(vectis_static STATIC ${vectis_sources} ${vectis_public_headers})
  add_library(vectis::vectis_static ALIAS vectis_static)
  target_include_directories(vectis_static
    PUBLIC
      "${CMAKE_SOURCE_DIR}/include"
      "${CMAKE_BINARY_DIR}/generated/include"
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
  )
  if(VECTIS_WITH_KORE_RUNTIME AND UNIX AND NOT APPLE)
    target_link_options(vectis_static INTERFACE -Wl,--export-dynamic)
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
      "${CMAKE_BINARY_DIR}/generated/include"
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
      ${vectis_lockdc_shared_target}
      ${vectis_shared_dependency_links}
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
configure_package_config_file(
  "${CMAKE_SOURCE_DIR}/cmake/vectisConfig.cmake.in"
  "${CMAKE_BINARY_DIR}/generated/cmake/vectisConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vectis"
)
write_basic_package_version_file(
  "${CMAKE_BINARY_DIR}/generated/cmake/vectisConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)

if(VECTIS_BUILD_STATIC)
  set(vectis_test_link_target vectis_static)
elseif(VECTIS_BUILD_SHARED)
  set(vectis_test_link_target vectis_shared)
endif()

if(VECTIS_BUILD_BINARY)
  vectis_generate_lua_init_header(
    "${vectis_lockdc_lua_source_dir}/lua/lockdc/init.lua"
    "vectis_lockdc_lua_init"
    "${CMAKE_BINARY_DIR}/generated/include/vectis_lockdc_lua_init.h"
  )
  vectis_generate_lua_init_header(
    "${vectis_libmdf_lua_source_dir}/lua/libmdf.lua"
    "vectis_libmdf_lua_init"
    "${CMAKE_BINARY_DIR}/generated/include/vectis_libmdf_lua_init.h"
  )
  set(vectis_lockdc_lua_source
    "${vectis_lockdc_lua_source_dir}/src/lua/lockdc_lua.c")
  set(vectis_lonejson_lua_source
    "${vectis_lonejson_lua_source_dir}/src/lua/lonejson_lua.c")
  set(vectis_cai_lua_source "${vectis_cai_lua_source_dir}/lua/cai_lua.c")
  set(vectis_libmdf_lua_source "${vectis_libmdf_lua_source_dir}/lua/libmdf_core.c")
  add_library(vectis_lockdc_lua OBJECT "${vectis_lockdc_lua_source}")
  target_include_directories(vectis_lockdc_lua PRIVATE
    "${vectis_external_include_dir}"
    "${vectis_lockdc_lua_source_dir}/src"
    "${vectis_lockdc_lua_source_dir}/src/lua"
  )
  target_compile_definitions(vectis_lockdc_lua PRIVATE
    _POSIX_C_SOURCE=200809L)
  set_target_properties(vectis_lockdc_lua PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  vectis_apply_relocatable_paths(vectis_lockdc_lua)

  add_library(vectis_lonejson_lua OBJECT "${vectis_lonejson_lua_source}")
  target_include_directories(vectis_lonejson_lua PRIVATE
    "${vectis_external_include_dir}"
    "${vectis_lonejson_lua_source_dir}/src"
    "${vectis_lonejson_lua_source_dir}/src/lua"
  )
  target_compile_definitions(vectis_lonejson_lua PRIVATE
    _POSIX_C_SOURCE=200809L)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_lonejson_lua PRIVATE -fvisibility=hidden)
  endif()
  set_target_properties(vectis_lonejson_lua PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  vectis_apply_relocatable_paths(vectis_lonejson_lua)
  if(CMAKE_OBJCOPY)
    set(vectis_objcopy "${CMAKE_OBJCOPY}")
  else()
    find_program(vectis_objcopy NAMES llvm-objcopy objcopy)
  endif()
  if(NOT vectis_objcopy)
    message(FATAL_ERROR
      "objcopy is required to embed lonejson Lua without exporting its private lonejson symbols")
  endif()
  set(vectis_lonejson_lua_private_object
    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/vectis_lonejson_lua_private.dir/lonejson_lua.c${CMAKE_C_OUTPUT_EXTENSION}")
  add_custom_command(
    OUTPUT "${vectis_lonejson_lua_private_object}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/vectis_lonejson_lua_private.dir"
    COMMAND "${vectis_objcopy}" --wildcard --localize-symbol=lonejson_*
            $<TARGET_OBJECTS:vectis_lonejson_lua>
            "${vectis_lonejson_lua_private_object}"
    DEPENDS vectis_lonejson_lua $<TARGET_OBJECTS:vectis_lonejson_lua>
    VERBATIM
  )
  set_source_files_properties("${vectis_lonejson_lua_private_object}" PROPERTIES
    EXTERNAL_OBJECT TRUE
    GENERATED TRUE
  )
  add_library(vectis_lonejson_lua_archive STATIC
    "${vectis_lonejson_lua_private_object}")
  set_target_properties(vectis_lonejson_lua_archive PROPERTIES
    OUTPUT_NAME vectis_lonejson_lua
    LINKER_LANGUAGE C
  )

  add_library(vectis_cai_lua OBJECT "${vectis_cai_lua_source}")
  target_include_directories(vectis_cai_lua PRIVATE
    "${vectis_external_include_dir}"
  )
  target_compile_definitions(vectis_cai_lua PRIVATE
    _POSIX_C_SOURCE=200809L)
  set_target_properties(vectis_cai_lua PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  vectis_apply_relocatable_paths(vectis_cai_lua)

  add_library(vectis_libmdf_lua OBJECT "${vectis_libmdf_lua_source}")
  target_include_directories(vectis_libmdf_lua PRIVATE
    "${vectis_external_include_dir}"
  )
  target_compile_definitions(vectis_libmdf_lua PRIVATE
    _POSIX_C_SOURCE=200809L)
  set_target_properties(vectis_libmdf_lua PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  vectis_apply_relocatable_paths(vectis_libmdf_lua)

  add_executable(vectis_bin
    src/vectis_cli.c
    src/vectis_main.c
    $<TARGET_OBJECTS:vectis_lockdc_lua>
    $<TARGET_OBJECTS:vectis_cai_lua>
    $<TARGET_OBJECTS:vectis_libmdf_lua>
  )
  target_include_directories(vectis_bin PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated/include"
    "${vectis_external_include_dir}"
  )
  if(VECTIS_BUILD_STATIC)
    target_link_libraries(vectis_bin PRIVATE vectis_static)
  elseif(VECTIS_BUILD_SHARED)
    target_link_libraries(vectis_bin PRIVATE vectis_shared)
  endif()
  target_link_libraries(vectis_bin PRIVATE
    cpkt::lua_runtime
    cai::cai_static
    libmdf::mdf_static
    vectis_lonejson_lua_archive)
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
  vectis_apply_relocatable_paths(vectis_bin)
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
  install(FILES
          "${CMAKE_SOURCE_DIR}/LICENSE"
          "${CMAKE_SOURCE_DIR}/README.md"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/vectis")
  install(FILES
          "${CMAKE_BINARY_DIR}/generated/cmake/vectisConfig.cmake"
          "${CMAKE_BINARY_DIR}/generated/cmake/vectisConfigVersion.cmake"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vectis")
  if(EXISTS "${vectis_kore_include_dir}/kore/kore.h")
    install(DIRECTORY "${vectis_kore_include_dir}/kore"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  endif()
  install(DIRECTORY examples/
          DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/vectis/examples"
          FILES_MATCHING
          PATTERN "*.c"
          PATTERN "*.lua"
          PATTERN "README.md"
          PATTERN "CMakeLists.txt")
endif()
