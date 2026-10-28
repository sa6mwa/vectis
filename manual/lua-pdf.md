# PDF reports

`require('vectis.pdf')` (also `vectis.pdf`) creates process/thread-confined
documents through the bundled public PDF API. Operations return a result or
`nil, error`; argument type/unknown option errors raise. Errors include the
native diagnostic code and operation. Close resources explicitly or use Lua
to-be-closed variables. Closing a document invalidates its pages/fonts/images.

```lua
local pdf = require('vectis.pdf')
local doc <close> = assert(pdf.new({title='Daily report', author='Vectis'}))
local page <close> = assert(doc:add_page())
assert(page:text(36,790,'Daily report',{size=18}))
assert(page:line(36,780,550,780,{color={0,0,1},line_width=1}))
assert(doc:save('report.pdf'))
```

`new` accepts `title`, `author`, `subject`. `add_page({width,height})` defaults
to A4 portrait; geometry uses points with the origin at the lower left. Native
floating-point precision applies. Explicit dimensions are 3–14400 points.

- `doc:font(name,{encoding=...})` selects a standard font.
- `doc:load_font(path,{encoding=...,embed=true})` loads a TrueType font. Select
  `encoding='UTF-8'` for UTF-8 text supported by that font; shaping is not provided.
- `page:text(x,y,text,{font=...,size=12,color={0,0,0}})` defaults to Helvetica.
- `page:line(x1,y1,x2,y2,{color=...,line_width=1})` draws a line; width zero
  selects native hairline rendering.
- `page:rectangle(x,y,width,height,{color=...,line_width=1,fill=false,stroke=true})`
  draws an outline and/or filled rectangle.
- `doc:load_image(path,{format='png'})` loads PNG or `format='jpeg'`.
  `doc:load_image_materialized(bytes,opts)` uses complete encoded bytes instead;
  input is limited to 16 MiB and borrowed only during the call. This supports
  generated image data without a staging file.
- `image:info()` returns `width,height`. `page:image(image,x,y,width,height)`
  embeds the image. Fonts and images must belong to the same document as the page.

Closing a child view releases its facade shell. The document retains the native
objects required for output. `doc:save(path)` writes the selected file;
`doc:bytes_materialized({max_bytes=16777216})` returns complete PDF bytes.
The limit bounds the facade output copy, not the native document buffer. Haru
buffers the document internally; neither operation is a streaming PDF API.

Native errors latch until `doc:reset_error()` or close. Reset acknowledges the
error; it does not undo partially completed page operations. Validation failures
before native execution leave the document usable. Metadata and text reject NUL
bytes. Forms, encryption, JavaScript and arbitrary font internals are outside
this report-generation surface. See `examples/lua/pdf.lua`.
