local vectis = require("vectis")
local lockdc = require("lockdc")
local lonejson = require("lonejson")
local cai = require("cai")
local pslog = require("pslog")
local libmdf = require("libmdf")
local softline = require("softline")

assert(type(vectis) == "table")
assert(vectis.version == (os.getenv("VECTIS_EXPECTED_VERSION") or "0.0.0"))
assert(vectis.status_string(vectis.OK) == "ok")
assert(vectis.status_string(vectis.ERR_INVALID) == "invalid")
assert(arg[0]:match("smoke%.lua$"))
assert(arg[1] == "first")
assert(arg[2] == "second")

assert(type(lonejson) == "table")
assert(lonejson.encode_json(lonejson.json_null) == "null")

assert(type(lockdc) == "table")
assert(type(lockdc.open) == "function")
assert(type(lockdc.version_string()) == "string")
assert(lockdc.encode_json({ ok = true }) == '{"ok":true}')

assert(type(cai) == "table")
assert(type(cai.open) == "function")
assert(type(cai.mcp_handler) == "function")
assert(type(cai.MODEL_DEFAULT_RESPONSES) == "string")
assert(type(cai.MCP_PROTOCOL_VERSION) == "string")

assert(type(pslog) == "table")
assert(type(pslog.new_json) == "function")
assert(type(pslog.version()) == "string")
local log_chunks = {}
local log = assert(pslog.new_json(function(chunk)
  log_chunks[#log_chunks + 1] = chunk
end, { timestamps = false }))
log:info("lua smoke", "component", "vectis")
log:close()
local log_payload = table.concat(log_chunks)
assert(log_payload:match('"msg":"lua smoke"'))
assert(log_payload:match('"component":"vectis"'))

assert(type(libmdf) == "table")
assert(type(libmdf.render) == "function")
assert(type(libmdf.render_stream) == "function")
local rendered_markdown = libmdf.render("# Vectis\n\n**ok**", { format = "html" })
assert(rendered_markdown:match("Vectis"))
assert(rendered_markdown:match("ok"))

assert(type(softline) == "table")
assert(type(softline.new) == "function")
local line = assert(softline.new({ line_max_len = 32 }))
assert(line:set_buffer("draft"))
assert(line:insert("++"))
assert(line:buffer():match("%+%+"))
line:close()

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
