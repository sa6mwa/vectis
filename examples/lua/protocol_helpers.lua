local mqtt = require("vectis.mqtt")
local smtp = require("vectis.smtp")
local vectis = require("vectis")

local function join_path(dir, name)
  if dir:sub(-1) == "/" then
    return dir .. name
  end
  return dir .. "/" .. name
end

local work_dir = os.getenv("VECTIS_LUA_PROTOCOL_EXAMPLE_DIR") or "."
local message_path = join_path(work_dir, "vectis-lua-protocol-message.eml")
local scp_upload_path = join_path(work_dir, "vectis-lua-protocol-scp.txt")

local message_file = assert(io.open(message_path, "wb"))
message_file:write(table.concat({
  "From: sender@example.test\r\n",
  "To: recipient@example.test\r\n",
  "Subject: Vectis protocol example\r\n",
  "\r\n",
  "hello from vectis smtp\r\n",
}))
message_file:close()

local scp_file = assert(io.open(scp_upload_path, "wb"))
scp_file:write("scp payload\n")
scp_file:close()

local mqtt_result = mqtt.publish({
  url = "mqtt://127.0.0.1:1/vectis/example",
  payload = "{\"event\":\"example\"}\n",
  timeout_ms = 200,
  connect_timeout_ms = 50,
  retry = false,
  no_signal = true,
})
assert(mqtt_result.ok == false)
assert(mqtt_result.transport_ok == false)
assert(type(mqtt_result.error) == "table")
assert(type(mqtt_result.error.message) == "string")

local smtp_result = smtp.send({
  url = "smtp://127.0.0.1:9",
  from = "sender@example.test",
  to = {"recipient@example.test"},
  body_path = message_path,
  timeout_ms = 200,
  connect_timeout_ms = 50,
  retry = false,
  no_signal = true,
})
assert(smtp_result.ok == false)
assert(smtp_result.transport_ok == false)
assert(type(smtp_result.error) == "table")
assert(type(smtp_result.error.message) == "string")

local scp_ok, scp_err = vectis.ssh.scp_upload_file({
  host = "127.0.0.1",
  username = "vectis",
  password = "secret",
  local_path = scp_upload_path,
})
assert(scp_ok == nil)
assert(type(scp_err) == "table")
assert(scp_err.status == vectis.ERR_INVALID)
assert(scp_err.message:find("remote_path", 1, true))

os.remove(message_path)
os.remove(scp_upload_path)

print("lua protocol helpers example ok")
