local v = require('vectis')
local app = assert(v.app.new({app_name='source-lifetime', bind='127.0.0.1',
  port=assert(tonumber(os.getenv('VECTIS_SOURCE_PORT'))), worker_count=1,
  worker_set_affinity_disabled=true, shutdown_grace_ms=1000,
  request_body_spool_dir=assert(os.getenv('TMPDIR'))..'/spool', tls={mode='disabled'}}))
local exit_callback_weak=setmetatable({}, {__mode='v'})
do
  local replaced=function() error('replaced fork exit callback ran') end
  exit_callback_weak[1]=replaced
  assert(app:on_fork_exit(replaced))
end
assert(app:on_fork_exit(function(role)
  assert(role=='kore_parent' or role=='kore_worker')
  assert(not pcall(app.start,app))
  assert(not pcall(app.close,app))
  assert(not pcall(app.on_fork_exit,app,nil))
  io.stdout:write('fork exit '..role..'\n');io.stdout:flush()
end))
collectgarbage('collect');assert(exit_callback_weak[1]==nil)
local function reject(phase)
  collectgarbage('collect')
  local ok, err = pcall(app.close, app)
  assert(not ok and tostring(err):find('app callback', 1, true), phase..': close was accepted')
  io.stdout:write(phase..' rejected\n'); io.stdout:flush()
end
for _, kind in ipairs({'spooled_source', 'stream_source'}) do
  assert(app:route({path='/'..kind, method='GET', handler=function()
    local sent = false
    return {[kind]={
      reset=function() reject(kind..' reset'); sent=false; return true end,
      read=function()
        reject(kind..' read')
        if sent then return nil end
        sent=true
        -- Exercise pending chunk storage and balanced callback stack cleanup.
        return string.rep('x', 9000)
      end,
      close=function() reject(kind..' close'); error('caught source cleanup failure') end,
    }}
  end}))
end
for _, phase in ipairs({'read','reset'}) do
  assert(app:route({path='/fail-'..phase, method='GET', handler=function()
    return {spooled_source={
      reset=function() if phase=='reset' then app:close() end; return true end,
      read=function() app:close(); return nil end,
      close=function() reject('failed '..phase..' close') end,
    }}
  end}))
end
local function owned_response(label, kind)
  local sent=false
  return {[kind]={
    reset=function() reject(label..' reset'); sent=false; return true end,
    read=function() reject(label..' read'); if sent then return nil end; sent=true; return 'owned' end,
    close=function() reject(label..' close') end,
  }}
end
assert(app:upload({path='/upload',buffer_bytes=2,max_body_bytes=32,
  open=function() reject('upload open'); return {bytes=0} end,
  on_chunk=function(_,chunk,state) reject('upload chunk'); state.bytes=state.bytes+#chunk; return true end,
  on_complete=function(_,state) reject('upload complete'); assert(state.bytes==3); return owned_response('upload_source','stream_source') end,
  close=function() reject('upload cleanup') end,
}))
local j=require('lonejson')
assert(app:dsv({path='/dsv',buffer_bytes=2,
  schema=j.schema('source-lifetime-row',{j.field('id',j.string({required=true}))}),
  on_row=function(_,row) reject('dsv row'); assert(row.id=='one') end,
  on_complete=function(_,summary) reject('dsv complete'); assert(summary.rows==1); return owned_response('dsv_source','spooled_source') end,
}))
-- Getter failures occur after the handler's protected call. No partial native
-- source or callback registry roots may survive these conversion failures.
local weak=setmetatable({}, {__mode='v'})
local serial=0
local upload_cleanups=0
local function conversion(phase, mode)
  local function effect()
    reject('getter '..phase)
    if mode=='throw' then error('conversion getter fault '..phase) end
  end
  local function tracked(fn)
    serial=serial+1; weak[serial]=fn; return fn
  end
  local source=setmetatable({}, {__index=function(_,key)
    if key==phase then effect() end
    if key=='read' then
      local sent=false
      return tracked(function() if sent then return nil end; sent=true; return 'resolved' end)
    elseif key=='reset' then return tracked(function() return true end)
    elseif key=='close' then return tracked(function() reject('resolved cleanup') end) end
  end})
  return setmetatable({}, {__index=function(_,key)
    if key==phase then effect() end
    if key=='content_type' then return 'text/'..string.rep('x',100) end
    if key=='spooled_source' then collectgarbage('collect'); return source end
  end})
end
for _,context in ipairs({'route','before','after','dsv','upload'}) do
  for _,phase in ipairs({'read','reset','close','content_type','headers','body'}) do
    for _,mode in ipairs({'throw','guard'}) do
      local path='/convert-'..context..'-'..phase..'-'..mode
      local function result()
        if phase=='body' then
          return setmetatable({}, {__index=function(_,key)
            if key=='body' then reject('getter body'); if mode=='throw' then error('body getter fault') end; return 'resolved' end
          end})
        end
        return conversion(phase,mode)
      end
      if context=='dsv' then
        assert(app:dsv({path=path,buffer_bytes=2,
          schema=j.schema(path,{j.field('id',j.string({required=true}))}),
          on_row=function() end,on_complete=result}))
      elseif context=='upload' then
        assert(app:upload({path=path,buffer_bytes=2,open=function() return {} end,
          on_chunk=function() return true end,on_complete=result,
          close=function() upload_cleanups=upload_cleanups+1 end}))
      else
        local route={path=path,method='GET',handler=context=='route' and result or function() return 'initial' end}
        if context=='before' then route.before=result elseif context=='after' then route.after=result end
        assert(app:route(route))
      end
    end
  end
end
-- Tiny read/reset error objects exercise normalization through each shared
-- completion seam; upload cleanup must run even when response consumption fails.
local source_error_closes, source_upload_closes = 0, 0
for _,context in ipairs({'route','dsv','upload'}) do
  for _,phase in ipairs({'read','reset'}) do
    for _,shape in ipairs({'number','table'}) do
      local path='/source-error-'..context..'-'..phase..'-'..shape
      local function result()
        local source={read=function() return nil end, close=function()
          reject(path..' close'); source_error_closes=source_error_closes+1
        end}
        source[phase]=function()
          error(shape=='number' and 9384756.125 or setmetatable({}, {
            __tostring=function() error('error object must not be coerced') end}))
        end
        return {spooled_source=source}
      end
      if context=='dsv' then
        assert(app:dsv({path=path,buffer_bytes=2,
          schema=j.schema(path,{j.field('id',j.string({required=true}))}),
          on_row=function() end,on_complete=result}))
      elseif context=='upload' then
        assert(app:upload({path=path,buffer_bytes=2,open=function() return {} end,
          on_chunk=function() return true end,on_complete=result,
          close=function() source_upload_closes=source_upload_closes+1 end}))
      else
        assert(app:route({path=path,method='GET',handler=result}))
      end
    end
  end
end
assert(app:route({path='/source-error-cleanup',method='GET',handler=function()
  assert(source_error_closes==12 and source_upload_closes==4)
  return 'clean'
end}))
for _,mode in ipairs({'throw','guard'}) do
  local provider=assert(v.auth.provider_callback(function()
    return setmetatable({}, {__index=function(_,key)
      if key=='action' then reject('auth getter'); if mode=='throw' then error('auth getter fault') end; return 'allow' end
      if key=='principal' then collectgarbage('collect'); return 'getter-user' end
    end})
  end))
  assert(app:route({path='/convert-auth-'..mode,method='GET',auth=provider,handler=function() return 'resolved' end}))
end
assert(app:route({path='/conversion-ownership',method='GET',handler=function()
  collectgarbage('collect'); collectgarbage('collect')
  assert(next(weak)==nil,'conversion retained callback')
  return tostring(upload_cleanups)
end}))
assert(app:route({path='/healthy',method='GET',handler=function() return 'healthy' end}))
assert(app:route({path='/graceful',method='GET',handler=function()
  assert(app:stop())
  io.stdout:write('graceful route stop accepted\n'); io.stdout:flush()
  return 'stopping'
end}))
assert(app:start())
io.stdout:write('ready\n'); io.stdout:flush()
local ok, err = app:wait()
assert(ok, 'wait: '..tostring(err and err.message))
-- wait performs the normal stop; a second stop is documented as not started.
app:close(); app:close()
collectgarbage('collect')
io.stdout:write('closed\n'); io.stdout:flush()
