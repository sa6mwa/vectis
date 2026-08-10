file(MAKE_DIRECTORY "${WORK_DIR}/admin")
set(store "${WORK_DIR}/admin/credentials.json")
file(REMOVE "${store}" "${store}.lock")

execute_process(
  COMMAND "${VECTIS_BIN}" -a credentials --store "${store}" --init
  RESULT_VARIABLE init_result
  OUTPUT_VARIABLE init_output
  ERROR_VARIABLE init_error)
if(NOT init_result EQUAL 0)
  message(FATAL_ERROR "credentials init failed: ${init_error}")
endif()
if(NOT init_output MATCHES "initialized=")
  message(FATAL_ERROR "credentials init did not report initialized path")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a credentials --store "${store}" --issue
          --subject "admin@example.com" --purpose "webdav" --bearer
  RESULT_VARIABLE issue_result
  OUTPUT_VARIABLE issue_output
  ERROR_VARIABLE issue_error)
if(NOT issue_result EQUAL 0)
  message(FATAL_ERROR "credentials issue failed: ${issue_error}")
endif()
string(REGEX MATCH "client_id=([^\n]+)" client_id_match "${issue_output}")
if(NOT client_id_match)
  message(FATAL_ERROR "credentials issue did not print client_id")
endif()
set(client_id "${CMAKE_MATCH_1}")
string(REGEX MATCH "api_key=([^\n]+)" api_key_match "${issue_output}")
if(NOT api_key_match)
  message(FATAL_ERROR "credentials issue did not print api_key")
endif()
set(api_key "${CMAKE_MATCH_1}")

execute_process(
  COMMAND "${VECTIS_BIN}" -a credentials --store "${store}" --verify
          "Bearer ${api_key}" --bearer
  RESULT_VARIABLE verify_result
  OUTPUT_VARIABLE verify_output
  ERROR_VARIABLE verify_error)
if(NOT verify_result EQUAL 0)
  message(FATAL_ERROR "credentials verify failed: ${verify_error}")
endif()
if(NOT verify_output MATCHES "authenticated=true")
  message(FATAL_ERROR "credentials verify did not authenticate issued key")
endif()
if(NOT verify_output MATCHES "auth_mode=bearer")
  message(FATAL_ERROR "credentials verify did not report bearer mode")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a credentials --store "${store}" --revoke
          "${client_id}"
  RESULT_VARIABLE revoke_result
  OUTPUT_VARIABLE revoke_output
  ERROR_VARIABLE revoke_error)
if(NOT revoke_result EQUAL 0)
  message(FATAL_ERROR "credentials revoke failed: ${revoke_error}")
endif()
if(NOT revoke_output MATCHES "revoked=${client_id}")
  message(FATAL_ERROR "credentials revoke did not report revoked client")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a credentials --store "${store}" --verify
          "Bearer ${api_key}" --bearer
  RESULT_VARIABLE revoked_verify_result
  OUTPUT_VARIABLE revoked_verify_output
  ERROR_VARIABLE revoked_verify_error)
if(NOT revoked_verify_result EQUAL 0)
  message(FATAL_ERROR "revoked credentials verify failed: ${revoked_verify_error}")
endif()
if(NOT revoked_verify_output MATCHES "authenticated=false")
  message(FATAL_ERROR "revoked credentials still authenticated")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a users --store "${store}" --add
          "admin-user@example.com" --password "admin-password"
          --totp-secret "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
          --label "Vectis:admin-user@example.com" --issuer "Vectis"
  RESULT_VARIABLE user_add_result
  OUTPUT_VARIABLE user_add_output
  ERROR_VARIABLE user_add_error)
if(NOT user_add_result EQUAL 0)
  message(FATAL_ERROR "users add failed: ${user_add_error}")
endif()
if(NOT user_add_output MATCHES "username=admin-user@example.com")
  message(FATAL_ERROR "users add did not report username")
endif()
if(NOT user_add_output MATCHES "totp_secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ")
  message(FATAL_ERROR "users add did not report TOTP secret")
endif()
if(NOT user_add_output MATCHES "totp_qr:")
  message(FATAL_ERROR "users add did not print terminal QR")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a users --store "${store}" --login
          "admin-user@example.com" --password "admin-password"
  RESULT_VARIABLE user_login_missing_result
  OUTPUT_VARIABLE user_login_missing_output
  ERROR_VARIABLE user_login_missing_error)
if(NOT user_login_missing_result EQUAL 0)
  message(FATAL_ERROR "users login missing TOTP failed hard: ${user_login_missing_error}")
endif()
if(NOT user_login_missing_output MATCHES "authenticated=false")
  message(FATAL_ERROR "users login without TOTP was not rejected")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a users --store "${store}" --login
          "admin-user@example.com" --password "admin-password"
          --totp-code "287082" --time "59" --window "0"
  RESULT_VARIABLE user_login_result
  OUTPUT_VARIABLE user_login_output
  ERROR_VARIABLE user_login_error)
if(NOT user_login_result EQUAL 0)
  message(FATAL_ERROR "users login failed: ${user_login_error}")
endif()
if(NOT user_login_output MATCHES "authenticated=true")
  message(FATAL_ERROR "users login did not authenticate with TOTP")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a users --store "${store}" --webdav-key
          "admin-user@example.com" --password "admin-password"
          --totp-code "287082" --time "59" --window "0"
  RESULT_VARIABLE webdav_key_result
  OUTPUT_VARIABLE webdav_key_output
  ERROR_VARIABLE webdav_key_error)
if(NOT webdav_key_result EQUAL 0)
  message(FATAL_ERROR "users webdav-key failed: ${webdav_key_error}")
endif()
if(NOT webdav_key_output MATCHES "client_id=")
  message(FATAL_ERROR "users webdav-key did not print client_id")
endif()
if(NOT webdav_key_output MATCHES "client_secret=")
  message(FATAL_ERROR "users webdav-key did not print client_secret")
endif()
if(NOT webdav_key_output MATCHES "\"purpose\":\"webdav\"")
  message(FATAL_ERROR "users webdav-key did not carry webdav purpose")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -x "${CMAKE_SOURCE_DIR}/tests/lua/smoke.lua"
  RESULT_VARIABLE trace_result
  OUTPUT_VARIABLE trace_output
  ERROR_VARIABLE trace_error)
if(trace_result EQUAL 0)
  message(FATAL_ERROR "-x unexpectedly executed Lua tracing successfully")
endif()
if(NOT trace_error MATCHES "Lua execution tracing")
  message(FATAL_ERROR "-x did not report reserved Lua tracing semantics")
endif()
