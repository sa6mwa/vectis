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
  assert(stack_mdf.version == '0.23.0')
  do
    local h=stack_mdf.new({ansi_mode='off',width=80})
    local emitted=0
    assert(h:set_sink(function(chunk)
      local copy <close> = assert(buffer.from_materialized(chunk))
      emitted=emitted+#copy:read_materialized(0,#chunk)
    end))
    assert(h:set_word_buffer(64,false))
    assert(h:write_literal(string.rep('x',5000)))
    assert(emitted==0)
    assert(h:finish() and emitted>=5000)
    h:close()
  end
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
  local address='https://example.com/abcdefghijklmnopqrstuvwxyz'
  local position,tail=1,''
  local text='- **'..address..'** after\n'
  assert(stack_mdf.render_stream(function(cap)
    if position>#text then return nil end
    local last=math.min(#text,position+math.min(cap,3)-1)
    local chunk=text:sub(position,last);position=last+1;return chunk
  end,function(chunk)
    local copy <close> = assert(buffer.from_materialized(chunk))
    tail=(tail..copy:read_materialized(0,#chunk)):sub(-256)
  end,{width=20,margin_left=2,margin_right=0,ansi_mode='on',osc8=false}))
  assert(tail:find(address,1,true))
  do
    -- Keep the native digest frame active across released collection callbacks.
    assert(stack_lockdc.version_string()=='0.28.0')
    local client=assert(stack_lockdc.open({endpoints={'pouch://'..stack_root..'/lua-collections'},
      logger_disabled=true,pouch={background_compaction=false,compression='zlib',
        crypto_key=assert(stack_lockdc.pouch_crypto_generate_key())}}))
    local c=assert(client:create_collection({name='worker',lease={owner='worker',private_read=true}}))
    assert(c:append({append_id='before'},'{}'))
    local phase,remaining=0,262144
    local result=assert(c:edit({operation_id='worker-edit',expected_head=c:head().etag,
      ranges={{after='0',through='1'}},replacement={append_id='body'},
      recovery='worker',recovery_anchor='replacement'}, {read=function(cap)
      local piece
      if phase==0 then piece='{"data":"';phase=1
      elseif remaining>0 then
        local n=math.min(cap,4096,remaining);remaining=remaining-n;piece=string.rep('x',n)
      elseif phase==1 then piece='"}';phase=2
      else return nil end
      local copy <close> = assert(buffer.from_materialized(piece))
      return copy:read_materialized(0,#piece)
    end}))
    assert(result.deleted_count=='1' and result.replacement.position=='2')
    local bytes=0
    local _,progress,code=c:read({expected_head=result.head_etag},{write=function(piece)
      assert(#piece<=65536)
      local copy <close> = assert(buffer.from_materialized(piece))
      bytes=bytes+#copy:read_materialized(0,#piece);return true
    end})
    assert(code==nil and progress.complete and bytes==262156)
    assert(c:edit_status('worker-edit').found and c:head().recovery=='worker')
    assert(c:release());client:close()
  end
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
  for _,algorithm in ipairs({'zstd','lz4','xz','zlib-ng'}) do
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
dofile(stack_script_path:gsub('stack_worker%.lua$','sqlite_upgrade_cases.lua'))(db,sqlite.NULL)
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
-- UTF-8 TrueType fonts build CID maps; keep the default worker stack intact.
local font_path=stack_script_path:gsub('lua/stack_worker%.lua$','fixtures/pdf/VectisTest.ttf')
local font <close> = assert(pdf:load_font(font_path,{encoding='UTF-8'}))
assert(page:text(20,170,'A中',{font=font,size=14}))
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
