include("${VECTIS_SOURCE_DIR}/tests/lua/port_retry.cmake")

if(NOT DEFINED VECTIS_BIN)
  message(FATAL_ERROR "VECTIS_BIN is required")
endif()
if(NOT DEFINED VECTIS_SOURCE_DIR)
  message(FATAL_ERROR "VECTIS_SOURCE_DIR is required")
endif()
if(NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "WORK_DIR is required")
endif()

set(auth_path "${WORK_DIR}/lua-metrics-auth-example-credentials.json")
set(state_path "${WORK_DIR}/lua-metrics-auth-example-state.json")
set(storage_dir "${WORK_DIR}/lua-metrics-auth-pouch")

file(REMOVE "${auth_path}" "${auth_path}.lock" "${state_path}"
            "${state_path}.lock")
file(REMOVE_RECURSE "${storage_dir}")

execute_process(
  COMMAND "${VECTIS_BIN}" -a users --store "${auth_path}" --add
          "metrics-admin" --password "metrics-password"
          --totp-secret "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
          --label "Vectis:metrics-admin" --issuer "Vectis Metrics Example"
  RESULT_VARIABLE user_add_result
  OUTPUT_VARIABLE user_add_output
  ERROR_VARIABLE user_add_error)
if(NOT user_add_result EQUAL 0)
  message(FATAL_ERROR "metrics example users add failed: ${user_add_error}")
endif()
if(NOT user_add_output MATCHES "username=metrics-admin")
  message(FATAL_ERROR "metrics example users add did not report username")
endif()
if(NOT user_add_output MATCHES
       "totp_secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ")
  message(FATAL_ERROR "metrics example users add did not report TOTP secret")
endif()
if(NOT user_add_output MATCHES "totp_qr:")
  message(FATAL_ERROR "metrics example users add did not print TOTP QR")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a users --store "${auth_path}" --login
          "metrics-admin" --password "metrics-password"
  RESULT_VARIABLE password_only_result
  OUTPUT_VARIABLE password_only_output
  ERROR_VARIABLE password_only_error)
if(NOT password_only_result EQUAL 0)
  message(FATAL_ERROR
          "metrics example password-only login failed hard: ${password_only_error}")
endif()
if(NOT password_only_output MATCHES "authenticated=false")
  message(FATAL_ERROR "metrics example password-only login was accepted")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a users --store "${auth_path}" --login
          "metrics-admin" --password "metrics-password"
          --totp-code "287082" --time "59" --window "0"
  RESULT_VARIABLE totp_login_result
  OUTPUT_VARIABLE totp_login_output
  ERROR_VARIABLE totp_login_error)
if(NOT totp_login_result EQUAL 0)
  message(FATAL_ERROR "metrics example TOTP login failed: ${totp_login_error}")
endif()
if(NOT totp_login_output MATCHES "authenticated=true")
  message(FATAL_ERROR "metrics example TOTP login did not authenticate")
endif()

vectis_run_command_with_port(
  LABEL "Lua metrics authenticated example"
  PORT_ENV "VECTIS_LUA_METRICS_AUTH_EXAMPLE_PORT"
  SUCCESS_MARKER "lua metrics authenticated example ok"
  EXTRA_ENV
    "VECTIS_LUA_METRICS_AUTH_EXAMPLE_AUTH_PATH=${auth_path}"
    "VECTIS_LUA_METRICS_AUTH_EXAMPLE_STATE_PATH=${state_path}"
    "VECTIS_LUA_METRICS_AUTH_EXAMPLE_STORAGE=${storage_dir}"
  COMMAND "${VECTIS_BIN}"
          "${VECTIS_SOURCE_DIR}/examples/lua/metrics_authenticated.lua")
