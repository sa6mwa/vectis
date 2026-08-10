if(NOT DEFINED VECTIS_BIN)
  message(FATAL_ERROR "VECTIS_BIN is required")
endif()
if(NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "WORK_DIR is required")
endif()
if(NOT DEFINED VECTIS_SOURCE_DIR)
  message(FATAL_ERROR "VECTIS_SOURCE_DIR is required")
endif()

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
  COMMAND "${VECTIS_BIN}" -a oauth2 --authorize
          --authorization-endpoint "https://idp.example.test/authorize"
          --client-id "admin-client"
          --redirect-uri "http://127.0.0.1/callback"
          --scope "openid dav"
          --state "admin-state"
          --nonce "admin-nonce"
          --code-verifier "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~admin"
  RESULT_VARIABLE oauth_authorize_result
  OUTPUT_VARIABLE oauth_authorize_output
  ERROR_VARIABLE oauth_authorize_error)
if(NOT oauth_authorize_result EQUAL 0)
  message(FATAL_ERROR "oauth2 authorize failed: ${oauth_authorize_error}")
endif()
if(NOT oauth_authorize_output MATCHES "authorization_url=https://idp.example.test/authorize\\?")
  message(FATAL_ERROR "oauth2 authorize did not print authorization URL")
endif()
if(NOT oauth_authorize_output MATCHES "code_challenge=")
  message(FATAL_ERROR "oauth2 authorize did not print PKCE challenge")
endif()
if(NOT oauth_authorize_output MATCHES "state=admin-state")
  message(FATAL_ERROR "oauth2 authorize did not preserve state")
endif()
if(NOT oauth_authorize_output MATCHES "nonce=admin-nonce")
  message(FATAL_ERROR "oauth2 authorize did not preserve nonce")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a oauth2 --store "${store}" --exchange-callback
          "bad-callback-flow" --subject "admin-oidc@example.com"
          --token-endpoint "https://idp.example.test/token"
          --client-id "admin-client"
          --redirect-uri "http://127.0.0.1/callback"
          --code-verifier "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~admin"
          --callback-query "?code=admin-code&state=wrong-state"
          --expected-state "admin-state"
  RESULT_VARIABLE oauth_exchange_bad_result
  OUTPUT_VARIABLE oauth_exchange_bad_output
  ERROR_VARIABLE oauth_exchange_bad_error)
if(oauth_exchange_bad_result EQUAL 0)
  message(FATAL_ERROR "oauth2 exchange-callback accepted mismatched state")
endif()
if(NOT oauth_exchange_bad_error MATCHES "OIDC callback token exchange failed")
  message(FATAL_ERROR "oauth2 exchange-callback did not report callback failure")
endif()
if(oauth_exchange_bad_output MATCHES "stored_flow=")
  message(FATAL_ERROR "oauth2 exchange-callback stored a failed callback flow")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a oauth2 --store "${store}" --client-credentials
          "bad-client-flow" --subject "admin-m2m@example.com"
          --client-id "admin-client" --client-secret "admin-secret"
  RESULT_VARIABLE oauth_client_bad_result
  OUTPUT_VARIABLE oauth_client_bad_output
  ERROR_VARIABLE oauth_client_bad_error)
if(oauth_client_bad_result EQUAL 0)
  message(FATAL_ERROR "oauth2 client-credentials accepted missing token endpoint")
endif()
if(NOT oauth_client_bad_error MATCHES "OAuth2 client credentials require token endpoint")
  message(FATAL_ERROR "oauth2 client-credentials did not validate token endpoint")
endif()
if(oauth_client_bad_output MATCHES "stored_flow=")
  message(FATAL_ERROR "oauth2 client-credentials stored an invalid flow")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a oauth2 --store "${store}" --upsert-flow
          "admin-flow" --subject "admin-oidc@example.com"
          --access-token "admin-access-token" --token-type "Bearer"
          --refresh-token "admin-refresh-token" --scope "openid dav"
          --id-token "admin-id-token" --expires-at "5200"
  RESULT_VARIABLE oauth_upsert_result
  OUTPUT_VARIABLE oauth_upsert_output
  ERROR_VARIABLE oauth_upsert_error)
if(NOT oauth_upsert_result EQUAL 0)
  message(FATAL_ERROR "oauth2 upsert-flow failed: ${oauth_upsert_error}")
endif()
if(NOT oauth_upsert_output MATCHES "stored_flow=admin-flow")
  message(FATAL_ERROR "oauth2 upsert-flow did not report stored flow")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a oauth2 --store "${store}" --load-flow
          "admin-flow"
  RESULT_VARIABLE oauth_load_result
  OUTPUT_VARIABLE oauth_load_output
  ERROR_VARIABLE oauth_load_error)
if(NOT oauth_load_result EQUAL 0)
  message(FATAL_ERROR "oauth2 load-flow failed: ${oauth_load_error}")
endif()
if(NOT oauth_load_output MATCHES "found=true")
  message(FATAL_ERROR "oauth2 load-flow did not find stored flow")
endif()
if(NOT oauth_load_output MATCHES "subject=admin-oidc@example.com")
  message(FATAL_ERROR "oauth2 load-flow did not print subject")
endif()
if(NOT oauth_load_output MATCHES "access_token=admin-access-token")
  message(FATAL_ERROR "oauth2 load-flow did not print access token")
endif()
if(NOT oauth_load_output MATCHES "refresh_token=admin-refresh-token")
  message(FATAL_ERROR "oauth2 load-flow did not print refresh token")
endif()
if(NOT oauth_load_output MATCHES "expires_at=5200")
  message(FATAL_ERROR "oauth2 load-flow did not print expiration")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a oauth2 --store "${store}" --ensure-flow
          "admin-flow" --now "1000"
  RESULT_VARIABLE oauth_ensure_result
  OUTPUT_VARIABLE oauth_ensure_output
  ERROR_VARIABLE oauth_ensure_error)
if(NOT oauth_ensure_result EQUAL 0)
  message(FATAL_ERROR "oauth2 ensure-flow failed: ${oauth_ensure_error}")
endif()
if(NOT oauth_ensure_output MATCHES "flow_state=ready")
  message(FATAL_ERROR "oauth2 ensure-flow did not report ready state")
endif()
if(NOT oauth_ensure_output MATCHES "refreshed=false")
  message(FATAL_ERROR "oauth2 ensure-flow unexpectedly refreshed")
endif()
if(NOT oauth_ensure_output MATCHES "flow_id=admin-flow")
  message(FATAL_ERROR "oauth2 ensure-flow did not print flow id")
endif()
if(NOT oauth_ensure_output MATCHES "access_token=admin-access-token")
  message(FATAL_ERROR "oauth2 ensure-flow did not print access token")
endif()

execute_process(
  COMMAND "${VECTIS_BIN}" -a oauth2 --store "${store}" --webdav-key
          "admin-flow" --subject "admin-oidc@example.com"
  RESULT_VARIABLE oauth_webdav_key_result
  OUTPUT_VARIABLE oauth_webdav_key_output
  ERROR_VARIABLE oauth_webdav_key_error)
if(NOT oauth_webdav_key_result EQUAL 0)
  message(FATAL_ERROR "oauth2 webdav-key failed: ${oauth_webdav_key_error}")
endif()
if(NOT oauth_webdav_key_output MATCHES "client_id=")
  message(FATAL_ERROR "oauth2 webdav-key did not print client_id")
endif()
if(NOT oauth_webdav_key_output MATCHES "client_secret=")
  message(FATAL_ERROR "oauth2 webdav-key did not print client_secret")
endif()
if(NOT oauth_webdav_key_output MATCHES "\"oauth2_flow_id\":\"admin-flow\"")
  message(FATAL_ERROR "oauth2 webdav-key did not carry OAuth flow id")
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
  COMMAND "${VECTIS_BIN}" -x "${VECTIS_SOURCE_DIR}/tests/lua/smoke.lua" first
          second
  RESULT_VARIABLE trace_result
  OUTPUT_VARIABLE trace_output
  ERROR_VARIABLE trace_error)
if(NOT trace_result EQUAL 0)
  message(FATAL_ERROR "-x Lua tracing failed (${trace_result}): ${trace_error}")
endif()
if(NOT trace_error MATCHES "\\+ .*smoke\\.lua:[0-9]+")
  message(FATAL_ERROR "-x did not emit Lua line trace output: ${trace_error}")
endif()
