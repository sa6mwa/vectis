local png=require('vectis.png.core')
local image={width=2,height=1,format='rgba8',pixels=string.char(255,0,0,127,0,0,255,255),text={Title='Owned'},physical={x=100,y=100,unit='meter'}}
local encoded=assert(png.encode_materialized(image))
local decoded=assert(png.decode_materialized(encoded))
assert(decoded.pixels==image.pixels and decoded.text.Title=='Owned')
local chunks={}
local writer <close> = assert(png.writer(function(chunk) chunks[#chunks+1]=chunk; return true end,{width=2,height=1,text=image.text}))
assert(writer:write_row(image.pixels) and writer:finish())
encoded=table.concat(chunks)
local position=1
local reader <close> = assert(png.reader(function(capacity)
  if position>#encoded then return nil end
  local chunk=encoded:sub(position,position+capacity-1)
  position=position+#chunk
  return chunk
end))
assert(reader:info().text.Title=='Owned')
assert(reader:next_row()==image.pixels)
assert(reader:next_row()==nil and reader:finish())
return true
