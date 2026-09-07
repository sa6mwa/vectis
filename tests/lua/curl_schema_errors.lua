local curl = require("curl")
local lonejson = require("lonejson")
local schema = lonejson.schema("validation-errors", {
  lonejson.field("value", lonejson.string()),
})
local cases = {
  {request_schema = schema},
  {request_schema = {}, body_json = {value = "ok"}},
  {request_schema = schema, body_json = {value = "bad\000string"}},
  {response_schema = {}},
  {request_schema = schema, body_json = {value = "ok"}, response_schema = {}},
  {upload_path = arg[1], response_schema = {}},
  {request_schema = schema, body_json = {value = "ok"}, upload_path = arg[1]},
  {response_schema = schema, download_path = arg[1]},
}
for _ = 1, 20 do
  for _, opts in ipairs(cases) do
    opts.url = "http://127.0.0.1:1/"
    opts.timeout_ms = 100
    local ok, err = pcall(curl.stream_json, opts)
    assert(not ok and type(err) == "string", "expected schema validation error")
  end
end
collectgarbage("collect")
