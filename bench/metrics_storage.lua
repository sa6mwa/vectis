-- Finite, buffered JSON state documents, not a streaming API.
local lockdc = require("lockdc")
local root, days, layout, schedule = assert(arg[1]), assert(tonumber(arg[2])), arg[3], arg[4]
local minutes = assert(tonumber(arg[5]))
local smoke = arg[6] == "smoke"
assert(days == 180 or days == 365)
assert(layout == "sample" or layout == "partition" or layout == "shard")
assert(schedule == "boundary" or schedule == "eager")
assert(math.type(minutes) == "integer" and minutes > 0 and minutes <= days * 1440)
local tiers = {
  {name = "minute", step = 1, capacity = 1440},
  {name = "five", step = 5, capacity = 30 * 1440 // 5},
  {name = "hour", step = 60, capacity = 90 * 24},
  {name = "day", step = 1440, capacity = days},
}
local function checked(value, err)
  assert(value ~= nil and value ~= false,
         type(err) == "table" and err.message or tostring(err))
  return value
end
local config = {
  endpoints = {"pouch://" .. root .. "/state"},
  default_namespace = "vectis.metrics.bench",
  pouch = {
    crypto_key_file = root .. "/key",
    crypto_generate_key_file = true,
  },
}
local client = checked(lockdc.open(config))
local writes, read_bytes, written_bytes, reads = 0, 0, 0, 0
local function with_lease(key, fn)
  local lease = checked(client:acquire({key = key, owner = "metrics-bench", ttl_seconds = 30}))
  local ok, err = xpcall(function() fn(lease) end, debug.traceback)
  if ok then
    local released, release_err = lease:release()
    lease:close()
    checked(released, release_err)
  else
    lease:close()
    error(err)
  end
end
local function update(tier, minute)
  local index = (minute - 1) // tier.step
  local slot = index % tier.capacity
  local group, position = slot // tier.width, slot % tier.width + 1
  local key = tier.name .. "/" .. group
  with_lease(key, function(lease)
    local bytes, meta = lease:read()
    local state
    if bytes == nil then
      assert(meta and meta.no_content, type(meta) == "table" and meta.message)
      state = {samples = {}}
    elseif #bytes == 0 then
      state = {samples = {}}
    else
      read_bytes = read_bytes + #bytes
      state = checked(lockdc.decode_json(bytes))
    end
    local values = {}
    for field = 1, 8 do values[field] = tier.sums[field] end
    state.samples[position] = {t = index * tier.step, n = tier.count, v = values}
    local encoded = checked(lockdc.encode_json(state))
    checked(lease:update(encoded, {content_type = "application/json"}))
    writes, written_bytes = writes + 1, written_bytes + #encoded
  end)
  tier.keys[key] = true
end
local function phase(name)
  print(name)
  io.stdout:flush()
end
local ok, err = xpcall(function()
  for _, tier in ipairs(tiers) do
    if smoke then tier.capacity = math.min(tier.capacity, 3) end
    tier.width = layout == "sample" and 1 or
        (layout == "shard" and math.min(tier.capacity, smoke and 2 or 64) or tier.capacity)
    tier.keys, tier.sums, tier.count = {}, {0,0,0,0,0,0,0,0}, 0
  end
  phase("WRITE")
  for minute = 1, minutes do
    for _, tier in ipairs(tiers) do
      tier.count = tier.count + 1
      for field = 1, 8 do tier.sums[field] = tier.sums[field] + minute * field end
      local complete = minute % tier.step == 0
      if schedule == "eager" or complete or minute == minutes then
        update(tier, minute)
      end
      if complete then tier.count, tier.sums = 0, {0,0,0,0,0,0,0,0} end
    end
  end
  local expected_updates = 0
  for _, tier in ipairs(tiers) do
    expected_updates = expected_updates +
        (schedule == "eager" and minutes or (minutes + tier.step - 1) // tier.step)
  end
  assert(writes == expected_updates, "incorrect persistence schedule")
  client:close()
  phase("READ")
  client = checked(lockdc.open(config))
  local retained, digest = 0, 0
  for _, tier in ipairs(tiers) do
    local seen = {}
    local last = (minutes - 1) // tier.step
    local first = math.max(0, last - tier.capacity + 1)
    local keys = {}
    for key in pairs(tier.keys) do keys[#keys + 1] = key end
    table.sort(keys)
    for _, key in ipairs(keys) do
      with_lease(key, function(lease)
        local encoded = checked(lease:read())
        reads, read_bytes = reads + 1, read_bytes + #encoded
        local state = checked(lockdc.decode_json(encoded))
        for _, record in ipairs(state.samples) do
          local index = record.t // tier.step
          assert(record.t % tier.step == 0 and index >= first and index <= last)
          assert(not seen[index], "duplicate retained sample")
          seen[index] = true
          local count = math.min(tier.step, minutes - record.t)
          assert(record.n == count)
          assert(#record.v == 8)
          for field = 1, 8 do
            local expected = (2 * record.t + count + 1) * count // 2 * field
            assert(record.v[field] == expected, "incorrect aggregate")
            digest = digest + expected * field
          end
          retained = retained + 1
        end
      end)
    end
    for index = first, last do assert(seen[index], "missing retained sample") end
  end
  phase("DONE")
  print(checked(lockdc.encode_json({source_samples = minutes, retained = retained,
    digest = digest, updates = writes, read_requests = reads,
    read_bytes = read_bytes, written_bytes = written_bytes})))
end, debug.traceback)
client:close()
if not ok then error(err) end
