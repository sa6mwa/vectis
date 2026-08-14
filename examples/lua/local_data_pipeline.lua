local lql = require("lql")
local pslog = require("pslog")
local softline = require("softline")
local zlib = require("zlib")

local lql_client = assert(lql.new())
local capabilities = lql_client:capabilities()
assert(capabilities.selector_parse == true)
assert(capabilities.apply_string_spooled == true)
assert(lql.path_is_regular_file(arg[0]) == true)

local log_chunks = {}
local log = assert(pslog.new_json({
  output = function(chunk)
    log_chunks[#log_chunks + 1] = chunk
  end,
  disable_timestamp = true,
  no_color = true,
})):with("service", "vectis-example")
log:info("local data pipeline ready", "selector", "$.orders[*]", "fields", 2)
log:close()
local log_payload = table.concat(log_chunks)
assert(log_payload:find('"msg":"local data pipeline ready"', 1, true))
assert(log_payload:find('"service":"vectis-example"', 1, true))
assert(log_payload:find('"selector":"$.orders[*]"', 1, true))

local editor = assert(softline.new({ line_max_len = 64 }))
assert(editor:set_buffer("vectis"))
assert(editor:insert(" orders"))
assert(editor:buffer() == "vectis orders")
assert(editor:set_cursor(6))
assert(editor:insert(" local"))
assert(editor:buffer() == "vectis local orders")
editor:close()

local compressed = assert(zlib.gzip("order report\n"))
assert(assert(zlib.decompress(compressed)) == "order report\n")
local zlib_input_path = os.tmpname()
local zlib_gzip_path = os.tmpname()
local zlib_output_path = os.tmpname()
local zlib_file = assert(io.open(zlib_input_path, "wb"))
zlib_file:write("order report file\n")
zlib_file:close()
assert(zlib.gzip_file({
  input_path = zlib_input_path,
  output_path = zlib_gzip_path,
}).ok)
assert(zlib.decompress_file({
  input_path = zlib_gzip_path,
  output_path = zlib_output_path,
}).ok)
zlib_file = assert(io.open(zlib_output_path, "rb"))
assert(zlib_file:read("*a") == "order report file\n")
zlib_file:close()
os.remove(zlib_input_path)
os.remove(zlib_gzip_path)
os.remove(zlib_output_path)

print("lua local data pipeline example ok")
