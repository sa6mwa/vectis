local pg = require('vectis.pg')
local db <close> = assert(pg.connect({parameters={
  host=assert(os.getenv('PGHOST'),'PGHOST required'),
  dbname=assert(os.getenv('PGDATABASE'),'PGDATABASE required'),
  user=assert(os.getenv('PGUSER'),'PGUSER required'),
}}))
local rows <close> = assert(db:query('SELECT $1::text AS message',{'Hello PostgreSQL'}))
while true do
  local row,err = rows:next()
  if not row then assert(err==nil,err); break end
  print(row[1])
end
