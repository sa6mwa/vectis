local mdf = require('libmdf')
local path = assert(arg[1])
local marker = {}
assert(not pcall(mdf.pager,path,{cache_budget=65535}))
local completed,reason = mdf.pager(path,{format='text',cache_budget=65536,
  should_cancel=function() return true end})
assert(completed==false and reason=='cancelled')
local ok,err=pcall(mdf.pager,path,{format='markdown',cache_budget=65536,
  should_cancel=function() error(marker) end})
assert(not ok and err==marker)
-- Successful paging after cancellation and a callback exception proves reuse.
assert(mdf.pager(path,{format='text',cache_budget=65536}))
print('MDF pager cancellation, callback cleanup and bounded cache passed')
