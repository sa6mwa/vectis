local core = require("lonejson.core")
local M = {}

local function field(kind, opts)
  opts = opts or {}
  opts.kind = kind
  return opts
end

function M.field(name, spec)
  spec = spec or {}
  spec.name = name
  return spec
end

function M.string(opts) return field("string", opts) end
function M.spooled_text(opts) return field("spooled_text", opts) end
function M.spooled_bytes(opts) return field("spooled_bytes", opts) end
function M.json_value(opts) return field("json_value", opts) end
function M.i64(opts) return field("i64", opts) end
function M.u64(opts) return field("u64", opts) end
function M.f64(opts) return field("f64", opts) end
function M.boolean(opts) return field("boolean", opts) end
M.bool = M.boolean
function M.object(opts) return field("object", opts) end
function M.string_array(opts) return field("string_array", opts) end
function M.i64_array(opts) return field("i64_array", opts) end
function M.u64_array(opts) return field("u64_array", opts) end
function M.f64_array(opts) return field("f64_array", opts) end
function M.boolean_array(opts) return field("boolean_array", opts) end
function M.object_array(opts) return field("object_array", opts) end

function M.json_array(value)
  value = value or {}
  return setmetatable(value, { __lonejson_json_kind = "array" })
end

function M.json_object(value)
  value = value or {}
  return setmetatable(value, { __lonejson_json_kind = "object" })
end

function M.schema(name, fields) return core.new():schema(name, fields) end

function M.chunks(spool, chunk_size)
  spool:rewind()
  return function() return spool:read(chunk_size or 4096) end
end

M.array_rewrite_string = core.array_rewrite_string
M.array_rewrite_path = core.array_rewrite_path
M.encode_json = core.encode_json
M.encode_json_to_sink = core.encode_json_to_sink
M.encode_value = core.encode_json
M.encode_value_to_sink = core.encode_json_to_sink
M.decode_json = core.decode_json
M.decode_value = core.decode_json
M.core = core
M.json_null = core.json_null()

return M
