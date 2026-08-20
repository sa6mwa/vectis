local vectis = require("vectis")
local vcai = require("vectis.cai")
local curl = require("curl")

local bind = os.getenv("VECTIS_LUA_CAI_ADAPTER_EXAMPLE_BIND") or "127.0.0.1"
local port_env = os.getenv("VECTIS_LUA_CAI_ADAPTER_EXAMPLE_PORT")
local port = tonumber(port_env or "28586")
local should_probe = port_env ~= nil and port > 0
local base_url = "http://" .. bind .. ":" .. tostring(port)

local app = assert(vectis.app.new({
  app_name = "lua-cai-live-service-adapter-example",
  bind = bind,
  port = port,
}))

assert(app:json({
  path = "/ready",
  body = '{"ok":true,"service":"lua-cai-live-service-adapter-example"}\n',
  cache_control = "no-store",
}) == true)

assert(app:mcp({
  path = "/mcp",
  name = "lua-cai-service-adapter-example",
  tools = {
    {
      name = "echo",
      description = "echo raw JSON arguments",
      schema_json = '{"type":"object","properties":{"text":{"type":"string"}}}',
      callback = function(arguments_json)
        return '{"content":[{"type":"text","text":' ..
            string.format("%q", arguments_json) .. '}]}'
      end,
    },
  },
}) == true)

assert(app:start() == true)
if should_probe then
  local ready
  for _ = 1, 20 do
    ready = curl.perform({
      url = base_url .. "/ready",
      protocols = "http",
      timeout_ms = 2000,
      connect_timeout_ms = 1000,
      no_signal = true,
    })
    if ready.ok then
      break
    end
    assert(vectis.sleep_ms(100) == true)
  end
  assert(ready.ok == true, ready.error)
  assert(ready.status == 200)
  assert(ready.body:find('"ok":true', 1, true))
end
assert(app:stop() == true)
app:close()

if os.getenv("VECTIS_LUA_CAI_LIVE") ~= "1" then
  print("SKIP: set VECTIS_LUA_CAI_LIVE=1 to run live CAI provider validation")
else
  local provider = os.getenv("VECTIS_LUA_CAI_PROVIDER") or "openai"
  local api_key_env = os.getenv("VECTIS_LUA_CAI_API_KEY_ENV")
  if api_key_env == nil or api_key_env == "" then
    if provider == "openrouter" then
      api_key_env = "OPENROUTER_API_KEY"
    else
      api_key_env = "OPENAI_API_KEY"
    end
  end

  local api_key = os.getenv("VECTIS_LUA_CAI_API_KEY")
  if api_key == "" then
    api_key = nil
  end
  if api_key == nil then
    local configured_key = os.getenv(api_key_env)
    if configured_key == nil or configured_key == "" then
      error("live CAI provider validation requires VECTIS_LUA_CAI_API_KEY or " ..
          api_key_env)
    end
  end

  local base_url_env = os.getenv("VECTIS_LUA_CAI_BASE_URL")
  if base_url_env == "" then
    base_url_env = nil
  end
  local model = os.getenv("VECTIS_LUA_CAI_MODEL")
  if model == nil or model == "" then
    model = vcai.native.MODEL_DEFAULT_RESPONSES
  end

  local response, err = vcai.send_text({
    provider = provider,
    api_key = api_key,
    api_key_env = api_key_env,
    base_url = base_url_env,
    timeout_ms = tonumber(os.getenv("VECTIS_LUA_CAI_TIMEOUT_MS") or "30000"),
    logger_disabled = true,
    usage_limits = {
      max_output_tokens = 64,
      max_total_tokens = 4096,
      max_spend_usd = tonumber(os.getenv("VECTIS_LUA_CAI_MAX_SPEND_USD") or "0.05"),
    },
    agent_config = {
      model = model,
      instructions = "Return concise deterministic validation text.",
      max_output_tokens = 32,
      session_usage_limits = {
        max_output_tokens = 64,
        max_total_tokens = 4096,
      },
    },
  }, "Return only this exact ASCII token: vectis-cai-live-ok")
  if response == nil then
    error(err and err.message or tostring(err))
  end

  local text = assert(response:output_text())
  assert(text:find("vectis%-cai%-live%-ok"))
  assert(response:close() == true)
  print("lua cai live provider probe ok")
end

print("lua cai live service adapter example ok")
