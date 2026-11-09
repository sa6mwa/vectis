cmake_minimum_required(VERSION 3.24)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/vectis_owned_paths.cmake")
vectis_require_owned_path("${CMAKE_BINARY_DIR}" "Vectis build directory")

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR
    "Linux builds require a pinned toolchain. Use cmake --preset debug, or set CMAKE_TOOLCHAIN_FILE to a cmake/toolchains/<target>.cmake file; use a fresh build directory when changing compilers.")
endif()

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
option(VECTIS_BUILD_BENCHMARKS "Build Vectis performance benchmarks." OFF)
option(VECTIS_TEST_POSTGRES "Enable live rootless Podman Kube PostgreSQL XA tests." OFF)
option(VECTIS_BUILD_FUZZERS "Build fuzz targets." OFF)
option(VECTIS_WITH_KORE_RUNTIME "Build the embedded Kore runtime adapter." ON)
option(VECTIS_ENABLE_COVERAGE "Build with coverage instrumentation." OFF)
option(VECTIS_INSTALL "Enable install rules." ON)

if(NOT DEFINED CPKT_DEPENDENCY_CACHE OR CPKT_DEPENDENCY_CACHE STREQUAL "")
  if(DEFINED ENV{CPKT_DEPENDENCY_CACHE} AND NOT "$ENV{CPKT_DEPENDENCY_CACHE}" STREQUAL "")
    set(CPKT_DEPENDENCY_CACHE "$ENV{CPKT_DEPENDENCY_CACHE}")
  elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
    set(CPKT_DEPENDENCY_CACHE "$ENV{XDG_CACHE_HOME}/cpkt/deps")
  elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    set(CPKT_DEPENDENCY_CACHE "$ENV{HOME}/.cache/cpkt/deps")
  else()
    message(FATAL_ERROR
      "CPKT_DEPENDENCY_CACHE is not set and neither XDG_CACHE_HOME nor HOME is available")
  endif()
endif()
set(CPKT_DEPENDENCY_CACHE "${CPKT_DEPENDENCY_CACHE}" CACHE PATH
    "Shared verified dependency archive cache.")

set(VECTIS_EXTERNAL_ROOT "${CMAKE_SOURCE_DIR}/.cache/deps/host-debug"
    CACHE PATH "Provisioned third-party dependency root.")
set(VECTIS_DIST_DIR "${CMAKE_BINARY_DIR}/dist"
    CACHE PATH "Distribution output directory.")
vectis_require_owned_path("${VECTIS_DIST_DIR}" VECTIS_DIST_DIR "${CMAKE_SOURCE_DIR}/dist")
set(VECTIS_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}"
    CACHE STRING "Release target architecture.")
set(VECTIS_TARGET_OS "${CMAKE_SYSTEM_NAME}"
    CACHE STRING "Release target operating system.")
set(VECTIS_TARGET_LIBC ""
    CACHE STRING "Release target libc when applicable.")
set(VECTIS_TARGET_ID ""
    CACHE STRING "Release target identifier.")

string(TOLOWER "${VECTIS_TARGET_ARCH}" VECTIS_TARGET_ARCH_LOWER)
string(TOLOWER "${VECTIS_TARGET_OS}" VECTIS_TARGET_OS_LOWER)
string(TOLOWER "${VECTIS_TARGET_LIBC}" VECTIS_TARGET_LIBC_LOWER)
if(VECTIS_TARGET_ID STREQUAL "")
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

include("${CMAKE_SOURCE_DIR}/cmake/vectis_dependencies.cmake")

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

if(VECTIS_BUILD_FUZZERS AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  message(FATAL_ERROR "VECTIS_BUILD_FUZZERS=ON requires Clang or GNU.")
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

function(vectis_configure_c89_target target)
  # The lifecycle requires the explicit C89 spelling for every project-owned
  # C89 target instead of CMake's built-in language-standard mapping.
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(${target} PRIVATE
      -std=c89
      -Wall
      -Wextra
      -Wpedantic
      -pedantic-errors
    )
    # Project quality policy extends the lifecycle's portable C89 baseline.
    # Vectis retains long long for persisted 64-bit Lockd/CAI values.
    target_compile_options(${target} PRIVATE
      -Werror
      -Wno-long-long
      -Wstrict-prototypes
      -Wmissing-prototypes
      -Wshadow
      -Wpointer-arith
      -Wcast-qual
      -Wwrite-strings
      -Wdeclaration-after-statement
    )
  endif()
  if(UNIX OR APPLE OR CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(${target} PRIVATE _DARWIN_C_SOURCE)
  endif()
endfunction()

function(vectis_configure_c_target target)
  get_target_property(target_type ${target} TYPE)
  vectis_configure_c89_target(${target})
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

set(vectis_external_include_dir "${vectis_external_include_dirs}")
set(vectis_kore_include_dir "${CMAKE_SOURCE_DIR}/vendor/kore/upstream/include")
set(vectis_lockdc_lua_source_dir "${VECTIS_EXTERNAL_ROOT}/sources/liblockdc")
set(vectis_lonejson_lua_source_dir "${VECTIS_EXTERNAL_ROOT}/sources/lonejson")
set(vectis_pslog_lua_source_dir "${VECTIS_EXTERNAL_ROOT}/sources/pslog")
set(vectis_cai_lua_source_dir "${VECTIS_EXTERNAL_ROOT}/sources/cai")
set(vectis_liblql_lua_source_dir "${VECTIS_EXTERNAL_ROOT}/sources/liblql")
set(vectis_libmdf_lua_source_dir "${VECTIS_EXTERNAL_ROOT}/sources/libmdf")
set(vectis_softline_lua_source_dir
    "${VECTIS_EXTERNAL_ROOT}/sources/softline")

# Embed release-matched public documentation and Lua source in the CLI binary.
# Entries use source_path|display_path|module|origin and are intentionally
# configured at generate time so a source or documentation change reconfigures
# the build before recompiling vectis_cli.c.
function(vectis_generate_cli_resource_header output_path symbol output_include)
  set(vectis_cli_resource_entries "")
  set(vectis_cli_resource_sources "")
  set(vectis_cli_resource_offset 0)
  set(vectis_cli_resource_include
      "${CMAKE_BINARY_DIR}/generated/include/${symbol}_data.inc")
  set(vectis_cli_resource_inputs
      "${CMAKE_BINARY_DIR}/generated/${symbol}_inputs.cmake")

  foreach(vectis_cli_resource_entry IN LISTS ARGN)
    string(REPLACE "|" ";" vectis_cli_resource_fields
                   "${vectis_cli_resource_entry}")
    list(GET vectis_cli_resource_fields 0 vectis_cli_resource_source_path)
    list(GET vectis_cli_resource_fields 1 vectis_cli_resource_display_path)
    list(GET vectis_cli_resource_fields 2 vectis_cli_resource_module)
    list(GET vectis_cli_resource_fields 3 vectis_cli_resource_origin)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${vectis_cli_resource_source_path}")
    file(SIZE "${vectis_cli_resource_source_path}" vectis_cli_resource_size)
    file(SHA256 "${vectis_cli_resource_source_path}" vectis_cli_resource_sha256)
    list(APPEND vectis_cli_resource_sources
      "${vectis_cli_resource_source_path}")
    string(APPEND vectis_cli_resource_entries
      "  {\"${vectis_cli_resource_display_path}\", \"${vectis_cli_resource_module}\", \"${vectis_cli_resource_origin}\", \"${vectis_cli_resource_sha256}\", ${vectis_cli_resource_offset}u, ${vectis_cli_resource_size}u},\n")
    math(EXPR vectis_cli_resource_offset
         "${vectis_cli_resource_offset} + ${vectis_cli_resource_size}")
  endforeach()

  set(vectis_cli_resource_input_content "set(VECTIS_BYTE_ARRAY_INPUTS\n")
  foreach(vectis_cli_resource_source_path IN LISTS vectis_cli_resource_sources)
    file(TO_CMAKE_PATH "${vectis_cli_resource_source_path}"
         vectis_cli_resource_source_path)
    string(APPEND vectis_cli_resource_input_content
      "  \"${vectis_cli_resource_source_path}\"\n")
  endforeach()
  string(APPEND vectis_cli_resource_input_content ")\n")
  file(CONFIGURE OUTPUT "${vectis_cli_resource_inputs}"
    CONTENT "${vectis_cli_resource_input_content}" @ONLY)

  add_custom_command(
    OUTPUT "${vectis_cli_resource_include}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT_LIST_FILE=${vectis_cli_resource_inputs}"
            "-DOUTPUT=${vectis_cli_resource_include}"
            -P "${CMAKE_SOURCE_DIR}/cmake/vectis_generate_byte_array.cmake"
    DEPENDS ${vectis_cli_resource_sources}
            "${vectis_cli_resource_inputs}"
            # Metadata includes byte sizes, offsets and digests. An SDK upgrade
            # may replace sources with older archive mtimes at the same paths.
            # Its changed metadata must invalidate the matching byte blob too.
            "${output_path}"
            "${CMAKE_SOURCE_DIR}/cmake/vectis_generate_byte_array.cmake"
    VERBATIM
  )
  set(vectis_cli_resource_symbol "${symbol}")
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/vectis_cli_resources.h.in"
    "${output_path}"
    @ONLY
  )
  set(${output_include} "${vectis_cli_resource_include}" PARENT_SCOPE)
endfunction()

add_library(vectis_external_headers INTERFACE)
target_include_directories(vectis_external_headers INTERFACE "${vectis_external_include_dir}")
if(EXISTS "${vectis_kore_include_dir}/kore/kore.h")
  target_include_directories(vectis_external_headers INTERFACE "${vectis_kore_include_dir}")
endif()

set(vectis_lockdc_static_target lockdc::static)
set(vectis_lockdc_shared_target lockdc::shared)

set(vectis_static_dependency_links
  cpkt::pdf
  cpkt::png
  cpkt::zstd
  cpkt::lz4
  cpkt::lzma
  cpkt::zlibng
  cpkt::sqlite
  cpkt::postgres
  lockdc::postgres_static
  cpkt::opcua
  cpkt::sus
  cpkt::audio
  cai::cai_static
  libmdf::mdf_static
  softline::softline_static
  pslog::pslog_static
  CURL::libcurl
  Libssh2::libssh2
  OpenSSL::SSL
  OpenSSL::Crypto
  lonejson::lonejson_static
  lonejson::openssl
  lonejson::oidc
  liblql::lql_static
  LibXml2::LibXml2
  nghttp2::nghttp2
  ZLIB::ZLIB
)
set(vectis_shared_dependency_links
  cpkt::pdf_shared
  cpkt::png_shared
  cpkt::zstd_shared
  cpkt::lz4_shared
  cpkt::lzma_shared
  cpkt::zlibng_facade_shared
  cpkt::sqlite_shared
  cpkt::postgres_shared
  lockdc::postgres_shared
  cpkt::opcua_shared
  cpkt::sus_shared
  cpkt::audio_shared
  cai::cai_shared
  libmdf::mdf_shared
  softline::softline
  pslog::pslog_shared
  cpkt::curl_shared
  cpkt::libssh2_shared
  cpkt::openssl_ssl_shared
  cpkt::openssl_crypto_shared
  lonejson::lonejson
  lonejson::openssl
  lonejson::oidc
  liblql::lql_shared
  cpkt::libxml2_shared
  cpkt::nghttp2_shared
  cpkt::zlib_shared
)

set(vectis_public_headers
  include/vectis/pdf.h
  include/vectis/curl.h
  include/vectis/xml.h
  include/vectis/crypto.h
  include/vectis/png.h
  include/vectis/compression.h
  include/vectis/sql.h
  include/vectis/sqlite.h
  include/vectis/storage.h
  include/vectis/pg.h
  include/vectis/vectis.h
  include/vectis/proxy.h
  include/vectis/auth.h
  include/vectis/embedded_fs.h
  include/vectis/totp_qr.h
  include/vectis/webdav.h
  include/vectis/supervisor.h
  include/vectis/digest.h
  include/vectis/buffer.h
  include/vectis/zlib.h
  include/vectis/zlib_raw.h
  include/vectis/gz.h
  "${CMAKE_BINARY_DIR}/generated/include/vectis/vectis_version.h"
)

set(vectis_sources
  src/vectis_pdf.c
  src/vectis_curl.c
  src/vectis_xml.c
  src/vectis_xml_sax.c
  src/vectis_crypto.c
  src/vectis_png.c
  src/vectis_compression.c
  src/vectis_sqlite.c
  src/vectis.c
  src/vectis_supervisor.c
  src/vectis_storage_wait.c
  src/vectis_pg.c
  src/vectis_pg_connection.c
  src/vectis_digest.c
  src/vectis_buffer.c
  src/vectis_zlib.c
  src/vectis_zlib_raw.c
  src/vectis_gz.c
  src/vectis_acme_state.c
  src/vectis_auth.c
  src/vectis_embedded_fs.c
  src/vectis_proxy_route.c
  src/vectis_proxy_director.c
  src/vectis_proxy_response.c
  src/vectis_proxy_local.c
  src/vectis_proxy_headers.c
  src/vectis_proxy_framing.c
  src/vectis_proxy_url.c
  src/vectis_smith.c
  src/vectis_totp_qr.c
  src/vectis_webdav.c
)

set(vectis_kore_objects)
if(VECTIS_WITH_KORE_RUNTIME)
  set(vectis_kore_root "${CMAKE_SOURCE_DIR}/vendor/kore/upstream")
  if(NOT EXISTS "${vectis_kore_root}/src/kore.c")
    message(FATAL_ERROR "VECTIS_WITH_KORE_RUNTIME=ON requires vendor/kore/upstream.")
  endif()
  set(vectis_kore_generated_dir "${CMAKE_BINARY_DIR}/kore-generated")
  file(MAKE_DIRECTORY "${vectis_kore_generated_dir}")
  file(CONFIGURE OUTPUT "${vectis_kore_generated_dir}/version.c" CONTENT
    "const char *kore_version = \"vectis-embedded\";\nconst char *kore_build_date = \"@CMAKE_PROJECT_VERSION@\";\n" @ONLY)
  file(CONFIGURE OUTPUT "${vectis_kore_generated_dir}/assets.c" CONTENT
    "#include <sys/types.h>\nu_int8_t asset_builtin_kore_conf[] = {0};\nu_int32_t asset_len_builtin_kore_conf = 0;\n" @ONLY)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${vectis_kore_root}/misc/linux-platform.sh")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env "TARGET_PLATFORM=${VECTIS_TARGET_ARCH}"
              "${vectis_kore_root}/misc/linux-platform.sh"
      WORKING_DIRECTORY "${vectis_kore_root}"
      OUTPUT_VARIABLE vectis_kore_platform_content
      RESULT_VARIABLE vectis_kore_platform_result
    )
    if(NOT vectis_kore_platform_result EQUAL 0)
      message(FATAL_ERROR "failed to generate Kore Linux platform header")
    endif()
    file(CONFIGURE OUTPUT "${vectis_kore_generated_dir}/platform.h"
      CONTENT "${vectis_kore_platform_content}" @ONLY)
    set(vectis_kore_platform_sources
      "${vectis_kore_root}/src/linux.c"
      "${vectis_kore_root}/src/seccomp.c"
    )
  else()
    file(CONFIGURE OUTPUT "${vectis_kore_generated_dir}/platform.h" CONTENT "" @ONLY)
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
    "${CMAKE_SOURCE_DIR}/src"
    "${vectis_external_include_dir}"
  )
  target_compile_definitions(vectis_kore_runtime PRIVATE
    KORE_SINGLE_BINARY
    KORE_USE_ACME
    KORE_USE_CURL
    KORE_USE_LONEJSON
    KORE_USE_PSLOG
    KORE_VECTIS_STATIC_RUNTIME
    KORE_VECTIS_EMBEDDED_RUNTIME
    KORE_VECTIS_AUTOBLOCK_HOOKS
    KORE_VECTIS_ACME_STATE_HOOK
    KORE_VECTIS_DIRECT_TLS
    KORE_VECTIS_NO_SECCOMP
    KORE_TMPDIR="/tmp"
    TLS_BACKEND_OPENSSL
    main=vectis_kore_main
    PREFIX="/usr/local"
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(vectis_kore_runtime PRIVATE
      _GNU_SOURCE=1
      "$<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>"
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
    set_source_files_properties(
      "${vectis_kore_generated_dir}/version.c"
      "${vectis_kore_generated_dir}/assets.c"
      PROPERTIES COMPILE_OPTIONS "-Werror"
    )
  endif()
  vectis_apply_relocatable_paths(vectis_kore_runtime)
  list(APPEND vectis_sources
    src/vectis_kore_bridge.c
    src/vectis_kore_proxy.c
    src/vectis_kore_proxy_local.c
    src/vectis_kore_proxy_ws.c
    src/vectis_proxy_events.c
    src/vectis_proxy_telemetry.c
    src/vectis_proxy_curl.c
    src/vectis_proxy_select.c
    src/vectis_proxy_http.c
    src/vectis_proxy_http_upstream.c
    src/vectis_proxy_http_wire.c
    src/vectis_proxy_upload.c
    src/vectis_proxy_ws_handshake.c
    src/vectis_proxy_ws_wire.c
    src/vectis_proxy_ws_rejection.c
    $<TARGET_OBJECTS:vectis_kore_runtime>)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(src/vectis_kore_bridge.c PROPERTIES
      COMPILE_OPTIONS "-std=gnu99;-Wno-pedantic;-Wno-long-long;-Wno-cast-qual;-Wno-missing-prototypes"
      COMPILE_DEFINITIONS "KORE_USE_ACME;KORE_USE_CURL;KORE_USE_LONEJSON;KORE_USE_PSLOG;KORE_VECTIS_STATIC_RUNTIME;KORE_VECTIS_DIRECT_TLS;KORE_VECTIS_NO_SECCOMP;KORE_VECTIS_ACME_STATE_HOOK;TLS_BACKEND_OPENSSL"
    )
    set_source_files_properties(src/vectis_kore_proxy.c PROPERTIES
      COMPILE_DEFINITIONS "KORE_USE_ACME;KORE_USE_CURL;KORE_USE_LONEJSON;KORE_USE_PSLOG;KORE_VECTIS_STATIC_RUNTIME;KORE_VECTIS_DIRECT_TLS;KORE_VECTIS_NO_SECCOMP;KORE_VECTIS_ACME_STATE_HOOK;TLS_BACKEND_OPENSSL"
    )
  endif()
else()
  list(APPEND vectis_sources src/vectis_kore_stub.c)
endif()

set(vectis_private_includes
  "${CMAKE_SOURCE_DIR}/src"
)

# Keep linker controls generated from the source-controlled dynamic export
# allowlist instead of relying on default compiler visibility or implementation
# naming conventions.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/cmake/vectis.exports"
  "${CMAKE_SOURCE_DIR}/cmake/vectis_export_policy.cmake")
set(vectis_export_policy_dir "${CMAKE_BINARY_DIR}/generated/exports")
file(MAKE_DIRECTORY "${vectis_export_policy_dir}")
set(vectis_export_policy_args
  "-DVECTIS_EXPORT_LIST=${CMAKE_SOURCE_DIR}/cmake/vectis.exports"
  "-DVECTIS_EXPORT_ELF_MAP=${vectis_export_policy_dir}/vectis.map"
  "-DVECTIS_EXPORT_DARWIN_LIST=${vectis_export_policy_dir}/vectis.exports"
  "-DVECTIS_EXPORT_EXPECTED_LIST=${vectis_export_policy_dir}/vectis.symbols"
  -P "${CMAKE_SOURCE_DIR}/cmake/vectis_export_policy.cmake")
execute_process(
  COMMAND "${CMAKE_COMMAND}" ${vectis_export_policy_args}
  RESULT_VARIABLE vectis_export_policy_result
  ERROR_VARIABLE vectis_export_policy_error)
if(NOT vectis_export_policy_result EQUAL 0)
  message(FATAL_ERROR
    "failed to generate the Vectis shared-library export policy: "
    "${vectis_export_policy_error}")
endif()

configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/vectis_version.h.in"
  "${CMAKE_BINARY_DIR}/generated/include/vectis/vectis_version.h"
  @ONLY
)

# Public headers get their own normal C89 compile commands, including clangd's
# ordinary header discovery. No editor-only dependency or generated-header lane.
set(vectis_compilable_public_headers ${vectis_public_headers})
# The version header contains macros only; validate it through a real C TU.
list(FILTER vectis_compilable_public_headers EXCLUDE REGEX "/generated/")
set_source_files_properties(${vectis_compilable_public_headers} PROPERTIES LANGUAGE C HEADER_FILE_ONLY FALSE)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/generated/version_header_check.c"
  CONTENT "#include <vectis/vectis_version.h>\ntypedef char vectis_version_header_check;\n")
add_library(vectis_public_header_checks OBJECT ${vectis_compilable_public_headers}
  "${CMAKE_BINARY_DIR}/generated/version_header_check.c")
target_include_directories(vectis_public_header_checks PRIVATE
  "${CMAKE_SOURCE_DIR}/include" "${CMAKE_BINARY_DIR}/generated/include")
target_link_libraries(vectis_public_header_checks PRIVATE vectis_external_headers)
vectis_configure_c89_target(vectis_public_header_checks)
set_source_files_properties(${vectis_sources} ${vectis_public_headers}
  PROPERTIES OBJECT_DEPENDS "${vectis_dependency_manifest}")

if(VECTIS_BUILD_STATIC)
  add_library(vectis_static STATIC ${vectis_sources})
  add_dependencies(vectis_static vectis_public_header_checks)
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
      ${vectis_external_library_dirs}
  )
  target_link_libraries(vectis_static
    PUBLIC
      Threads::Threads
      vectis_external_headers
      ${vectis_lockdc_static_target}
      ${vectis_static_dependency_links}
  )
  target_compile_definitions(vectis_static
    PRIVATE VECTIS_WITH_KORE_RUNTIME=$<BOOL:${VECTIS_WITH_KORE_RUNTIME}>)
  if(VECTIS_WITH_KORE_RUNTIME AND UNIX AND NOT APPLE)
    target_link_options(vectis_static INTERFACE -Wl,--export-dynamic)
  endif()
  if(VECTIS_BUILD_FUZZERS)
    target_compile_definitions(vectis_static PRIVATE VECTIS_BUILD_FUZZERS)
  endif()
  set_target_properties(vectis_static PROPERTIES OUTPUT_NAME vectis)
  vectis_configure_c_target(vectis_static)
endif()

if(VECTIS_BUILD_SHARED)
  add_library(vectis_shared SHARED ${vectis_sources})
  add_dependencies(vectis_shared vectis_public_header_checks)
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
      ${vectis_external_library_dirs}
  )
  target_link_libraries(vectis_shared
    PUBLIC
      Threads::Threads
      vectis_external_headers
      ${vectis_lockdc_shared_target}
      ${vectis_shared_dependency_links}
  )
  target_compile_definitions(vectis_shared
    PRIVATE VECTIS_WITH_KORE_RUNTIME=$<BOOL:${VECTIS_WITH_KORE_RUNTIME}>)
  set_target_properties(vectis_shared PROPERTIES
    OUTPUT_NAME vectis
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
  )
  if(APPLE)
    set(vectis_loader_origin "@loader_path")
  else()
    set(vectis_loader_origin "$ORIGIN")
  endif()
  set(vectis_installed_rpaths "${vectis_loader_origin}")
  foreach(prefix IN LISTS vectis_dependency_prefixes)
    if(IS_DIRECTORY "${prefix}/lib")
      get_filename_component(name "${prefix}" NAME)
      list(APPEND vectis_installed_rpaths "${vectis_loader_origin}/../deps/${name}/lib")
    endif()
  endforeach()
  set_target_properties(vectis_shared PROPERTIES INSTALL_RPATH "${vectis_installed_rpaths}")
  if(APPLE)
    set_target_properties(vectis_shared PROPERTIES
      MACOSX_RPATH ON
      BUILD_WITH_INSTALL_NAME_DIR ON
      BUILD_WITH_INSTALL_RPATH TRUE
      INSTALL_NAME_DIR "@rpath"
    )
    target_link_options(vectis_shared PRIVATE
      "-Wl,-exported_symbols_list,${vectis_export_policy_dir}/vectis.exports")
  elseif(UNIX)
    target_link_options(vectis_shared PRIVATE
      # Provider-owned peers retain their payloads. Relative RPATH also
      # resolves their transitive dependencies after SDK relocation.
      "LINKER:--disable-new-dtags"
      "LINKER:--version-script=${vectis_export_policy_dir}/vectis.map")
  endif()
  if(VECTIS_BUILD_FUZZERS)
    target_compile_definitions(vectis_shared PRIVATE VECTIS_BUILD_FUZZERS)
  endif()
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
configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/vectis.pc.in"
  "${CMAKE_BINARY_DIR}/generated/pkgconfig/vectis.pc"
  @ONLY
)

if(VECTIS_BUILD_STATIC)
  set(vectis_test_link_target vectis_static)
elseif(VECTIS_BUILD_SHARED)
  set(vectis_test_link_target vectis_shared)
endif()

if(VECTIS_BUILD_BINARY)
  set(vectis_libmdf_lua_source "${vectis_libmdf_lua_source_dir}/lua/libmdf_core.c")
  set(vectis_libmdf_lua_module "${vectis_libmdf_lua_source_dir}/lua/libmdf.lua")
  set(vectis_pslog_lua_source "${vectis_pslog_lua_source_dir}/lua/src/pslog_lua.c")
  set(vectis_pslog_lua_init "${vectis_pslog_lua_source_dir}/lua/pslog/init.lua")
  file(GLOB_RECURSE vectis_cli_doc_files CONFIGURE_DEPENDS
       RELATIVE "${CMAKE_SOURCE_DIR}/manual" "${CMAKE_SOURCE_DIR}/manual/*.md")
  set(vectis_cli_doc_resources
    "${CMAKE_SOURCE_DIR}/README.md|README.md||vectis")
  foreach(vectis_cli_doc_file IN LISTS vectis_cli_doc_files)
    list(APPEND vectis_cli_doc_resources
      "${CMAKE_SOURCE_DIR}/manual/${vectis_cli_doc_file}|manual/${vectis_cli_doc_file}||vectis")
  endforeach()
  vectis_generate_cli_resource_header(
    "${CMAKE_BINARY_DIR}/generated/include/vectis_cli_docs.h"
    "vectis_cli_docs"
    vectis_cli_docs_resource_include
    ${vectis_cli_doc_resources}
  )

  set(vectis_cli_lua_resources
    "${CMAKE_SOURCE_DIR}/lua/vectis/pdf.lua|vectis/pdf.lua|vectis.pdf|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/png.lua|vectis/png.lua|vectis.png|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/compression.lua|vectis/compression.lua|vectis.compression|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/sql.lua|vectis/sql.lua|vectis.sql|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/sqlite.lua|vectis/sqlite.lua|vectis.sqlite|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis.lua|vectis.lua|vectis|vectis"
    "${CMAKE_SOURCE_DIR}/lua/curl/init.lua|curl/init.lua|curl|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/auth.lua|vectis/auth.lua|vectis.auth|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/cai.lua|vectis/cai.lua|vectis.cai|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/cli.lua|vectis/cli.lua|vectis.cli|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/smith.lua|vectis/smith.lua|vectis.smith|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/dsv.lua|vectis/dsv.lua|vectis.dsv|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/http.lua|vectis/http.lua|vectis.http|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/kore.lua|vectis/kore.lua|vectis.kore|vectis standalone fallback"
    "${CMAKE_SOURCE_DIR}/lua/vectis/supervisor.lua|vectis/supervisor.lua|vectis.supervisor|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/workflow.lua|vectis/workflow.lua|vectis.workflow|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/storage.lua|vectis/storage.lua|vectis.storage|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/pg.lua|vectis/pg.lua|vectis.pg|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/lockd.lua|vectis/lockd.lua|vectis.lockd|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/log.lua|vectis/log.lua|vectis.log|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/mqtt.lua|vectis/mqtt.lua|vectis.mqtt|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/rest.lua|vectis/rest.lua|vectis.rest|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/smtp.lua|vectis/smtp.lua|vectis.smtp|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/status.lua|vectis/status.lua|vectis.status|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/terminal.lua|vectis/terminal.lua|vectis.terminal|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/version.lua|vectis/version.lua|vectis.version|vectis standalone fallback"
    "${CMAKE_SOURCE_DIR}/lua/vectis/webdav.lua|vectis/webdav.lua|vectis.webdav|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/xml.lua|vectis/xml.lua|vectis.xml|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/digest.lua|vectis/digest.lua|vectis.digest|vectis"
    "${CMAKE_SOURCE_DIR}/lua/vectis/buffer.lua|vectis/buffer.lua|vectis.buffer|vectis"
    "${vectis_lockdc_lua_source_dir}/lua/lockdc/init.lua|lockdc/init.lua|lockdc|liblockdc"
    "${vectis_lonejson_lua_source_dir}/lua/lonejson/init.lua|lonejson/upstream.lua|lonejson.upstream|lonejson"
    "${CMAKE_SOURCE_DIR}/lua/lonejson/init.lua|lonejson/init.lua|lonejson|vectis compatibility facade"
    "${vectis_pslog_lua_init}|pslog/init.lua|pslog|pslog"
    "${vectis_liblql_lua_source_dir}/lua/lql/init.lua|lql/init.lua|lql|liblql"
    "${vectis_libmdf_lua_module}|libmdf.lua|libmdf|libmdf"
  )
  vectis_generate_cli_resource_header(
    "${CMAKE_BINARY_DIR}/generated/include/vectis_cli_lua_sources.h"
    "vectis_cli_lua_sources"
    vectis_cli_lua_sources_resource_include
    ${vectis_cli_lua_resources}
  )
  set(vectis_cli_resource_source
      "${CMAKE_BINARY_DIR}/generated/vectis_cli_resources.c")
  file(GENERATE OUTPUT "${vectis_cli_resource_source}" CONTENT
"#include <stddef.h>

const unsigned char vectis_cli_docs_data[] = {
#include \"vectis_cli_docs_data.inc\"
};

const unsigned char vectis_cli_lua_sources_data[] = {
#include \"vectis_cli_lua_sources_data.inc\"
};
")
  set_source_files_properties("${vectis_cli_resource_source}" PROPERTIES
    GENERATED TRUE
    OBJECT_DEPENDS
      "${vectis_cli_docs_resource_include};${vectis_cli_lua_sources_resource_include}"
  )
  add_library(vectis_cli_resources OBJECT "${vectis_cli_resource_source}")
  target_include_directories(vectis_cli_resources PRIVATE
    "${CMAKE_BINARY_DIR}/generated/include")
  vectis_configure_c89_target(vectis_cli_resources)
  set(vectis_lockdc_lua_source
    "${vectis_lockdc_lua_source_dir}/src/lua/lockdc_lua.c")
  set(vectis_lonejson_lua_source
    "${vectis_lonejson_lua_source_dir}/src/lua/lonejson_lua.c")
  set(vectis_cai_lua_source "${vectis_cai_lua_source_dir}/lua/cai_lua.c")
  set(vectis_liblql_lua_source "${vectis_liblql_lua_source_dir}/lua/lql_core.c")
  set(vectis_softline_lua_source
    "${vectis_softline_lua_source_dir}/lua/softline_lua.c")
  set(vectis_opcua_lua_source "${CMAKE_SOURCE_DIR}/src/vectis_opcua_lua.c")
  set(vectis_audio_lua_source "${CMAKE_SOURCE_DIR}/src/vectis_audio_lua.c")
  set(vectis_sus_lua_source "${CMAKE_SOURCE_DIR}/src/vectis_sus_lua.c")
  set(vectis_embedded_lua_c_sources
    "${vectis_lockdc_lua_source}"
    "${vectis_lonejson_lua_source}"
    "${vectis_pslog_lua_source}"
    "${vectis_cai_lua_source}"
    "${vectis_liblql_lua_source}"
    "${vectis_libmdf_lua_source}"
    "${vectis_softline_lua_source}"
    "${vectis_opcua_lua_source}"
    "${vectis_audio_lua_source}"
    "${vectis_sus_lua_source}"
  )
  set_source_files_properties(${vectis_embedded_lua_c_sources} PROPERTIES
    OBJECT_DEPENDS "${vectis_dependency_manifest}"
  )
  add_library(vectis_lockdc_lua OBJECT "${vectis_lockdc_lua_source}"
    "${vectis_lockdc_lua_source_dir}/src/lua/lockdc_collections_lua.c"
    "${vectis_lockdc_lua_source_dir}/src/lua/lockdc_xa_lua.c"
    "${vectis_lockdc_lua_source_dir}/src/lua/lockdc_xa_resource_lua.c"
    "${vectis_lockdc_lua_source_dir}/src/lua/lockdc_xa_provider_lua.c"
    "${vectis_lockdc_lua_source_dir}/src/lua/lockdc_postgres_lua.c")
  target_include_directories(vectis_lockdc_lua PRIVATE
    "${vectis_external_include_dir}"
    "${vectis_lockdc_lua_source_dir}/src"
    "${vectis_lockdc_lua_source_dir}/src/lua"
  )
  target_compile_definitions(vectis_lockdc_lua PRIVATE
    lc_client_open=vectis_lua_lockdc_client_open
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
  target_link_libraries(vectis_lonejson_lua PRIVATE
    lonejson::openssl lonejson::oidc lonejson::curl)
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
  set(vectis_lonejson_lua_localized_object
    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/vectis_lonejson_lua_private.dir/lonejson_lua.localized${CMAKE_C_OUTPUT_EXTENSION}")
  add_custom_command(
    OUTPUT "${vectis_lonejson_lua_private_object}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/vectis_lonejson_lua_private.dir"
    COMMAND "${vectis_objcopy}" --wildcard --localize-symbol=lonejson_*
            $<TARGET_OBJECTS:vectis_lonejson_lua>
            "${vectis_lonejson_lua_localized_object}"
    COMMAND "${vectis_objcopy}" --wildcard --globalize-symbol=lonejson_lua_*
            "${vectis_lonejson_lua_localized_object}"
            "${vectis_lonejson_lua_private_object}"
    DEPENDS vectis_lonejson_lua $<TARGET_OBJECTS:vectis_lonejson_lua>
            "${vectis_dependency_manifest}"
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

  add_library(vectis_pslog_lua OBJECT "${vectis_pslog_lua_source}")
  target_include_directories(vectis_pslog_lua PRIVATE
    "${vectis_external_include_dir}"
    "${vectis_pslog_lua_source_dir}/include"
  )
  target_compile_definitions(vectis_pslog_lua PRIVATE
    _POSIX_C_SOURCE=200809L)
  set_target_properties(vectis_pslog_lua PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  vectis_apply_relocatable_paths(vectis_pslog_lua)

  add_library(vectis_cai_lua OBJECT "${vectis_cai_lua_source}")
  target_include_directories(vectis_cai_lua PRIVATE
    "${vectis_external_include_dir}"
    "${vectis_pslog_lua_source_dir}/include"
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

  add_library(vectis_liblql_lua OBJECT "${vectis_liblql_lua_source}")
  target_include_directories(vectis_liblql_lua PRIVATE
    ${vectis_external_include_dirs})
  target_compile_definitions(vectis_liblql_lua PRIVATE
    LUA_COMPAT_5_4=1)
  set_target_properties(vectis_liblql_lua PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    COMPILE_WARNING_AS_ERROR ON)
  vectis_apply_relocatable_paths(vectis_liblql_lua)

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

  add_library(vectis_buffer_lua OBJECT src/vectis_lua_buffer.c)
  target_include_directories(vectis_buffer_lua PRIVATE
    "${vectis_external_include_dir}" "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_BINARY_DIR}/generated/include" "${CMAKE_SOURCE_DIR}/src")
  vectis_configure_c89_target(vectis_buffer_lua)
  set_target_properties(vectis_buffer_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
  vectis_apply_relocatable_paths(vectis_buffer_lua)

  add_library(vectis_softline_lua OBJECT "${vectis_softline_lua_source}")
  target_include_directories(vectis_softline_lua PRIVATE
    "${vectis_external_include_dir}"
    "${vectis_softline_lua_source_dir}/include"
  )
  target_compile_definitions(vectis_softline_lua PRIVATE
    _POSIX_C_SOURCE=200809L)
  set_target_properties(vectis_softline_lua PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  vectis_apply_relocatable_paths(vectis_softline_lua)

  add_library(vectis_opcua_lua OBJECT "${vectis_opcua_lua_source}")
  target_include_directories(vectis_opcua_lua PRIVATE
    "${vectis_pslog_lua_source_dir}/include"
    "${CMAKE_SOURCE_DIR}/include"
    "${vectis_external_include_dir}"
  )
  target_compile_definitions(vectis_opcua_lua PRIVATE
    _POSIX_C_SOURCE=200809L)
  set_target_properties(vectis_opcua_lua PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_opcua_lua PRIVATE
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
  vectis_apply_relocatable_paths(vectis_opcua_lua)

  add_library(vectis_audio_lua OBJECT "${vectis_audio_lua_source}")
  target_include_directories(vectis_audio_lua PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${vectis_external_include_dir}"
  )
  target_compile_definitions(vectis_audio_lua PRIVATE
    _POSIX_C_SOURCE=200809L)
  set_target_properties(vectis_audio_lua PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_audio_lua PRIVATE
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
  vectis_apply_relocatable_paths(vectis_audio_lua)

  add_library(vectis_sus_lua OBJECT "${vectis_sus_lua_source}")
  target_include_directories(vectis_sus_lua PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${vectis_external_include_dir}"
  )
  target_compile_definitions(vectis_sus_lua PRIVATE
    _POSIX_C_SOURCE=200809L)
  set_target_properties(vectis_sus_lua PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
  )
  if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_sus_lua PRIVATE
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
  vectis_apply_relocatable_paths(vectis_sus_lua)

  set(vectis_binary_runner_sources
    src/vectis_lua_pdf.c
    src/vectis_lua_curl.c
    src/vectis_lua_xml_objects.c
    src/vectis_lua_crypto.c
    src/vectis_lua_png.c
    src/vectis_lua_compression.c
    src/vectis_lua_sqlite.c
    src/vectis_cli.c
    src/vectis_lua_supervisor.c
    src/vectis_lua_digest.c
    src/vectis_lua_pg.c
    src/vectis_lua_pg_connection.c
    $<TARGET_OBJECTS:vectis_buffer_lua>
    src/vectis_lua_zlib_stream.c
    src/vectis_lua_zlib_raw.c
    src/vectis_lua_gz.c
    src/vectis_lua_pouch.c
    src/vectis_lua_lockdc.c
    src/vectis_cli_proxy_lua.c
    src/vectis_smith_cli.c
    src/vectis_main.c
    $<TARGET_OBJECTS:vectis_cli_resources>
    $<TARGET_OBJECTS:vectis_lockdc_lua>
    $<TARGET_OBJECTS:vectis_pslog_lua>
    $<TARGET_OBJECTS:vectis_cai_lua>
    $<TARGET_OBJECTS:vectis_liblql_lua>
    $<TARGET_OBJECTS:vectis_libmdf_lua>
    $<TARGET_OBJECTS:vectis_softline_lua>
    $<TARGET_OBJECTS:vectis_opcua_lua>
    $<TARGET_OBJECTS:vectis_audio_lua>
    $<TARGET_OBJECTS:vectis_sus_lua>
  )
  add_executable(vectis_bin ${vectis_binary_runner_sources})
  target_include_directories(vectis_bin PRIVATE "${vectis_lockdc_lua_source_dir}/src/lua")
  target_include_directories(vectis_bin PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated/include"
    "${vectis_external_include_dir}"
    "${vectis_pslog_lua_source_dir}/include"
    "${vectis_lonejson_lua_source_dir}/src/lua"
  )
  if(VECTIS_BUILD_STATIC)
    target_link_libraries(vectis_bin PRIVATE vectis_static)
  elseif(VECTIS_BUILD_SHARED)
    target_link_libraries(vectis_bin PRIVATE vectis_shared)
  endif()
  target_link_libraries(vectis_bin PRIVATE
    cpkt::lua_runtime
    cpkt::opcua
    cpkt::audio
    cpkt::sus
    cai::cai_static
    libmdf::mdf_static
    pslog::pslog_static
    softline::softline_static
    vectis_lonejson_lua_archive)
  if(UNIX AND NOT APPLE)
    target_link_libraries(vectis_bin PRIVATE m)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(vectis_bin PRIVATE -static -no-pie)
  endif()
  set_target_properties(vectis_bin PROPERTIES
    OUTPUT_NAME vectis
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
  )
  if(APPLE)
    set_target_properties(vectis_bin PROPERTIES
      SKIP_BUILD_RPATH TRUE
    )
  endif()
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
  target_compile_definitions(vectis_bin
    PRIVATE VECTIS_WITH_KORE_RUNTIME=$<BOOL:${VECTIS_WITH_KORE_RUNTIME}>)
  vectis_apply_relocatable_paths(vectis_bin)

endif()

if(VECTIS_BUILD_TESTS OR VECTIS_BUILD_FUZZERS)
  enable_testing()
  add_subdirectory(tests)
  if(VECTIS_BUILD_SHARED)
    add_test(NAME vectis_shared_export_policy
      COMMAND "${CMAKE_COMMAND}"
              "-DVECTIS_EXPORT_LIST=${CMAKE_SOURCE_DIR}/cmake/vectis.exports"
              "-DVECTIS_EXPORT_LIBRARY=$<TARGET_FILE:vectis_shared>"
              "-DVECTIS_EXPORT_NM=${CMAKE_NM}"
              "-DVECTIS_EXPORT_DARWIN=$<BOOL:${APPLE}>"
              -P "${CMAKE_SOURCE_DIR}/cmake/vectis_export_policy.cmake")
    set_tests_properties(vectis_shared_export_policy PROPERTIES
      LABELS "unit;packaging;local"
      TIMEOUT 30)
  endif()
endif()

if(VECTIS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(VECTIS_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

include(cmake/test_runtime.cmake)

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
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
      message(STATUS "Local development CLI is not installed; use the static Release CLI for distribution")
    else()
      install(TARGETS vectis_bin RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
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
  install(FILES
          "${CMAKE_BINARY_DIR}/generated/pkgconfig/vectis.pc"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
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
