cmake_minimum_required(VERSION 3.21)

project(vectis_install_consumer LANGUAGES C)

set(VECTIS_CONSUMER_LINK "static" CACHE STRING "Vectis imported target to link: static or shared.")

find_package(vectis CONFIG REQUIRED)

function(vectis_install_link_target target_name)
  if(VECTIS_CONSUMER_LINK STREQUAL "shared")
    if(NOT TARGET vectis::shared)
      message(FATAL_ERROR "vectis::shared is not available in this install tree")
    endif()
    target_link_libraries(${target_name} PRIVATE vectis::shared)
  elseif(VECTIS_CONSUMER_LINK STREQUAL "static")
    if(NOT TARGET vectis::static)
      message(FATAL_ERROR "vectis::static is not available in this install tree")
    endif()
    target_link_libraries(${target_name} PRIVATE vectis::static)
  else()
    message(FATAL_ERROR "VECTIS_CONSUMER_LINK must be static or shared")
  endif()
endfunction()

if(VECTIS_CONSUMER_LINK STREQUAL "static")
  get_target_property(vectis_static_links vectis::static INTERFACE_LINK_LIBRARIES)
  foreach(required_target IN ITEMS cai::cai_static pslog::pslog_static softline::softline_static CURL::libcurl OpenSSL::Crypto OpenSSL::SSL ZLIB::ZLIB)
    if(NOT required_target IN_LIST vectis_static_links)
      message(FATAL_ERROR "installed vectis::static is missing ${required_target}")
    endif()
  endforeach()
endif()

if(VECTIS_CONSUMER_LINK STREQUAL "static" AND UNIX AND NOT APPLE AND CMAKE_DL_LIBS)
  get_target_property(openssl_crypto_links OpenSSL::Crypto INTERFACE_LINK_LIBRARIES)
  if(NOT CMAKE_DL_LIBS IN_LIST openssl_crypto_links)
    message(FATAL_ERROR
      "OpenSSL::Crypto is missing CMAKE_DL_LIBS (${CMAKE_DL_LIBS})")
  endif()
endif()

add_executable(vectis_install_consumer consumer.c)
vectis_install_link_target(vectis_install_consumer)

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

include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
  enable_language(CXX)
  add_executable(vectis_install_consumer_cpp consumer_cpp.cpp)
  vectis_install_link_target(vectis_install_consumer_cpp)
  set_target_properties(vectis_install_consumer_cpp PROPERTIES
    CXX_STANDARD 98
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(vectis_install_consumer_cpp PRIVATE
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
else()
  message(STATUS "C++ compiler not available; skipping installed C++ consumer")
endif()
