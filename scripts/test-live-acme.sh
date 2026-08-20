#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

if [ "${VECTIS_LIVE_ACME_ENABLE:-0}" != "1" ]; then
  printf '%s\n' \
    "SKIP: set VECTIS_LIVE_ACME_ENABLE=1 to run the public Let's Encrypt ACME check"
  exit 0
fi

vectis_bin=${VECTIS_BIN:-"$repo_root/build/debug/vectis"}
if [ ! -x "$vectis_bin" ]; then
  printf '%s\n' "VECTIS_BIN is not an executable: $vectis_bin" >&2
  exit 2
fi
if [ -z "${VECTIS_LIVE_ACME_EMAIL:-}" ]; then
  printf '%s\n' \
    "VECTIS_LIVE_ACME_EMAIL is required for the public Let's Encrypt ACME check" >&2
  exit 2
fi

domain=${VECTIS_LIVE_ACME_DOMAIN:-vectisdemo.c89.systems}
bind=${VECTIS_LIVE_ACME_BIND:-0.0.0.0}
port=${VECTIS_LIVE_ACME_PORT:-8443}
directory_url=${VECTIS_LIVE_ACME_DIRECTORY_URL:-https://acme-v02.api.letsencrypt.org/directory}
work=$(mktemp -d "${TMPDIR:-/tmp}/vectis-live-acme.XXXXXX")
storage_endpoint=${VECTIS_LIVE_ACME_STORAGE_ENDPOINT:-"pouch://$work/storage"}
storage_namespace=${VECTIS_LIVE_ACME_STORAGE_NAMESPACE:-}
storage_key=${VECTIS_LIVE_ACME_STORAGE_KEY:-}
probe_url=${VECTIS_LIVE_ACME_PROBE_URL:-"https://$domain/.vectis-live-acme"}
config_home=${VECTIS_LIVE_ACME_CONFIG_HOME:-"$work/config"}
cache_home=${VECTIS_LIVE_ACME_CACHE_HOME:-"$work/cache"}

export XDG_CONFIG_HOME="$config_home"
export XDG_CACHE_HOME="$cache_home"

cleanup() {
  rm -rf "$work"
}
trap cleanup EXIT INT TERM

if [[ -n "$storage_endpoint" && "$storage_endpoint" == pouch://* ]]; then
  mkdir -p "${storage_endpoint#pouch://}"
fi

cat >"$work/live-acme.lua" <<'LUA'
local curl = require("curl")
local vectis = require("vectis")

local domain = assert(os.getenv("VECTIS_LIVE_ACME_DOMAIN"))
local email = assert(os.getenv("VECTIS_LIVE_ACME_EMAIL"))
local bind = assert(os.getenv("VECTIS_LIVE_ACME_BIND"))
local port = assert(tonumber(os.getenv("VECTIS_LIVE_ACME_PORT")))
local provider = assert(os.getenv("VECTIS_LIVE_ACME_DIRECTORY_URL"))
local endpoint = os.getenv("VECTIS_LIVE_ACME_STORAGE_ENDPOINT")
local namespace = os.getenv("VECTIS_LIVE_ACME_STORAGE_NAMESPACE")
local key = os.getenv("VECTIS_LIVE_ACME_STORAGE_KEY")
local probe_url = assert(os.getenv("VECTIS_LIVE_ACME_PROBE_URL"))
local missing_key_file = assert(os.getenv("VECTIS_LIVE_ACME_MISSING_KEY_FILE"))

local function require_ok(value, err, phase)
  if value == true then return end
  if type(err) == "table" then
    error(phase .. ": " .. tostring(err.message) .. ": " ..
          tostring(err.detail))
  end
  error(phase .. ": " .. tostring(err))
end

local function require_value(value, err, phase)
  if value ~= nil then return value end
  require_ok(false, err, phase)
end

local function new_server(name, lockd)
  local tls = {
    mode = "acme",
    bind = bind,
    port = port,
    domains = {domain},
    email = email,
    provider = provider,
  }
  if endpoint then tls.acme_storage_endpoint = endpoint end
  if namespace then tls.acme_storage_namespace = namespace end
  if key then tls.acme_storage_key = key end
  local server, new_error = vectis.app.new({
    app_name = name,
    bind = bind,
    port = port,
    tls = tls,
    lockd = lockd,
  })
  server = require_value(server, new_error, "construct " .. name)
  assert(server:json({
    path = "/.vectis-live-acme",
    body = "{\"service\":\"vectis-live-acme\"}",
  }) == true)
  return server
end

local function wait_for_public_https(phase)
  local last_error = "no response"
  for attempt = 1, 180 do
    local response = curl.perform({
      url = probe_url,
      protocols = "https",
      timeout_ms = 5000,
      connect_timeout_ms = 3000,
      no_signal = true,
    })
    if response.ok and response.status == 200 and
       response.body:find("vectis%-live%-acme") then
      print("public_https_" .. phase .. "=ok")
      return
    end
    last_error = response.error or
                 ("HTTP status " .. tostring(response.status))
    assert(vectis.sleep_ms(1000) == true)
  end
  error("public HTTPS probe did not succeed during " .. phase .. ": " ..
        last_error)
end

local local_lockd = {endpoints = {assert(endpoint)}}
local first = new_server("vectis-live-acme-issue", local_lockd)
local started, start_error = first:start()
require_ok(started, start_error, "initial ACME start")
wait_for_public_https("issue")
local stopped, stop_error = first:stop()
require_ok(stopped, stop_error, "initial ACME stop")
first:close()

local restored = new_server("vectis-live-acme-restore", local_lockd)
started, start_error = restored:start()
require_ok(started, start_error, "restored ACME start")
wait_for_public_https("restore")
stopped, stop_error = restored:stop()
require_ok(stopped, stop_error, "restored ACME stop")
restored:close()

local rejected = new_server("vectis-live-acme-missing-key", {
  endpoints = {assert(endpoint)},
  pouch_crypto_key_file = missing_key_file,
  pouch_crypto_generate_key_file = false,
})
started, start_error = rejected:start()
assert(started == nil)
assert(start_error ~= nil)
rejected:close()

print("live_acme=ok")
print("domain=" .. domain)
print("storage_endpoint=" .. (endpoint or "default-pouch"))
LUA

live_env=(
  "VECTIS_LIVE_ACME_DOMAIN=$domain"
  "VECTIS_LIVE_ACME_EMAIL=$VECTIS_LIVE_ACME_EMAIL"
  "VECTIS_LIVE_ACME_BIND=$bind"
  "VECTIS_LIVE_ACME_PORT=$port"
  "VECTIS_LIVE_ACME_DIRECTORY_URL=$directory_url"
  "VECTIS_LIVE_ACME_PROBE_URL=$probe_url"
  "VECTIS_LIVE_ACME_MISSING_KEY_FILE=$work/missing-pouch.key"
)
if [ -n "$storage_endpoint" ]; then
  live_env+=("VECTIS_LIVE_ACME_STORAGE_ENDPOINT=$storage_endpoint")
fi
if [ -n "$storage_namespace" ]; then
  live_env+=("VECTIS_LIVE_ACME_STORAGE_NAMESPACE=$storage_namespace")
fi
if [ -n "$storage_key" ]; then
  live_env+=("VECTIS_LIVE_ACME_STORAGE_KEY=$storage_key")
fi

if ! env "${live_env[@]}" "$vectis_bin" "$work/live-acme.lua"; then
  printf '%s\n' 'live ACME check failed; inspect Vectis/Kore output above' >&2
  exit 1
fi

key_file="$config_home/vectis/pouch.key"
if [ ! -f "$key_file" ]; then
  printf '%s\n' "missing generated Pouch key file: $key_file" >&2
  exit 1
fi
if [ "$(stat -c '%a' "$key_file")" != "600" ]; then
  printf '%s\n' "Pouch key file is not mode 0600: $key_file" >&2
  exit 1
fi
if rg -a -F 'vectis-live-acme' "${storage_endpoint#pouch://}" >/dev/null; then
  printf '%s\n' "Pouch state exposes plaintext ACME snapshot data" >&2
  exit 1
fi
if find "$cache_home" -maxdepth 1 -type d -name 'acme-*' | grep -q .; then
  printf '%s\n' "Vectis-owned ACME runtime directory was not removed" >&2
  exit 1
fi
