#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

if [ "${VECTIS_OPCUA_PUBSUB_LIVE:-0}" != "1" ]; then
  printf '%s\n' \
    'SKIP: set VECTIS_OPCUA_PUBSUB_LIVE=1 to run live OPC UA PubSub/MQTT broker validation'
  exit 0
fi

vectis_bin=${VECTIS_BIN:-"$repo_root/build/debug/vectis"}
if [ ! -x "$vectis_bin" ]; then
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=tool-discovery
status=failed
class=external-tool-unavailable
reason=vectis-binary-missing
artifact=$vectis_bin
next=run make build-debug or set VECTIS_BIN to a built vectis executable
PKT_DIAGNOSTIC_END
EOF
  exit 2
fi

services_started=0
cleanup_services() {
  if [ "$services_started" -eq 1 ] &&
     [ "${VECTIS_E2E_KEEP_DEVSERVICES:-0}" != "1" ]; then
    "$script_dir/dev-down.sh" >/dev/null 2>&1 || true
  fi
}
trap cleanup_services EXIT INT TERM

if [ "${VECTIS_OPCUA_PUBSUB_USE_EXISTING_MQTT:-0}" != "1" ]; then
  if ! "$script_dir/dev-up.sh"; then
    cat >&2 <<'EOF'
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=dev-services
status=failed
class=e2e-service
reason=dev-up-failed
next=inspect docker/nerdctl availability and local MQTT service logs
PKT_DIAGNOSTIC_END
EOF
    exit 2
  fi
  services_started=1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/vectis-opcua-pubsub.XXXXXX")
cleanup_work() {
  rm -rf "$work"
}
trap 'cleanup_work; cleanup_services' EXIT INT TERM

mqtt_host=${VECTIS_OPCUA_PUBSUB_HOST:-127.0.0.1}
mqtt_port=${VECTIS_OPCUA_PUBSUB_PORT:-${VECTIS_MQTT_PORT:-21883}}
mqtt_topic=${VECTIS_OPCUA_PUBSUB_TOPIC:-"vectis/live/opcua/pubsub/$$"}

cat >"$work/live-opcua-pubsub.lua" <<'LUA'
local opcua = require("opcua")

local host = os.getenv("VECTIS_OPCUA_PUBSUB_HOST") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_OPCUA_PUBSUB_PORT") or
                      os.getenv("VECTIS_MQTT_PORT") or "21883")
local topic = os.getenv("VECTIS_OPCUA_PUBSUB_TOPIC") or
              ("vectis/live/opcua/pubsub/" .. tostring(os.time()))

local server = assert(opcua.server({ port = 0 }))
local ns = assert(server:add_namespace("urn:vectis:live:opcua:pubsub"))
local variable = opcua.node_id_numeric(ns, 9101)

assert(server:add_variable({
  node_id = variable,
  browse_name = "livePubSubValue",
  display_name = "Live PubSub Value",
  value = opcua.value_integer(1),
}) == true)

local connection = assert(server:add_mqtt_pubsub_connection({
  name = "vectisLiveMqtt",
  host = host,
  port = port,
  topic = topic,
  publisher_id = 91001,
  keep_alive_seconds = 5,
  validate_only = false,
  enabled = true,
}))

local dataset = assert(server:add_published_dataset("vectisLiveDataset"))
assert(server:add_published_variable({
  published_dataset_id = dataset,
  variable_node_id = variable,
  field_name = "livePubSubValue",
}))

local writer_group = assert(server:add_pubsub_writer_group(connection, {
  name = "vectisLiveWriterGroup",
  writer_group_id = 910,
  publishing_interval_ms = 25,
  json_encoding = true,
  enabled = true,
}))

assert(server:add_pubsub_data_set_writer(writer_group, dataset, {
  name = "vectisLiveDataSetWriter",
  data_set_writer_id = 911,
  key_frame_count = 1,
  enabled = true,
}))

local config = assert(server:write_pubsub_configuration())
assert(type(config) == "string" and #config > 0,
       "live PubSub configuration should serialize")

assert(server:startup() == true)
for i = 1, 8 do
  assert(server:write(variable, opcua.value_integer(i)) == true)
  assert(type(server:iterate(false)) == "number")
end
assert(server:shutdown() == true)
assert(server:close() == true)

print("opcua_pubsub_live=ok")
print("mqtt_endpoint=mqtt://" .. host .. ":" .. tostring(port))
print("mqtt_topic=" .. topic)
LUA

if ! VECTIS_OPCUA_PUBSUB_HOST="$mqtt_host" \
     VECTIS_OPCUA_PUBSUB_PORT="$mqtt_port" \
     VECTIS_OPCUA_PUBSUB_TOPIC="$mqtt_topic" \
     "$vectis_bin" "$work/live-opcua-pubsub.lua" \
       >"$work/live-opcua-pubsub.out" \
       2>"$work/live-opcua-pubsub.err"; then
  cat >&2 <<EOF
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=opcua-pubsub
status=failed
class=e2e-service
reason=live-opcua-pubsub-command-failed
artifact=$work/live-opcua-pubsub.err
next=inspect the local MQTT broker endpoint and cpkt OPC UA PubSub/MQTT support
PKT_DIAGNOSTIC_END
EOF
  cat "$work/live-opcua-pubsub.err" >&2
  exit 1
fi

if ! grep -Fxq 'opcua_pubsub_live=ok' "$work/live-opcua-pubsub.out"; then
  cat >&2 <<'EOF'
PKT_DIAGNOSTIC_BEGIN
surface=prerelease-live
phase=opcua-pubsub
status=failed
class=test
reason=missing-success-marker
next=inspect live OPC UA PubSub script output
PKT_DIAGNOSTIC_END
EOF
  cat "$work/live-opcua-pubsub.out" >&2
  exit 1
fi

cat "$work/live-opcua-pubsub.out"
