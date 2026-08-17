include("${VECTIS_SOURCE_DIR}/tests/lua/port_retry.cmake")

set(script "${WORK_DIR}/vectis-server-restart.lua")

file(WRITE "${script}" [[
local vectis = require("vectis")

local port = assert(tonumber(os.getenv("VECTIS_SERVER_RESTART_PORT")))
local server = assert(vectis.server.new({
  bind = "127.0.0.1",
  port = port,
  tls = { mode = "disabled" },
}))

assert(type(server.restart) == "function")
assert(type(server.reload) == "function")
assert(server:json({
  path = "/",
  body = "restart-ok\n",
  content_type = "text/plain",
}) == true)

local client = vectis.http.client({
  protocols = "http",
  timeout_ms = 5000,
  connect_timeout_ms = 500,
  no_signal = true,
  retry = {
    max_attempts = 40,
    initial_delay_ms = 25,
    max_delay_ms = 25,
    conditions = {"transport", "5xx"},
  },
})
local url = "http://127.0.0.1:" .. tostring(port) .. "/"

assert(server:start() == true)
local first = client.get(url)
assert(first.ok == true, first.error and first.error.message)
assert(first.status == 200)
assert(first.body == "restart-ok\n")

assert(server:restart() == true)
local second = client.get(url)
assert(second.ok == true, second.error and second.error.message)
assert(second.status == 200)
assert(second.body == "restart-ok\n")

assert(server:reload() == true)
local third = client.get(url)
assert(third.ok == true, third.error and third.error.message)
assert(third.status == 200)
assert(third.body == "restart-ok\n")

assert(server:stop() == true)
server:close()
print("vectis-server-restart-ok")
]])

vectis_run_command_with_port(
  LABEL "vectis lua server restart"
  PORT_ENV VECTIS_SERVER_RESTART_PORT
  SUCCESS_MARKER "vectis-server-restart-ok"
  COMMAND "${VECTIS_BIN}" "${script}")
