# Lua OPC UA

`require("opcua")` exposes the dependency-native cpkt OPC UA facade bundled
with Vectis. It is also available as `require("vectis").libs.opcua`; both names
return the same module table.

The module is intentionally thin over the public `cpkt_opcua_*` C89 facade.
Lua names keep the same nouns and lifecycle as the C API, with Lua userdata for
owned handles and structured Vectis error envelopes for dependency failures.

## Implemented Surface

- Module metadata and diagnostics: `open62541_version()`,
  `facade_version()`, `result_string(result)`, and `status_name(status)`.
- Node IDs: `node_id(value)`, `node_id_parse(value)`, `node_id_null()`,
  `node_id_numeric(ns, id)`, `node_id_string(ns, id)`,
  `node_id_guid(ns, guid)`, and `node_id_byte_string(ns, bytes)`.
- Scalar values: `value_empty()`, `value_boolean(value)`,
  `value_integer(value)`, `value_double(value)`, `value_string(value)`,
  `value_byte_string(value)`, `value_guid(value)`, `value_status(value)`,
  `value_uint64(value)`, `value_datetime(value)`,
  `value_qualified_name(ns, name)`, and
  `value_localized_text(locale, text)`.
- Clients: `client()`, `connect(endpoint[, opts])`,
  `client:connect(endpoint[, opts])`, `client:disconnect()`,
  `client:iterate(timeout_ms)`, `client:read(node_id)`,
  `client:write(node_id, value)`, `client:namespace_index(uri)`,
  `client:namespace_uri(index)`, and `client:close()`.
- Servers: `server([port_or_opts])`, `server:set_endpoint(opts_or_host, port)`,
  `server:set_application_identity(opts)`, `server:set_access_control(opts)`,
  `server:add_namespace(uri)`, `server:add_variable(opts)`,
  `server:add_object(opts)`, `server:add_variable_under(opts)`,
  `server:add_object_type(opts)`, `server:add_variable_type(opts)`,
  `server:add_reference_type(opts)`, `server:add_data_type(opts)`,
  `server:add_view(opts)`, `server:delete_node(node_id, delete_target_refs)`,
  `server:add_reference(opts)`, `server:delete_reference(opts)`,
  `server:read(node_id)`, `server:write(node_id, value)`,
  `server:read_node_id(node_id)`, `server:read_node_class(node_id)`,
  `server:read_browse_name(node_id)`, `server:read_display_name(node_id)`,
  `server:read_description(node_id)`,
  `server:write_display_name(node_id, value)`,
  `server:write_description(node_id, value)`,
  `server:read_write_mask(node_id)`,
  `server:read_user_write_mask(node_id)`,
  `server:write_write_mask(node_id, value)`,
  `server:read_is_abstract(node_id)`,
  `server:write_is_abstract(node_id, value)`,
  `server:read_symmetric(node_id)`, `server:write_symmetric(node_id, value)`,
  `server:read_inverse_name(node_id)`,
  `server:write_inverse_name(node_id, value)`,
  `server:read_contains_no_loops(node_id)`,
  `server:write_contains_no_loops(node_id, value)`,
  `server:read_event_notifier(node_id)`,
  `server:write_event_notifier(node_id, value)`,
  `server:read_data_type(node_id)`, `server:write_data_type(node_id, value)`,
  `server:read_value_rank(node_id)`,
  `server:write_value_rank(node_id, value)`,
  `server:read_access_level(node_id)`,
  `server:read_user_access_level(node_id)`,
  `server:write_access_level(node_id, value)`,
  `server:read_access_level_ex(node_id)`,
  `server:write_access_level_ex(node_id, value)`,
  `server:read_minimum_sampling_interval(node_id)`,
  `server:write_minimum_sampling_interval(node_id, value)`,
  `server:read_historizing(node_id)`,
  `server:write_historizing(node_id, value)`,
  `server:read_executable(node_id)`,
  `server:read_user_executable(node_id)`,
  `server:write_executable(node_id, value)`,
  `server:startup()`, `server:iterate(wait_internal)`,
  `server:shutdown()`, `server:endpoint_url()`, and `server:close()`.

`opcua.server({ json = bytes })` and
`opcua.server({ json_path = path })` construct from cpkt JSON5 server
configuration. `config_path` is accepted as an alias for `json_path`.

## Server Example

```lua
local opcua = require("opcua")

local server = assert(opcua.server({ port = 4840 }))
assert(server:set_endpoint({ host = "127.0.0.1", port = 4840 }) == true)
assert(server:set_application_identity({
  application_uri = "urn:vectis:example:opcua",
  product_uri = "urn:vectis",
  application_name = "Vectis OPC UA Lua Example",
}) == true)
assert(server:set_access_control({ allow_anonymous = true }) == true)

local ns = assert(server:add_namespace("urn:vectis:example:opcua"))
local node = opcua.node_id_numeric(ns, 8201)
assert(server:add_variable({
  node_id = node,
  browse_name = "exampleValue",
  display_name = "Example Value",
  value = opcua.value_integer(5),
}) == true)

assert(assert(server:read(node)):get() == 5)
assert(server:write(node, opcua.value_integer(6)) == true)
assert(assert(server:read(node)):get() == 6)

assert(server:startup() == true)
assert(type(server:endpoint_url()) == "string")
assert(type(server:iterate(false)) == "number")
assert(server:shutdown() == true)
assert(server:close() == true)
```

## Callback And Concurrency Contract

Every Lua-created OPC UA client or server is owned by the `lua_State` that
created it. Lua callbacks registered with OPC UA handles must run only while
that same Lua state is explicitly pumped by the application, such as through
`client:iterate(...)`, `server:iterate(...)`, or a Vectis-owned loop that runs
on the owner state.

Background threads must not call Lua callbacks directly. The existing Kore
server and liblockdc consumer-service concurrency uses C-owned services and
C-owned receivers, so Kore serving and lockd consumption can run simultaneously
without cross-thread Lua access. OPC UA follows the same rule: a background OPC
UA service with no Lua callback involvement can be C-owned, while any service
that invokes Lua must communicate with the owner state through an explicit
queue/mailbox or another C-owned shared resource and be pumped on that state.

Borrowed callback payloads from the C facade are copied into Lua-owned tables or
userdata before the Lua function receives them. Callback registry references
belong to the owning client or server handle and are released when the operation,
subscription, or handle is closed. Lua callback errors become structured OPC UA
callback errors and stop the relevant operation deterministically.

## Complete Surface Target

The dependency-native facade must cover the relevant public
`cpkt_opcua_client_*` and `cpkt_opcua_server_*` application surface. Remaining
server work includes security configuration, access-control callbacks, methods,
browse/path traversal, data values, array/range attributes, method argument
metadata, events, PubSub/MQTT configuration, and explicit C-only native-pointer
exclusions. Remaining client work includes async calls, subscriptions,
browse/method callbacks, remote node management, and deterministic local e2e for
callback ownership and error propagation.
