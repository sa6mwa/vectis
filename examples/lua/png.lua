local png = require('vectis.png')
local source <close> = assert(png.reader(assert(arg[1],'input PNG required')))
local info = assert(source:info())
local target <close> = assert(png.writer(assert(arg[2],'output PNG required'),{
  width=info.width,height=info.height,format='rgba8',text=info.text,
  physical=info.physical,
}))
while true do
  local row,err = source:next_row()
  if not row then assert(err==nil,err); break end
  assert(target:write_row(row))
end
assert(target:finish())
