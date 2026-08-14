local opcua = require("opcua")

local endpoint = OPCUA_ENDPOINT or os.getenv("OPCUA_ENDPOINT")
assert(endpoint ~= nil and endpoint ~= "", "OPCUA_ENDPOINT is required")

local server_port =
    tonumber(OPCUA_LUA_SERVER_PORT or os.getenv("OPCUA_LUA_SERVER_PORT") or "")
if server_port then
  local server = assert(opcua.server({ port = server_port }))
  assert(server:set_endpoint({ host = "127.0.0.1", port = server_port }) == true)
  assert(server:set_application_identity({
    application_uri = "urn:vectis:example:opcua",
    product_uri = "urn:vectis",
    application_name = "Vectis OPC UA Lua Example",
  }) == true)
  assert(server:set_access_control({ allow_anonymous = true }) == true)
  local namespace_index =
      assert(server:add_namespace("urn:vectis:example:opcua"))
  local owned_node = opcua.node_id_numeric(namespace_index, 8201)
  assert(server:add_variable({
    node_id = owned_node,
    browse_name = "exampleValue",
    display_name = "Example Value",
    value = opcua.value_integer(5),
  }) == true)
  assert(assert(server:read(owned_node)):get() == 5)
  assert(server:write(owned_node, opcua.value_integer(6)) == true)
  assert(assert(server:read(owned_node)):get() == 6)
  assert(server:startup() == true)
  assert(type(server:endpoint_url()) == "string")
  assert(type(server:iterate(false)) == "number")
  assert(server:shutdown() == true)
  assert(server:close() == true)
end

local node = opcua.node_id_numeric(1, 7101)
local client = assert(opcua.connect(endpoint))

local value = assert(client:read(node))
assert(value:type() == opcua.VALUE_INTEGER)
local initial = value:get()
assert(type(initial) == "number")

local updated_value = initial + 35
assert(client:write(node, opcua.value_integer(updated_value)) == true)
local updated = assert(client:read(node))
assert(updated:type() == opcua.VALUE_INTEGER)
assert(updated:get() == updated_value)

assert(client:disconnect() == true)
assert(client:close() == true)

local manual = assert(opcua.client())
assert(manual:connect(endpoint) == true)
assert(manual:disconnect() == true)
assert(manual:close() == true)

print("lua opcua client example ok")
