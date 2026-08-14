local lql = require("lql")
local pslog = require("pslog")
local softline = require("softline")

local lql_client = assert(lql.new())
local capabilities = lql_client:capabilities()
assert(type(lql.version()) == "string")
assert(capabilities.selector_parse == true)
assert(capabilities.apply_string_spooled == true)
assert(type(lql.status_string(0)) == "string")
assert(lql.path_is_regular_file(arg[0]) == true)

local log_chunks = {}
local log = assert(pslog.new_json({
  output = function(chunk)
    log_chunks[#log_chunks + 1] = chunk
  end,
  disable_timestamp = true,
  no_color = true,
})):with("service", "vectis-example")
log:info("raw dependency example", "module", "lql", "ok", true)
log:close()
local log_payload = table.concat(log_chunks)
assert(log_payload:find('"msg":"raw dependency example"', 1, true))
assert(log_payload:find('"service":"vectis-example"', 1, true))
assert(log_payload:find('"module":"lql"', 1, true))

local editor = assert(softline.new({ line_max_len = 64 }))
assert(editor:set_buffer("vectis"))
assert(editor:insert(" lua"))
assert(editor:buffer() == "vectis lua")
assert(editor:set_cursor(6))
assert(editor:insert(" raw"))
assert(editor:buffer() == "vectis raw lua")
editor:close()

print("lua raw dependency tools example ok")
