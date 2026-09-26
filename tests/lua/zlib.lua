local function run()
  local zlib = require("zlib")
  local vectis = require("vectis")
  local input, hardlink, symlink, output = arg[1], arg[2], arg[3], arg[4]
  local function read(path)
    local file = assert(io.open(path, "rb"))
    local data = assert(file:read("*a"))
    assert(file:close())
    return data
  end
  local function write(path, data)
    local file = assert(io.open(path, "wb"))
    assert(file:write(data))
    assert(file:close())
  end
  local original = read(input)
  for _, name in ipairs({"deflate_file", "compress_file", "gzip_file",
                         "inflate_file", "gunzip_file", "decompress_file"}) do
    for _, alias in ipairs({input, hardlink, symlink}) do
      local result, err = zlib[name]({input_path = input, output_path = alias})
      assert(result == nil and err.status == vectis.ERR_INVALID)
      assert(err.message:find("distinct files", 1, true))
      assert(read(input) == original and read(alias) == original)
    end
  end
  local empty = assert(zlib.gzip(""))
  assert(zlib.gunzip(empty, {max_output_bytes = 0}) == "")
  for _, decode in ipairs({zlib.gunzip, zlib.decompress}) do
    local first, second = "first", ("second"):rep(1000)
    local members = assert(zlib.gzip(first)) .. empty .. assert(zlib.gzip(second)) .. empty
    assert(decode(members, {max_output_bytes = #first + #second}) == first .. second)
    local value, err = decode(members, {max_output_bytes = #first + #second - 1})
    assert(value == nil and err.status == vectis.ERR_INVALID)
    for _, bad in ipairs({members:sub(1, -2), members .. "garbage"}) do
      value, err = decode(bad)
      assert(value == nil and err.status == vectis.ERR_INVALID)
    end
  end
  -- Stored blocks allow exact placement of member endings around input chunks.
  for _, first_size in ipairs({3, 32745, 32746, 32747, 65536}) do
    local first, second = ("a"):rep(first_size), ("b"):rep(70000)
    local packed_first = assert(zlib.gzip(first, {level = 0}))
    if first_size == 32745 then assert(#packed_first == 32768) end
    local members = packed_first .. empty .. assert(zlib.gzip(second, {level = 0})) .. empty
    write(input, members)
    for _, decode in ipairs({zlib.gunzip_file, zlib.decompress_file}) do
      local result = assert(decode({input_path = input, output_path = output,
                                   max_output_bytes = #first + #second}))
      assert(result.ok and result.input_bytes == #members)
      assert(result.output_bytes == #first + #second)
      assert(read(output) == first .. second)
      local value, err = decode({input_path = input, output_path = output,
                                max_output_bytes = #first + #second - 1})
      assert(value == nil and err.status == vectis.ERR_INVALID)
      assert(io.open(output, "rb") == nil)
      write(input, members:sub(1, -2))
      value, err = decode({input_path = input, output_path = output})
      assert(value == nil and err.status == vectis.ERR_INVALID)
      assert(io.open(output, "rb") == nil)
      write(input, members)
    end
  end
  -- Ordinary output replacement still truncates stale contents.
  write(input, "short")
  write(output, ("stale"):rep(100))
  assert(zlib.gzip_file({input_path = input, output_path = output}).ok)
  assert(zlib.gunzip(read(output)) == "short")
  print("zlib regression passed")
end
local ok, err = xpcall(run, function(err)
  if type(err) == "table" then err = err.message or tostring(err) end
  return debug.traceback(tostring(err), 2)
end)
if not ok then io.stderr:write(err, "\n"); error(err) end
