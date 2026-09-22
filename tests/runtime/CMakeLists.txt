cmake_minimum_required(VERSION 3.24)
project(vectis_runtime_fixture LANGUAGES C)

option(WITH_EXTRA "Build an optional local executable" ON)
option(VECTIS_BUILD_BINARY "CLI availability metadata for the runtime gate" OFF)
add_executable(runtime_kept ../helpers/vectis_runtime_probe.c)
if(WITH_EXTRA)
  add_executable(runtime_removed ../helpers/vectis_runtime_probe.c)
endif()
