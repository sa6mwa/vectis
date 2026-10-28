local codec = require('vectis.compression')
assert(require('vectis').compression == codec)
local payload = 'hello\0binary\255' .. string.rep('bounded chunks ',20000)
local sentinel = {}
for _,algorithm in ipairs({'zstd','lz4','xz'}) do
  local compressed = assert(codec.materialized(algorithm,'compress',payload))
  assert(codec.materialized(algorithm,'decompress',compressed) == payload)
  local empty = assert(codec.materialized(algorithm,'compress',''))
  assert(codec.materialized(algorithm,'decompress',empty) == '')
  for _,invalid in ipairs({compressed:sub(1,-2),compressed..'x',compressed..compressed,'invalid frame'}) do
    local value,failure=codec.materialized(algorithm,'decompress',invalid)
    assert(value==nil and type(failure.source)=='string' and type(failure.source_code)=='number')
    assert(type(failure.status_string)=='string' and type(failure.dependency_code)=='number')
  end
  local result,err = codec.materialized(algorithm,'decompress',compressed,{max_bytes=16})
  assert(result==nil and err.message:find('max_bytes',1,true))
  local receiver = assert(codec.new(algorithm,'compress'))
  local ok,raised = pcall(function() receiver:finish(function() error(sentinel) end) end)
  assert(not ok and raised==sentinel)
  assert(receiver:finish(function() error('must not run') end)==nil)
  assert(receiver:close() and receiver:close())
  receiver = assert(codec.new(algorithm,'compress'))
  result,err = receiver:finish(function() return nil,sentinel end)
  assert(result==nil and err==sentinel)
  assert(receiver:close())
  receiver = assert(codec.new(algorithm,'compress'))
  result,err = receiver:finish(function()
    assert(not pcall(function() receiver:close() end))
    return true
  end)
  assert(result==nil and err.status ~= 0)
  assert(receiver:close())
  local input_calls,output_calls = 0,0
  assert(codec.stream(algorithm,'compress',function(capacity)
    assert(capacity==65536)
    input_calls=input_calls+1
    if input_calls>8 then return nil end
    return string.rep('chunk',10000)
  end,function(chunk)
    assert(#chunk>0 and #chunk<=65536)
    output_calls=output_calls+1
    return true
  end))
  assert(output_calls>0)
  result,err=codec.stream(algorithm,'compress',function() return nil,sentinel end,function() return true end)
  assert(result==nil and err==sentinel)
end
for _,opts in ipairs({{unknown=1},{level=1.5},{memory_limit=1},{max_window_log=26},{['level\0hidden']=1}}) do
  assert(not pcall(function() codec.new('lz4','compress',opts) end))
end
assert(not pcall(function() codec.new('zstd','decompress',{level=0}) end))
local function discard() return true end
local receiver = assert(codec.new('zstd','compress'))
assert(receiver:process('',discard) and receiver:process('',discard))
assert(receiver:finish(discard) and receiver:finish(discard))
assert(receiver:process('',discard)==nil)
assert(receiver:close())
assert(not pcall(function() receiver:finish(discard) end))
-- Decoder memory/window options reach the providers.
local xz = assert(codec.materialized('xz','compress',payload,{level=0}))
assert(codec.materialized('xz','decompress',xz,{memory_limit=1024})==nil)
assert(codec.materialized('xz','decompress',xz,{memory_limit=67108864})==payload)
local zstd = assert(codec.materialized('zstd','compress',payload))
assert(codec.materialized('zstd','decompress',zstd,{max_window_log=10})==nil)
assert(codec.materialized('zstd','decompress',zstd,{max_window_log=26})==payload)
assert(codec.new('lzo','compress')==nil)
collectgarbage('collect')
print('upgrade seam behavior ok')
