-- Reused by a service-free binding test and the real PostgreSQL XA fixture.
return function(db)
  local diagnostic=db:diagnostic()
  assert(type(diagnostic.status)=='number' and type(diagnostic.status_string)=='string')
  assert(type(diagnostic.source)=='string' and type(diagnostic.source_code)=='number')
  local function must(value,err) assert(value~=nil,type(err)=='table' and err.message or tostring(err));return value end
  local names,params={},{}
  for i=1,512 do names[i]='$'..i..'::text AS col'..i;params[i]='value'..i end
  local sql='SELECT '..table.concat(names,',')
  local old_gc=collectgarbage('incremental')
  local old_params={}
  for name,value in pairs({pause=0,stepmul=100000,stepsize=0}) do
    old_params[name]=collectgarbage('param',name,value)
  end
  local seen=0
  local function schedule_close(value,kind)
    collectgarbage('collect');collectgarbage('stop')
    do local doomed=setmetatable({}, {__gc=function()
      local ok,err=pcall(value.close,value)
      assert(not ok and tostring(err):find('cannot close an active PostgreSQL '..kind,1,true),tostring(err))
      seen=seen+1
    end}) end
    collectgarbage('restart')
  end
  schedule_close(db,'adapter')
  local result=must(db:execute_materialized(sql,params))
  collectgarbage('stop');assert(seen==1,'query allocation did not exercise finalizer')
  collectgarbage('restart')
  schedule_close(result,'result')
  local columns=result:columns()
  collectgarbage('stop');assert(seen==2,'metadata allocation did not exercise finalizer')
  collectgarbage('restart')
  assert(#columns==512 and columns[512].name=='col512')
  assert(result:value_materialized(1,512)=='value512')
  assert(not pcall(result.value_materialized,result,0,1))
  assert(result:value_materialized(1,1)=='value1')
  must(result:close())
  for name,value in pairs(old_params) do collectgarbage('param',name,value) end
  collectgarbage(old_gc)
end
