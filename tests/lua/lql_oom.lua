local lql = require('lql.core')
local cases = {}
for _, method in ipairs({'stream_apply', 'stream_apply_spooled'}) do
  cases[#cases+1] = {name=method, run=function()
    local c <close> = assert(lql.new())
    local s <close> = assert(c:selector_parse('/x=1'))
    local input = '{"x":1,"pad":"'..string.rep('a',1000)..'"}\n'
    local sent = false
    return c[method](c, function()
      if sent then return nil end
      sent = true
      return input
    end, s, {input_is_compact=true, range_writer=function(_,_,writer)
      saved_writer = writer
      return writer:write(input)
    end, on_value=function(value)
      saved_value = value
      arm()
      return value:write_to(function()return string.rep('b',1000)end)
    end})
  end}
end
cases[#cases+1] = {name='mutation reader cleanup', run=function()
  local c <close> = assert(lql.new())
  opened_readers, closed_readers = 0, 0
  local m <close> = assert(c:mutation_parse('textfile:/payload=virtual', {
    file_value_open=function()
      opened_readers = opened_readers+1
      local sent=false
      return function()
        arm()
        if sent then return nil end
        sent=true
        return string.rep('a',1000)
      end
    end,
    file_value_close=function()closed_readers=closed_readers+1;return true end,
  }))
  return c:apply_string_spooled(nil,'{}\n',{mutation=m})
end}
cases[#cases+1] = {name='result adoption', run=function()
  local c <close> = assert(lql.new())
  arm()
  return c:apply_string_spooled('/x=1','{"x":1}\n',{projection={'/x'},mutation={'/y=2'}})
end}
return cases
