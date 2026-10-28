local compression = require('vectis.compression')
local source <close> = assert(io.open(assert(arg[1],'input path required'),'rb'))
local target <close> = assert(io.open(assert(arg[2],'output path required'),'wb'))
assert(compression.stream(arg[3] or 'zstd','compress',function(capacity)
  return source:read(capacity)
end,function(chunk)
  local ok,err = target:write(chunk)
  if not ok then return nil,err end
  return true
end))
