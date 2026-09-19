local curl = require("curl")
local http = require("vectis.http")
local lonejson = require("lonejson")
local base, path = assert(arg[1]), assert(arg[2])
local payload = string.rep("upload\000bytes", 4096)
local schema = lonejson.schema("redirect-upload", {
  lonejson.field("payload", lonejson.string()),
})
for _, method in ipairs({"POST", "PUT", "PATCH"}) do
  for _, kind in ipairs({"file", "multipart", "raw", "json", "buffered"}) do
    local opts = {
      url = base .. "/" .. kind,
      method = method,
      headers = {Expect = ""},
      timeout_ms = 5000,
    }
    if kind == "file" then
      opts.upload_path = path
    elseif kind == "multipart" then
      opts.multipart = {payload = {path = path, filename = "payload.bin"}}
    elseif kind == "raw" then
      opts.upload = true
      opts.body = payload
    elseif kind == "buffered" then
      opts.body = payload
    else
      opts.request = {schema = schema, value = {payload = payload:gsub("%z", "_")}}
    end
    local result = kind == "json" and curl.stream_json(opts)
        or http[method:lower()](opts)
    assert(result.ok, result.error_message or result.error)
    assert(result.status == 200 and result.body == method, kind .. " " .. method)
    do
      for _, code in ipairs({307, 308}) do
        opts.url = base .. "/" .. kind .. "/" .. code
        opts.follow_redirects = true
        result = kind == "json" and curl.stream_json(opts) or curl.perform(opts)
        assert(result.ok, result.error)
        assert(result.status == 200 and result.body == method)
      end
    end
    if method == "POST" then
      for _, code in ipairs({301, 302, 303}) do
        opts.url = base .. "/" .. kind .. "/" .. code
        opts.follow_redirects = true
        result = kind == "json" and curl.stream_json(opts) or curl.perform(opts)
        assert(result.ok, result.error)
        assert(result.status == 200 and result.body == "GET", kind .. " redirect " .. code)
      end
    else
      for _, code in ipairs({301, 302}) do
        opts.url = base .. "/" .. kind .. "/" .. code
        opts.follow_redirects = true
        result = kind == "json" and curl.stream_json(opts) or curl.perform(opts)
        assert(result.ok, result.error)
        assert(result.status == 200 and result.body == method,
            kind .. " " .. method .. " redirect " .. code)
      end
      opts.url = base .. "/" .. kind .. "/303"
      opts.follow_redirects = true
      result = kind == "json" and curl.stream_json(opts) or curl.perform(opts)
      assert(result.ok, result.error)
      assert(result.status == 200 and result.body == "GET",
          kind .. " " .. method .. " redirect 303")
    end
    if kind == "json" then
      opts.url = base .. "/json/retry"
      opts.retry = {max_attempts = 2, initial_delay_ms = 0, max_delay_ms = 0,
                    conditions = {"5xx"}}
      result = curl.stream_json(opts)
      assert(result.ok, result.error)
      assert(result.status == 200 and result.body == method)
    end
  end
end
local result = curl.perform({url = base .. "/file", upload_path = path})
assert(result.ok and result.body == "PUT", "default file method remains PUT")
