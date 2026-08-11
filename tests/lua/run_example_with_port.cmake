include("${VECTIS_SOURCE_DIR}/tests/lua/port_retry.cmake")

if(NOT DEFINED VECTIS_BIN)
  message(FATAL_ERROR "VECTIS_BIN is required")
endif()
if(NOT DEFINED VECTIS_SCRIPT)
  message(FATAL_ERROR "VECTIS_SCRIPT is required")
endif()
if(NOT DEFINED VECTIS_PORT_ENV)
  message(FATAL_ERROR "VECTIS_PORT_ENV is required")
endif()

set(extra_env)
if(DEFINED VECTIS_EXTRA_ENV)
  set(extra_env ${VECTIS_EXTRA_ENV})
endif()

vectis_run_command_with_port(
  LABEL "${VECTIS_LABEL}"
  PORT_ENV "${VECTIS_PORT_ENV}"
  SUCCESS_MARKER "${VECTIS_SUCCESS_MARKER}"
  EXTRA_ENV ${extra_env}
  COMMAND "${VECTIS_BIN}" "${VECTIS_SCRIPT}")
