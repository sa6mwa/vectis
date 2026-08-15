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
- Expanded node IDs: `expanded_node_id(value)`,
  `expanded_node_id_parse(value)`, `expanded_node_id_local(node_id)`,
  `expanded_node_id_uri(namespace_uri, node_id)`,
  `expanded_node_id_server(server_index, node_id)`, and
  `expanded_node_id_server_uri(server_index, namespace_uri, node_id)`.
- Scalar values: `value_empty()`, `value_boolean(value)`,
  `value_integer(value)`, `value_double(value)`, `value_string(value)`,
  `value_byte_string(value)`, `value_guid(value)`, `value_status(value)`,
  `value_uint64(value)`, `value_datetime(value)`,
  `value_qualified_name(ns, name)`, and
  `value_localized_text(locale, text)`.
- Array values: `value_boolean_array(values)`, `value_integer_array(values)`,
  `value_double_array(values)`, `value_string_array(values)`,
  `value_byte_string_array(values)`, `value_uint64_array(values)`,
  `value_datetime_array(values)`, `value_status_array(values)`,
  `value_guid_array(values)`, `value_qualified_name_array(values)`, and
  `value_localized_text_array(values)`.
- Clients: `client()`, `connect(endpoint[, opts])`,
  `client:connect(endpoint[, opts])`, `client:disconnect()`,
  `client:iterate(timeout_ms)`,
  `client:create_subscription(publishing_interval_ms)`,
  `client:modify_subscription(subscription_id, publishing_interval_ms)`,
  `client:delete_subscription(subscription_id)`,
  `client:monitor_value(subscription_id, node_id, sampling_interval_ms,
  callback)`, `client:monitor_value_ex(subscription_id, node_id, opts,
  callback)`, `client:set_monitoring_mode(subscription_id, monitored_item_id,
  mode)`, `client:delete_monitored_item(subscription_id, monitored_item_id)`,
  `client:monitor_events(subscription_id, node_id, sampling_interval_ms,
  callback)`, `client:monitor_event_fields(subscription_id, node_id,
  sampling_interval_ms, field_names, callback)`,
  `client:read(node_id)`,
  `client:write(node_id, value)`, `client:add_object(opts)`,
  `client:add_variable(opts)`, `client:add_variable_under(opts)`,
  `client:add_object_type(opts)`, `client:add_variable_type(opts)`,
  `client:add_reference_type(opts)`, `client:add_data_type(opts)`,
  `client:add_view(opts)`,
  `client:delete_node(node_id, delete_target_refs)`,
  `client:add_reference(opts)`, `client:add_reference_ex(opts)`,
  `client:delete_reference(opts)`, `client:delete_reference_ex(opts)`,
  `client:read_node_id(node_id)`, `client:read_node_class(node_id)`,
  `client:read_browse_name(node_id)`, `client:read_display_name(node_id)`,
  `client:read_description(node_id)`,
  `client:write_display_name(node_id, value)`,
  `client:write_description(node_id, value)`,
  `client:read_write_mask(node_id)`,
  `client:read_user_write_mask(node_id)`,
  `client:write_write_mask(node_id, value)`,
  `client:read_is_abstract(node_id)`,
  `client:write_is_abstract(node_id, value)`,
  `client:read_symmetric(node_id)`, `client:write_symmetric(node_id, value)`,
  `client:read_inverse_name(node_id)`,
  `client:write_inverse_name(node_id, value)`,
  `client:read_contains_no_loops(node_id)`,
  `client:write_contains_no_loops(node_id, value)`,
  `client:read_event_notifier(node_id)`,
  `client:write_event_notifier(node_id, value)`,
  `client:read_data_type(node_id)`, `client:write_data_type(node_id, value)`,
  `client:read_value_rank(node_id)`,
  `client:write_value_rank(node_id, value)`,
  `client:read_array_dimensions(node_id)`,
  `client:write_array_dimensions(node_id, values)`,
  `client:read_access_level(node_id)`,
  `client:read_user_access_level(node_id)`,
  `client:write_access_level(node_id, value)`,
  `client:read_access_level_ex(node_id)`,
  `client:write_access_level_ex(node_id, value)`,
  `client:read_minimum_sampling_interval(node_id)`,
  `client:write_minimum_sampling_interval(node_id, value)`,
  `client:read_historizing(node_id)`,
  `client:write_historizing(node_id, value)`,
  `client:read_executable(node_id)`,
  `client:read_user_executable(node_id)`,
  `client:write_executable(node_id, value)`,
  `client:read_data_value(node_id)`,
  `client:read_boolean_array(node_id)`,
  `client:read_boolean_array_range(node_id, range)`,
  `client:read_integer_array(node_id)`,
  `client:read_integer_array_range(node_id, range)`,
  `client:read_double_array(node_id)`,
  `client:read_double_array_range(node_id, range)`,
  `client:read_string_array(node_id)`,
  `client:read_string_array_range(node_id, range)`,
  `client:read_byte_string_array(node_id)`,
  `client:read_byte_string_array_range(node_id, range)`,
  `client:read_uint64_array(node_id)`,
  `client:read_uint64_array_range(node_id, range)`,
  `client:read_datetime_array(node_id)`,
  `client:read_datetime_array_range(node_id, range)`,
  `client:read_status_array(node_id)`,
  `client:read_status_array_range(node_id, range)`,
  `client:read_guid_array(node_id)`,
  `client:read_guid_array_range(node_id, range)`,
  `client:read_qualified_name_array(node_id)`,
  `client:read_qualified_name_array_range(node_id, range)`,
  `client:read_localized_text_array(node_id)`,
  `client:read_localized_text_array_range(node_id, range)`,
  `client:write_index_range(node_id, range, value)`,
  `client:browse_children(node_id[, opts])`,
  `client:browse_children_ex(node_id, opts)`,
  `client:browse_children_page(node_id[, opts])`,
  `client:browse_next(continuation_point[, release])`,
  `client:read_method_argument_count(method_node_id, direction)`,
  `client:read_method_argument(method_node_id, direction, argument_index)`,
  `client:call_method(object_node_id, method_node_id, inputs)`,
  `client:call_method_many(object_node_id, method_node_id, inputs,
  output_count)`,
  `client:translate_browse_path(start_node_id, elements)`,
  `client:namespace_index(uri)`, `client:namespace_uri(index)`, and
  `client:close()`.
- Servers: `server([port_or_opts])`, `server:set_endpoint(opts_or_host, port)`,
  `server:set_application_identity(opts)`, `server:set_access_control(opts)`,
  `server:add_namespace(uri)`, `server:add_variable(opts)`,
  `server:add_object(opts)`, `server:add_variable_under(opts)`,
  `server:add_object_type(opts)`, `server:add_variable_type(opts)`,
  `server:add_reference_type(opts)`, `server:add_data_type(opts)`,
  `server:add_view(opts)`, `server:delete_node(node_id, delete_target_refs)`,
  `server:add_reference(opts)`, `server:add_reference_ex(opts)`,
  `server:delete_reference(opts)`, `server:delete_reference_ex(opts)`,
  `server:read(node_id)`, `server:write(node_id, value)`,
  `server:read_data_value(node_id)`,
  `server:read_boolean_array(node_id)`,
  `server:read_boolean_array_range(node_id, range)`,
  `server:read_integer_array(node_id)`,
  `server:read_integer_array_range(node_id, range)`,
  `server:read_double_array(node_id)`,
  `server:read_double_array_range(node_id, range)`,
  `server:read_string_array(node_id)`,
  `server:read_string_array_range(node_id, range)`,
  `server:read_byte_string_array(node_id)`,
  `server:read_byte_string_array_range(node_id, range)`,
  `server:read_uint64_array(node_id)`,
  `server:read_uint64_array_range(node_id, range)`,
  `server:read_datetime_array(node_id)`,
  `server:read_datetime_array_range(node_id, range)`,
  `server:read_status_array(node_id)`,
  `server:read_status_array_range(node_id, range)`,
  `server:read_guid_array(node_id)`,
  `server:read_guid_array_range(node_id, range)`,
  `server:read_qualified_name_array(node_id)`,
  `server:read_qualified_name_array_range(node_id, range)`,
  `server:read_localized_text_array(node_id)`,
  `server:read_localized_text_array_range(node_id, range)`,
  `server:write_index_range(node_id, range, value)`,
  `server:browse_children(node_id[, opts])`,
  `server:browse_children_ex(node_id, opts)`,
  `server:browse_children_page(node_id[, opts])`,
  `server:browse_next(continuation_point[, release])`,
  `server:add_method(opts)`, `server:add_method_many(opts)`,
  `server:read_method_argument_count(method_node_id, direction)`,
  `server:read_method_argument(method_node_id, direction, argument_index)`,
  `server:translate_browse_path(start_node_id, elements)`,
  `server:create_event(opts)`, `event:set_field(ns, name, value)`,
  `event:trigger(server)`, `event:close()`, `server:trigger_event(opts)`,
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
  `server:read_array_dimensions(node_id)`,
  `server:write_array_dimensions(node_id, values)`,
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

Browse methods materialize each borrowed cpkt callback entry into an owned Lua
table:

```lua
{
  target_node_id = opcua.node_id(...),
  node_class = opcua.NODE_CLASS_VARIABLE,
  browse_name = { namespace_index = 1, name = "child" },
  display_name = "Child",
  is_forward = true,
}
```

`browse_children()` returns an array of entries. `browse_children_page()` and
`browse_next()` return `{ entries = { ... }, continuation_point = bytes_or_nil
}`. Options accept `browse_direction` or `direction`, `include_subtypes`,
`reference_type_id` or `reference_type`, `node_class_mask`, `result_mask`, and
`max_references`.

Method callbacks registered through `server:add_method()` and
`server:add_method_many()` are retained by the server userdata until
`server:close()` or garbage collection. They run only when the owning Lua state
is explicitly pumping the OPC UA server, receive an array of owned Lua
`opcua.value` inputs, and must return one output value or an output-value array
for `add_method_many()`. String-like callback outputs are copied into C-owned
per-method storage before returning to cpkt.

Method argument metadata readers use one-based `argument_index` and return:

```lua
{
  data_type = opcua.node_id(...),
  value_rank = -1,
  name = "InputArguments",
}
```

Client value subscriptions are explicit: create a subscription, register one or
more monitored items, pump with `client:iterate(...)`, then delete monitored
items and the subscription when done. `client:monitor_value()` accepts a simple
sampling interval. `client:monitor_value_ex()` accepts
`sampling_interval_ms`, `queue_size`, `discard_oldest`, `deadband_type`, and
`deadband_value`; deadband constants are exposed as `DEADBAND_NONE`,
`DEADBAND_ABSOLUTE`, and `DEADBAND_PERCENT`. Monitoring modes are
`MONITORING_DISABLED`, `MONITORING_SAMPLING`, and `MONITORING_REPORTING`.

Value-monitor callbacks receive owned Lua tables:

```lua
{
  subscription_id = 1,
  monitored_item_id = 2,
  value = opcua.value_integer(7),
  opcua_status = 0,
  opcua_status_name = "Good",
}
```

The callback reference is retained by the client until
`client:delete_monitored_item()`, `client:delete_subscription()`,
`client:close()`, or garbage collection. Callback failures are converted into
structured OPC UA callback errors returned by the next `client:iterate(...)`
call.

Event-monitor callbacks follow the same retention and error rules.
`client:monitor_events()` receives compact event data:

```lua
{
  subscription_id = 1,
  monitored_item_id = 2,
  event = {
    event_id = "...",
    source_name = "Example Object",
    message = "Example event",
    severity = 100,
  },
  opcua_status = 0,
  opcua_status_name = "Good",
}
```

`client:monitor_event_fields()` accepts an array of field names and receives
selected fields as owned `opcua.value` userdata:

```lua
{
  subscription_id = 1,
  monitored_item_id = 3,
  fields = {
    {
      name = "Message",
      value = opcua.value_localized_text("en-US", "Example event"),
      opcua_status = 0,
      opcua_status_name = "Good",
    },
  },
  opcua_status = 0,
  opcua_status_name = "Good",
}
```

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
server work includes security configuration, access-control callbacks,
PubSub/MQTT configuration, event monitoring workflows, async-safe callback
queueing, and explicit C-only native-pointer exclusions. Remaining client work
includes async calls, native-pointer exclusions, and PubSub-related workflows.
