local vectis = require("vectis")
local lonejson = require("lonejson")

assert(type(vectis) == "table")
assert(vectis.version == "0.0.0")
assert(vectis.status_string(vectis.OK) == "ok")
assert(vectis.status_string(vectis.ERR_INVALID) == "invalid")
assert(arg[0]:match("smoke%.lua$"))
assert(arg[1] == "first")
assert(arg[2] == "second")

assert(type(lonejson) == "table")
assert(lonejson.encode_json(lonejson.json_null) == "null")

local encoded = lonejson.encode_json({
  b = true,
  a = lonejson.json_array({ "first", lonejson.json_null, 3 }),
})
assert(encoded == '{"a":["first",null,3],"b":true}')

local decoded = lonejson.decode_json(encoded)
assert(decoded.a[1] == "first")
assert(decoded.a[2] == lonejson.json_null)
assert(decoded.a[3] == 3)
assert(decoded.b == true)

local chunks = {}
lonejson.encode_json_to_sink({ z = "sink", a = lonejson.json_array({ true, false }) }, function(chunk)
  chunks[#chunks + 1] = chunk
end)
assert(table.concat(chunks) == '{"a":[true,false],"z":"sink"}')
