cmake_minimum_required(VERSION 3.21)
project(vectis_runtime_fixture LANGUAGES C)

option(WITH_EXTRA "Build an optional local executable" ON)
add_executable(runtime_kept ../helpers/vectis_runtime_probe.c)
if(WITH_EXTRA)
  add_executable(runtime_removed ../helpers/vectis_runtime_probe.c)
endif()
