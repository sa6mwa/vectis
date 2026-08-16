# CAI Lua Facade

`require("cai")` exposes the dependency-native CAI Lua module bundled into the
`vectis` executable. It is the primary Lua surface for CAI/OpenAI mechanics.
Vectis should add only service-integration glue around CAI when a workflow needs
to cross Vectis-owned concepts such as routes, lockd payloads, files, logging,
or auth.

The pinned CAI dependency is `0.3.0`. The module is preloaded and also
available as `require("vectis").libs.cai`; both names return the same module
table.

## Local Surface

The deterministic local surface currently covered by Vectis examples and smoke
tests includes:

- `cai.open`
- `cai.load_dotenv_api_key`
- model metadata such as `MODEL_DEFAULT_RESPONSES`, `MCP_PROTOCOL_VERSION`,
  `model_info`, and `model_can_estimate_usage_usd`
- `cai.tool_schema`
- `cai.response_params`
- `cai.tool_registry`
- `cai.mcp_handler`

Use CAI directly for OpenAI request construction, sessions, tool schemas, MCP
handling, model metadata, and credential loading. `vectis.cai` is a service DX
layer around those native objects, not a second AI SDK.

## Vectis Boundary

`require("vectis.cai")` exposes:

- `native`, which is the same table as `require("cai")`
- aliases for common CAI constructors and metadata helpers such as
  `tool_schema`, `response_params`, `tool_registry`, `mcp_handler`,
  `model_info`, and ChatGPT auth/login helpers
- `config(opts)`, which normalizes Vectis service naming such as
  `provider = "openrouter"` and `auth_json_path`
- `open(opts)`, `with_client(opts, handler)`, `new_agent(opts)`, and
  `with_agent(opts, handler)` for owned/borrowed client workflows
- `send_text(opts, text)` as a small one-shot convenience over CAI agents that
  returns the CAI response handle
- `error(err, message)`, which maps CAI failures into Vectis structured error
  tables with `source_code = ERROR_SOURCE_CAI`

The C API also exposes service adapters for embedders:

- adapting a Vectis request-body reader into a `cai_source`
- writing CAI output to a Vectis HTTP response, lockd payload, or file sink
- mapping CAI errors into Vectis errors
- logger inheritance where Vectis owns the surrounding service component

The C adapters preserve true streaming. The Lua helper does not claim to wrap a
Vectis HTTP request/response userdata because that userdata bridge is not part
of the current Lua server surface; Lua code should use CAI's dependency-native
spooled and callback-backed APIs for large values.
