local v = require('vectis')
local app = assert(v.app.new({app_name='journal',tls={mode='disabled'}}))
local store = app:storage() -- app-owned encrypted Pouch defaults; opens after start
assert(app:start())
local collection = assert(store:create_collection({
  namespace='journal',name=require('lockdc').xid_new(),max_entries='1000',
}))
local writer = assert(collection:append_begin({
  append_id=require('lockdc').xid_new(),collection_retention_seconds=7776000,
}))
-- Native encoded staging stays in bounded memory, spilling large items to an
-- anonymous file. Publication into a shared segment happens only at commit.
assert(writer:write('{"message":"'))
assert(writer:write('hello'))
assert(writer:write('"}'))
local entry = assert(writer:commit())
writer:close()
print('committed position '..entry.position)
local _,progress=collection:read({}, {write=function(chunk)
  io.stdout:write(chunk);return true
end})
assert(progress.complete)
collection:close()
assert(app:stop())
app:close()
