local v = require('vectis')
local app = assert(v.app.new({app_name='fork-exit-failure', bind='127.0.0.1',
  port=assert(tonumber(os.getenv('VECTIS_SOURCE_PORT'))), worker_count=1,
  worker_set_affinity_disabled=true, shutdown_grace_ms=2000,
  tls={mode='disabled'}}))
local weak = setmetatable({}, {__mode='v'})
do
  local removed = function() error('cleared callback ran') end
  weak[1] = removed
  assert(app:on_fork_exit(removed))
end
assert(app:on_fork_exit(nil))
collectgarbage('collect')
assert(weak[1] == nil)
assert(app:on_fork_exit(function(role)
  if role == 'kore_parent' then error('injected Lua fork exit failure') end
end))
assert(app:route({path='/',method='GET',handler=function() return 'ok' end}))
assert(app:start())
io.stdout:write('ready\n'); io.stdout:flush()
local ok, err = app:stop()
assert(not ok and err.message:find('exited with status 1',1,true), tostring(err))
app:close()
io.stdout:write('fork failure behavior ok\n'); io.stdout:flush()
