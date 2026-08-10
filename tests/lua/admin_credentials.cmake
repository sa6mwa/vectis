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
