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

set(storage_dir "${WORK_DIR}/lua-metrics-auth-pouch")
set(lockd_endpoint "pouch://${storage_dir}?single_writer=false")

file(REMOVE_RECURSE "${storage_dir}")

execute_process(
  COMMAND "${VECTIS_BIN}" -a users --lockd-endpoint "${lockd_endpoint}" --add
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
  COMMAND "${VECTIS_BIN}" -a users --lockd-endpoint "${lockd_endpoint}" --login
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
  COMMAND "${VECTIS_BIN}" -a users --lockd-endpoint "${lockd_endpoint}" --login
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

foreach(totp_time IN ITEMS 29 89)
  execute_process(
    COMMAND "${VECTIS_BIN}" -a users --lockd-endpoint "${lockd_endpoint}" --login
            "metrics-admin" --password "metrics-password"
            --totp-code "287082" --time "${totp_time}" --window "0"
    RESULT_VARIABLE window_result
    OUTPUT_VARIABLE window_output
    ERROR_VARIABLE window_error)
  if(NOT window_result EQUAL 0 OR NOT window_output MATCHES "authenticated=false")
    message(FATAL_ERROR "zero TOTP window accepted an adjacent period at ${totp_time}: ${window_error}")
  endif()
endforeach()
execute_process(
  COMMAND "${VECTIS_BIN}" -a users --lockd-endpoint "${lockd_endpoint}" --login
          "metrics-admin" --password "metrics-password"
          --totp-code "287082" --time "89"
  RESULT_VARIABLE default_window_result
  OUTPUT_VARIABLE default_window_output
  ERROR_VARIABLE default_window_error)
if(NOT default_window_result EQUAL 0 OR NOT default_window_output MATCHES "authenticated=true")
  message(FATAL_ERROR "default TOTP window rejected an adjacent period: ${default_window_error}")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a credentials --lockd-endpoint "${lockd_endpoint}"
          --issue --subject "metrics-agent" --purpose "metrics" --bearer
  RESULT_VARIABLE credential_issue_result
  OUTPUT_VARIABLE credential_issue_output
  ERROR_VARIABLE credential_issue_error)
if(NOT credential_issue_result EQUAL 0)
  message(FATAL_ERROR
          "metrics example credential issue failed: ${credential_issue_error}")
endif()
string(REGEX MATCH "api_key=([^\n]+)" credential_api_key_match
       "${credential_issue_output}")
if(NOT credential_api_key_match)
  message(FATAL_ERROR "metrics example credential issue did not print an API key")
endif()
set(metrics_authorization "Bearer ${CMAKE_MATCH_1}")

vectis_run_command_with_port(
  LABEL "Lua metrics authenticated example"
  PORT_ENV "VECTIS_LUA_METRICS_AUTH_EXAMPLE_PORT"
  SUCCESS_MARKER "lua metrics authenticated example ok"
  EXTRA_ENV
    "VECTIS_LUA_METRICS_AUTH_EXAMPLE_STORAGE=${storage_dir}"
    "VECTIS_LUA_METRICS_AUTH_EXAMPLE_AUTHORIZATION=${metrics_authorization}"
  COMMAND "${VECTIS_BIN}"
          "${VECTIS_SOURCE_DIR}/examples/lua/metrics_authenticated.lua")
