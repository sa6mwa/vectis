cmake_minimum_required(VERSION 3.21)

project(vectis_install_consumer LANGUAGES C)

set(VECTIS_CONSUMER_LINK "static" CACHE STRING "Vectis imported target to link: static or shared.")

find_package(vectis CONFIG REQUIRED)

add_executable(vectis_install_consumer consumer.c)

if(VECTIS_CONSUMER_LINK STREQUAL "shared")
  if(NOT TARGET vectis::shared)
    message(FATAL_ERROR "vectis::shared is not available in this install tree")
  endif()
  target_link_libraries(vectis_install_consumer PRIVATE vectis::shared)
elseif(VECTIS_CONSUMER_LINK STREQUAL "static")
  if(NOT TARGET vectis::static)
    message(FATAL_ERROR "vectis::static is not available in this install tree")
  endif()
  target_link_libraries(vectis_install_consumer PRIVATE vectis::static)
else()
  message(FATAL_ERROR "VECTIS_CONSUMER_LINK must be static or shared")
endif()

set_target_properties(vectis_install_consumer PROPERTIES
  C_STANDARD 90
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(vectis_install_consumer PRIVATE
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
endif()
