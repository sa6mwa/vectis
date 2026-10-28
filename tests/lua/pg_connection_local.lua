local v=require('vectis')
local pg=require('vectis.pg')
assert(v.pg==pg and pg.NULL==require('vectis.sql').NULL)
for _,constructor in ipairs({'connect','connect_start'}) do
  local db,err=pg[constructor]({parameters={sslmode='vectis-invalid-mode'}})
  assert(db==nil and err.operation==constructor and err.message:find('sslmode',1,true))
  assert(type(err.source)=='string' and type(err.dependency_code)=='number')
  db,err=pg[constructor]({parameters={vectis_invalid_keyword='unused'}})
  assert(db==nil and err.message:find('vectis_invalid_keyword',1,true))
end
for _,options in ipairs({
  {unknown=true},{connect_timeout_ms=-1},{io_timeout_ms=1.5},
  {parameters={port=1234}},{parameters={['host\0hidden']='ignored'}},
}) do assert(not pcall(pg.connect,options)) end
local app=assert(v.app.new({tls={mode='disabled'}}))
local options={name='copied',parameters={sslmode='original-invalid-mode'}}
local owned=assert(app:pg(options))
options.parameters.sslmode='changed-invalid-mode'
assert(owned:exec('SELECT 1',{})==nil)
assert(app:start())
local value,err=owned:exec('SELECT 1',{})
assert(value==nil and err.message:find('original-invalid-mode',1,true))
assert(owned:exec('SELECT 1',{})==nil)
assert(app:restart())
value,err=owned:exec('SELECT 1',{})
assert(value==nil and err.message:find('original-invalid-mode',1,true))
app:close()
assert(owned:close() and owned:close())
collectgarbage('collect')
print('upgrade seam behavior ok')
