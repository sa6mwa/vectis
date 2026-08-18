#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname -- "$0")/.." && pwd)
cmake_tests="$repo_root/tests/CMakeLists.txt"
matrix="$repo_root/docs/lua-coverage-matrix.md"
todo="$repo_root/TODO.md"

fail() {
  printf 'lua facade behavior coverage failed: %s\n' "$*" >&2
  exit 1
}

require_fixed() {
  local file=$1
  local text=$2
  local label=$3
  grep -F -- "$text" "$file" >/dev/null ||
    fail "$label missing in ${file#$repo_root/}: $text"
}

require_ctest() {
  local name=$1
  require_fixed "$cmake_tests" "add_test(NAME $name" "CTest registration"
}

require_row() {
  local row=$1
  require_fixed "$matrix" "| $row |" "coverage matrix row"
}

require_row "workflow:libs"
require_ctest "vectis_lua_smoke"
require_ctest "vectis_lua_facade_contracts"
require_fixed "$todo" "Add Lua unit and end-to-end tests for every Lua facade" \
  "umbrella Lua facade TODO"
require_fixed "$todo" "Add an executable Lua facade behavior coverage audit" \
  "Lua facade behavior audit TODO entry"

# Dependency-native modules and Vectis-owned workflow helpers must have more
# than preload evidence: keep a local behavior test/example and, where the
# module is packable, a packed execution path.
local_behavior=(
  "lockdc|vectis_lua_lockd_helpers|tests/lua/lockd_helpers.cmake|with_dequeued_json"
  "lonejson|vectis_lua_facade_contracts|tests/lua/facade_contracts.cmake|lonejson.schema"
  "pslog|vectis_example_lua_logging|examples/lua/logging.lua|log.log_error"
  "lql|vectis_example_lua_local_data_pipeline|examples/lua/local_data_pipeline.lua|lql.new"
  "cai|vectis_example_lua_cai_local|examples/lua/cai_local.lua|cai.mcp_client"
  "libmdf|vectis_example_lua_mdf_render|examples/lua/mdf_render.lua|libmdf"
  "softline|vectis_example_lua_terminal_tools|examples/lua/terminal_tools.lua|terminal.editor"
  "curl|vectis_lua_curl|tests/lua/curl.cmake|curl.stream_json"
  "openssl|vectis_lua_openssl|tests/lua/openssl.cmake|openssl.sign"
  "zlib|vectis_lua_smoke|tests/lua/smoke.lua|zlib.gzip_file"
  "opcua|vectis_lua_opcua_e2e|examples/lua/opcua_client.lua|opcua.client"
  "audio|vectis_lua_audio_sus|tests/lua/audio_sus.cmake|audio.vox.open"
  "sus|vectis_lua_audio_sus|tests/lua/audio_sus.cmake|sus.open_cached"
  "auth|vectis_cli_admin_credentials|tests/lua/admin_credentials.cmake|webdav_key"
  "cert|vectis_lua_certs|tests/lua/certs.cmake|generate_bundle"
  "dsv|vectis_lua_dsv|tests/lua/dsv.cmake|parse_spill"
  "embedded|vectis_lua_pack|tests/lua/pack.cmake|vectis.embedded"
  "http|vectis_lua_http|tests/lua/http.cmake|vectis.http.stream_json"
  "mailbox|vectis_example_lua_mailbox_pipeline|examples/lua/mailbox_pipeline.lua|mailbox.new"
  "mqtt|vectis_lua_mqtt|tests/lua/mqtt.cmake|mqtt.publish"
  "rest|vectis_lua_facade_contracts|tests/lua/facade_contracts.cmake|rest.error_response"
  "server|vectis_lua_http|tests/lua/http.cmake|server:upload"
  "smtp|vectis_lua_curl|tests/lua/curl.cmake|smtp.send"
  "ssh|vectis_lua_ssh_sftp|tests/lua/ssh_sftp.cmake|vectis.ssh.open"
  "terminal|vectis_example_lua_terminal_tools|examples/lua/terminal_tools.lua|terminal.editor"
  "webdav|vectis_lua_webdav|tests/lua/webdav.cmake|webdav.propfind"
  "xml|vectis_lua_smoke|tests/lua/smoke.lua|vectis.xml"
)

for entry in "${local_behavior[@]}"; do
  IFS='|' read -r surface test_name evidence snippet <<<"$entry"
  require_ctest "$test_name"
  require_fixed "$repo_root/$evidence" "$snippet" "$surface behavior evidence"
done

packed_behavior=(
  "local dependency facades|vectis_example_lua_local_facades_pack|examples/lua/local_data_pipeline.lua|zlib.gzip_file"
  "API/server/http/rest routes|vectis_example_lua_api_server_pack|examples/lua/api_server.lua|stream_source"
  "downstream HTTP|vectis_example_lua_downstream_api_pack|examples/lua/downstream_api.lua|rest.client"
  "WebDAV server|vectis_example_lua_webdav_fileserver_pack|examples/lua/webdav_fileserver.lua|server:webdav"
  "embedded/auth/certs/lockd bundle|vectis_lua_pack|tests/lua/pack.cmake|embedded_lockd_bundle_source"
  "OPC UA|vectis_lua_opcua_e2e|examples/lua/opcua_client.lua|opcua.server"
  "SUS/audio|vectis_example_lua_audio_sus|examples/lua/audio_sus.lua|sus.transcriber"
)

for entry in "${packed_behavior[@]}"; do
  IFS='|' read -r surface test_name evidence snippet <<<"$entry"
  require_ctest "$test_name"
  require_fixed "$repo_root/$evidence" "$snippet" "$surface packed evidence"
done

require_fixed "$repo_root/tests/lua/facade_contracts.cmake" \
  'collectgarbage("collect")' "ownership/finalizer coverage"
require_fixed "$repo_root/tests/lua/facade_contracts.cmake" \
  "assert_status_error" "structured nil-error coverage"
require_fixed "$repo_root/tests/lua/http.cmake" \
  "spooled_source" "file-backed spooled response coverage"
require_fixed "$repo_root/tests/lua/http.cmake" \
  "stream_source" "live response streaming coverage"
require_fixed "$repo_root/tests/lua/http.cmake" \
  "server:upload" "true request streaming coverage"
require_fixed "$repo_root/tests/lua/dsv.cmake" \
  "spooled_to_disk == true" "large-value DSV spill coverage"
require_fixed "$repo_root/tests/lua/curl.cmake" \
  "streaming responses cannot be retried safely" \
  "streaming retry guard coverage"
require_fixed "$repo_root/tests/lua/audio_sus.cmake" \
  "bad_ptt:release()" "callback failure structured error coverage"
require_fixed "$repo_root/examples/lua/audio_worker_service.lua" \
  "decode_vox_segment" "worker VOX event coverage"
require_fixed "$repo_root/examples/lua/cai_worker_service.lua" \
  "decode_reply" "worker reply decode coverage"
require_fixed "$repo_root/scripts/test-e2e.sh" \
  "packed lua ssh command rejects wrong known_hosts pin" \
  "packed SSH host-key rejection e2e coverage"

printf '%s\n' "lua facade behavior coverage ok"
