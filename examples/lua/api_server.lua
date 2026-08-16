local vectis = require("vectis")
local lonejson = require("lonejson")
local http = require("vectis.http")
local rest = require("vectis.rest")

local bind = os.getenv("VECTIS_LUA_API_EXAMPLE_BIND") or "127.0.0.1"
local port = tonumber(os.getenv("VECTIS_LUA_API_EXAMPLE_PORT") or "28585")
local credentials_path = os.getenv("VECTIS_LUA_API_EXAMPLE_AUTH_PATH") or
    "vectis-lua-api-example-credentials.json"
local serve_forever = os.getenv("VECTIS_LUA_API_EXAMPLE_SERVE") == "1"

local api = rest.client({
  base_url = "http://" .. bind .. ":" .. tostring(port),
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
local base_url = "http://" .. bind .. ":" .. tostring(port)
local http_defaults = {
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
}

if not serve_forever then
  os.remove(credentials_path)
  os.remove(credentials_path .. ".lock")
end

assert(vectis.auth.store_init({ credentials_path = credentials_path }) == true)
assert(vectis.auth.user_add({
  credentials_path = credentials_path,
  username = "api-user",
  password = "api-password",
}).username == "api-user")

local auth_flow = vectis.auth.browser_flow({
  credentials_path = credentials_path,
  realm = "lua-api-example",
  purpose = "webdav",
})

local credential = assert(auth_flow:webdav_key({
  username = "api-user",
  password = "api-password",
}))
local authorization = assert(vectis.auth.basic_authorization(credential))

local server = assert(vectis.server.new({
  app_name = "lua-api-example",
  bind = bind,
  port = port,
}))

local order_request_schema = lonejson.schema("lua-api-order-request", {
  lonejson.field("sku", lonejson.string({required = true})),
  lonejson.field("quantity", lonejson.i64({required = true})),
})
local order_response_schema = lonejson.schema("lua-api-order-response", {
  lonejson.field("ok", lonejson.boolean({required = true})),
  lonejson.field("created", lonejson.boolean({required = true})),
})

assert(server:json({
  path = "/health",
  body = '{"ok":true,"service":"lua-api-example"}\n',
  cache_control = "no-store",
}) == true)
assert(server:json({
  path = "/orders",
  method = "POST",
  status = 201,
  body = '{"ok":true,"created":true}\n',
  openapi = {
    summary = "Create order",
    operation_id = "createOrder",
    tags = {"orders"},
    request = {
      name = "OrderRequest",
      schema = order_request_schema,
    },
    responses = {
      {
        status = 201,
        description = "Created",
        name = "OrderCreated",
        schema = order_response_schema,
      },
    },
  },
}) == true)
assert(server:auth_json({
  path = "/admin/status",
  body = '{"ok":true,"admin":true}\n',
  auth = auth_flow:provider(),
}) == true)
assert(server:route({
  path = "/stream",
  handler = function()
    local chunks = {"streaming ", "api ", "response\n"}
    local index = 1
    return {
      status = 202,
      content_type = "text/plain; charset=utf-8",
      headers = {
        ["x-vectis-stream"] = "api-example",
      },
      stream_source = {
        read = function(max_bytes)
          assert(type(max_bytes) == "number")
          assert(max_bytes > 0)
          local chunk = chunks[index]
          index = index + 1
          return chunk
        end,
      },
    }
  end,
}) == true)
assert(server:sse({
  path = "/events",
  open = function(request)
    assert(request.path == "/events")
    return { index = 1 }
  end,
  read = function(state, max_bytes)
    assert(type(max_bytes) == "number")
    assert(max_bytes > 0)
    if state.index == 1 then
      state.index = 2
      return {
        id = "1",
        event = "ready",
        data = "api example",
      }
    end
    if state.index == 2 then
      state.index = 3
      return "done"
    end
    return nil
  end,
}) == true)
assert(server:upload({
  path = "/upload/:name",
  buffer_bytes = 5,
  max_body_bytes = 4096,
  open = function(request)
    assert(request.path == "/upload/report")
    assert(request.param("name") == "report")
    assert(request.body == nil)
    assert(request.body_size == 0)
    assert(request.body_streaming_upload == true)
    return {
      chunks = 0,
      bytes = 0,
      body = {},
    }
  end,
  on_chunk = function(request, chunk, state)
    assert(request.body == nil)
    assert(request.body_streaming_upload == true)
    assert(type(chunk) == "string")
    assert(#chunk > 0)
    state.chunks = state.chunks + 1
    state.bytes = state.bytes + #chunk
    state.body[#state.body + 1] = chunk
    return true
  end,
  on_complete = function(request, state)
    return {
      status = 200,
      content_type = "application/json",
      headers = {
        ["x-vectis-upload"] = "streaming",
      },
      body = '{"ok":true,"name":"' .. request.param("name") ..
          '","chunks":' .. tostring(state.chunks) ..
          ',"bytes":' .. tostring(state.bytes) ..
          ',"body":"' .. table.concat(state.body) .. '"}\n',
    }
  end,
}) == true)

local openapi_json = assert(server:openapi({
  title = "Lua API Example",
  version = "1.0.0",
  format = "json",
}))
assert(openapi_json:find('"openapi":"3.1.0"', 1, true))
assert(openapi_json:find('"/orders"', 1, true))
assert(openapi_json:find('"operationId":"createOrder"', 1, true))
assert(server:json({
  path = "/openapi.json",
  body = openapi_json,
  content_type = "application/json",
  cache_control = "no-store",
}) == true)

assert(server:start() == true)

local health
for _ = 1, 20 do
  health = api.get("/health")
  if health.ok then
    break
  end
  vectis.sleep(0.1)
end
assert(health.ok == true, health.error and health.error.message)
assert(health.status == 200)
assert(health.json.ok == true)
assert(health.json.service == "lua-api-example")
assert(health.headers:lower():find("cache-control: no-store", 1, true))

local created = api.post("/orders")
assert(created.ok == true, created.error and created.error.message)
assert(created.status == 201)
assert(created.json.ok == true)
assert(created.json.created == true)

local docs = api.get("/openapi.json")
assert(docs.ok == true, docs.error and docs.error.message)
assert(docs.status == 200)
assert(docs.body:find('"title":"Lua API Example"', 1, true))
assert(docs.body:find('"OrderCreated"', 1, true))

local anonymous = api.get("/admin/status")
assert(anonymous.status == 401)
assert(anonymous.headers:lower():find(
    'www-authenticate: basic realm="lua-api-example"', 1, true))

local guarded = api.get("/admin/status", {
  headers = { Authorization = authorization },
})
assert(guarded.ok == true, guarded.error and guarded.error.message)
assert(guarded.status == 200)
assert(guarded.json.ok == true)
assert(guarded.json.admin == true)
assert(guarded.headers:lower():find("cache-control: no-store", 1, true))

local streamed = http.get(base_url .. "/stream", http_defaults)
assert(streamed.ok == true, streamed.error and streamed.error.message)
assert(streamed.status == 202)
assert(streamed.body == "streaming api response\n")
assert(streamed.headers:lower():find("x-vectis-stream: api-example", 1, true))
assert(streamed.headers:lower():find("transfer-encoding: chunked", 1, true))

local events = http.get(base_url .. "/events", http_defaults)
assert(events.ok == true, events.error and events.error.message)
assert(events.status == 200)
assert(events.body ==
    "id: 1\nevent: ready\ndata: api example\n\n" ..
    "data: done\n\n")
assert(events.headers:lower():find(
    "content-type: text/event-stream", 1, true))
assert(events.headers:lower():find("x-accel-buffering: no", 1, true))
assert(events.headers:lower():find("transfer-encoding: chunked", 1, true))

local uploaded = http.post(base_url .. "/upload/report", {
  body = "streaming upload body",
  protocols = "http",
  timeout_ms = 2000,
  connect_timeout_ms = 1000,
  no_signal = true,
})
assert(uploaded.ok == true, uploaded.error and uploaded.error.message)
assert(uploaded.status == 200)
assert(uploaded.body ==
    '{"ok":true,"name":"report","chunks":5,' ..
    '"bytes":21,"body":"streaming upload body"}\n')
assert(uploaded.headers:lower():find("x-vectis-upload: streaming", 1, true))

if serve_forever then
  print("lua api example listening on http://" .. bind .. ":" .. tostring(port))
  assert(server:wait() == true)
  server:close()
else
  assert(server:stop() == true)
  server:close()
  print("lua api server example ok")
end
