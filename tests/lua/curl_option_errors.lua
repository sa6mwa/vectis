local curl = require("curl")
local path = assert(arg[1])
local source = assert(io.open(path, "rb"))
local original = source:read("*a")
source:close()
local url = "file://" .. path

for _ = 1, 30 do
  for _, protocols in ipairs({"", "https,typo", "typo", "file,typo",
                              "file\000,typo", false, {"https"}}) do
    local ok, err = pcall(curl.perform, {
      url = url, protocols = protocols, timeout_ms = 100,
    })
    assert(not ok and tostring(err):find("protocols", 1, true))
    -- Invalid restrictions must also close upload files and must not open
    -- (and truncate) a download destination, even when they share a path.
    ok, err = pcall(curl.perform, {
      url = url, protocols = protocols, upload_path = path,
      download_path = path, timeout_ms = 100,
    })
    assert(not ok and tostring(err):find("protocols", 1, true))
  end
  for _, method in ipairs({"INVALID", "", "POST\000INVALID", false, {}}) do
    local ok, err = pcall(curl.perform, {
      url = url, method = method, upload_path = path, timeout_ms = 100,
    })
    assert(not ok and tostring(err):find("method", 1, true))
  end
end
collectgarbage("collect")
local blocked = curl.perform({url = url, protocols = "http,https"})
assert(not blocked.ok and blocked.body == "", "HTTP restriction must block file reads")
local allowed = curl.perform({url = url, protocols = "file"})
assert(allowed.ok and allowed.body == original, "explicit file allowlist works")
source = assert(io.open(path, "rb"))
assert(source:read("*a") == original, "rejected options must not truncate files")
source:close()
