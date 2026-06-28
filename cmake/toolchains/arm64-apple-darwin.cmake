set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(DEFINED ENV{OSXCROSS_ROOT} AND NOT "$ENV{OSXCROSS_ROOT}" STREQUAL "")
  set(VECTIS_OSXCROSS_ROOT "$ENV{OSXCROSS_ROOT}")
elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  set(VECTIS_OSXCROSS_ROOT "$ENV{HOME}/.local/cross/osxcross")
else()
  message(FATAL_ERROR "OSXCROSS_ROOT is not set and HOME is unavailable")
endif()

if(DEFINED ENV{VECTIS_OSXCROSS_HOST} AND NOT "$ENV{VECTIS_OSXCROSS_HOST}" STREQUAL "")
  set(_vectis_default_osxcross_host "$ENV{VECTIS_OSXCROSS_HOST}")
elseif(DEFINED ENV{CPKT_OSXCROSS_HOST} AND NOT "$ENV{CPKT_OSXCROSS_HOST}" STREQUAL "")
  set(_vectis_default_osxcross_host "$ENV{CPKT_OSXCROSS_HOST}")
else()
  set(_vectis_default_osxcross_host "arm64-apple-darwin25")
endif()
set(VECTIS_OSXCROSS_HOST "${_vectis_default_osxcross_host}" CACHE STRING "osxcross target host triple")
unset(_vectis_default_osxcross_host)
set(VECTIS_MACOS_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS deployment target")
set(CMAKE_OSX_DEPLOYMENT_TARGET "${VECTIS_MACOS_DEPLOYMENT_TARGET}" CACHE STRING "" FORCE)

set(VECTIS_OSXCROSS_BIN_DIR "${VECTIS_OSXCROSS_ROOT}/bin" CACHE PATH "osxcross tool directory")
set(ENV{PATH} "${VECTIS_OSXCROSS_BIN_DIR}:$ENV{PATH}")
set(CMAKE_C_COMPILER "${VECTIS_OSXCROSS_BIN_DIR}/${VECTIS_OSXCROSS_HOST}-clang" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${VECTIS_OSXCROSS_BIN_DIR}/${VECTIS_OSXCROSS_HOST}-clang++" CACHE FILEPATH "")
set(CMAKE_AR "${VECTIS_OSXCROSS_BIN_DIR}/${VECTIS_OSXCROSS_HOST}-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${VECTIS_OSXCROSS_BIN_DIR}/${VECTIS_OSXCROSS_HOST}-ranlib" CACHE FILEPATH "")
set(CMAKE_LINKER "${VECTIS_OSXCROSS_BIN_DIR}/${VECTIS_OSXCROSS_HOST}-ld" CACHE FILEPATH "")
if(EXISTS "${VECTIS_OSXCROSS_BIN_DIR}/${VECTIS_OSXCROSS_HOST}-install_name_tool")
  set(CMAKE_INSTALL_NAME_TOOL "${VECTIS_OSXCROSS_BIN_DIR}/${VECTIS_OSXCROSS_HOST}-install_name_tool" CACHE FILEPATH "")
endif()
set(VECTIS_OTOOL "${VECTIS_OSXCROSS_BIN_DIR}/${VECTIS_OSXCROSS_HOST}-otool" CACHE FILEPATH "")

foreach(_vectis_required_tool
        CMAKE_C_COMPILER
        CMAKE_AR
        CMAKE_RANLIB
        CMAKE_LINKER
        VECTIS_OTOOL)
  if(NOT EXISTS "${${_vectis_required_tool}}")
    message(FATAL_ERROR
      "The arm64 Apple Darwin osxcross toolchain is missing ${_vectis_required_tool}: "
      "${${_vectis_required_tool}}. Set OSXCROSS_ROOT or install osxcross under $HOME/.local/cross/osxcross.")
  endif()
endforeach()

set(_vectis_darwin_linker_flag "-fuse-ld=${CMAKE_LINKER}")
foreach(_vectis_linker_flags
        CMAKE_EXE_LINKER_FLAGS
        CMAKE_SHARED_LINKER_FLAGS
        CMAKE_MODULE_LINKER_FLAGS)
  if(NOT "${${_vectis_linker_flags}}" MATCHES "(^| )-fuse-ld=")
    set(${_vectis_linker_flags} "${_vectis_darwin_linker_flag} ${${_vectis_linker_flags}}" CACHE STRING "" FORCE)
  endif()
endforeach()
unset(_vectis_linker_flags)
unset(_vectis_darwin_linker_flag)

file(GLOB _vectis_osxcross_sdks LIST_DIRECTORIES true "${VECTIS_OSXCROSS_ROOT}/SDK/MacOSX*.sdk")
if(NOT _vectis_osxcross_sdks)
  message(FATAL_ERROR "failed to locate a usable osxcross macOS SDK under ${VECTIS_OSXCROSS_ROOT}/SDK")
endif()
list(SORT _vectis_osxcross_sdks)
list(REVERSE _vectis_osxcross_sdks)
list(GET _vectis_osxcross_sdks 0 VECTIS_OSXCROSS_SDK)
if(NOT EXISTS "${VECTIS_OSXCROSS_SDK}/usr/include")
  message(FATAL_ERROR "failed to locate a usable osxcross macOS SDK under ${VECTIS_OSXCROSS_ROOT}/SDK")
endif()

set(CMAKE_OSX_SYSROOT "${VECTIS_OSXCROSS_SDK}" CACHE PATH "" FORCE)
set(_vectis_find_roots "${VECTIS_OSXCROSS_SDK}")
if(DEFINED VECTIS_EXTERNAL_ROOT AND NOT "${VECTIS_EXTERNAL_ROOT}" STREQUAL "")
  list(APPEND _vectis_find_roots "${VECTIS_EXTERNAL_ROOT}")
endif()
set(CMAKE_FIND_ROOT_PATH "${_vectis_find_roots}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(VECTIS_TARGET_ARCH arm64 CACHE STRING "" FORCE)
set(VECTIS_TARGET_OS darwin CACHE STRING "" FORCE)
set(VECTIS_TARGET_LIBC "" CACHE STRING "" FORCE)
