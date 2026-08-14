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

Use CAI directly for OpenAI request construction, tool schemas, MCP handling,
model metadata, and credential loading. Do not add a `vectis.cai` helper that
duplicates CAI request-building APIs.

## Vectis Boundary

Future Vectis-owned CAI helpers should be limited to integration surfaces:

- adapting a Vectis request-body reader into a `cai_source`
- writing CAI output to a Vectis HTTP response, lockd payload, or file sink
- mapping CAI errors into Vectis JSON API responses
- logger inheritance where Vectis owns the surrounding service component

Those helpers need an explicit ownership and streaming contract before they are
implemented. They must preserve CAI as the primary SDK for AI mechanics.
