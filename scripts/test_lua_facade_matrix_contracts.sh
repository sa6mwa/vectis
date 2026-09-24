#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname -- "$0")/.." && pwd)
lua_index="$repo_root/manual/lua.md"
matrix="$repo_root/docs/lua-coverage-matrix.md"
smoke="$repo_root/tests/lua/smoke.lua"
top_level="$repo_root/lua/vectis.lua"
api_index="$repo_root/manual/api.md"
app_header="$repo_root/include/vectis/vectis.h"
app_binding="$repo_root/src/vectis_cli.c"
app_docs="$repo_root/manual/lua-app.md"

fail() {
  printf 'lua facade matrix contract failed: %s\n' "$*" >&2
  exit 1
}

require_fixed() {
  local file=$1
  local text=$2
  local label=$3
  grep -F -- "$text" "$file" >/dev/null ||
    fail "$label missing in ${file#$repo_root/}: $text"
}

require_file() {
  local file=$1
  [ -f "$file" ] || fail "missing file: ${file#$repo_root/}"
}

dependency_modules=(
  "lockdc|dep:lockdc|package.loaded.lockdc == lockdc|vectis.libs.lockdc == lockdc|"
  "lonejson|dep:lonejson|package.loaded.lonejson == lonejson|vectis.libs.lonejson == lonejson|"
  "pslog|dep:pslog|package.loaded.pslog == pslog|vectis.libs.pslog == pslog|lua-pslog.md"
  "lql|dep:lql|package.loaded.lql == lql|vectis.libs.lql == lql|lua-lql.md"
  "cai|dep:cai|package.loaded.cai == cai|vectis.libs.cai == cai|lua-cai.md"
  "libmdf|dep:libmdf|package.loaded.libmdf == libmdf|vectis.libs.libmdf == libmdf|lua-libmdf.md"
  "softline|dep:softline|package.loaded.softline == softline|vectis.libs.softline == softline|lua-softline.md"
  "curl|dep:curl|package.loaded.curl == curl|vectis.libs.curl == curl|lua-curl.md"
  "openssl|dep:openssl|package.loaded.openssl == openssl|vectis.libs.openssl == openssl|lua-openssl.md"
  "zlib|dep:zlib|package.loaded.zlib == zlib|vectis.libs.zlib == zlib|lua-zlib.md"
  "opcua|dep:opcua|package.loaded.opcua == opcua|vectis.libs.opcua == opcua|lua-opcua.md"
  "audio|dep:audio|package.loaded.audio == audio|vectis.libs.audio == audio|lua-audio.md"
  "sus|dep:sus|package.loaded.sus == sus|vectis.libs.sus == sus|lua-sus.md"
)

workflow_modules=(
  "vectis.auth|workflow:auth|package.loaded[\"vectis.auth\"] == auth|vectis.auth == auth|lua-auth.md"
  "vectis.audio_worker|workflow:audio|package.loaded[\"vectis.audio_worker\"] == vectis.audio_worker|vectis.audio_worker == require(\"vectis.audio_worker\")|lua-audio.md"
  "vectis.cai|workflow:cai|package.loaded[\"vectis.cai\"] == vcai|vectis.cai == vcai|lua-cai.md"
  "vectis.cai_worker|workflow:cai|package.loaded[\"vectis.cai_worker\"] == vectis.cai_worker|vectis.cai_worker == require(\"vectis.cai_worker\")|lua-cai.md"
  "vectis.cert|workflow:certs|package.loaded[\"vectis.cert\"] == cert|vectis.cert == cert|lua-certs.md"
  "vectis.cli|workflow:cli|package.loaded[\"vectis.cli\"] == cli|vectis.cli == cli|lua-cli.md"
  "vectis.curl_worker|dep:curl|package.loaded[\"vectis.curl_worker\"] == vectis.curl_worker|vectis.curl_worker == require(\"vectis.curl_worker\")|lua-curl.md"
  "vectis.dsv|workflow:dsv|package.loaded[\"vectis.dsv\"] == dsv|vectis.dsv == dsv|lua-dsv.md"
  "vectis.embedded|workflow:static-assets|package.loaded[\"vectis.embedded\"] == embedded|vectis.embedded == embedded|lua-embedded.md"
  "vectis.http|workflow:http-client|package.loaded[\"vectis.http\"] == http|vectis.http == http|lua-http.md"
  "vectis.kore|workflow:server-runtime|package.loaded[\"vectis.kore\"] == vectis.kore|assert(require(\"vectis.kore\"))|lua-kore.md"
  "vectis.lockd|workflow:lockd-state|package.loaded[\"vectis.lockd\"] == lockd|vectis.lockd == lockd|lua-lockd.md"
  "vectis.log|workflow:logging|package.loaded[\"vectis.log\"] == log|vectis.log == log|lua-log.md"
  "vectis.mailbox|workflow:mailbox|package.loaded[\"vectis.mailbox\"] == mailbox|vectis.mailbox == mailbox|lua-mailbox.md"
  "vectis.mqtt|workflow:mqtt|package.loaded[\"vectis.mqtt\"] == mqtt|vectis.mqtt == mqtt|lua-mqtt.md"
  "vectis.rest|workflow:rest|package.loaded[\"vectis.rest\"] == rest|vectis.rest == rest|lua-rest.md"
  "vectis.app|workflow:server-runtime|package.loaded[\"vectis.app\"] == app_module|vectis.app == app_module|lua-app.md"
  "vectis.smith|workflow:terminal-agent|package.loaded[\"vectis.smith\"] == smith|vectis.smith == smith|agent-smith.md"
  "vectis.smtp|workflow:smtp|package.loaded[\"vectis.smtp\"] == smtp|vectis.smtp == smtp|lua-smtp.md"
  "vectis.ssh|workflow:ssh-exec|package.loaded[\"vectis.ssh\"] == ssh|vectis.ssh == ssh|lua-ssh.md"
  "vectis.status|workflow:status-errors|package.loaded[\"vectis.status\"] == status|vectis.status == status|lua-status.md"
  "vectis.sus_worker|workflow:sus|package.loaded[\"vectis.sus_worker\"] == vectis.sus_worker|vectis.sus_worker == require(\"vectis.sus_worker\")|lua-sus.md"
  "vectis.terminal|workflow:terminal-agent|package.loaded[\"vectis.terminal\"] == terminal|vectis.terminal == terminal|lua-terminal.md"
  "vectis.webdav|workflow:webdav-client|package.loaded[\"vectis.webdav\"] == webdav|vectis.webdav == webdav|lua-webdav.md"
  "vectis.xml|workflow:xml|package.loaded[\"vectis.xml\"] == xml|vectis.xml == xml|lua-xml.md"
)

require_fixed "$lua_index" '- `vectis`: top-level runtime helpers' \
  "top-level vectis index row"
require_fixed "$smoke" 'package.loaded.vectis == vectis' \
  "top-level vectis preload identity"

for entry in "${dependency_modules[@]}"; do
  IFS='|' read -r module row preload libs_alias doc <<<"$entry"
  require_fixed "$lua_index" "- \`$module\`:" "dependency index row"
  require_fixed "$matrix" "| $row |" "dependency matrix row"
  require_fixed "$smoke" "$preload" "dependency preload identity"
  require_fixed "$smoke" "$libs_alias" "dependency vectis.libs alias"
  if [ -n "$doc" ]; then
    require_fixed "$lua_index" "($doc)" "dependency doc link"
    require_fixed "$api_index" "($doc)" "C API Lua doc link"
    require_file "$repo_root/manual/$doc"
  fi
done

for entry in "${workflow_modules[@]}"; do
  IFS='|' read -r module row preload top_alias doc <<<"$entry"
  require_fixed "$lua_index" "- \`$module\`:" "workflow index row"
  require_fixed "$matrix" "| $row |" "workflow matrix row"
  require_fixed "$smoke" "$preload" "workflow preload identity"
  require_fixed "$smoke" "$top_alias" "workflow top-level alias"
  require_fixed "$lua_index" "($doc)" "workflow doc link"
  require_fixed "$api_index" "($doc)" "C API Lua doc link"
  require_file "$repo_root/manual/$doc"

  field=${module#vectis.}
  if [ "$field" = "status" ]; then
    require_fixed "$top_level" 'local status = require("vectis.status")' \
      "standalone workflow top-level alias"
  else
    require_fixed "$top_level" "$field = \"$module\"" \
      "standalone workflow top-level alias"
  fi
done

require_fixed "$matrix" '| workflow:libs | Bundled dependency namespace | yes | yes | yes | yes | n/a |' \
  "vectis.libs matrix row"
require_fixed "$lua_index" 'Direct `require(...)` remains the canonical way' \
  "direct dependency load policy"

# Keep the Lua receiver facade in lockstep with the public C app receiver. The
# non-direct mappings are documented because they require C callbacks or raw
# process-local handles that cannot be safely fabricated in Lua.
direct_receivers=(
  start stop restart run wait route route_count static_file
  static_directory static_embedded webdav webdav_embedded_site webdav_embedded \
  auth_routes metrics websocket openapi_doc openapi consumer_service \
  opcua_server_service curl_worker_service cai_worker_service \
  audio_worker_service sus_worker_service close
)
c_only_receivers=(
  json_route json_typed_route xml_route dsv_route
  upload_stream upload_file upload_reader cai_mcp_route
  prefixed_route prefixed_json_route prefixed_json_typed_route
  prefixed_xml_route prefixed_dsv_route logger cai_client lockd_client
  managed_service register_consumer_receiver consumer_service_receiver
)

for receiver in "${direct_receivers[@]}"; do
  require_fixed "$app_header" "(*$receiver)" "public C app receiver"
  require_fixed "$app_binding" "\"$receiver\"" "Lua app receiver binding"
done
for receiver in "${c_only_receivers[@]}"; do
  require_fixed "$app_header" "(*$receiver)" "public C app receiver"
  require_fixed "$app_docs" "app->$receiver()" "C-only receiver mapping"
done

# A future public receiver must be classified explicitly rather than silently
# escaping the Lua facade audit.
while IFS= read -r receiver; do
  case " ${direct_receivers[*]} ${c_only_receivers[*]} " in
    *" $receiver "*) ;;
    *) fail "unclassified public C app receiver: $receiver" ;;
  esac
done < <(sed -n '/^struct vectis_app {$/,/^};$/p' "$app_header" |
  sed -n 's/.*(\*\([a-z_][a-z_]*\)).*/\1/p')

require_fixed "$app_binding" "vectis_lua_app_static_file" \
  "Lua static-file receiver implementation"
require_fixed "$app_docs" '## C Receiver Mapping' "C/Lua receiver mapping docs"
require_fixed "$app_docs" 'The C forms require C-owned maps and C callbacks' \
  "typed C route mapping rationale"
require_fixed "$app_docs" 'borrowed process-local native handles' \
  "C-only receiver rationale"

echo "lua facade matrix contracts ok"
