local v,lc=require('vectis'),require('lockdc')
local storage,pg=require('vectis.storage'),require('vectis.pg')
local function describe(err)
  if type(err)=='table' then return tostring(err.message)..(err.cause and ': '..describe(err.cause) or '') end
  return tostring(err)
end
local function must(value,err) assert(value~=nil,describe(err));return value end
local password_file=assert(io.open(assert(os.getenv('VECTIS_PG_PASSWORD_FILE')),'r'))
local password=password_file:read('a');password_file:close()
local root=assert(os.getenv('VECTIS_UPGRADE_TEST_ROOT'))
local app=must(v.app.new({app_name='pg-xa-test',tls={mode='disabled'}}))
local store=app:storage({client={endpoints={'pouch://'..root},
  pouch={crypto_key=lc.pouch_crypto_generate_key(),single_writer=false},logger_disabled=true}})
must(app:start())
local co=must(store:xa_coordinator())
local id=assert(lc.xid_new())
local resource_config={resource_id=id,
  connection_info='host=127.0.0.1 port='..assert(os.getenv('VECTIS_PG_PORT'))..
  ' user=vectis dbname=vectis password='..password}
local registration=must(co:register_start('postgres',resource_config))
must(storage.await_blocking(registration));must(registration:close())
local function transaction(fn)
  return co:transaction_blocking(function(tx)
    must(tx:enroll(id))
    return tx:with_connection(id,pg.from_borrow,fn)
  end)
end
local function execute(db,sql,params)
  local result=must(db:execute_materialized(sql,params))
  must(result:close())
end
local table_name='vectis_'..lc.xid_new()
must(transaction(dofile(assert(os.getenv('VECTIS_PG_GC_SCRIPT')))))
must(transaction(function(db)
  execute(db,'CREATE TABLE '..table_name..' (id text PRIMARY KEY, body text, raw bytea, empty text)',{})
end))
local dangerous="'); DROP TABLE "..table_name..'; --'
local inserted=must(transaction(function(db)
  execute(db,'INSERT INTO '..table_name..' VALUES ($1,$2,$3,$4)',
    {'one',dangerous,pg.binary('a\0b',17),pg.NULL})
  local result=must(db:execute_materialized('SELECT body, raw, empty FROM '..table_name..' WHERE id=$1',{'one'}))
  assert(result:rows()==1 and result:columns()[1].name=='body')
  assert(result:value_materialized(1,1)==dangerous)
  assert(result:value_materialized(1,2)=='\\x610062' and result:value_materialized(1,3)==pg.NULL)
  must(result:close())
end))
local rejected,err=transaction(function(db)
  execute(db,'INSERT INTO '..table_name..' (id) VALUES ($1)',{'rollback'})
  error('abort callback')
end)
assert(rejected==nil and describe(err):find('abort callback',1,true))
-- A caught SQL error must still force rollback via the poisoned adapter close.
rejected,err=transaction(function(db)
  local result,query_error=db:execute_materialized('SELECT 1/0',{})
  assert(not result and query_error.sqlstate=='22012')
  assert(query_error.status==v.ERR_STATE and query_error.status_string=='state')
  assert(query_error.source=='cpkt' and query_error.source_code==v.ERROR_SOURCE_CPKT)
end)
assert(rejected==nil)
rejected=transaction(function(db)
  assert(not pcall(db.execute_materialized,db,'SELECT $1',{[2]='hole'}))
end)
assert(rejected==nil)
-- Retained recovery survives a coordinator restart and needs no re-registration
-- or application callback/SQL replay to acknowledge its original decision.
must(co:close())
co=must(store:xa_coordinator())
for attempt=1,2 do
  local recovery=must(co:recover_start(inserted.txn_id))
  must(storage.await_blocking(recovery))
  local outcome=recovery:result()
  assert(outcome.done and outcome.applied and outcome.decision==lc.XA_DECISION_COMMIT)
  must(recovery:close())
end
registration=must(co:register_start('postgres',resource_config))
must(storage.await_blocking(registration));must(registration:close())
must(transaction(function(db)
  local result=must(db:execute_materialized('SELECT count(*) FROM '..table_name,{}))
  assert(result:value_materialized(1,1)=='1');must(result:close())
  execute(db,'DROP TABLE '..table_name,{})
end))
must(co:close());must(app:stop());app:close()
collectgarbage('collect')
print('PostgreSQL XA behavior ok')
