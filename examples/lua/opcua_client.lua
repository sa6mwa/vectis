local opcua = require("opcua")

local endpoint = OPCUA_ENDPOINT or os.getenv("OPCUA_ENDPOINT")
assert(endpoint ~= nil and endpoint ~= "", "OPCUA_ENDPOINT is required")

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
