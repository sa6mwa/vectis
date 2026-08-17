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

port=$("$port_helper")
cat >"$script" <<LUA
local vectis = require("vectis")
local server = assert(vectis.server.new({
  bind = "127.0.0.1",
  port = ${port},
  tls = { mode = "disabled" },
}))
assert(server:websocket({
  path = "/ws",
  message = function(ws, opcode, payload)
    assert(opcode == vectis.websocket.TEXT)
    assert(ws:send_text("lua:" .. payload) == true)
  end,
}) == true)
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
