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

domain=${VECTIS_LIVE_ACME_DOMAIN:-vectis_demo.c89.systems}
bind=${VECTIS_LIVE_ACME_BIND:-0.0.0.0}
port=${VECTIS_LIVE_ACME_PORT:-8443}
directory_url=${VECTIS_LIVE_ACME_DIRECTORY_URL:-https://acme-v02.api.letsencrypt.org/directory}
storage_endpoint=${VECTIS_LIVE_ACME_STORAGE_ENDPOINT:-}
storage_namespace=${VECTIS_LIVE_ACME_STORAGE_NAMESPACE:-}
storage_key=${VECTIS_LIVE_ACME_STORAGE_KEY:-}
probe_url=${VECTIS_LIVE_ACME_PROBE_URL:-"https://$domain/.vectis-live-acme"}
work=$(mktemp -d "${TMPDIR:-/tmp}/vectis-live-acme.XXXXXX")

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

local function new_server(name)
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
  local server, new_error = vectis.server.new({
    app_name = name,
    bind = bind,
    port = port,
    tls = tls,
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

local first = new_server("vectis-live-acme-issue")
local started, start_error = first:start()
require_ok(started, start_error, "initial ACME start")
wait_for_public_https("issue")
local stopped, stop_error = first:stop()
require_ok(stopped, stop_error, "initial ACME stop")
first:close()

local restored = new_server("vectis-live-acme-restore")
started, start_error = restored:start()
require_ok(started, start_error, "restored ACME start")
wait_for_public_https("restore")
stopped, stop_error = restored:stop()
require_ok(stopped, stop_error, "restored ACME stop")
restored:close()

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
