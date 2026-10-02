local json = require("lonejson")
local failures, checks = 0, 0

local function check(label, encoder, value, verify)
  checks = checks + 1
  local ok, encoded = pcall(encoder, value)
  if ok then ok = pcall(verify, encoded) end
  if not ok then
    failures = failures + 1
    io.stderr:write("FAIL lonejson encoder: " .. label .. "\n")
  end
end

for _, method in ipairs({ "encode_json", "encode_value" }) do
  local encoder = assert(json[method])
  for _, size in ipairs({ 253, 254, 255, 256, 512, 1024 }) do
    local text = string.rep("x", size)
    check(method .. "/plain/" .. size, encoder, text, function(encoded)
      assert(encoded == '"' .. text .. '"')
    end)
    local escaped = string.rep('Å"\\\n', size)
    check(method .. "/escaped/" .. size, encoder, escaped, function(encoded)
      assert(json.decode_value(encoded) == escaped)
    end)
  end
  for _, count in ipairs({ 1, 10, 50, 100, 200, 400, 600 }) do
    local records = {}
    for i = 1, count do
      records[i] = {
        id = i, name = "Synthetic opportunity " .. i,
        stage_id = (i % 5) + 1, owner_id = (i % 3) + 1,
        expected_revenue = i * 1000, contact_ids = { i + 1000 },
        description = string.rep("Synthetic context. ", 12),
      }
    end
    check(method .. "/pipeline/" .. count, encoder,
      { backend = "synthetic-probe", total = count, opportunities = records },
      function(encoded)
        local decoded = json.decode_value(encoded)
        assert(decoded.backend == "synthetic-probe" and decoded.total == count)
        assert(#decoded.opportunities == count)
        for i, row in ipairs(decoded.opportunities) do
          for key, value in pairs(records[i]) do
            if key == "contact_ids" then
              assert(#row[key] == 1 and row[key][1] == value[1])
            else
              assert(row[key] == value)
            end
          end
        end
      end)
  end
end
assert(failures == 0,
  "lonejson encoder contract: " .. failures .. "/" .. checks .. " checks failed")
print("JSON encoder boundaries ok: " .. checks .. " checks")
