local png = require('vectis.png')
assert(require('vectis').png == png)
local root = assert(os.getenv('VECTIS_UPGRADE_TEST_ROOT'))
local fixtures = assert(os.getenv('VECTIS_PNG_FIXTURES'))
local pixels = string.char(255,0,0,127,0,0,255,255)
local image = {width=2,height=1,format='rgba8',pixels=pixels,
  text={Title='Lua image'},physical={x=3780,y=3780,unit='meter'}}
local bytes = assert(png.encode_materialized(image))
local decoded = assert(png.decode_materialized(bytes))
assert(decoded.pixels==pixels and decoded.width==2 and decoded.height==1)
assert(decoded.row_bytes==8 and decoded.bit_depth==8 and decoded.color_type==6 and not decoded.interlaced)
assert(decoded.text.Title=='Lua image' and decoded.physical.x==3780 and decoded.physical.unit=='meter')
assert(png.decode_materialized(assert(png.encode_materialized(decoded))).pixels==pixels)
for _,format in ipairs({'rgb8','gray8'}) do
  local value = {width=2,height=1,format=format,pixels=format=='rgb8' and string.char(255,0,0,0,0,255) or string.char(0,255)}
  local result = assert(png.decode_materialized(assert(png.encode_materialized(value))))
  assert(result.pixels == (format=='rgb8' and string.char(255,0,0,255,0,0,255,255) or string.char(0,0,0,255,255,255,255,255)))
end
local collected = {}
local row_image = {width=2,height=1,format='rgba8',text={Title='Copied'},physical=image.physical}
local writer = assert(png.writer(function(chunk)
  assert(#chunk>0 and #chunk<=65536)
  collected[#collected+1]=chunk
  return true
end,row_image))
row_image.text.Title='Changed'
assert(writer:info().text.Title=='Copied')
assert(writer:finish()==nil)
assert(writer:write_row(pixels:sub(1,7))==nil)
assert(writer:write_row(pixels))
assert(writer:finish() and writer:finish())
assert(writer:write_row(pixels)==nil)
local count=#collected
assert(writer:close() and writer:close() and #collected==count)
local encoded=table.concat(collected)
local position,reads=1,0
local reader = assert(png.reader(function(capacity)
  assert(capacity>0 and capacity<=65536)
  reads=reads+1
  if position>#encoded then return nil end
  local chunk=encoded:sub(position,position)
  position=position+#chunk
  return chunk
end))
assert(reader:info().text.Title=='Copied')
assert(reader:next_row()==pixels)
local value,err=reader:next_row()
assert(value==nil and err==nil and reader:finish() and reads>1)
assert(reader:next_row()==nil)
assert(reader:close())
local output=root..'/rows.png'
writer=assert(png.writer(output,{width=2,height=1}))
assert(writer:write_row(pixels) and writer:finish() and writer:close())
reader=assert(png.reader(output))
assert(reader:next_row()==pixels and reader:finish() and reader:close())
local file=assert(io.open(fixtures..'/interlaced.png','rb'))
local interlaced=assert(file:read('a')); assert(file:close())
assert(png.decode_materialized(interlaced).pixels==pixels)
assert(png.reader(fixtures..'/interlaced.png')==nil)
for _,invalid in ipairs({bytes:sub(1,-2),'invalid PNG'}) do assert(png.decode_materialized(invalid)==nil) end
assert(png.decode_materialized(bytes,{max_width=1})==nil)
assert(png.decode_materialized(bytes,{max_row_bytes=7})==nil)
assert(png.decode_materialized(bytes,{max_bytes=7})==nil)
assert(png.encode_materialized(image,{max_bytes=8})==nil)
assert(png.encode_materialized({width=2,height=1,pixels='short'})==nil)
for _,callback in ipairs({
  function() png.reader(output,{unknown=true}) end,
  function() png.writer(output,{width=2,height=1,unknown=true}) end,
  function() png.encode_materialized({width=2,height=1,pixels=pixels,text={Title='a\0b'}}) end,
  function() png.reader(output,{['max_width\0hidden']=3}) end,
  function() png.writer(output,{width=2,height=1,pixels=pixels}) end,
}) do assert(not pcall(callback)) end
local sentinel={}
local ok,raised=pcall(function() png.reader(function() error(sentinel) end) end)
assert(not ok and raised==sentinel)
value,err=png.writer(function() return nil,sentinel end,{width=2,height=1})
assert(value==nil and err==sentinel)
local fail=false
position=1
reader=assert(png.reader(function(capacity)
  if fail then error(sentinel) end
  local chunk=encoded:sub(position,position+capacity-1); position=position+#chunk; return chunk
end))
fail=true
ok,raised=pcall(function() reader:next_row() end)
assert(not ok and raised==sentinel)
assert(reader:close())
fail=false
writer=assert(png.writer(function(chunk)
  if fail then
    assert(not pcall(function() writer:close() end))
  end
  return true
end,{width=2,height=1}))
fail=true
value,err=writer:write_row(pixels)
if value then value,err=writer:finish() end
assert(value==nil and err.status~=0)
assert(writer:close())
value,err=png.reader(function() return nil,sentinel end)
assert(value==nil and err==sentinel)
-- A large image reaches a row consumer before the source reaches EOF.
local state=1
local function random_row()
  local row={}
  for i=1,4096 do
    state=(state ~ (state << 13)) & 0xffffffff
    state=(state ~ (state >> 17)) & 0xffffffff
    state=(state ~ (state << 5)) & 0xffffffff
    row[i]=string.char(state & 255)
  end
  return table.concat(row)
end
local large_chunks,delivered={},0
writer=assert(png.writer(function(chunk)
  delivered=delivered+#chunk
  large_chunks[#large_chunks+1]=chunk
  return true
end,{width=1024,height=64}))
for _=1,64 do assert(writer:write_row(random_row())) end
assert(delivered>1000)
assert(writer:finish() and writer:close())
local large=table.concat(large_chunks)
position=1
reader=assert(png.reader(function(capacity)
  local chunk=large:sub(position,position+capacity-1)
  position=position+#chunk
  if #chunk==0 then return nil end
  return chunk
end))
state=1
assert(reader:next_row()==random_row())
assert(position<#large)
for _=2,64 do assert(reader:next_row()==random_row()) end
assert(reader:finish() and reader:close())
local method=reader.next_row
assert(not pcall(method,{}))
local calls=0
writer=assert(png.writer(function() calls=calls+1; return true end,{width=2,height=1}))
local before=calls
assert(writer:close() and calls==before)
-- Generated PNG bytes feed PDF without staging or encoded-input retention.
local pdf = require('vectis.pdf')
local doc <close> = assert(pdf.new())
local page <close> = assert(doc:add_page())
local embedded <close> = assert(doc:load_image_materialized(bytes))
bytes=nil; collectgarbage('collect')
assert(page:image(embedded,10,10,40,20))
assert(doc:bytes_materialized():find('%PDF-',1,true))
collectgarbage('collect')
print('upgrade seam behavior ok')
