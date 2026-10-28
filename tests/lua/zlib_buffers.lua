local z=require('zlib');local b=require('vectis.buffer')
local function fails(fn,...) assert(not pcall(fn,...)) end
local message='binary\0buffer operations'
for _,suffix in ipairs({'','_z'}) do
  assert(z['compressBound'..suffix](#message)>=#message)
  assert(type(z['compressBound'..suffix](suffix=='' and z.Z_ULONG_MAX or z.Z_SIZE_MAX))~='nil')
  fails(z['compressBound'..suffix],-1)
  fails(z['compressBound'..suffix],'18446744073709551616')
  for _,compress in ipairs({'compress','compress2'}) do
    local source=assert(b.from_materialized(message));local output=assert(b.new(z['compressBound'..suffix](#message)))
    local code,length=z[compress..suffix](output,source,6)
    assert(code==z.Z_OK and length>0)
    local input=output:borrow(0,length);output:close()
    local decoded=assert(b.new(#message))
    for _,uncompress in ipairs({'uncompress','uncompress2'}) do
      local code,produced,consumed=z[uncompress..suffix](decoded,input)
      assert(code==z.Z_OK and produced==#message and decoded:read_materialized(0,produced)==message)
      assert(uncompress=='uncompress' and consumed==nil or consumed==length)
    end
    local trailing=assert(b.from_materialized(input:read_materialized(0,length)..'TAIL'))
    local code,produced,consumed=z['uncompress2'..suffix](decoded,trailing)
    assert(code==z.Z_OK and produced==#message and consumed==length)
    trailing:close()
    local small=decoded:borrow(0,3,false)
    local code,produced,consumed=z['uncompress2'..suffix](small,input)
    assert(code==z.Z_BUF_ERROR and produced==3 and consumed<=length)
    small:close()
    local code=z['uncompress'..suffix](decoded,source);assert(code==z.Z_DATA_ERROR)
    input:close();decoded:close();source:close()
  end
  local empty=assert(b.new(0));local compressed=assert(b.new(32));local decoded=assert(b.new(0))
  local code,length=z['compress'..suffix](compressed,nil);assert(code==z.Z_OK)
  local span=compressed:borrow(0,length)
  local code,produced=z['uncompress'..suffix](decoded,span);assert(code==z.Z_OK and produced==0)
  local code,length=z['compress'..suffix](compressed,empty);assert(code==z.Z_OK)
  span:close();compressed:close();empty:close();decoded:close()
end
local source=assert(b.from_materialized('123456789'));local empty=assert(b.new(0))
for _,case in ipairs({{'crc32',0,0xcbf43926},{'adler32',1,0x091e01de}}) do
  local name,seed,expected=table.unpack(case)
  for _,suffix in ipairs({'','_z'}) do
    assert(z[name..suffix](seed,source)==expected)
    assert(z[name..suffix](123,nil)==seed)
    assert(z[name..suffix](123,empty)==123)
    assert(z[name..suffix](seed,source,4)==z[name..'_materialized']('1234'))
    assert(type(z[name..suffix](z.Z_ULONG_MAX,source))=='number')
    fails(z[name..suffix],seed,source,10);fails(z[name..suffix],seed,nil,1)
    fails(z[name..suffix],-1,source);fails(z[name..suffix],1.0,source)
    fails(z[name..suffix],'18446744073709551616',source)
  end
end
source:close();empty:close()
-- Setup rejection releases leases immediately; caller buffers remain reusable.
local buffer=assert(b.new(128));local input=assert(b.from_materialized('valid input'))
fails(z.compress,buffer,buffer);fails(z.compress,buffer,'materialized string')
assert(buffer:resize(128)==buffer)
local readonly=buffer:borrow(0,128)
fails(z.compress,readonly,input);readonly:close()
local lock=buffer:lock_writes();fails(z.compress,buffer,input);lock:close()
local code,length=z.compress2(buffer,input,99);assert(code==z.Z_STREAM_ERROR)
assert(buffer:resize(128)==buffer and input:resize(11)==input)
buffer:close();input:close();fails(z.compress,buffer,nil)
-- Native 1.3.2 explicitly returns zero for a zero operator. High-bit-only
-- operators cannot terminate its multiplication and violate the polynomial domain.
assert(z.crc32_combine_op(0,0,0)==0)
fails(z.crc32_combine_op,0,0,0x100000000)
assert(math.type(z.crc32_combine_gen64('9223372036854775807'))=='integer')
fails(z.crc32_combine_gen64,'9223372036854775808')
if type(z.Z_ULONG_MAX)=='string' then
  assert(z.crc32_combine_op(0,z.Z_ULONG_MAX,z.crc32_combine_gen(1))==0xffffffff)
end
local table=assert(z.get_crc_table());local other=assert(z.get_crc_table())
assert(table:size()==256 and table:native()==table and table:entry(0)==0 and table:entry(1)==0x77073096)
-- Every exposed entry agrees with the public byte-wise CRC recurrence.
for byte=0,255 do
  local initial=0xffffffff
  local index=(initial~byte)&255
  assert(z.crc32_materialized(string.char(byte))==((table:entry(index)~(initial>>8))~0xffffffff))
end
fails(table.entry,table,-1);fails(table.entry,table,256);fails(table.entry,table,1.0)
table:close();table:close();fails(table.size,table)
assert(other:entry(1)==0x77073096);other:close()
collectgarbage('collect')
print('upgrade seam behavior ok')
