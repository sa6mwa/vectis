-- App-owned collection and managed XA lifetimes; native lockdc owns storage,
-- streaming, durability, participant protocols and wait descriptors.
local supervisor = require('vectis.supervisor')
local M = {}
-- Blocking is explicit. Event loops may instead drive start/advance/wait and
-- their own readiness scheduler; native absolute deadlines remain authoritative.
function M.await_blocking(operation)
  while true do
    local done,err,code = operation:advance()
    if done then return true end
    local result = operation:result()
    if result.done then return nil,err,code end
    if done==nil then return nil,err,code end
    require('vectis.lockd.core').wait_blocking(assert(operation:wait()))
  end
end
local resource_methods = {
  collection = {'describe','metadata','append','append_begin','append_status','entry_metadata','read','delete_entry','drop'},
  collection_writer = {'write','commit','abort'},
  coordinator = {'register_start','begin_start','adopt_start','recover_start','retire_start','resource'},
  operation = {'advance','wait','result','cancel','take_transaction','take_lease'},
  transaction = {'id','enroll_start','acquire_start','renew_start','commit_start','rollback_start','borrow'},
  borrow = {'view'},
  postgres = {'execute_materialized','diagnostic'},
  postgres_result = {'rows','columns','value_materialized'},
  lease = {'id','key','namespace','etag','version','state','load','save','update','metadata','remove','attach',
    'delete_attachment','delete_all_attachments','release','keepalive','write_state','write_attachment'},
  resource = {'identity','register_start','branch_start','recover_start','status_start','inventory_start'},
  resource_operation = {'advance','wait','result','cancel','take_branch'},
  branch = {'record','prepare_start','resolve_start','borrow'},
}
local result_kinds = {
  collection = {append_begin='collection_writer'},
  postgres = {execute_materialized='postgres_result'},
  coordinator = {register_start='operation',begin_start='operation',adopt_start='operation',
    recover_start='operation',retire_start='operation',resource='resource'},
  operation = {take_transaction='transaction',take_lease='lease'},
  transaction = {enroll_start='operation',acquire_start='operation',renew_start='operation',
    commit_start='operation',rollback_start='operation',borrow='borrow'},
  resource = {register_start='resource_operation',branch_start='resource_operation',
    recover_start='resource_operation',status_start='resource_operation',inventory_start='resource_operation'},
  resource_operation = {take_branch='branch'},
  branch = {prepare_start='resource_operation',resolve_start='resource_operation',borrow='borrow'},
}
local close_names = {coordinator='close_blocking',operation='close_blocking',transaction='close_blocking',
  resource='close_blocking',resource_operation='close_blocking'}
local function already_closed(value)
  if type(value)=='table' then value=value.message end
  if type(value)~='string' then return false end
  for _,text in ipairs({'lockdc client is closed','lockdc collection is closed',
      'XA coordinator is closed','XA transaction is closed','XA operation is closed','XA borrow is closed'}) do
    if value:find(text,1,true) then return true end
  end
  return false
end

function M.declare(app, options)
  options = options or {}
  supervisor.check_options(options, {client=true}, 'storage')
  local state = supervisor.state(app)
  assert(not state.storage, 'app storage is already declared')
  local config = supervisor.client_config(options.client or state.lockd)
  local scope = supervisor.declare(app, {name='@storage'})
  local client, closed, busy, serial = nil, false, false, 0
  local owned = {}
  local store = {}
  local function call(fn, ...)
    assert(not closed, 'app storage is closed')
    assert(not busy, 'app storage callback reentry is not allowed')
    local args = table.pack(...)
    busy = true
    local result = table.pack(pcall(app._workflow_call, app, function()
      local domain = scope.native:domain()
      assert(domain=='producer' or domain=='owner', 'storage materializes after app startup/fork')
      if not client then client = assert(app:_open_lockdc_client(supervisor.client_options(app, config))) end
      return fn(table.unpack(args,1,args.n))
    end))
    busy = false
    if not result[1] then error(result[2],0) end
    return table.unpack(result,2,result.n)
  end
  local function own(native, kind)
    if not native then return nil end
    local proxy = {}
    serial = serial + 1
    local item = {native=native,kind=kind,serial=serial}
    owned[proxy] = item
    local function live() assert(owned[proxy], 'app storage resource is closed') end
    function proxy:close()
      assert(not busy, 'cannot close storage resources from an active callback')
      local value = owned[self]
      if not value then return true end
      local result = table.pack(pcall(value.native[close_names[kind] or 'close'],value.native))
      owned[self] = nil
      if not result[1] then error(result[2],0) end
      return table.unpack(result,2,result.n)
    end
    proxy.close_blocking = proxy.close
    if kind=='borrow' then
      function proxy:postgres()
        live()
        return call(function()
          local value,err=require('vectis.pg').from_borrow(native)
          return own(value,'postgres'),err
        end)
      end
    elseif kind=='coordinator' then
      function proxy:transaction_blocking(callback)
        live()
        assert(type(callback)=='function','XA transaction requires a callback')
        return call(function() return native:transaction(callback,M.await_blocking) end)
      end
    end
    for _, name in ipairs(resource_methods[kind]) do
      if type(native[name])=='function' then
      proxy[name] = function(_, ...)
        live()
        local args = table.pack(...)
        return call(function()
          local result = table.pack(native[name](native,table.unpack(args,1,args.n)))
          local next_kind = result_kinds[kind] and result_kinds[kind][name]
          if next_kind and result[1] then result[1] = own(result[1],next_kind) end
          return table.unpack(result,1,result.n)
        end)
      end
      end
    end
    return proxy
  end
  function store:create_collection(config)
    return call(function()
      local value,err,code = client:create_collection(config)
      return own(value,'collection'),err,code
    end)
  end
  function store:open_collection(config)
    return call(function()
      local value,err,code = client:open_collection(config)
      return own(value,'collection'),err,code
    end)
  end
  function store:list_collections(options, visitor)
    return call(function() return client:list_collections(options,visitor) end)
  end
  function store:collection_capabilities()
    return call(function() return client:collection_capabilities() end)
  end
  function store:xa_coordinator(config)
    return call(function()
      require('lockdc.postgres') -- explicit linked registration, no loader
      local value,err,code = client:xa_coordinator(config)
      return own(value,'coordinator'),err,code
    end)
  end
  local function release_all()
    assert(not busy, 'cannot close active app storage')
    if closed then return true end
    local order, failure = {}, nil
    for proxy,item in pairs(owned) do order[#order+1] = {proxy=proxy,serial=item.serial} end
    table.sort(order,function(a,b) return a.serial>b.serial end)
    for _,item in ipairs(order) do
      local result = table.pack(pcall(item.proxy.close,item.proxy))
      if not result[1] or result[2]==false or (result[2]==nil and result[3]~=nil) then
        local problem = result[3] or result[2] or 'storage resource cleanup failed'
        if not already_closed(problem) then failure=failure or problem end
      end
    end
    if client then
      local ok,err = pcall(client.close,client)
      client = nil
      if not ok and not already_closed(err) then failure = failure or err end
    end
    if failure then error(failure,0) end
    return true
  end
  function store:close()
    local ok = release_all()
    closed = true
    return ok
  end
  scope.sources[#scope.sources+1] = {stop=release_all}
  state.storage = store
  return store
end
return M
