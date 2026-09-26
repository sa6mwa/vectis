#!/usr/bin/env sh
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: server_websocket.sh VECTIS_BIN PORT_HELPER WS_CLIENT" >&2
  exit 2
fi

vectis_bin=$1
port_helper=$2
ws_client=$3

if [ ! -x "$vectis_bin" ]; then
  echo "skipping: vectis binary is not built: $vectis_bin" >&2
  exit 77
fi
if [ ! -x "$port_helper" ]; then
  echo "skipping: port helper is not built: $port_helper" >&2
  exit 77
fi
if [ ! -x "$ws_client" ]; then
  echo "skipping: websocket client helper is not built: $ws_client" >&2
  exit 77
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/vectis-lua-websocket.XXXXXX")
script="$tmpdir/websocket.lua"
log="$tmpdir/server.log"
server_pid=

cleanup() {
  if [ "${server_pid:-}" ]; then
    kill -TERM "-$server_pid" 2>/dev/null || kill "$server_pid" 2>/dev/null || true
    sleep 0.2
    kill -KILL "-$server_pid" 2>/dev/null || kill -KILL "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$tmpdir"
}
trap cleanup EXIT INT TERM

process_running() {
  pid=$1
  if ! kill -0 "$pid" 2>/dev/null; then
    return 1
  fi
  if [ -r "/proc/$pid/stat" ]; then
    state=$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null || true)
    [ "$state" != "Z" ]
    return $?
  fi
  return 0
}

wait_for_log() {
  pattern=$1
  i=0
  while [ "$i" -lt 100 ]; do
    if grep -q "$pattern" "$log" 2>/dev/null; then
      return 0
    fi
    if [ "${server_pid:-}" ] && ! process_running "$server_pid"; then
      return 1
    fi
    i=$((i + 1))
    sleep 0.1
  done
  return 1
}

for logging in true false; do
port=$("$port_helper")
backend_log="$tmpdir/backend-$logging.jsonl"
cat >"$script" <<LUA
local vectis = require("vectis")
assert(require("vectis.log").configure({
  env = false,
  output = "$backend_log",
  subs = { vectis = $logging },
}))
local server = assert(vectis.app.new({
  bind = "127.0.0.1",
  port = ${port},
  tls = { mode = "disabled" },
}))
assert(server:websocket({
  path = "/ws",
  connect = function() error("fixture/connect") end,
  message = function(ws, opcode, payload)
    assert(opcode == vectis.websocket.TEXT)
    assert(ws:send_text("lua:" .. payload) == true)
    error("fixture/message")
  end,
  disconnect = function() error("fixture/disconnect") end,
}) == true)
collectgarbage("collect")
assert(server:start() == true)
io.stdout:write("READY\\n")
io.stdout:flush()
assert(server:wait() == true)
server:close()
io.stdout:write("STOPPED\\n")
io.stdout:flush()
LUA

(
  exec setsid "$vectis_bin" "$script"
) >"$log" 2>&1 &
server_pid=$!

if ! wait_for_log '^READY$'; then
  echo "Lua websocket server did not become ready" >&2
  cat "$log" >&2 || true
  exit 1
fi

"$ws_client" "$port" /ws ping lua:ping

kill -TERM "-$server_pid"
if ! wait_for_log '^STOPPED$'; then
  echo "Lua websocket server did not stop" >&2
  cat "$log" >&2 || true
  exit 1
fi
wait "$server_pid" || true
server_pid=

python3 - "$backend_log" "$log" "$logging" <<'PY'
import json
import pathlib
import sys

records = [json.loads(line) for line in pathlib.Path(sys.argv[1]).read_text().splitlines()]
errors = [record for record in records if record.get("msg") == "vectis.websocket.callback_failed"]
if sys.argv[3] == "true":
    assert len(errors) == 3, records
    assert {record["callback"] for record in errors} == {"connect", "message", "disconnect"}, errors
    for record in errors:
        assert record["lvl"] == "error", record
        assert record["sub"] == "vectis", record
        assert "fixture/" + record["callback"] in record["error"], record
else:
    assert not errors, errors
raw = pathlib.Path(sys.argv[2]).read_text()
assert "fixture/" not in raw and "callback_failed" not in raw, raw
PY
done
