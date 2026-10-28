local http = require('vectis.http')
local url = assert(arg[1], 'usage: curl_session.lua URL [URL ...]')
local client <close> = assert(http.session({timeout_ms=5000}))
for _, target in ipairs(arg) do
  local result = assert(client.get(target))
  print(target, result.status, result.ok)
end
