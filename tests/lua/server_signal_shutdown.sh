#!/usr/bin/env sh
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: server_signal_shutdown.sh VECTIS_BIN PORT_HELPER C_EXAMPLE" >&2
  exit 2
fi

vectis_bin=$1
port_helper=$2
c_example=$3

if [ ! -x "$vectis_bin" ]; then
  echo "skipping: vectis binary is not built: $vectis_bin" >&2
  exit 77
fi
if [ ! -x "$port_helper" ]; then
  echo "skipping: port helper is not built: $port_helper" >&2
  exit 77
fi

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/vectis-signal-shutdown.XXXXXX")
script="$tmpdir/signal-shutdown.lua"
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

wait_for_http() {
  url=$1
  expected=$2
  i=0
  while [ "$i" -lt 100 ]; do
    if curl -fsS "$url" 2>/dev/null | grep -qx "$expected"; then
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

run_lua_case() {
  signal=$1
  port=$("$port_helper")
  rm -f "$log"
  cat >"$script" <<LUA
local vectis = require("vectis")
local server = assert(vectis.app.new({
  bind = "127.0.0.1",
  port = ${port},
  tls = { mode = "disabled" },
}))
assert(server:json({
  path = "/",
  body = "signal\\n",
  content_type = "text/plain",
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
    echo "lua server did not become ready for $signal" >&2
    cat "$log" >&2 || true
    exit 1
  fi
  if ! wait_for_http "http://127.0.0.1:${port}/" 'signal'; then
    echo "lua server did not accept requests for $signal" >&2
    cat "$log" >&2 || true
    exit 1
  fi
  kill "-$signal" "-$server_pid"
  if ! wait_for_log '^STOPPED$'; then
    echo "lua server did not stop after SIG$signal" >&2
    cat "$log" >&2 || true
    exit 1
  fi
  wait "$server_pid" || true
  server_pid=
}

run_c_case() {
  signal=$1
  if [ ! -x "$c_example" ]; then
    echo "skipping C example signal case: $c_example is not built" >&2
    return 0
  fi
  port=$("$port_helper")
  rm -f "$log"
  (
    exec setsid "$c_example" "$port"
  ) >"$log" 2>&1 &
  server_pid=$!
  if ! wait_for_http "http://127.0.0.1:${port}/health" 'ok'; then
    echo "C example did not become ready for $signal" >&2
    cat "$log" >&2 || true
    exit 1
  fi
  kill "-$signal" "-$server_pid"
  i=0
  while process_running "$server_pid" && [ "$i" -lt 50 ]; do
    i=$((i + 1))
    sleep 0.1
  done
  if process_running "$server_pid"; then
    echo "C example did not exit after SIG$signal" >&2
    cat "$log" >&2 || true
    exit 1
  fi
  wait "$server_pid" || true
  server_pid=
}

run_lua_case INT
run_lua_case TERM
run_c_case INT
run_c_case TERM
