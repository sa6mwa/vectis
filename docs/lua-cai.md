# CAI Lua Facade

`require("cai")` exposes the dependency-native CAI Lua module bundled into the
`vectis` executable. It is the primary Lua surface for CAI/OpenAI mechanics.
Vectis should add only service-integration glue around CAI when a workflow needs
to cross Vectis-owned concepts such as routes, lockd payloads, files, logging,
or auth.

The pinned CAI dependency is `0.4.0`. The module is preloaded and also
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
- `cai.mcp_client`

Use CAI directly for OpenAI request construction, sessions, tool schemas,
dependency-native MCP handlers, model metadata, and credential loading.
`vectis.cai` is a service DX layer around those native objects, not a second AI SDK.
MCP client bindings are CAI-owned and available through `cai.mcp_client`,
`vectis.libs.cai.mcp_client`, and the `vectis.cai.mcp_client` alias.

## Vectis Boundary

`require("vectis.cai")` exposes:

- `native`, which is the same table as `require("cai")`
- aliases for common CAI constructors and metadata helpers such as
  `tool_schema`, `response_params`, `tool_registry`, `mcp_handler`,
  `mcp_client`, `model_info`, and ChatGPT auth/login helpers
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
- mounting a CAI Streamable HTTP MCP handler as a Vectis/Kore route

The C adapters preserve true streaming where the public API says they stream.
The request side of the MCP route preserves true upload-reader streaming into a
`cai_source`. The response side is file-backed through the current Vectis route
model because CAI writes to a sink while Kore pulls route responses after the
handler returns; it is intentionally not described as live response streaming.

Lua applications can mount MCP servers through `server:mcp()` from
`require("vectis.server")` or `require("vectis").server`. That helper builds a
CAI tool registry in C from Lua callbacks and registers the handler with
libvectis:

```lua
assert(server:mcp({
  path = "/mcp",
  name = "site-tools",
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
}))
```

Each Lua tool callback receives the raw arguments JSON string and must return
the raw JSON/text payload that CAI writes to the tool result sink. For the full
dependency-native CAI table/schema conversion semantics, create CAI registries
and handlers directly through `require("cai")`.

## Managed Worker Contract

Route-mounted MCP servers belong to the Kore child domain. Background CAI work
belongs to the Vectis managed-service lifecycle and must be declared as a
descriptor before runtime start.

The managed-worker surface is:

- C: `vectis_cai_worker_service` over `vectis_managed_service`
- C request helper: `vectis_cai_worker_event_build()` producing
  `vectis.cai.request`
- C reply helper: `vectis_cai_worker_response_decode()` consuming
  `vectis.cai.reply`
- Lua: `server:cai_worker_service(opts)`
- Lua helpers: `vectis.cai_worker.request(opts)` and
  `vectis.cai_worker.decode_reply(event)`

Worker request payloads are bounded JSON records for one-shot CAI text/JSON
work. The worker materializes its CAI client or agent in the selected runtime
domain, never in the declaration domain. It does not borrow Lua callbacks,
Kore request pointers, CAI clients, agents, or tool registries across the fork
boundary. Worker replies carry Vectis status/source metadata, CAI dependency
diagnostics, and explicitly named `text` or `raw_json` output.

`server:cai_worker_service(opts)` accepts `request_mailbox`, optional
`reply_broker`, optional `name`, optional `poll_timeout_ms`, optional `start`,
and a `client` table with copied CAI client defaults such as `api_key`,
`api_key_env`, `base_url`, `timeout_ms`, `ca_bundle_path`, `ca_path`, and
`logger_disabled`. Lua callbacks are deliberately rejected by this service;
tool-callback MCP flows belong to `server:mcp()` or an owner-state mailbox
pump.

Lua tool-callback MCP servers stay on `server:mcp()` unless a separate
owner-state tool pump is specified. Managed CAI workers must publish copied
events only.
