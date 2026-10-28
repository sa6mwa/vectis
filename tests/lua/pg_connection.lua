local v = require('vectis')
local pg = require('vectis.pg')
assert(v.pg==pg and pg.NULL==require('vectis.sql').NULL)
local function describe(err)
  if type(err)=='table' then return tostring(err.message)..' ['..tostring(err.sqlstate)..']' end
  return tostring(err)
end
local function must(value,err) assert(value~=nil,describe(err)); return value end
local password_file=assert(io.open(assert(os.getenv('VECTIS_PG_PASSWORD_FILE')),'rb'))
local password=assert(password_file:read('a')):gsub('[\r\n]+$','')
assert(password_file:close())
local parameters={host='127.0.0.1',port=assert(os.getenv('VECTIS_PG_PORT')),
  user='vectis',dbname='vectis',password=password,sslmode='disable'}
local options={parameters=parameters}
local function connect() return must(pg.connect(options)) end
local function query(db,sql,params)
  return must(db:query(sql,params or {}))
end
local function scalar(db,sql,params)
  local cursor <close> = query(db,sql,params)
  local row=must(cursor:next())
  local result=row[1]
  local eof,err=cursor:next(); assert(eof==nil and err==nil,describe(err))
  return result
end
local wait = require('vectis.lockd.core').wait_blocking
local pending=must(pg.connect_start(options))
while true do
  local ready,err=pending:poll()
  assert(ready~=nil,describe(err))
  if ready then break end
  local readiness=must(pending:readiness())
  assert(readiness.fd>=0 and (readiness.read or readiness.write))
  wait({fd=readiness.fd,events=(readiness.read and 1 or 0)+(readiness.write and 2 or 0),deadline_ms=-1})
end
assert(scalar(pending,'SELECT 1')=='1')
assert(pending:close() and pending:close())
local db <close> = connect()
must(db:exec('CREATE TEMP TABLE own_data(value bigint)',{}))
local command=must(db:exec('INSERT INTO own_data VALUES($1)',{math.maxinteger}))
assert(command.command=='INSERT 0 1' and command.affected=='1')
assert(scalar(db,'SELECT value FROM own_data')==tostring(math.maxinteger))
assert(scalar(db,'SELECT $1::bigint',{math.mininteger})==tostring(math.mininteger))
assert(scalar(db,'SELECT $1::boolean',{true})=='t')
local input={'original',pg.binary('a\0b',17),pg.NULL}
local cursor=query(db,'SELECT $1::text AS duplicate,$2::bytea AS duplicate,$3::text',input)
input[1]='changed'; input[2].value='changed'
local row=must(cursor:next())
assert(row[1]=='original' and row[2]=='\\x610062' and row[3]==pg.NULL and #row==3)
local columns=must(cursor:columns())
assert(columns[1].name==columns[2].name and columns[2].oid==17 and columns[2].format==0)
assert(cursor:next()==nil and cursor:next()==nil and cursor:close())
local prepared=must(db:prepare('reusable','SELECT $1::text,NULL::text',{parameter_types={25}}))
for _,value in ipairs({'','again'}) do
  cursor=must(prepared:query({value}))
  row=must(cursor:next()); assert(row[1]==value and row[2]==pg.NULL)
  assert(cursor:next()==nil and cursor:close())
end
assert(prepared:close() and prepared:close())
cursor=query(db,'SELECT n FROM generate_series(1,128) AS n')
for i=1,128 do assert(must(cursor:next())[1]==tostring(i)) end
assert(cursor:next()==nil and cursor:close())
cursor=query(db,'SELECT 1/(2-n) FROM generate_series(1,3) AS n')
assert(must(cursor:next())[1]=='1')
local value,err=cursor:next()
assert(value==nil and err.sqlstate=='22012' and err.operation=='next')
assert(cursor:next()==nil and cursor:close())
assert(scalar(db,'SELECT 1')=='1')
local results=table.pack(db:transaction(function(connection)
  assert(connection==db)
  must(connection:exec('INSERT INTO own_data VALUES(7)',{}))
  return 'value',nil,7
end))
assert(results.n==4 and results[1]==true and results[2]=='value' and results[3]==nil and results[4]==7)
local sentinel={}
value,err=db:transaction(function(connection)
  must(connection:exec('INSERT INTO own_data VALUES(9)',{}))
  return nil,sentinel
end)
assert(value==nil and err.cause==sentinel and err.transaction_outcome=='rolled_back')
local ok,raised=pcall(function() db:transaction(function() error(sentinel) end) end)
assert(not ok and raised==sentinel and db:transaction_diagnostic().transaction_outcome=='rolled_back')
value,err=db:transaction(function(connection)
  assert(connection:exec('SELECT 1/0',{})==nil)
  assert(connection:exec('SELECT 1',{})==nil)
end)
assert(value==nil and err.sqlstate=='22012' and err.transaction_outcome=='rolled_back')
assert(scalar(db,'SELECT count(*) FROM own_data')=='2')
local controlled=must(db:prepare('manual_commit','COMMIT'))
for _,callback in ipairs({
  function(connection) assert(connection:exec('/* nested /* comment */ */ COMMIT',{})==nil) end,
  function(connection) assert(connection:transaction(function() end)==nil) end,
  function(connection) assert(connection:close()==nil) end,
  function() assert(controlled:query({})==nil) end,
}) do
  value,err=db:transaction(callback)
  assert(value==nil and err.transaction_outcome=='rolled_back')
end
assert(controlled:close())
must(db:transaction(function(connection)
  assert(scalar(connection,"SELECT current_setting('transaction_isolation')")=='serializable')
  assert(scalar(connection,"SELECT current_setting('transaction_read_only')")=='on')
end,{isolation='serializable',read_only=true}))
must(db:exec('CREATE TEMP TABLE parent_key(id integer PRIMARY KEY)',{}))
must(db:exec('CREATE TEMP TABLE child_key(id integer REFERENCES parent_key(id) DEFERRABLE INITIALLY DEFERRED)',{}))
value,err=db:transaction(function(connection) must(connection:exec('INSERT INTO child_key VALUES(99)',{})) end)
assert(value==nil and err.sqlstate=='23503' and err.transaction_outcome=='rolled_back' and not err.cleanup_error)
assert(scalar(db,'SELECT count(*) FROM child_key')=='0')
for _,callback in ipairs({
  function() pg.connect({unknown=true}) end,
  function() pg.connect({parameters={port=1234}}) end,
  function() db:query('SELECT 1') end,
  function() db:query('SELECT $1',{[2]='sparse'}) end,
  function() db:query('SELECT $1',{1.5}) end,
  function() db:query('SELECT 1\0ignored',{}) end,
  function() db:query('SELECT 1',{}, {unknown=true}) end,
  function() db:prepare('invalid','SELECT $1',{parameter_types={-1}}) end,
}) do assert(not pcall(callback)) end
value,err=db:exec('COPY own_data TO STDOUT',{})
assert(value==nil and err.status~=0)
assert(db:exec('SELECT 1',{})==nil)
assert(db:close())
-- An unconsumed cursor and an escaped transaction cursor invalidate ownership.
local abandoned=connect()
cursor=query(abandoned,'SELECT generate_series(1,100)')
assert(cursor:close())
assert(abandoned:exec('SELECT 1',{})==nil and abandoned:close())
local escaped_db=connect()
local escaped
value,err=escaped_db:transaction(function(connection) escaped=query(connection,'SELECT generate_series(1,100)') end)
assert(value==nil and err.transaction_outcome=='unknown' and err.cleanup_error)
assert(escaped:next()==nil and escaped:close() and escaped_db:close())
-- A held advisory lock supplies deterministic pending input without a delay.
local blocker <close> = connect()
must(blocker:exec('SELECT pg_advisory_lock(82736195)',{}))
local waiting=connect()
cursor=query(waiting,'SELECT pg_advisory_lock(82736195)')
value,err=cursor:next_ready()
assert(value==nil and err=='pending')
assert(cursor:readiness().read)
must(blocker:exec('SELECT pg_advisory_unlock(82736195)',{}))
assert(cursor:next() and cursor:next()==nil and cursor:close() and waiting:close())
assert(blocker:close())
-- App declarations copy options and materialize only after startup/fork.
local app=must(v.app.new({tls={mode='disabled'}}))
local copied={}
for key,item in pairs(parameters) do copied[key]=item end
local declaration={name='owned',parameters=copied}
local owned=must(app:pg(declaration))
copied.host='/vectis-no-such-socket-directory'; declaration.name='changed'
assert(owned:exec('SELECT 1',{})==nil)
assert(app:pg({name='owned',parameters=parameters})==nil)
must(app:start())
assert(scalar(owned,'SELECT 1')=='1')
assert(app:pg({name='late',parameters=parameters})==nil)
for _,method in ipairs({'start','stop','restart','run','close'}) do
  value,err=owned:transaction(function() assert(not pcall(app[method],app)) end)
  assert(value==nil and err.transaction_outcome=='rolled_back',method)
end
cursor=query(owned,'SELECT 1')
must(app:stop()); assert(cursor:next()==nil and cursor:close())
assert(owned:exec('SELECT 1',{})==nil)
must(app:start()); assert(scalar(owned,'SELECT 1')=='1')
must(app:restart()); assert(scalar(owned,'SELECT 1')=='1')
app:close(); assert(owned:exec('SELECT 1',{})==nil and owned:close())
collectgarbage('collect')
collectgarbage('collect')
print('standalone PostgreSQL Lua behavior ok')
