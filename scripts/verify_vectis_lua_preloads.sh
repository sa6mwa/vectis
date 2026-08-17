#!/usr/bin/env bash
set -eu

vectis_bin=${1:?usage: verify_vectis_lua_preloads.sh VECTIS_BIN [EXPECTED_VERSION]}
expected_version=${2:-${VECTIS_VERSION:-}}

fail() {
  reason=$1
  artifact=${2:-$vectis_bin}
  echo "$reason${artifact:+: $artifact}" >&2
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=vectis-lua-preloads
phase=installed-binary
status=failed
class=lua-runtime-artifact
reason=$reason
artifact=$artifact
next=ensure bin/vectis statically registers all bundled Lua modules and rerun package verification
PKT_DIAGNOSTIC_END
EOF
  exit 1
}

[ -x "$vectis_bin" ] || fail "vectis binary is not executable"

tmp_dir=${TMPDIR:-/tmp}
tmp_script=$(mktemp "${tmp_dir%/}/vectis-lua-preloads.XXXXXX.lua")
trap 'rm -f "$tmp_script"' EXIT HUP INT TERM

cat >"$tmp_script" <<'LUA'
local expected_version = os.getenv("VECTIS_EXPECTED_VERSION")
if expected_version == "" then
  expected_version = nil
end

local modules = {
  "vectis.auth.core",
  "vectis.auth",
  "vectis.cert",
  "vectis.embedded",
  "vectis.server",
  "vectis.kore",
  "vectis.curl_worker",
  "vectis.cai_worker",
  "vectis.audio_worker",
  "vectis.sus_worker",
  "vectis.ssh",
  "vectis",
  "lockdc.core",
  "lockdc",
  "lonejson.core",
  "lonejson",
  "curl.core",
  "curl",
  "openssl",
  "zlib",
  "vectis.status",
  "vectis.log",
  "vectis.http",
  "vectis.rest",
  "vectis.terminal",
  "vectis.webdav",
  "vectis.mqtt",
  "vectis.smtp",
  "vectis.lockd",
  "vectis.dsv.core",
  "vectis.dsv",
  "vectis.xml.core",
  "vectis.xml",
  "cai",
  "lql.core",
  "lql",
  "pslog.core",
  "pslog",
  "libmdf.core",
  "libmdf",
  "softline",
  "opcua",
  "audio",
  "sus",
}

local loaded = {}
for _, name in ipairs(modules) do
  local ok, module_or_error = pcall(require, name)
  assert(ok, name .. ": " .. tostring(module_or_error))
  assert(module_or_error ~= nil, name .. " returned nil")
  loaded[name] = module_or_error
end

assert(package.loaded.luarocks == nil)
assert(not package.path:lower():find("luarocks", 1, true))
assert(not package.cpath:lower():find("luarocks", 1, true))
assert(loaded["vectis"].auth == loaded["vectis.auth"])
assert(loaded["vectis.auth"].core == loaded["vectis.auth.core"])
assert(loaded["vectis"].cert == loaded["vectis.cert"])
assert(loaded["vectis"].embedded == loaded["vectis.embedded"])
assert(loaded["vectis"].server == loaded["vectis.server"])
assert(loaded["vectis"].kore == loaded["vectis.kore"])
assert(loaded["vectis"].curl_worker == loaded["vectis.curl_worker"])
assert(loaded["vectis"].cai_worker == loaded["vectis.cai_worker"])
assert(loaded["vectis"].audio_worker == loaded["vectis.audio_worker"])
assert(loaded["vectis"].sus_worker == loaded["vectis.sus_worker"])
assert(loaded["vectis"].ssh == loaded["vectis.ssh"])
assert(loaded["vectis"].http == loaded["vectis.http"])
assert(loaded["vectis"].rest == loaded["vectis.rest"])
assert(loaded["vectis"].terminal == loaded["vectis.terminal"])
assert(loaded["vectis"].status == loaded["vectis.status"])
assert(loaded["vectis"].log == loaded["vectis.log"])
assert(loaded["vectis"].webdav == loaded["vectis.webdav"])
assert(loaded["vectis"].mqtt == loaded["vectis.mqtt"])
assert(loaded["vectis"].smtp == loaded["vectis.smtp"])
assert(loaded["vectis"].lockd == loaded["vectis.lockd"])
assert(loaded["vectis"].dsv == loaded["vectis.dsv"])
assert(loaded["vectis"].xml == loaded["vectis.xml"])
assert(type(loaded["vectis"].libs) == "table")
for _, name in ipairs({
  "lockdc",
  "lonejson",
  "pslog",
  "lql",
  "cai",
  "libmdf",
  "softline",
  "curl",
  "opcua",
  "openssl",
  "zlib",
  "audio",
  "sus",
}) do
  assert(loaded["vectis"].libs[name] == loaded[name], "vectis.libs." .. name)
end
assert(type(loaded["vectis"].error_source_string) == "function")
assert(loaded["vectis"].error_source_string(loaded["vectis"].ERROR_SOURCE_VECTIS) == "vectis")
assert(type(loaded["vectis.auth"].provider_native) == "function")
assert(type(loaded["vectis.auth"].browser_flow) == "function")
assert(type(loaded["vectis.cert"].generate_bundle) == "function")
assert(type(loaded["vectis.embedded"].read) == "function")
assert(type(loaded["vectis.server"].new) == "function")
assert(loaded["vectis.kore"].runtime_available == true)
assert(loaded["vectis.kore"].runtime_model == "embedded")
assert(loaded["vectis.kore"].websocket.TEXT == loaded["vectis"].websocket.TEXT)
assert(type(loaded["vectis.curl_worker"].http_request) == "function")
assert(type(loaded["vectis.curl_worker"].decode_http_response) == "function")
assert(type(loaded["vectis.cai_worker"].request) == "function")
assert(type(loaded["vectis.cai_worker"].decode_reply) == "function")
assert(type(loaded["vectis.audio_worker"].decode_file_request) == "function")
assert(type(loaded["vectis.audio_worker"].encode_file_request) == "function")
assert(type(loaded["vectis.audio_worker"].vox_request) == "function")
assert(type(loaded["vectis.audio_worker"].decode_reply) == "function")
assert(type(loaded["vectis.audio_worker"].decode_vox_state) == "function")
assert(type(loaded["vectis.audio_worker"].decode_vox_segment) == "function")
assert(type(loaded["vectis.sus_worker"].transcribe_pcm_request) == "function")
assert(type(loaded["vectis.sus_worker"].transcribe_file_request) == "function")
assert(type(loaded["vectis.sus_worker"].decode_reply) == "function")
assert(type(loaded["vectis.ssh"].scp_upload_file) == "function")
assert(type(loaded["vectis.rest"].route) == "function")
assert(type(loaded["vectis.log"].new) == "function")
assert(type(loaded["vectis.terminal"].markdown) == "function")
assert(type(loaded["vectis.smtp"].send) == "function")
assert(type(loaded["curl"].perform) == "function")
assert(type(loaded["openssl"].sha256_hex) == "function")
assert(type(loaded["zlib"].deflate) == "function")
assert(loaded["zlib"].inflate(loaded["zlib"].deflate("preload")) == "preload")
assert(type(loaded["opcua"].client) == "function")
assert(type(loaded["audio"].can_decode) == "function")
assert(type(loaded["sus"].model_catalog_count) == "function")

if expected_version ~= nil then
  assert(loaded["vectis"].version == expected_version,
         loaded["vectis"].version .. " ~= " .. expected_version)
end

print("vectis lua preloads ok")
LUA

if ! output=$(env \
       VECTIS_EXPECTED_VERSION="$expected_version" \
       LUA_PATH="/__vectis_no_lua_path__/?.lua" \
       LUA_CPATH="/__vectis_no_lua_cpath__/?.so" \
       "$vectis_bin" "$tmp_script" 2>&1); then
  printf '%s\n' "$output" >&2
  fail "vectis binary failed Lua preload verification"
fi

case "$output" in
  *"vectis lua preloads ok"*) ;;
  *) printf '%s\n' "$output" >&2
     fail "vectis binary did not report Lua preload success" ;;
esac

printf '%s\n' "vectis lua preloads ok"
