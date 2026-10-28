local z=require('zlib');local b=require('vectis.buffer')
local function fails(fn,...) assert(not pcall(fn,...)) end
local function compressed(text)
  local raw=assert(z.deflateInit2(6,z.Z_DEFLATED,-15,8,z.Z_DEFAULT_STRATEGY))
  local result=assert(raw:process_materialized(text,65536,z.Z_FINISH))
  assert(result.zlib_status==z.Z_STREAM_END);raw:close();return result.output
end
local first=compressed(('window payload '):rep(3000));local second=compressed('second stream')
local window=assert(b.new(32768));local alias=window:borrow(0,32768,false)
local raw=assert(z.inflateBackInit(15,window));window:close()
local reader={done=false,calls=0};local writer={chunks={},writes=0}
local result=assert(z.inflateBack(raw,{userdata=reader,read=function(cap,user)
  assert(cap==65536 and user==reader);user.calls=user.calls+1
  if user.done then return nil end;user.done=true;return first..second
end},{userdata=writer,write=function(chunk,user)
  assert(user==writer and #chunk<=32768);user.writes=user.writes+1
  fails(alias.write,alias,0,'x');fails(alias.data_mutable,alias)
  local own=alias:lock_writes();own:close();fails(alias.write,alias,0,'x')
  fails(raw.close,raw);fails(raw.fields,raw);fails(raw.back,raw,function() end,function() end)
  user.chunks[#user.chunks+1]=chunk;return true
end}))
assert(result.zlib_status==z.Z_STREAM_END and result.output_bytes==45000 and writer.writes>1)
assert(table.concat(writer.chunks)==('window payload '):rep(3000))
assert(result.supplied==#first+#second and result.consumed==#first)
assert(result.unused_materialized==second and result.unused_length==#second)
assert(result.unused:read_materialized(0,#second)==second);result.unused:close()
local chunks={};local called=false
local next=assert(raw:back(function() called=true;return nil end,function(chunk) chunks[#chunks+1]=chunk;return 0 end))
assert(not called and next.zlib_status==z.Z_STREAM_END and table.concat(chunks)=='second stream')
assert(next.consumed==#second and next.unused_length==0)
if next.unused then next.unused:close() end
assert(alias:write(0,'x')==alias)
assert(z.inflateEnd(raw)==z.Z_STREAM_ERROR)
assert(raw:inflateReset()==z.Z_STREAM_ERROR)
assert(z.inflateBackEnd(raw)==z.Z_OK);assert(raw:end_native()==z.Z_OK);raw:close();alias:close()
-- Callback EOF is a native buffer error, not success. Output errors and thrown
-- objects preserve their first identity after the provider frame unwinds.
local window=assert(b.new(32768));local marker={}
local function run_fault(read,write)
  local owner=assert(z.inflateBackInit(15,window))
  local ok,value=pcall(owner.back,owner,read,write)
  if ok then if value.unused then value.unused:close() end else fails(owner.fields,owner) end
  owner:close();assert(window:resize(32768)==window);return ok,value
end
local ok,short=run_fault(function() return nil end,function() return true end)
assert(ok and short.zlib_status==z.Z_BUF_ERROR and short.input_failed)
local ok,value=run_fault(function() error(marker) end,function() return true end);assert(not ok and value==marker)
local ok,value=run_fault(function() error(false) end,function() return true end);assert(not ok and value==false)
local done=false
local ok,value=run_fault(function() if done then return nil end;done=true;return first end,
  function() return nil,marker end);assert(not ok and value==marker)
for _,read in ipairs({function() return '' end,function() return 12 end,
                      function() return ('x'):rep(65537) end}) do
  local ok,value=run_fault(read,function() return true end);assert(not ok)
end
local once=false
local ok,bad=run_fault(function() if once then return nil end;once=true;return '\255\255\255' end,function() return true end)
assert(ok and bad.zlib_status==z.Z_DATA_ERROR)
-- Invalid ABI/window and unsupported state operations cannot consume storage.
local tiny=assert(b.new(255));local bad,error=z.inflateBackInit(8,tiny);assert(bad==nil)
assert(tiny:resize(256)==tiny);tiny:close()
local ro=window:borrow(0,32768);local bad,error=z.inflateBackInit(15,ro);assert(bad==nil);ro:close()
local bad,error=z.inflateBackInit_(15,window,'incompatible',z.Z_STREAM_SIZE);assert(bad==nil and error.zlib_status==z.Z_VERSION_ERROR)
local valid=assert(z.inflateBackInit_(15,window,z.ZLIB_VERSION,z.Z_STREAM_SIZE));valid:close()
window:close();collectgarbage('collect')
do
  local window=assert(b.new(32768));local owner=assert(z.inflateBackInit(15,window))
  local weak=setmetatable({},{__mode='v'})
  do
    local context={};weak[1]=context
    fails(owner.back,owner,{userdata=context,read=function() return nil end},{write=12})
  end
  collectgarbage('collect');collectgarbage('collect');assert(weak[1]==nil)
  assert(type(owner:fields())=='table');owner:close();window:close()
end
print('upgrade seam behavior ok')
