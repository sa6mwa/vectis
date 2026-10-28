local zlib = require("zlib")
local vectis = require("vectis")
assert(zlib.zlibVersion == zlib.version)
assert(zlib.ZLIB_VERSION == zlib.version())
assert(math.type(zlib.zlibCompileFlags()) == "integer")
assert(zlib.Z_OK == 0 and zlib.Z_STREAM_END == 1 and zlib.Z_NEED_DICT == 2)
assert(zlib.Z_FINISH == 4 and zlib.Z_TREES == 6)
assert(zlib.Z_BEST_COMPRESSION == 9 and zlib.Z_DEFAULT_COMPRESSION == -1)
assert(zlib.Z_ASCII == zlib.Z_TEXT and zlib.Z_DEFLATED == 8)
assert(zlib.zError(zlib.Z_OK) == "")
assert(zlib.zError(zlib.Z_DATA_ERROR) == "data error")
assert(not pcall(zlib.zError, 1.0))
assert(not pcall(zlib.zError, 0x100000000))
assert(not pcall(zlib.zError, 3))
assert(not pcall(zlib.zError, -7))
assert(not pcall(zlib.zError, 0x7fffffff))
for _, item in ipairs({{"crc32", 0, 0xcbf43926}, {"adler32", 1, 0x091e01de}}) do
  local name, initial, expected = table.unpack(item)
  assert(zlib[name .. "_initial"]() == initial)
  assert(zlib[name .. "_materialized"]("123456789") == expected)
  assert(zlib[name .. "_materialized"]("", 123) == 123)
  local prefix = zlib[name .. "_materialized"]("1234")
  local suffix = zlib[name .. "_materialized"]("56789")
  assert(zlib[name .. "_update"](prefix, "56789") == expected)
  assert(zlib[name .. "_combine"](prefix, suffix, 5) == expected)
  assert(zlib[name .. "_combine64"](prefix, suffix, {high=0, low=5}) == expected)
  for _, bad in ipairs({-1, 0x100000000, 1.0, "1"}) do
    assert(not pcall(zlib[name .. "_update"], bad, "x"))
  end
  assert(not pcall(zlib[name .. "_update"], initial, ("x"):rep(65537)))
  assert(not pcall(zlib[name .. "_combine"], prefix, suffix, -1))
  assert(not pcall(zlib[name .. "_combine64"], prefix, suffix,
                  {high=0x80000000, low=0}))
end
local polynomial = zlib.crc32_combine_gen(5)
assert(polynomial == zlib.crc32_combine_gen64({high=0, low=5}))
assert(zlib.crc32_combine_op(zlib.crc32_materialized("1234"),
                            zlib.crc32_materialized("56789"), polynomial) == 0xcbf43926)
assert(not pcall(zlib.crc32_combine_gen, -1))
assert(not pcall(zlib.crc32_combine_op, 0, 0, 0x100000000))
assert(zlib.compress_stream == zlib.deflate_stream)
local function reader(data, stride)
  local offset = 1
  return function(capacity)
    assert(capacity == 65536)
    if offset > #data then return nil end
    local chunk = data:sub(offset, offset + (stride or capacity) - 1)
    offset = offset + #chunk
    return chunk
  end
end
local function collect(transform, data, opts, stride)
  local chunks = {}
  local result, err = transform(reader(data, stride), function(chunk)
    assert(#chunk > 0 and #chunk <= 65536)
    chunks[#chunks + 1] = chunk
    return true
  end, opts)
  return result and table.concat(chunks), result or err
end
local binary = "\0\255\1\0hello\128world"
for _, names in ipairs({{"deflate_stream", "inflate_stream", "deflate_materialized"},
                       {"compress_stream", "decompress_stream", "compress_materialized"},
                       {"gzip_stream", "gunzip_stream", "gzip_materialized"}}) do
  for _, data in ipairs({"", binary, binary:rep(100)}) do
    local packed, result = collect(zlib[names[1]], data, {level = 9}, 1)
    assert(packed and result.ok and result.input_bytes == #data)
    assert(result.output_bytes == #packed and result.members == 1)
    assert(packed == assert(zlib[names[3]](data, {level = 9})))
    local restored, decoded = collect(zlib[names[2]], packed, nil, 1)
    assert(restored == data and decoded.input_bytes == #packed)
    assert(decoded.output_bytes == #data and decoded.members == 1)
    assert(collect(zlib.decompress_stream, packed, nil, 1) == data)
  end
end
assert(zlib.deflate == nil and zlib.inflate == nil and zlib.gzip == nil)

-- A >1 MiB producer generates and releases fragments, retaining one tiny block.
-- The decompressor validates every byte incrementally without collecting output.
local block = {}
for i = 0, 255 do block[#block + 1] = string.char(i) end
block = table.concat(block)
local total = 1024 * 1024 + 173
for _, opts in ipairs({{level = 0}, {level = 6, sync_flush = true}}) do
  local offset, eof, early_bytes, decoded, decoded_early = 0, false, 0, 0, 0
  local decoder = assert(zlib.gunzip_stream_new())
  local function verify(chunk)
    local start = decoded % #block
    local expected = block:rep(math.ceil((#chunk + start) / #block)):sub(start + 1, start + #chunk)
    assert(chunk == expected)
    decoded = decoded + #chunk
    if not eof then decoded_early = decoded_early + #chunk end
  end
  local result = assert(zlib.gzip_stream(function(capacity)
    assert(capacity == 65536)
    if offset == total then eof = true; return nil end
    local size = math.min(1 + (offset % 7919), total - offset)
    local start = offset % #block
    local chunk = block:rep(math.ceil((size + start) / #block)):sub(start + 1, start + size)
    assert(#chunk == size and size <= capacity)
    offset = offset + size
    return chunk
  end, function(chunk)
    if not eof then early_bytes = early_bytes + #chunk end
    local offset = 0
    repeat
      local output, consumed = assert(decoder:process(chunk:sub(offset + 1)))
      offset = offset + consumed
      verify(output)
      if offset == #chunk and #output < 65536 then break end
    until false
    collectgarbage("collect")
    return true
  end, opts))
  assert(early_bytes > 32 and eof and result.input_bytes == total)
  assert(decoded_early > 0)
  if opts.level == 0 then assert(early_bytes > 65536) end
  local finished
  repeat
    local output, consumed
    output, consumed, finished = decoder:process("", "finish")
    assert(output)
    assert(consumed == 0)
    verify(output)
  until finished
  assert(decoded == total and decoder:result().input_bytes == result.output_bytes)
  decoder:close()
end

-- A bounded compressed producer also exercises the callback decoding helper
-- above 1 MiB, with no full input or output value at either end.
do
  local encoder = assert(zlib.gzip_stream_new({level = 0}))
  local offset, decoded, finished = 0, 0, false
  local result = assert(zlib.gunzip_stream(function(capacity)
    assert(capacity == 65536)
    while not finished do
      local chunk, consumed
      if offset < total then
        local size = math.min(4093, total - offset)
        local start = offset % #block
        local input = block:rep(math.ceil((size + start) / #block)):sub(start + 1, start + size)
        chunk, consumed = encoder:process(input)
        assert(consumed >= 0 and consumed <= size)
        offset = offset + consumed
      else
        chunk, consumed, finished = encoder:process("", "finish")
      end
      assert(chunk)
      if #chunk > 0 then return chunk end
    end
    return nil
  end, function(chunk)
    local start = decoded % #block
    assert(chunk == block:rep(math.ceil((#chunk + start) / #block)):sub(start + 1, start + #chunk))
    decoded = decoded + #chunk
    return true
  end, {max_output_bytes = total}))
  assert(result.output_bytes == total and decoded == total)
  assert(result.input_bytes == encoder:result().output_bytes)
  encoder:close()
end

local empty = assert(zlib.gzip_materialized(""))
local gzip = assert(zlib.gzip_materialized(binary))
local compressed = assert(zlib.deflate_materialized(binary))
local members = empty .. gzip .. empty .. gzip .. empty
for _, decode in ipairs({zlib.gunzip_stream, zlib.decompress_stream}) do
  local value, result = collect(decode, members, {max_output_bytes = #binary * 2}, 1)
  assert(value == binary .. binary and result.members == 5)
  assert(collect(decode, empty .. empty, {max_output_bytes = 0}, 1) == "")
  for _, bad in ipairs({members:sub(1, -2), members .. "garbage", "", gzip:sub(1, -2),
                       gzip:sub(1, -2) .. string.char(gzip:byte(-1) ~ 1)}) do
    value, result = collect(decode, bad, nil, 1)
    assert(value == nil and result.status == vectis.ERR_INVALID)
  end
end
for _, decode in ipairs({zlib.inflate_stream, zlib.decompress_stream}) do
  local value, err = collect(decode, compressed .. "x", nil, 1)
  assert(value == nil and err.status == vectis.ERR_INVALID)
end
local delivered = 0
local value, err = zlib.gunzip_stream(reader(gzip, 1), function(chunk)
  delivered = delivered + #chunk
  return true
end, {max_output_bytes = #binary - 1})
assert(value == nil and err.status == vectis.ERR_INVALID and delivered <= #binary - 1)
value, err = collect(zlib.gzip_stream, "", {max_output = 0})
assert(value == nil and err.status == vectis.ERR_INVALID)
assert(collect(zlib.gunzip_stream, gzip, {max_output_bytes = {high = 0xffffffff, low = 0xffffffff}}) == binary)

local identity = {}
local function same_error(fn)
  local ok, error_value = pcall(fn)
  assert(not ok and error_value == identity)
end
same_error(function() zlib.gzip_stream(function() error(identity) end, function() return true end) end)
same_error(function() zlib.gzip_stream(function() return nil, identity end, function() return true end) end)
same_error(function() zlib.gzip_stream(reader(binary), function() error(identity) end) end)
same_error(function() zlib.gzip_stream(reader(binary), function() return false, identity end) end)
for _, bad_reader in ipairs({function() return "" end, function() return false end,
                            function() return ("x"):rep(65537) end}) do
  value, err = zlib.gzip_stream(bad_reader, function() return true end)
  assert(value == nil and err.status == vectis.ERR_INVALID)
end
for _, sink in ipairs({function() return false end, function() return nil end}) do
  value, err = zlib.gzip_stream(reader(binary), sink)
  assert(value == nil and err.status == vectis.ERR_STATE)
end
for _, opts in ipairs({{unknown = true}, {level = 10}, {max_output_bytes = -1},
                       {max_output = 1, max_output_bytes = 1}, {sync_flush = 1},
                       {max_output_bytes = 1.0}, {max_output_bytes = {high = -1, low = 0}},
                       {max_output_bytes = {high = 0.0, low = 0}}, {["level\0hidden"] = 1}}) do
  local called = false
  local ok = pcall(zlib.gzip_stream, function() called = true; return nil end,
                    function() return true end, opts)
  assert(not ok and not called)
end
assert(not pcall(zlib.inflate_stream, reader(compressed), function() return true end, {sync_flush = true}))

-- Incremental shells: drain/final idempotence, early close and type validation.
for _, new in ipairs({zlib.deflate_stream_new, zlib.gzip_stream_new}) do
  local stream = assert(new())
  local first, consumed, finished = stream:process(binary, "sync")
  assert(consumed == #binary and not finished and #first > 0)
  local last
  last, consumed, finished = stream:process("", "finish")
  assert(consumed == 0 and finished)
  local again = stream:process("", "finish")
  assert(again == "" and stream:result().output_bytes == #first + #last)
  assert(zlib.decompress_materialized(first .. last) == binary)
  stream:close(); stream:close()
  assert(not pcall(stream.process, stream, "x"))
  assert(not pcall(stream.close, {}))
end
for _, pair in ipairs({{zlib.inflate_stream_new, compressed}, {zlib.gunzip_stream_new, gzip},
                      {zlib.decompress_stream_new, members}}) do
  local stream = assert(pair[1]())
  local chunk, consumed = stream:process(pair[2])
  local chunks = {chunk}
  local offset = consumed
  while offset < #pair[2] do
    chunk, consumed = stream:process(pair[2]:sub(offset + 1))
    chunks[#chunks + 1] = chunk
    offset = offset + consumed
  end
  local finished
  repeat
    chunk, consumed, finished = stream:process("", "finish")
    chunks[#chunks + 1] = chunk
  until finished
  assert(table.concat(chunks) == (pair[2] == members and binary .. binary or binary))
  stream:close()
end
do
  local stream <close> = assert(zlib.gzip_stream_new())
  assert(stream:process("partial"))
end
for _ = 1, 12 do assert(zlib.gunzip_stream_new()) end
collectgarbage("collect")

-- Debug can expose even a private helper owner. Explicit close/__gc/reentry
-- during a callback must defer destruction until the protected loop exits.
local function find_owner()
  for level = 2, 12 do
    if not debug.getinfo(level) then break end
    for index = 1, 12 do
      local name, local_value = debug.getlocal(level, index)
      if not name then break end
      if type(local_value) == "userdata" then
        local mt = getmetatable(local_value)
        if mt and mt.__index and mt.__index.process and mt.__index.result then
          return local_value
        end
      end
    end
  end
  error("stream owner was not reachable in active driver")
end
for _, action in ipairs({function(owner) owner:close() end,
                        function(owner) getmetatable(owner).__gc(owner) end,
                        function(owner) owner:process("reentry") end}) do
  local ok = pcall(zlib.gzip_stream, reader(binary), function()
    action(find_owner())
    collectgarbage("collect")
    return true
  end)
  assert(not ok)
end
assert(collect(zlib.gzip_stream, binary)) -- failures never poison future owners
local co = coroutine.create(function()
  zlib.gzip_stream(function() coroutine.yield("unsupported") end, function() return true end)
end)
local resumed, yield_error = coroutine.resume(co)
assert(not resumed and tostring(yield_error):find("yield", 1, true))
print("upgrade seam behavior ok")
