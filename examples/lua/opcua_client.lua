local opcua = require("opcua")

local endpoint = OPCUA_ENDPOINT or os.getenv("OPCUA_ENDPOINT")
assert(endpoint ~= nil and endpoint ~= "", "OPCUA_ENDPOINT is required")

local server_port =
    tonumber(OPCUA_LUA_SERVER_PORT or os.getenv("OPCUA_LUA_SERVER_PORT") or "")
if server_port then
  local server = assert(opcua.server({ port = server_port }))
  assert(server:set_endpoint({ host = "127.0.0.1", port = server_port }) == true)
  assert(server:set_application_identity({
    application_uri = "urn:vectis:example:opcua",
    product_uri = "urn:vectis",
    application_name = "Vectis OPC UA Lua Example",
  }) == true)
  assert(server:set_access_control({ allow_anonymous = true }) == true)
  local namespace_index =
      assert(server:add_namespace("urn:vectis:example:opcua"))
  local objects_folder = opcua.node_id_numeric(0, opcua.NODE_OBJECTS_FOLDER)
  local views_folder = opcua.node_id_numeric(0, opcua.NODE_VIEWS_FOLDER)
  local organizes = opcua.node_id_numeric(0, opcua.REFERENCE_ORGANIZES)

  local owned_node = opcua.node_id_numeric(namespace_index, 8201)
  assert(server:add_variable({
    node_id = owned_node,
    browse_name = "exampleValue",
    display_name = "Example Value",
    value = opcua.value_integer(5),
  }) == true)
  assert(assert(server:read(owned_node)):get() == 5)
  assert(server:write(owned_node, opcua.value_integer(6)) == true)
  assert(assert(server:read(owned_node)):get() == 6)
  assert(server:read_node_id(owned_node) == owned_node)
  assert(server:read_node_class(owned_node) == opcua.NODE_CLASS_VARIABLE)
  local browse_name = assert(server:read_browse_name(owned_node))
  assert(browse_name.namespace_index == namespace_index)
  assert(browse_name.name == "exampleValue")
  assert(server:write_display_name(owned_node, "Example Value Updated") == true)
  assert(server:read_display_name(owned_node) == "Example Value Updated")
  assert(server:write_description(owned_node, "Mutable example value") == true)
  assert(server:read_description(owned_node) == "Mutable example value")
  assert(type(server:read_write_mask(owned_node)) == "number")
  assert(type(server:read_user_write_mask(owned_node)) == "number")
  assert(type(server:read_access_level(owned_node)) == "number")
  assert(type(server:read_user_access_level(owned_node)) == "number")
  assert(type(server:read_access_level_ex(owned_node)) == "number")
  assert(type(server:read_value_rank(owned_node)) == "number")
  assert(type(tostring(server:read_data_type(owned_node))) == "string")

  local object_node = opcua.node_id_numeric(namespace_index, 8202)
  assert(server:add_object({
    node_id = object_node,
    parent_node_id = objects_folder,
    browse_name = "exampleObject",
    display_name = "Example Object",
  }) == true)
  assert(server:read_node_class(object_node) == opcua.NODE_CLASS_OBJECT)

  local child_node = opcua.node_id_numeric(namespace_index, 8203)
  assert(server:add_variable_under({
    node_id = child_node,
    parent_node_id = object_node,
    browse_name = "childValue",
    display_name = "Child Value",
    value = opcua.value_integer(11),
  }) == true)
  assert(server:read_node_class(child_node) == opcua.NODE_CLASS_VARIABLE)
  assert(assert(server:read(child_node)):get() == 11)

  local linked_object = opcua.node_id_numeric(namespace_index, 8204)
  assert(server:add_object({
    node_id = linked_object,
    parent_node_id = objects_folder,
    browse_name = "linkedObject",
    display_name = "Linked Object",
  }) == true)
  assert(server:add_reference({
    source_node_id = object_node,
    reference_type_id = organizes,
    is_forward = true,
    target_node_id = linked_object,
    target_node_class = opcua.NODE_CLASS_OBJECT,
  }) == true)
  assert(server:delete_reference({
    source_node_id = object_node,
    reference_type_id = organizes,
    is_forward = true,
    target_node_id = linked_object,
    delete_bidirectional = true,
  }) == true)

  local object_type = opcua.node_id_numeric(namespace_index, 8210)
  assert(server:add_object_type({
    node_id = object_type,
    parent_node_id = opcua.node_id_numeric(0, opcua.NODE_BASE_OBJECT_TYPE),
    browse_name = "ExampleObjectType",
    display_name = "Example Object Type",
    is_abstract = false,
  }) == true)
  assert(server:read_node_class(object_type) == opcua.NODE_CLASS_OBJECT_TYPE)
  assert(server:read_is_abstract(object_type) == false)

  local variable_type = opcua.node_id_numeric(namespace_index, 8211)
  assert(server:add_variable_type({
    node_id = variable_type,
    parent_node_id =
        opcua.node_id_numeric(0, opcua.NODE_BASE_DATA_VARIABLE_TYPE),
    browse_name = "ExampleVariableType",
    display_name = "Example Variable Type",
    value = opcua.value_integer(0),
  }) == true)
  assert(server:read_node_class(variable_type) == opcua.NODE_CLASS_VARIABLE_TYPE)

  local data_type = opcua.node_id_numeric(namespace_index, 8212)
  assert(server:add_data_type({
    node_id = data_type,
    parent_node_id = opcua.node_id_numeric(0, opcua.NODE_BASE_DATA_TYPE),
    browse_name = "ExampleDataType",
    display_name = "Example Data Type",
  }) == true)
  assert(server:read_node_class(data_type) == opcua.NODE_CLASS_DATA_TYPE)

  local reference_type = opcua.node_id_numeric(namespace_index, 8213)
  assert(server:add_reference_type({
    node_id = reference_type,
    parent_node_id = opcua.node_id_numeric(0, opcua.NODE_REFERENCES),
    browse_name = "ExampleReference",
    display_name = "Example Reference",
    inverse_name = "Example Reference Inverse",
    symmetric = false,
  }) == true)
  assert(server:read_node_class(reference_type) == opcua.NODE_CLASS_REFERENCE_TYPE)
  assert(server:read_symmetric(reference_type) == false)
  assert(server:read_inverse_name(reference_type) == "Example Reference Inverse")

  local view_node = opcua.node_id_numeric(namespace_index, 8214)
  assert(server:add_view({
    node_id = view_node,
    parent_node_id = views_folder,
    browse_name = "ExampleView",
    display_name = "Example View",
    contains_no_loops = true,
  }) == true)
  assert(server:read_node_class(view_node) == opcua.NODE_CLASS_VIEW)
  assert(server:read_contains_no_loops(view_node) == true)
  assert(type(server:read_event_notifier(view_node)) == "number")

  local disposable_node = opcua.node_id_numeric(namespace_index, 8215)
  assert(server:add_variable({
    node_id = disposable_node,
    browse_name = "disposableValue",
    display_name = "Disposable Value",
    value = opcua.value_integer(1),
  }) == true)
  assert(server:delete_node(disposable_node, true) == true)

  assert(server:startup() == true)
  assert(type(server:endpoint_url()) == "string")
  assert(type(server:iterate(false)) == "number")
  assert(server:shutdown() == true)
  assert(server:close() == true)
end

local node = opcua.node_id_numeric(1, 7101)
local client = assert(opcua.connect(endpoint))

local value = assert(client:read(node))
assert(value:type() == opcua.VALUE_INTEGER)
local initial = value:get()
assert(type(initial) == "number")

local updated_value = initial + 35
assert(client:write(node, opcua.value_integer(updated_value)) == true)
local updated = assert(client:read(node))
assert(updated:type() == opcua.VALUE_INTEGER)
assert(updated:get() == updated_value)

assert(client:disconnect() == true)
assert(client:close() == true)

local manual = assert(opcua.client())
assert(manual:connect(endpoint) == true)
assert(manual:disconnect() == true)
assert(manual:close() == true)

print("lua opcua client example ok")
