local core = require('vectis.compression.core')
local M = {new=core.new, CHUNK_BYTES=65536}

function M.stream(algorithm,direction,reader,sink,opts)
  assert(type(reader)=='function' and type(sink)=='function','reader and sink must be functions')
  local receiver,err = core.new(algorithm,direction,opts)
  if not receiver then return nil,err end
  local owned <close> = receiver
  while true do
    local chunk,read_error = reader(M.CHUNK_BYTES)
    if chunk == nil then
      if read_error ~= nil then return nil,read_error end
      break
    end
    assert(type(chunk)=='string' and #chunk>0 and #chunk<=M.CHUNK_BYTES,
      'reader must return 1..65536 bytes or nil[,error]')
    local ok,write_error = receiver:process(chunk,sink)
    if not ok then return nil,write_error end
  end
  return receiver:finish(sink)
end

function M.materialized(algorithm,direction,bytes,opts)
  assert(type(bytes)=='string','materialized input must be a string')
  opts = opts or {}
  assert(type(opts)=='table','options must be a table')
  local native,limit = {},16777216
  for key,value in pairs(opts) do
    if key=='max_bytes' then limit=value else native[key]=value end
  end
  assert(math.type(limit)=='integer' and limit>0,'max_bytes must be a positive integer')
  local chunks,total,offset = {},0,1
  local ok,err = M.stream(algorithm,direction,function(capacity)
    if offset>#bytes then return nil end
    local chunk=bytes:sub(offset,offset+capacity-1)
    offset=offset+#chunk
    return chunk
  end,function(chunk)
    if #chunk>limit-total then
      return nil,{status=1,status_string='invalid',source_code=1,source='vectis',
        dependency_code=0,operation='materialized',message='materialized output exceeds max_bytes'}
    end
    total=total+#chunk
    chunks[#chunks+1]=chunk
    return true
  end,native)
  if not ok then return nil,err end
  return table.concat(chunks)
end
return M
