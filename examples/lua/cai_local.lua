local cai = require("cai")

local work_dir = os.getenv("VECTIS_LUA_CAI_EXAMPLE_DIR") or "."
local dotenv_path = work_dir .. "/vectis-lua-cai.env"

local dotenv = assert(io.open(dotenv_path, "wb"))
dotenv:write("CAI_EXAMPLE_KEY=local-test-key\n")
dotenv:close()

assert(cai.load_dotenv_api_key(dotenv_path, "CAI_EXAMPLE_KEY") ==
       "local-test-key")
assert(type(cai.MODEL_DEFAULT_RESPONSES) == "string")
local model = assert(cai.model_info(cai.MODEL_DEFAULT_RESPONSES))
assert(model.id == cai.MODEL_DEFAULT_RESPONSES)
assert((model.capabilities & cai.MODEL_CAP_RESPONSES) ~= 0)
assert(type(cai.model_can_estimate_usage_usd(model.id)) == "boolean")

local schema = assert(cai.tool_schema())
assert(schema:set_strict(true) == true)
assert(schema:string("city", "City name", true) == true)
assert(schema:string_enum("units", "Temperature units", {"metric", "imperial"},
                          false) == true)
assert(schema:integer("days", "Forecast days", false) == true)
local schema_json = schema:json()
assert(schema_json:find('"city"', 1, true))
assert(schema_json:find('"metric"', 1, true))
assert(schema:strict() == true)

local params = assert(cai.response_params())
assert(params:set_model(cai.MODEL_DEFAULT_RESPONSES) == true)
assert(params:set_instructions("Return concise JSON.") == true)
assert(params:set_tool_choice(cai.TOOL_CHOICE_AUTO) == true)
assert(params:set_max_output_tokens(64) == true)
assert(params:set_text_format_json_schema("weather", "Weather response",
                                          schema_json, true) == true)
assert(params:add_text("user", "Weather in Stockholm tomorrow.") == true)
params:close()

local registry = assert(cai.tool_registry())
local handler = assert(cai.mcp_handler({
  name = "vectis-cai-example",
  version = "0.0.0",
  tools = registry,
  disable_origin_validation = 1,
}))
assert(type(handler.handle_http) == "function")
assert(type(cai.mcp_client) == "function")
handler:close()
registry:close()
schema:close()

os.remove(dotenv_path)

print("lua cai local example ok")
