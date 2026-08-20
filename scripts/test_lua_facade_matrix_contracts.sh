#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname -- "$0")/.." && pwd)
lua_index="$repo_root/docs/lua.md"
matrix="$repo_root/docs/lua-coverage-matrix.md"
smoke="$repo_root/tests/lua/smoke.lua"

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
    require_file "$repo_root/docs/$doc"
  fi
done

for entry in "${workflow_modules[@]}"; do
  IFS='|' read -r module row preload top_alias doc <<<"$entry"
  require_fixed "$lua_index" "- \`$module\`:" "workflow index row"
  require_fixed "$matrix" "| $row |" "workflow matrix row"
  require_fixed "$smoke" "$preload" "workflow preload identity"
  require_fixed "$smoke" "$top_alias" "workflow top-level alias"
  require_fixed "$lua_index" "($doc)" "workflow doc link"
  require_file "$repo_root/docs/$doc"
done

require_fixed "$matrix" '| workflow:libs | Bundled dependency namespace | yes | yes | yes | yes | n/a |' \
  "vectis.libs matrix row"
require_fixed "$lua_index" 'Direct `require(...)` remains the canonical way' \
  "direct dependency load policy"

echo "lua facade matrix contracts ok"
