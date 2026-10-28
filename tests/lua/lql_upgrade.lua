local lql = require('lql')
assert(lql == require('lql.core') and require('vectis').libs.lql == lql)
assert(lql.version() == '0.4.0', lql.version())
local function expired(h, method)
  assert(not pcall(h[method], h), 'closed handle remained usable')
end
for _, throwing in ipairs({false, true}) do
  local saved, sentinel = {}, {}
  local ok, err = pcall(function()
    local c <close> = assert(lql.new())
    assert(c:version() == '0.4.0')
    local s <close> = assert(c:selector_parse('/x=1'))
    local p <close> = assert(c:projection_parse('/x'))
    local m <close> = assert(c:mutation_parse('/y=2'))
    saved = {c, s, p, m, s:root()}
    if throwing then error(sentinel) end
  end)
  assert(ok == not throwing and (ok or err == sentinel))
  for i, method in ipairs({'version','root','path_count','count','kind'}) do
    expired(saved[i], method)
  end
  for i=1,4 do assert(saved[i]:close() and saved[i]:close()) end
end
do
  local c <close> = assert(lql.new())
  local s <close> = assert(c:selector_parse('/x=1'))
  assert(c:close())
  expired(s, 'root')
end
-- Reader ownership is paired even when reading, closing or active-close fails.
for _, mode in ipairs({'ok','read','close','both','active'}) do
  local c <close> = assert(lql.new())
  local readers, opened, closed = {}, 0, 0
  local m
  local opts = {
    file_value_open = function()
      local sent = false
      local reader = function(capacity)
        assert(capacity > 0)
        if mode == 'active' then assert(m:close()) end
        if mode == 'read' or mode == 'both' then return nil, 'first read failure' end
        if sent then return nil end
        sent = true
        return 'payload'
      end
      readers[reader] = true
      opened = opened + 1
      return reader
    end,
    file_value_close = function(reader)
      assert(readers[reader], 'wrong reader or duplicate close')
      readers[reader] = nil
      closed = closed + 1
      if mode == 'close' or mode == 'both' then return nil, 'close failure' end
      return true
    end,
  }
  m = assert(c:mutation_parse('textfile:/payload=virtual', opts))
  local mutation <close> = m
  opts.file_value_close = function() error('callbacks must be captured') end
  local result, err = c:apply_string_spooled(nil, '{}\n{}\n', {mutation=m})
  assert(opened > 0 and opened == closed)
  if mode == 'ok' then
    assert(result and result.records_seen == 2, err and err.message)
  else
    assert(result == nil and type(err.status) == 'number')
    if mode == 'both' or mode == 'read' then assert(err.message == 'first read failure') end
  end
end
do
  local c <close> = assert(lql.new())
  local s <close> = assert(c:selector_parse('/x=1'))
  local borrowed, nested, sent = nil, false, false
  local result, err = c:stream_apply_spooled(function()
    if sent then return nil end
    sent = true
    return '{"x":1}\n'
  end, s, {on_value=function(value)
    borrowed = value
    assert(value:size() == 7)
    local other <close> = assert(c:projection_parse('/x'))
    assert(other:path_count() == 1)
    nested = true
    return true
  end})
  assert(result and nested, err and err.message)
  expired(borrowed, 'size')
  local reads = 0
  result, err = c:stream_apply(function(capacity)
    reads = reads + 1
    assert(capacity >= 8)
    if reads > 100 then return nil end
    return '{"x":1}\n'
  end, s, {limits={max_records=3}})
  assert(result and result.records_seen == 3 and reads <= 4, err and err.message)
  result, err = c:stream_apply(function()return nil,'reader failed'end,s)
  assert(result == nil and err.message == 'reader failed')
end
print('upgrade seam behavior ok')
