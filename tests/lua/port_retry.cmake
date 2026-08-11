function(vectis_pick_test_port out_var)
  if(NOT DEFINED VECTIS_PORT_HELPER)
    message(FATAL_ERROR "VECTIS_PORT_HELPER is required")
  endif()
  execute_process(COMMAND "${VECTIS_PORT_HELPER}"
                  RESULT_VARIABLE port_result
                  OUTPUT_VARIABLE picked_port
                  ERROR_VARIABLE port_stderr
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT port_result EQUAL 0)
    message(FATAL_ERROR "test port allocation failed: ${port_stderr}")
  endif()
  if(NOT picked_port MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "test port helper returned invalid port: ${picked_port}")
  endif()
  set("${out_var}" "${picked_port}" PARENT_SCOPE)
endfunction()

function(vectis_run_command_with_port)
  set(one_value_args LABEL PORT_ENV SUCCESS_MARKER)
  set(multi_value_args COMMAND EXTRA_ENV)
  cmake_parse_arguments(VECTIS_PORT_RUN "" "${one_value_args}"
                        "${multi_value_args}" ${ARGN})

  if(NOT VECTIS_PORT_RUN_LABEL)
    set(VECTIS_PORT_RUN_LABEL "port-scoped command")
  endif()
  if(NOT VECTIS_PORT_RUN_PORT_ENV)
    message(FATAL_ERROR "${VECTIS_PORT_RUN_LABEL}: PORT_ENV is required")
  endif()
  if(NOT VECTIS_PORT_RUN_COMMAND)
    message(FATAL_ERROR "${VECTIS_PORT_RUN_LABEL}: COMMAND is required")
  endif()

  set(attempts 8)
  foreach(attempt RANGE 1 ${attempts})
    vectis_pick_test_port(selected_port)
    set(command_env "${VECTIS_PORT_RUN_PORT_ENV}=${selected_port}")
    if(VECTIS_PORT_RUN_EXTRA_ENV)
      list(APPEND command_env ${VECTIS_PORT_RUN_EXTRA_ENV})
    endif()

    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${command_env}
                            ${VECTIS_PORT_RUN_COMMAND}
                    RESULT_VARIABLE run_result
                    OUTPUT_VARIABLE run_stdout
                    ERROR_VARIABLE run_stderr)

    if(run_result EQUAL 0)
      if(VECTIS_PORT_RUN_SUCCESS_MARKER AND
         NOT run_stdout MATCHES "${VECTIS_PORT_RUN_SUCCESS_MARKER}")
        message(FATAL_ERROR
                "${VECTIS_PORT_RUN_LABEL} missed success marker on port "
                "${selected_port}: ${run_stdout}${run_stderr}")
      endif()
      return()
    endif()

    string(FIND "${run_stdout}${run_stderr}" "Address already in use"
           bind_error_offset)
    if(bind_error_offset EQUAL -1)
      message(FATAL_ERROR
              "${VECTIS_PORT_RUN_LABEL} failed on port ${selected_port}: "
              "${run_stdout}${run_stderr}")
    endif()
  endforeach()

  message(FATAL_ERROR
          "${VECTIS_PORT_RUN_LABEL} could not bind after ${attempts} attempts")
endfunction()
