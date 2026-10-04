local v = require('vectis')
local json, sl, opcua, pslog = require('lonejson'), require('softline'), require('opcua'), require('pslog')
local function must(value, err) assert(value ~= nil and value ~= false, debug.traceback(type(err)=='table' and err.message or tostring(err),2)); return value end
local function rejected(fn, ...) local ok,err=pcall(fn,...); assert(not ok and err~=nil,debug.traceback("expected rejection",2)) end
assert(json == require('lonejson.upstream'))
local runtime = json.new()
must(runtime:set_openssl_auth_provider())
local schema = runtime:schema('upgrade', {json.field('text',json.string())})
local record = must(schema:decode('{"text":"runtime"}'))
assert(record.text=='runtime')
local shortcut = json.schema('shortcut',{json.field('n',json.i64())})
assert(must(shortcut:decode('{"n":7}')).n==7)
local seen = {}
must(json.visit_path_value_string('{"a":[true,"text"]}', {
  boolean_value=function(path,value) assert(path[1]=='a' and value==true); seen.boolean=true end,
  string_chunk=function(path,chunk) assert(path[1]=='a'); seen.text=(seen.text or '')..chunk end,
}))
assert(seen.boolean and seen.text=='text')
local weak=setmetatable({}, {__mode='v'})
do
  local cb=function() error('visitor fault') end; weak[1]=cb
  local ok,err=json.visit_path_value_string('{"a":true}',{boolean_value=cb})
  assert(ok==nil and err.message:find('visitor fault',1,true))
end
collectgarbage('collect'); assert(weak[1]==nil)
assert(not json.visit_path_value_string('{',{ }))
local bytes='a\0b\255'
assert(must(json.base64_decode(must(json.base64_encode(bytes))))==bytes)
assert(not json.base64_decode('%%%'))
assert(not json.jwt_parse_compact('invalid'))
assert(not runtime:jwt_decode_compact('broken.token'))
assert(type(json.oauth2_refresh_token_body({refresh_token='token'}))=='string')
-- RFC 7636 deterministic PKCE vector, exercising runtime provider integration.
assert(must(json.oidc_pkce_challenge('dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk'))=='E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM')
for _, field in ipairs({'unknown','bounded','screen_x','screen_y','screen_height','height'}) do
  rejected(sl.new, {[field]=1})
end
local editor=must(v.terminal.editor({line_max_len=64,prompt_queue=true,prompt_queue_max_entries=2}))
assert(editor.set_bounds==nil)
must(editor:queue_append('first')); must(editor:queue_append('second'))
assert(editor:queue_mode(1)==sl.QUEUE_MODE_QUEUED)
must(editor:queue_set_mode(1,sl.QUEUE_MODE_STEER)); assert(editor:queue_mode(1)==sl.QUEUE_MODE_STEER)
must(editor:queue_replace(1,'changed')); assert(editor:queue_mode(1)==sl.QUEUE_MODE_STEER)
assert(editor:queue_take(1)=='changed'); assert(editor:queue_peek(1)=='second')
must(editor:set_queue_profile('queued_turns')); assert(editor:queue_profile()=='queued_turns')
must(editor:set_queue_delivery('manual')); assert(editor:queue_delivery()=='manual')
must(editor:set_image_paste_path_template('{{xdg_cache_home}}/vectis/*'))
rejected(editor.set_image_paste_path_template,editor,'/tmp/path\0suffix/*')
editor:close(); rejected(editor.queue_count,editor)
local root=assert(os.getenv('VECTIS_UPGRADE_TEST_ROOT'))
local file=assert(io.open(root..'/direct.log','w+b'))
local logger=must(pslog.new_json({output=file,disable_timestamp=true,no_color=true,level='trace'}))
local unsafe=must(pslog.new({output=function() error('must never enter Lua') end}))
rejected(opcua.client,{logger=unsafe})
rejected(opcua.connect,'opc.tcp://invalid',{logger=unsafe})
rejected(opcua.server,{logger=unsafe})
local weak_logger=setmetatable({logger},{__mode='v'})
local client
local co=coroutine.create(function() client=must(opcua.client({logger=logger})) end)
assert(coroutine.resume(co)); co=nil; logger=nil; collectgarbage('collect'); assert(weak_logger[1])
local replacement=must(pslog.new_json({output=file,disable_timestamp=true,no_color=true,level='trace'}))
must(client:set_logger({logger=replacement})); collectgarbage('collect'); assert(weak_logger[1]==nil)
must(client:set_logger({logger_disabled=true})); must(client:close()); must(client:close())
local server=must(opcua.server({logger=replacement,port=0}))
must(server:set_logger({logger_disabled=true})); must(server:set_logger({logger=replacement})); must(server:close())
local failed,err=opcua.server({json='not JSON',logger=replacement}); assert(not failed and err)
local malformed=assert(io.open(root..'/bad-config.json','w')); malformed:write('not JSON'); malformed:close()
failed,err=opcua.server({json_path=root..'/bad-config.json',logger=replacement}); assert(not failed and err)
local closed_logger=must(pslog.new_json({output=file,level='trace'}))
local closed_client=must(opcua.client({logger=closed_logger})); closed_logger:close()
rejected(closed_client.set_logger,closed_client,{logger=closed_logger}); must(closed_client:close())
file:flush(); file:seek('set',0); local output=file:read('a')
assert(output:find('"category":',1,true) and output:find('"format_status":',1,true))
-- Managed worker borrows the native server and installs its app logger before startup.
local managed_file=assert(io.open(root..'/managed.log','w+b'))
v.log.configure({output=managed_file,mode='json',env=false,level='trace'})
local managed_app=must(v.app.new({app_name='opcua-log-test',tls={mode='disabled'}}))
local managed_server=must(opcua.server({logger_disabled=true,port=0}))
must(managed_app:opcua_server_service({server=managed_server,max_wait_ms=1}))
rejected(managed_server.set_logger,managed_server,{logger=replacement})
rejected(managed_server.close,managed_server)
must(managed_app:start()); must(managed_app:stop()); managed_app:close()
managed_file:flush(); managed_file:seek('set',0); local managed_output=managed_file:read('a')
assert(managed_output:find('"sub":"opcua"',1,true) and managed_output:find('"category":',1,true))
-- Server outlives the app; its former app sink is now muted.
managed_file:seek('end',0); local before=managed_file:seek()
must(managed_server:close()); managed_file:flush(); assert(managed_file:seek('end',0)==before)
replacement:close(); unsafe:close(); file:close()
local lockd = require('vectis.lockd')
local endpoint=must(lockd.pouch_endpoint_build('/tmp/root ?#&%',{{name='crypto_key',value='a+b/=?#%'},{name='bare'},{name='bare',value=''}}))
assert(endpoint:find('%3F',1,true) and endpoint:find('%23',1,true))
assert(must(lockd.pouch_endpoint_has_option(endpoint,'bare')))
assert(must(lockd.pouch_endpoint_has_option('pouch:///tmp/root?%63rypto_key=value','crypto_key')))
assert(lockd.pouch_endpoint_has_option('pouch:///tmp/crypto_key?other=crypto_key','crypto_key')==false)
rejected(lockd.pouch_endpoint_build,'/tmp/root\0bad')
rejected(lockd.pouch_endpoint_has_option,endpoint,'bad\0name')
assert(not lockd.pouch_endpoint_build('relative',{}))
collectgarbage('collect'); collectgarbage('collect')
-- Writer output flows before later rows are requested. Native records and
-- option strings are privately retained across mutation, collection and reentry.
local xs=json.schema('writer', {json.field('text',json.string()),json.field('@id',json.string())})
local original_record=xs:new_record(); must(xs:decode_into(original_record,'{"text":"old","@id":"0"}'))
local xmlopts={schema=xs,root_element='initial-root',attribute_prefix='@',value={text='hello<&', ['@id']='7'}}
local chunks, total, writes={},0,0
local ok,n=v.xml.write(xmlopts,function(bytes)
  writes=writes+1; total=total+#bytes; chunks[#chunks+1]=bytes
  xmlopts.root_element='changed'; xmlopts.attribute_prefix='wrong'
  xmlopts.value.text='mutated'; original_record:clear(); xs:assign(original_record,{text='unrelated'})
  collectgarbage('collect')
  assert(v.xml.serialize({schema=xs,value={text='nested'}}):find('nested',1,true))
  return true
end)
assert(ok and n==total and writes>1, type(n)=="table" and n.message or tostring(n))
local output=table.concat(chunks)
assert(output=='<initial-root id="7">hello&lt;&amp;</initial-root>',output)
rejected(v.xml.write,{schema=xs,record=original_record},function() return true end)
assert(v.xml.serialize({schema=xs,record=original_record}):find('unrelated',1,true))
local big_runtime=json.new({spool_default={memory_limit=8,max_bytes=1048576,temp_dir=root}})
local big_schema=big_runtime:schema('large',{json.field('text',json.spooled_text())})
local spool_record=big_schema:new_record(); must(big_schema:decode_into(spool_record,'{"text":"'..string.rep('x',65536)..'"}'))
local spool=spool_record.text; assert(spool:spilled())
local largest, delivered, calls=0,0,0
must(v.xml.write({schema=big_schema,value={text=spool}},function(bytes)
  largest=math.max(largest,#bytes);delivered=delivered+#bytes;calls=calls+1
  if calls==1 then spool_record:clear(); spool=nil; collectgarbage('collect') end
  return true
end))
assert(largest<=4096 and delivered==65536+#'<large></large>' and calls>16)
for _,sink in ipairs({function() error({message='sink throw'}) end,function() return nil,{message='sink reject'} end}) do
  local done,err=v.xml.write({schema=xs,value={text='tiny'}},sink)
  assert(done==nil and err.message:find('sink ',1,true))
end
local ds=json.schema('rows',{json.field('id',json.string()),json.field('n',json.i64()),json.field('present',json.boolean({nullable=true}))})
local opts={schema=ds,columns={'n','id','present'},delimiter='2',header=false}
local produced,progress=0,0
opts.rows=function()
  if produced==2 then return nil end
  if produced>0 then assert(progress>0, 'producer advanced before native writer made progress') end
  produced=produced+1
  return {id=produced==1 and 'first' or 'second',n=12,present=produced==1 and true or nil}
end
local out={}
local success,size=v.dsv.core.write(opts,{write=function(bytes)
  progress=progress+#bytes; out[#out+1]=bytes
  opts.columns[1]='id'; opts.columns[2]='present'; opts.delimiter=','
  collectgarbage('collect'); return true
end})
assert(success and size==progress and produced==2)
assert(table.concat(out)=='"12"2first2true\n"12"2second2\n',table.concat(out))
assert(v.dsv.to_string({schema=ds,header=false,columns={'n','id','present'},delimiter='2',rows={{id='first',n=12,present=true},{id='second',n=12}}})==table.concat(out))
produced=0
local done,err=v.dsv.write({schema=ds,rows=function() produced=produced+1;return {id='x',n=12} end},function() return false,'stop immediately' end)
assert(done==nil and produced==1 and err.message:find('stop immediately',1,true))
local header={}
must(v.dsv.write({schema=ds,rows=function() return nil end},function(bytes) header[#header+1]=bytes;return true end))
assert(table.concat(header)=='id,n,present\n')
local buffered=ds:new_record(); must(ds:decode_into(buffered,'{"id":"borrowed","n":3}'))
rejected(v.dsv.write,{schema=ds,rows={buffered}},function() return true end)
assert(v.dsv.to_string({schema=ds,rows={buffered}}):find('borrowed,3,',1,true))
for _,bad in ipairs({{id='bad',n='invalid'},false}) do
  rejected(v.dsv.write,{schema=ds,columns={'id','n'},rows={{id='ok',n=2},bad}},function() collectgarbage('collect');return true end)
end
assert(v.dsv.to_string({schema=ds,rows={{id='after',n=4}}}):find('after,4,',1,true))
rejected(v.xml.write,{schema=false,value={}},function() return true end)
rejected(v.dsv.write,{schema=ds,columns={'id',false},rows={}},function() return true end)
-- Managed ownership blocks every server method after registration, including
-- callback installation before startup and duplicate registration in another app.
local a=must(v.app.new({app_name='managed-lease',tls={mode='disabled'}}))
local s
local co=coroutine.create(function() s=must(opcua.server({logger_disabled=true,port=0})) end)
assert(coroutine.resume(co)); co=nil; collectgarbage('collect')
must(a:opcua_server_service({server=s,max_wait_ms=1}))
local second=must(v.app.new({app_name='duplicate-lease',tls={mode='disabled'}}))
assert(not second:opcua_server_service({server=s})); second:close()
for _,name in ipairs({'close','set_logger','add_method','set_access_control','iterate','startup','shutdown'}) do
  if s[name] then rejected(s[name],s,{logger_disabled=true,handler=function() error('foreign Lua') end}) end
end
must(a:start()); rejected(s.close,s); rejected(s.set_logger,s,{logger_disabled=true})
must(a:stop()); rejected(s.close,s) -- lease ends on app close, after native logger reset
a:close(); must(s:set_logger({logger_disabled=true})); must(s:close())
local failed_a=must(v.app.new({app_name='failed-lease',tls={mode='disabled'}}))
local failed_s=must(opcua.server({logger_disabled=true,port=0}))
rejected(failed_a.opcua_server_service,failed_a,{server=failed_s,max_wait_ms="invalid"})
must(failed_s:set_logger({logger_disabled=true})); failed_a:close(); failed_s:close()
-- Unreachable cycles run the deferred native free after service join.
local weak_managed=setmetatable({}, {__mode='v'})
do
  local ca=must(v.app.new({app_name='managed-cycle',tls={mode='disabled'}}))
  local cs=must(opcua.server({logger_disabled=true,port=0}))
  must(ca:opcua_server_service({server=cs,max_wait_ms=1})); must(ca:start())
  weak_managed[1]=ca; weak_managed[2]=cs
end
collectgarbage('collect');collectgarbage('collect'); assert(weak_managed[1]==nil and weak_managed[2]==nil)
-- lua_close finalizes all userdata even when registry references exist. Keep
-- both creation orders alive until whole-VM teardown; Memcheck proves cleanup.
vm_app_first=must(v.app.new({app_name='vm-app-first',tls={mode='disabled'}}))
vm_server_second=must(opcua.server({logger_disabled=true,port=0}))
must(vm_app_first:opcua_server_service({server=vm_server_second,max_wait_ms=1})); must(vm_app_first:start())
vm_server_first=must(opcua.server({logger_disabled=true,port=0}))
vm_app_second=must(v.app.new({app_name='vm-server-first',tls={mode='disabled'}}))
must(vm_app_second:opcua_server_service({server=vm_server_first,max_wait_ms=1})); must(vm_app_second:start())
-- Getter-produced strings must survive subsequent getters and collection.
local produced_options={}
for i=1,4 do
  produced_options[i]=setmetatable({}, {__index=function(_,name)
    collectgarbage('collect')
    return name=='name' and ('option-'..i) or ('value+'..i..string.rep('x',80))
  end})
end
local generated=must(lockd.pouch_endpoint_build(root,produced_options))
for i=1,4 do assert(generated:find('option-'..i..'=value%2B'..i..string.rep('x',80),1,true),generated) end

print('upgrade seam behavior ok')
