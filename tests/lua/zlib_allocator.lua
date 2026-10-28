local z=require('zlib');local b=require('vectis.buffer')
local function fails(fn,...) assert(not pcall(fn,...)) end
local function fixture()
  local user={allocated=0,freed=0,handles={},views={}}
  local config={opaque=user,zalloc=function(items,size,opaque)
    assert(opaque==user and items>0 and size>0);user.allocated=user.allocated+1
    if user.fail_alloc then return nil,user.marker end
    local buffer=assert(b.new(items*size));user.handles[#user.handles+1]=buffer
    if user.owner then fails(user.owner.close,user.owner) end
    if user.sibling then fails(user.sibling.close,user.sibling) end
    collectgarbage('collect');return buffer
  end,zfree=function(view,opaque)
    assert(opaque==user and view:size()>0);user.freed=user.freed+1;user.views[#user.views+1]=view
    fails(view.write,view,0,'x')
    if user.owner then fails(user.owner.close,user.owner) end
    if user.sibling then fails(user.sibling.close,user.sibling) end
    if user.fail_free then return nil,user.marker end
  end}
  return user,config
end
local function expired(user)
  for _,buffer in ipairs(user.handles) do fails(buffer.size,buffer) end
  for _,view in ipairs(user.views) do fails(view.size,view) end
end
local user,config=fixture();local raw=assert(z.deflateInit(6,config));user.owner=raw
local fields=raw:fields();assert(fields.zalloc==config.zalloc and fields.zfree==config.zfree and fields.opaque==user)
config.zalloc=function() error('mutated config') end;config.zfree=nil;config.opaque=nil
local branch=assert(raw:copy());user.sibling=branch
assert(raw:process_materialized('allocator bytes',128,z.Z_FINISH).zlib_status==z.Z_STREAM_END)
raw:close();user.owner=nil
assert(branch:process_materialized('allocator bytes',128,z.Z_FINISH).zlib_status==z.Z_STREAM_END)
branch:close();user.sibling=nil
assert(user.allocated==user.freed);expired(user)
local default=assert(z.inflateInit());local fields=default:fields()
assert(type(fields.zalloc)=='function' and type(fields.zfree)=='function' and fields.opaque==nil)
local buffer=assert(fields.zalloc(2,3));assert(buffer:size()==6);fields.zfree(buffer);fails(buffer.size,buffer)
default:close()
local opaque={};local default=assert(z.inflateInit({opaque=opaque}));assert(default:fields().opaque==opaque);default:close()
-- Callback failure returns through native cleanup first and keeps its object.
local marker={};local user,config=fixture();user.fail_alloc=true;user.marker=marker
local ok,error=pcall(z.deflateInit,6,config);assert(not ok and error==marker)
expired(user)
local small=assert(b.new(1))
local ok,error=pcall(z.deflateInit,6,{zalloc=function() return small end,zfree=function() end})
assert(not ok and small:resize(2)==small);small:close()
for _,config in ipairs({{zalloc=function() end},{zfree=function() end},{zalloc=true,zfree=function() end}}) do
  fails(z.deflateInit,6,config)
end
-- A failed native copy must not consume its parent. After the error is reported,
-- the source can still be used with its existing allocations.
local user,config=fixture();local source=assert(z.deflateInit(6,config));user.owner=source
user.fail_alloc=true;user.marker=marker
local ok,error=pcall(source.copy,source);assert(not ok and error==marker)
user.fail_alloc=false
assert(source:process_materialized('survived copy failure',128,z.Z_FINISH).zlib_status==z.Z_STREAM_END)
source:close();user.owner=nil;expired(user)
-- Reporting an allocation error releases its graph while the parent stays live.
local weak=setmetatable({},{__mode='v'})
local reject=false
local source=assert(z.deflateInit(6,{zalloc=function(items,size)
  if reject then local error={};weak[1]=error;return nil,error end
  return b.new(items*size)
end,zfree=function() end}))
reject=true
do local ok,error=pcall(source.copy,source);assert(not ok and error==weak[1]) end
reject=false;collectgarbage('collect');collectgarbage('collect');assert(weak[1]==nil)
source:close()
-- Free failure must release every allocation before propagating the first error.
local user,config=fixture();local owner=assert(z.deflateInit(6,config));user.owner=owner
user.fail_free=true;user.marker=false
local ok,error=pcall(owner.close,owner);assert(not ok and error==false)
user.owner=nil;owner:close();assert(user.allocated==user.freed);expired(user)
-- Allocator state follows the invoking coroutine, including end after creator GC.
local expected,owner;local user,config=fixture()
local allocate,free=config.zalloc,config.zfree
config.zalloc=function(...) assert(coroutine.running()==expected);return allocate(...) end
config.zfree=function(...) assert(coroutine.running()==expected);return free(...) end
local co=coroutine.create(function() expected=coroutine.running();owner=assert(z.deflateInit(6,config)) end)
assert(coroutine.resume(co));co=nil;collectgarbage('collect');expected=coroutine.running()
owner:close();assert(user.allocated==user.freed);expired(user)
-- Back-inflate uses the same captured C89 allocator hooks.
local user,config=fixture();local window=assert(b.new(32768))
local back=assert(z.inflateBackInit(15,window,config));user.owner=back
local encoder=assert(z.deflateInit2(6,8,-15,8,0));local compressed=encoder:process_materialized('back hooks',128,4).output;encoder:close()
local once=false;assert(back:back(function() if once then return nil end;once=true;return compressed end,
  function(chunk) assert(chunk=='back hooks');return true end).zlib_status==z.Z_STREAM_END)
back:close();user.owner=nil;assert(user.allocated==user.freed);expired(user);window:close()
-- GC free observers still run after weak values were cleared, without retaining
-- callback graphs through a permanent registry cycle.
local allocated,freed=0,0
for _=1,10 do
  local owned=z.deflateInit(6,{zalloc=function(items,size) allocated=allocated+1;return b.new(items*size) end,
    zfree=function(view) assert(view:size()>0);freed=freed+1 end})
  owned=nil;collectgarbage('collect');collectgarbage('collect')
end
assert(allocated==freed)
collectgarbage('collect')
print('upgrade seam behavior ok')
