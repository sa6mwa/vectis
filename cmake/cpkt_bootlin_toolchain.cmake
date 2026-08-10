function(cpkt_configure_bootlin_toolchain target_id)
  set(cpkt_repo_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
  set(cpkt_resolver "${cpkt_repo_root}/scripts/cpkt-toolchains.sh")
  if(NOT EXISTS "${cpkt_resolver}")
    message(FATAL_ERROR "missing Bootlin resolver: ${cpkt_resolver}")
  endif()

  execute_process(
    COMMAND "${cpkt_resolver}" ensure "${target_id}"
    RESULT_VARIABLE cpkt_ensure_result
    OUTPUT_QUIET
    ERROR_VARIABLE cpkt_ensure_error
  )
  if(NOT cpkt_ensure_result EQUAL 0)
    message(FATAL_ERROR
      "unable to provision pinned Bootlin toolchain for ${target_id}: ${cpkt_ensure_error}")
  endif()

  execute_process(
    COMMAND "${cpkt_resolver}" discover "${target_id}"
    RESULT_VARIABLE cpkt_discover_result
    OUTPUT_VARIABLE cpkt_description
    ERROR_VARIABLE cpkt_discover_error
  )
  if(NOT cpkt_discover_result EQUAL 0)
    message(FATAL_ERROR
      "unable to inspect pinned Bootlin toolchain for ${target_id}: ${cpkt_discover_error}")
  endif()

  foreach(key cc cxx ld ar ranlib strip nm objcopy objdump addr2line readelf sysroot root)
    string(REGEX MATCH "(^|\n)${key}=([^\r\n]+)" cpkt_match "${cpkt_description}")
    if(NOT cpkt_match)
      message(FATAL_ERROR
        "Bootlin resolver did not report ${key} for ${target_id}")
    endif()
    set(cpkt_${key} "${CMAKE_MATCH_2}")
  endforeach()

  set(CMAKE_C_COMPILER "${cpkt_cc}" CACHE FILEPATH "" FORCE)
  set(CMAKE_CXX_COMPILER "${cpkt_cxx}" CACHE FILEPATH "" FORCE)
  set(CMAKE_LINKER "${cpkt_ld}" CACHE FILEPATH "" FORCE)
  set(CMAKE_AR "${cpkt_ar}" CACHE FILEPATH "" FORCE)
  set(CMAKE_RANLIB "${cpkt_ranlib}" CACHE FILEPATH "" FORCE)
  set(CMAKE_STRIP "${cpkt_strip}" CACHE FILEPATH "" FORCE)
  set(CMAKE_NM "${cpkt_nm}" CACHE FILEPATH "" FORCE)
  set(CMAKE_OBJCOPY "${cpkt_objcopy}" CACHE FILEPATH "" FORCE)
  set(CMAKE_OBJDUMP "${cpkt_objdump}" CACHE FILEPATH "" FORCE)
  set(CMAKE_ADDR2LINE "${cpkt_addr2line}" CACHE FILEPATH "" FORCE)
  set(CMAKE_READELF "${cpkt_readelf}" CACHE FILEPATH "" FORCE)
  set(cpkt_find_root_path)
  if(DEFINED VECTIS_EXTERNAL_ROOT AND NOT VECTIS_EXTERNAL_ROOT STREQUAL "")
    list(APPEND cpkt_find_root_path "${VECTIS_EXTERNAL_ROOT}")
  endif()
  list(APPEND cpkt_find_root_path "${cpkt_sysroot}" "${cpkt_root}")
  set(CMAKE_SYSROOT "${cpkt_sysroot}" CACHE PATH "" FORCE)
  set(CMAKE_FIND_ROOT_PATH "${cpkt_find_root_path}" CACHE STRING "" FORCE)
  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER CACHE STRING "" FORCE)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY CACHE STRING "" FORCE)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY CACHE STRING "" FORCE)
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY CACHE STRING "" FORCE)
endfunction()
