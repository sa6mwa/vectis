local curl = require("curl")
local path = assert(arg[1])
local source = assert(io.open(path, "rb"))
local original = source:read("*a")
source:close()
local url = "file://" .. path
local lonejson = require("lonejson")
local schema = lonejson.schema("retry-response", {
  lonejson.field("ok", lonejson.boolean()),
  lonejson.field("message", lonejson.string()),
})
local function fd_count()
  local count = 0
  for fd = 0, 1024 do
    local file = io.open("/proc/self/fd/" .. fd, "rb")
    if file then count = count + 1; file:close() end
  end
  return count
end
local before = fd_count()
for _ = 1, 20 do
  for _, opts in ipairs({{}, {body = false}, {body = {}}, {body = function() end}}) do
    opts.url = "http://127.0.0.1:1/"
    opts.upload = true
    opts.download_path = path
    opts.headers = {Accept = "text/plain"}
    local ok, err = pcall(curl.perform, opts)
    assert(not ok and tostring(err):find("body is required for upload", 1, true), tostring(err))
  end
end
collectgarbage("collect")
assert(fd_count() == before, "upload body rejection must not leak download files")
source = assert(io.open(path, "rb"))
assert(source:read("*a") == original, "upload body rejection must not truncate download files")
source:close()
for _ = 1, 20 do
  for _, headers in ipairs({false, "invalid", {broken = {}}, {broken = false},
      {"X-Valid: yes", {}}, {valid = "yes", broken = function() end}}) do
    local ok, err = pcall(curl.perform, {
      url = url, headers = headers, upload_path = path, download_path = path,
    })
    assert(not ok and tostring(err):find("header", 1, true), tostring(err))
  end
end
collectgarbage("collect")
assert(fd_count() == before, "header rejection must not leak upload/download files")
source = assert(io.open(path, "rb"))
assert(source:read("*a") == original, "header rejection must not truncate download files")
source:close()
for _ = 1, 20 do
  local ok, err = pcall(curl.stream_json, {
    url = url, upload_path = path, response = {schema = schema},
    retry = {max_attempts = 2},
  })
  assert(not ok and tostring(err):find("streaming responses cannot be retried safely", 1, true), tostring(err))
end
collectgarbage("collect")
assert(fd_count() == before, "streaming retry rejection must close upload files")
before = fd_count()
for _ = 1, 20 do
  for _, parts in ipairs({
    {{name = "valid", path = path}, {name = "missing", path = path .. ".missing"}},
    {missing = {path = path .. ".missing"}},
  }) do
    local ok, err = pcall(curl.perform, {
      url = "http://127.0.0.1:1/", multipart = parts,
      download_path = path .. ".download", timeout_ms = 100,
    })
    assert(not ok and tostring(err):find("multipart file path failed", 1, true), tostring(err))
  end
end
collectgarbage("collect")
assert(fd_count() == before, "multipart rejection must close download files")

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
  for _, proxy_type in ipairs({"INVALID", "", "http\000INVALID", false, {}}) do
    local ok, err = pcall(curl.perform, {
      url = url, proxy_type = proxy_type, upload_path = path,
      download_path = path, timeout_ms = 100,
    })
    assert(not ok and tostring(err):find("proxy_type", 1, true))
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
