local buffer = require('vectis.buffer.core')
local z = stack_zlib
local payload = string.rep('q', 65536)
local calls = 0
function stack_read(capacity)
  assert(capacity == 65536)
  calls = calls + 1
  if calls > 1 then return nil end
  local phase, emitted, offset = 0, 0, 1
  local markdown = string.rep('bounded ',1024)..'\n'
  assert(stack_mdf.version == '0.18.0')
  assert(stack_mdf.render_stream(function(cap)
    phase=phase+1
    if phase==1 then return '# Stack\n\n' end
    assert(emitted>0, 'MDF must emit before producer EOF')
    if offset<=#markdown then
      local last=math.min(#markdown,offset+cap-1)
      local chunk=markdown:sub(offset,last);offset=last+1;return chunk
    end
    return nil
  end,function(chunk)
    local copy=assert(buffer.from_materialized(chunk))
    assert(copy:read_materialized(0,#chunk)==chunk)
    emitted=emitted+#chunk;copy:close();return true
  end,{format='html',disable_embedded_font=true}))
  local b <close> = assert(buffer.from_materialized(payload))
  local compressed, sent = {}, false
  assert(z.gzip_stream(function()
    if sent then return nil end
    sent = true
    return b:read_materialized(0, #payload)
  end, function(chunk)
    local raw <close> = assert(z.deflateInit(6))
    assert(raw:process_materialized(chunk, 65536, 4).zlib_status == 1)
    assert(b:read_materialized(0, #payload) == payload)
    compressed[#compressed + 1] = chunk
    return true
  end))
  assert(#table.concat(compressed) > 0)
  local curl <close> = assert(require('vectis.curl.handles.core').session())
  local received = 0
  local transfer = assert(curl:perform({url='file://'..stack_script_path,write=function(chunk)
    local copy <close> = assert(buffer.from_materialized(chunk))
    assert(copy:read_materialized(0,#chunk)==chunk)
    received=received+#chunk
    return true
  end}))
  assert(transfer.ok and received>0)
  local compression=require('vectis.compression.core')
  for _,algorithm in ipairs({'zstd','lz4','xz'}) do
    local pieces, decoded={},{}
    local encoder <close> = assert(compression.new(algorithm,'compress'))
    local function collect(chunk) pieces[#pieces+1]=chunk; return true end
    assert(encoder:process(payload,collect) and encoder:finish(collect))
    local decoder <close> = assert(compression.new(algorithm,'decompress'))
    local function consume(chunk) decoded[#decoded+1]=chunk; return true end
    assert(decoder:process(table.concat(pieces),consume) and decoder:finish(consume))
    assert(table.concat(decoded)==payload)
  end
  local g <close> = assert(z.gzopen(stack_root .. '/data.gz', 'wb'))
  assert(g:write(payload) == #payload)
  assert(g:close() == 0)
  local r <close> = assert(z.gzopen(stack_root .. '/data.gz', 'rb'))
  assert(r:read(65536) == payload)
  assert(r:close() == 0)
  return payload
end
local sqlite = require('vectis.sqlite.core')
local db <close> = assert(sqlite.open(':memory:'))
local params, placeholders = {}, {}
for i=1,256 do params[i]=i; placeholders[i]='?' end
local row = assert(assert(db:query('SELECT '..table.concat(placeholders,','),params)):next())
assert(#row == 256 and row[256] == 256)
local png=require('vectis.png.core')
local pixels='\x12\x34\x56\xff'
local encoded=assert(png.encode_materialized({width=1,height=1,format='rgba8',pixels=pixels}))
assert(png.decode_materialized(encoded).pixels==pixels)
local pdf <close> = assert(require('vectis.pdf.core').new())
local page <close> = assert(pdf:add_page())
local image <close> = assert(pdf:load_image_materialized(encoded,{format='png'}))
assert(page:image(image,10,10,20,20))
assert(pdf:bytes_materialized():sub(1,5)=='%PDF-')
local xml=require('vectis.xml.objects.core')
local doc <close> = assert(xml.document('<r><x>text</x></r>'))
assert(doc:xpath('string(/r/x)')=='text')
local total=0
local sax <close> = assert(xml.sax({text=function(s)total=total+#s end}))
assert(sax:feed('<r>') and sax:feed('text') and sax:feed('</r>') and sax:finish())
assert(total==4)
local crypto=require('vectis.crypto.core')
local key,nonce=string.rep('\0',16),string.rep('\0',12)
local encrypted=assert(crypto.aes_gcm_encrypt_materialized({key=key,nonce=nonce,data=payload}))
assert(crypto.aes_gcm_decrypt_materialized({key=key,nonce=nonce,ciphertext=encrypted.ciphertext,tag=encrypted.tag})==payload)
