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
  local data_value, data_value_err = server:read_data_value(owned_node)
  assert(data_value ~= nil,
         data_value_err and data_value_err.message or "server read data value")
  assert(data_value.has_value == true, "data value should carry Value")
  assert(data_value.value:get() == 6, "data value should reflect written value")
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
  local method_node = opcua.node_id_numeric(namespace_index, 8222)
  assert(server:add_method({
    node_id = method_node,
    parent_node_id = object_node,
    browse_name = "triple",
    display_name = "Triple",
    input_types = { opcua.VALUE_INTEGER },
    output_type = opcua.VALUE_INTEGER,
    callback = function(inputs)
      return opcua.value_integer(inputs[1]:get() * 3)
    end,
  }) == true)
  assert(server:read_node_class(method_node) == opcua.NODE_CLASS_METHOD)
  assert(server:read_method_argument_count(
      method_node, opcua.METHOD_ARGUMENT_INPUT) == 1)
  assert(server:read_method_argument_count(
      method_node, opcua.METHOD_ARGUMENT_OUTPUT) == 1)
  local method_input_argument, method_input_argument_err =
      server:read_method_argument(method_node, opcua.METHOD_ARGUMENT_INPUT, 1)
  assert(method_input_argument ~= nil,
         method_input_argument_err and method_input_argument_err.message or
         "server read method input argument")
  assert(type(tostring(method_input_argument.data_type)) == "string")
  assert(type(method_input_argument.value_rank) == "number")
  local translated_node, translated_node_err =
      server:translate_browse_path(objects_folder, {
    { namespace_index = namespace_index, name = "exampleObject" },
  })
  assert(translated_node ~= nil,
         translated_node_err and translated_node_err.message or
         "server translate browse path")
  assert(translated_node == object_node,
         "browse path should resolve the object node")
  local server_children, server_children_err =
      server:browse_children(object_node)
  assert(server_children ~= nil,
         server_children_err and server_children_err.message or
         "server browse children")
  local found_server_child = false
  for i = 1, #server_children do
    if server_children[i].target_node_id == child_node then
      assert(server_children[i].node_class == opcua.NODE_CLASS_VARIABLE)
      assert(server_children[i].browse_name.namespace_index == namespace_index)
      assert(server_children[i].browse_name.name == "childValue")
      assert(server_children[i].display_name == "Child Value")
      found_server_child = true
    end
  end
  assert(found_server_child, "server browse children should return childValue")
  local server_variable_children, server_variable_children_err =
      server:browse_children_ex(object_node, {
    node_class_mask = opcua.NODE_CLASS_VARIABLE,
    result_mask = opcua.BROWSE_RESULT_ALL,
  })
  assert(server_variable_children ~= nil,
         server_variable_children_err and
         server_variable_children_err.message or
         "server browse children ex")
  assert(#server_variable_children >= 1,
         "server browse children ex should return variables")
  local server_page, server_page_err =
      server:browse_children_page(object_node, { max_references = 1 })
  assert(server_page ~= nil,
         server_page_err and server_page_err.message or
         "server browse children page")
  assert(type(server_page.entries) == "table")
  assert(server_page.entries[1].target_node_id == child_node)

  local array_node = opcua.node_id_numeric(namespace_index, 8205)
  local initial_integer_array = opcua.value_integer_array({ 1, 2, 3, 4 })
  local added_integer_array, added_integer_array_err = server:add_variable({
    node_id = array_node,
    browse_name = "arrayValue",
    display_name = "Array Value",
    value = initial_integer_array,
  })
  assert(added_integer_array == true,
         added_integer_array_err and added_integer_array_err.message or
         "server add integer array variable should return true")
  assert(initial_integer_array:type() == opcua.VALUE_INTEGER_ARRAY,
         "integer array constructor should return an integer array value")
  local array_items = assert(initial_integer_array:get())
  assert(array_items[1] == 1 and array_items[4] == 4,
         "integer array value should round-trip")
  local read_items, read_items_err = server:read_integer_array(array_node)
  assert(read_items ~= nil,
         read_items_err and read_items_err.message or
         "server read integer array")
  assert(read_items[2] == 2 and read_items[4] == 4,
         "server read_integer_array should return all items")
  local range_items, range_items_err =
      server:read_integer_array_range(array_node, "1:2")
  assert(range_items ~= nil,
         range_items_err and range_items_err.message or
         "server read integer array range")
  assert(range_items[1] == 2 and range_items[2] == 3,
         "integer array range should use OPC UA numeric range indexes")
  assert(server:write_index_range(
      array_node, "1:2", opcua.value_integer_array({ 20, 30 })) == true)
  local updated_items, updated_items_err = server:read_integer_array(array_node)
  assert(updated_items ~= nil,
         updated_items_err and updated_items_err.message or
         "server read updated integer array")
  assert(updated_items[1] == 1, "range write should preserve first item")
  assert(updated_items[2] == 20, "range write should update second item")
  assert(updated_items[3] == 30, "range write should update third item")
  assert(updated_items[4] == 4, "range write should preserve fourth item")
  assert(server:write_array_dimensions(array_node, { 4 }) == true)
  local array_dimensions, array_dimensions_err =
      server:read_array_dimensions(array_node)
  assert(array_dimensions ~= nil,
         array_dimensions_err and array_dimensions_err.message or
         "server read array dimensions")
  assert(#array_dimensions == 1 and array_dimensions[1] == 4,
         "server array dimensions should round-trip")

  local boolean_array_node = opcua.node_id_numeric(namespace_index, 8206)
  assert(server:add_variable({
    node_id = boolean_array_node,
    browse_name = "booleanArrayValue",
    display_name = "Boolean Array Value",
    value = opcua.value_boolean_array({ true, false, true }),
  }) == true)
  local boolean_items, boolean_items_err =
      server:read_boolean_array(boolean_array_node)
  assert(boolean_items ~= nil,
         boolean_items_err and boolean_items_err.message or
         "server read boolean array")
  assert(boolean_items[1] == true and boolean_items[2] == false,
         "boolean array should round-trip")

  local double_array_node = opcua.node_id_numeric(namespace_index, 8207)
  assert(server:add_variable({
    node_id = double_array_node,
    browse_name = "doubleArrayValue",
    display_name = "Double Array Value",
    value = opcua.value_double_array({ 1.5, 2.5 }),
  }) == true)
  local double_items, double_items_err =
      server:read_double_array(double_array_node)
  assert(double_items ~= nil,
         double_items_err and double_items_err.message or
         "server read double array")
  assert(double_items[1] == 1.5 and double_items[2] == 2.5,
         "double array should round-trip")

  local string_array_node = opcua.node_id_numeric(namespace_index, 8208)
  local added_string_array, added_string_array_err = server:add_variable({
    node_id = string_array_node,
    browse_name = "stringArrayValue",
    display_name = "String Array Value",
    value = opcua.value_string_array({ "alpha", "beta" }),
  })
  assert(added_string_array == true,
         added_string_array_err and added_string_array_err.message or
         "server add string array variable")
  local string_items, string_items_err =
      server:read_string_array(string_array_node)
  assert(string_items ~= nil,
         string_items_err and string_items_err.message or
         "server read string array")
  assert(string_items[1] == "alpha" and string_items[2] == "beta",
         "string array should round-trip")

  local byte_string_array_node = opcua.node_id_numeric(namespace_index, 8209)
  local added_byte_string_array, added_byte_string_array_err =
      server:add_variable({
    node_id = byte_string_array_node,
    browse_name = "byteStringArrayValue",
    display_name = "Byte String Array Value",
    value = opcua.value_byte_string_array({ "one", "two" }),
  })
  assert(added_byte_string_array == true,
         added_byte_string_array_err and
         added_byte_string_array_err.message or
         "server add byte string array variable")
  local byte_string_items, byte_string_items_err =
      server:read_byte_string_array(byte_string_array_node)
  assert(byte_string_items ~= nil,
         byte_string_items_err and byte_string_items_err.message or
         "server read byte string array")
  assert(byte_string_items[1] == "one" and byte_string_items[2] == "two",
         "byte string array should round-trip")

  local uint64_array_node = opcua.node_id_numeric(namespace_index, 8216)
  local added_uint64_array, added_uint64_array_err = server:add_variable({
    node_id = uint64_array_node,
    browse_name = "uint64ArrayValue",
    display_name = "UInt64 Array Value",
    value = opcua.value_uint64_array({
      { high32 = 0, low32 = 1 },
      { high32 = 2, low32 = 3 },
    }),
  })
  assert(added_uint64_array == true,
         added_uint64_array_err and added_uint64_array_err.message or
         "server add uint64 array variable")
  local uint64_items, uint64_items_err =
      server:read_uint64_array(uint64_array_node)
  assert(uint64_items ~= nil,
         uint64_items_err and uint64_items_err.message or
         "server read uint64 array")
  assert(uint64_items[1].low32 == 1 and uint64_items[2].high32 == 2,
         "uint64 array should round-trip")

  local datetime_array_node = opcua.node_id_numeric(namespace_index, 8217)
  local added_datetime_array, added_datetime_array_err = server:add_variable({
    node_id = datetime_array_node,
    browse_name = "datetimeArrayValue",
    display_name = "DateTime Array Value",
    value = opcua.value_datetime_array({
      { high32 = 1, low32 = 2 },
      { high32 = 3, low32 = 4 },
    }),
  })
  assert(added_datetime_array == true,
         added_datetime_array_err and added_datetime_array_err.message or
         "server add datetime array variable")
  local datetime_items, datetime_items_err =
      server:read_datetime_array(datetime_array_node)
  assert(datetime_items ~= nil,
         datetime_items_err and datetime_items_err.message or
         "server read datetime array")
  assert(datetime_items[1].high32 == 1 and datetime_items[2].low32 == 4,
         "datetime array should round-trip")

  local status_array_node = opcua.node_id_numeric(namespace_index, 8218)
  local added_status_array, added_status_array_err = server:add_variable({
    node_id = status_array_node,
    browse_name = "statusArrayValue",
    display_name = "Status Array Value",
    value = opcua.value_status_array({ 0, 0 }),
  })
  assert(added_status_array == true,
         added_status_array_err and added_status_array_err.message or
         "server add status array variable")
  local status_items, status_items_err =
      server:read_status_array(status_array_node)
  assert(status_items ~= nil,
         status_items_err and status_items_err.message or
         "server read status array")
  assert(status_items[1] == 0 and status_items[2] == 0,
         "status array should round-trip")

  local guid_array_node = opcua.node_id_numeric(namespace_index, 8219)
  local added_guid_array, added_guid_array_err = server:add_variable({
    node_id = guid_array_node,
    browse_name = "guidArrayValue",
    display_name = "Guid Array Value",
    value = opcua.value_guid_array({
      "00112233-4455-6677-8899-aabbccddeeff",
      "11112222-3333-4444-5555-666677778888",
    }),
  })
  assert(added_guid_array == true,
         added_guid_array_err and added_guid_array_err.message or
         "server add guid array variable")
  local guid_items, guid_items_err = server:read_guid_array(guid_array_node)
  assert(guid_items ~= nil,
         guid_items_err and guid_items_err.message or
         "server read guid array")
  assert(guid_items[1] == "00112233-4455-6677-8899-aabbccddeeff",
         "guid array should round-trip")

  local qualified_name_array_node = opcua.node_id_numeric(namespace_index, 8220)
  local added_qualified_name_array, added_qualified_name_array_err =
      server:add_variable({
    node_id = qualified_name_array_node,
    browse_name = "qualifiedNameArrayValue",
    display_name = "Qualified Name Array Value",
    value = opcua.value_qualified_name_array({
      { namespace_index = namespace_index, name = "firstName" },
      { namespace_index = namespace_index, name = "secondName" },
    }),
  })
  assert(added_qualified_name_array == true,
         added_qualified_name_array_err and
         added_qualified_name_array_err.message or
         "server add qualified name array variable")
  local qualified_name_items, qualified_name_items_err =
      server:read_qualified_name_array(qualified_name_array_node)
  assert(qualified_name_items ~= nil,
         qualified_name_items_err and qualified_name_items_err.message or
         "server read qualified name array")
  assert(qualified_name_items[1].namespace_index == namespace_index)
  assert(qualified_name_items[2].name == "secondName",
         "qualified name array should round-trip")

  local localized_text_array_node = opcua.node_id_numeric(namespace_index, 8221)
  local added_localized_text_array, added_localized_text_array_err =
      server:add_variable({
    node_id = localized_text_array_node,
    browse_name = "localizedTextArrayValue",
    display_name = "Localized Text Array Value",
    value = opcua.value_localized_text_array({
      { locale = "en-US", text = "First" },
      { locale = "sv-SE", text = "Second" },
    }),
  })
  assert(added_localized_text_array == true,
         added_localized_text_array_err and
         added_localized_text_array_err.message or
         "server add localized text array variable")
  local localized_text_items, localized_text_items_err =
      server:read_localized_text_array(localized_text_array_node)
  assert(localized_text_items ~= nil,
         localized_text_items_err and localized_text_items_err.message or
         "server read localized text array")
  assert(localized_text_items[1].locale == "en-US")
  assert(localized_text_items[2].text == "Second",
         "localized text array should round-trip")

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
  local linked_expanded = opcua.expanded_node_id_local(linked_object)
  assert(linked_expanded:node_id() == linked_object)
  assert(linked_expanded:server_index() == 0)
  assert(linked_expanded:namespace_uri() == nil)
  assert(opcua.expanded_node_id(tostring(linked_expanded)) == linked_expanded)
  assert(server:add_reference_ex({
    source_node_id = object_node,
    reference_type_id = organizes,
    is_forward = true,
    target_node_id = linked_expanded,
    target_node_class = opcua.NODE_CLASS_OBJECT,
  }) == true)
  assert(server:delete_reference_ex({
    source_node_id = object_node,
    reference_type_id = organizes,
    is_forward = true,
    target_node_id = linked_expanded,
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
  assert(server:write_event_notifier(object_node, 1) == true)

  local event, event_err = server:create_event({
    source_node_id = object_node,
    event_type_id = opcua.node_id_numeric(0, opcua.NODE_BASE_EVENT_TYPE),
    severity = 100,
    message = "Example event",
  })
  assert(event ~= nil, event_err and event_err.message or "server create event")
  assert(event:set_field(
      0, "Message", opcua.value_localized_text("en-US", "Example event")) == true,
      "event field set should succeed")

  local disposable_node = opcua.node_id_numeric(namespace_index, 8215)
  assert(server:add_variable({
    node_id = disposable_node,
    browse_name = "disposableValue",
    display_name = "Disposable Value",
    value = opcua.value_integer(1),
  }) == true)
  assert(server:delete_node(disposable_node, true) == true)

  assert(server:startup() == true)
  local event_id, event_id_err = event:trigger(server)
  assert(type(event_id) == "string",
         event_id_err and event_id_err.message or "event trigger")
  local direct_event_id, direct_event_id_err = server:trigger_event({
    source_node_id = object_node,
    event_type_id = opcua.node_id_numeric(0, opcua.NODE_BASE_EVENT_TYPE),
    severity = 50,
    message = "Direct example event",
  })
  assert(type(direct_event_id) == "string",
         direct_event_id_err and direct_event_id_err.message or
         "server trigger event")
  assert(event:close() == true)
  assert(type(server:endpoint_url()) == "string")
  assert(type(server:iterate(false)) == "number")
  assert(server:shutdown() == true)
  assert(server:close() == true)
end

local node = opcua.node_id_numeric(1, 7101)
local method_object = opcua.node_id_numeric(1, 7102)
local remote_method_node = opcua.node_id_numeric(1, 7103)
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

local remote_object = opcua.node_id_numeric(1, 7201)
local remote_array = opcua.node_id_numeric(1, 7202)
local remote_boolean_array = opcua.node_id_numeric(1, 7203)
local remote_double_array = opcua.node_id_numeric(1, 7204)
local remote_string_array = opcua.node_id_numeric(1, 7205)
local remote_byte_string_array = opcua.node_id_numeric(1, 7206)
local remote_uint64_array = opcua.node_id_numeric(1, 7207)
local remote_datetime_array = opcua.node_id_numeric(1, 7208)
local remote_status_array = opcua.node_id_numeric(1, 7209)
local remote_guid_array = opcua.node_id_numeric(1, 7210)
local remote_qualified_name_array = opcua.node_id_numeric(1, 7211)
local remote_localized_text_array = opcua.node_id_numeric(1, 7212)
local remote_object_type = opcua.node_id_numeric(1, 7220)
local remote_variable_type = opcua.node_id_numeric(1, 7221)
local remote_data_type = opcua.node_id_numeric(1, 7222)
local remote_reference_type = opcua.node_id_numeric(1, 7223)
local remote_view = opcua.node_id_numeric(1, 7224)
local remote_linked_object = opcua.node_id_numeric(1, 7225)
local objects_folder = opcua.node_id_numeric(0, opcua.NODE_OBJECTS_FOLDER)
local organizes = opcua.node_id_numeric(0, opcua.REFERENCE_ORGANIZES)
local client_object_added, client_object_add_err = client:add_object({
  node_id = remote_object,
  parent_node_id = objects_folder,
  browse_name = "clientObject",
  display_name = "Client Object",
})
assert(client_object_added == true,
	       client_object_add_err and client_object_add_err.message or
	       "client add object")
assert(client:read_node_class(remote_object) == opcua.NODE_CLASS_OBJECT)
assert(client:read_node_id(remote_object) == remote_object)
local client_object_browse_name, client_object_browse_name_err =
    client:read_browse_name(remote_object)
assert(client_object_browse_name ~= nil,
       client_object_browse_name_err and
       client_object_browse_name_err.message or "client read browse name")
assert(client_object_browse_name.namespace_index == 1)
assert(client_object_browse_name.name == "clientObject")
assert(client:read_display_name(remote_object) == "Client Object")
assert(client:write_display_name(remote_object, "Client Object Updated") == true)
assert(client:read_display_name(remote_object) == "Client Object Updated")
assert(client:write_description(remote_object, "Client-created object") == true)
assert(client:read_description(remote_object) == "Client-created object")
assert(type(client:read_write_mask(remote_object)) == "number")
assert(type(client:read_user_write_mask(remote_object)) == "number")
assert(client:add_object_type({
  node_id = remote_object_type,
  parent_node_id = opcua.node_id_numeric(0, opcua.NODE_BASE_OBJECT_TYPE),
  browse_name = "ClientObjectType",
  display_name = "Client Object Type",
  is_abstract = false,
}) == true, "client add object type")
assert(client:read_node_class(remote_object_type) == opcua.NODE_CLASS_OBJECT_TYPE)
assert(client:read_is_abstract(remote_object_type) == false)
assert(client:add_variable_type({
  node_id = remote_variable_type,
  parent_node_id = opcua.node_id_numeric(0, opcua.NODE_BASE_DATA_VARIABLE_TYPE),
  browse_name = "ClientVariableType",
  display_name = "Client Variable Type",
  value = opcua.value_integer(0),
  is_abstract = false,
}) == true, "client add variable type")
assert(client:read_node_class(remote_variable_type) ==
       opcua.NODE_CLASS_VARIABLE_TYPE)
assert(client:add_data_type({
  node_id = remote_data_type,
  parent_node_id = opcua.node_id_numeric(0, opcua.NODE_BASE_DATA_TYPE),
  browse_name = "ClientDataType",
  display_name = "Client Data Type",
  is_abstract = false,
}) == true, "client add data type")
assert(client:read_node_class(remote_data_type) == opcua.NODE_CLASS_DATA_TYPE)
assert(client:add_reference_type({
  node_id = remote_reference_type,
  parent_node_id = opcua.node_id_numeric(0, opcua.NODE_REFERENCES),
  browse_name = "ClientReference",
  display_name = "Client Reference",
  inverse_name = "Client Reference Inverse",
  is_abstract = false,
  symmetric = false,
}) == true, "client add reference type")
assert(client:read_node_class(remote_reference_type) ==
       opcua.NODE_CLASS_REFERENCE_TYPE)
assert(client:read_symmetric(remote_reference_type) == false)
assert(client:read_inverse_name(remote_reference_type) ==
       "Client Reference Inverse")
assert(client:add_view({
  node_id = remote_view,
  parent_node_id = opcua.node_id_numeric(0, opcua.NODE_VIEWS_FOLDER),
  browse_name = "ClientView",
  display_name = "Client View",
  contains_no_loops = true,
  event_notifier = 0,
}) == true, "client add view")
assert(client:read_node_class(remote_view) == opcua.NODE_CLASS_VIEW)
assert(client:read_contains_no_loops(remote_view) == true)
assert(type(client:read_event_notifier(remote_view)) == "number")
assert(client:add_object({
  node_id = remote_linked_object,
  parent_node_id = objects_folder,
  browse_name = "clientLinkedObject",
  display_name = "Client Linked Object",
}) == true, "client add linked object")
assert(client:add_reference({
  source_node_id = remote_object,
  reference_type_id = organizes,
  is_forward = true,
  target_node_id = remote_linked_object,
  target_node_class = opcua.NODE_CLASS_OBJECT,
}) == true, "client add reference")
assert(client:delete_reference({
  source_node_id = remote_object,
  reference_type_id = organizes,
  is_forward = true,
  target_node_id = remote_linked_object,
  delete_bidirectional = true,
}) == true, "client delete reference")
local remote_linked_expanded = opcua.expanded_node_id_local(remote_linked_object)
assert(remote_linked_expanded:node_id() == remote_linked_object)
assert(opcua.expanded_node_id(tostring(remote_linked_expanded)) ==
       remote_linked_expanded)
assert(client:add_reference_ex({
  source_node_id = remote_object,
  reference_type_id = organizes,
  is_forward = true,
  target_node_id = remote_linked_expanded,
  target_node_class = opcua.NODE_CLASS_OBJECT,
}) == true, "client add reference ex")
assert(client:delete_reference_ex({
  source_node_id = remote_object,
  reference_type_id = organizes,
  is_forward = true,
  target_node_id = remote_linked_expanded,
  delete_bidirectional = true,
}) == true, "client delete reference ex")
local client_array_added, client_array_add_err = client:add_variable_under({
  node_id = remote_array,
  parent_node_id = remote_object,
  browse_name = "clientArray",
  display_name = "Client Array",
  value = opcua.value_integer_array({ 1, 2, 3, 4 }),
})
assert(client_array_added == true,
       client_array_add_err and client_array_add_err.message or
       "client add array variable")
assert(type(tostring(client:read_data_type(remote_array))) == "string")
assert(type(client:read_value_rank(remote_array)) == "number")
assert(client:write_array_dimensions(remote_array, { 4 }) == true)
local client_array_dimensions, client_array_dimensions_err =
    client:read_array_dimensions(remote_array)
assert(client_array_dimensions ~= nil,
       client_array_dimensions_err and client_array_dimensions_err.message or
       "client read array dimensions")
assert(#client_array_dimensions == 1 and client_array_dimensions[1] == 4,
       "client array dimensions should round-trip")
assert(type(client:read_access_level(remote_array)) == "number")
assert(type(client:read_user_access_level(remote_array)) == "number")
assert(type(client:read_access_level_ex(remote_array)) == "number")
assert(type(client:read_minimum_sampling_interval(remote_array)) == "number")
assert(type(client:read_historizing(remote_array)) == "boolean")
local client_data_value, client_data_value_err = client:read_data_value(node)
assert(client_data_value ~= nil,
       client_data_value_err and client_data_value_err.message or
       "client read data value")
assert(client_data_value.has_value == true)
assert(client_data_value.value:type() == opcua.VALUE_INTEGER)
local subscription_id = assert(client:create_subscription(50))
assert(client:modify_subscription(subscription_id, 25) == true)
local subscription_changes = {}
local monitored_basic = assert(client:monitor_value(
    subscription_id, node, 10, function(change)
      assert(type(change.subscription_id) == "number")
      assert(type(change.monitored_item_id) == "number")
      assert(change.value:type() == opcua.VALUE_INTEGER)
      assert(type(change.opcua_status) == "number")
      assert(type(change.opcua_status_name) == "string")
      subscription_changes[#subscription_changes + 1] = change
    end))
local monitored_options = assert(client:monitor_value_ex(
    subscription_id, node, {
      sampling_interval_ms = 10,
      queue_size = 4,
      discard_oldest = true,
      deadband_type = opcua.DEADBAND_NONE,
    }, function(change)
      assert(change.value:type() == opcua.VALUE_INTEGER)
      subscription_changes[#subscription_changes + 1] = change
    end))
assert(client:set_monitoring_mode(
    subscription_id, monitored_basic, opcua.MONITORING_REPORTING) == true)
assert(client:set_monitoring_mode(
    subscription_id, monitored_options, opcua.MONITORING_REPORTING) == true)
local subscription_value = updated_value + 1
assert(client:write(node, opcua.value_integer(subscription_value)) == true)
local observed_basic = false
local observed_options = false
for _ = 1, 100 do
  local ok, err = client:iterate(20)
  assert(ok == true, err and err.message or "client iterate subscription")
  for i = 1, #subscription_changes do
    local change = subscription_changes[i]
    if change.value:get() == subscription_value then
      if change.monitored_item_id == monitored_basic then
        observed_basic = true
      end
      if change.monitored_item_id == monitored_options then
        observed_options = true
      end
    end
  end
  if observed_basic and observed_options then
    break
  end
end
assert(observed_basic, "client monitor_value callback should observe write")
assert(observed_options, "client monitor_value_ex callback should observe write")
assert(client:set_monitoring_mode(
    subscription_id, monitored_basic, opcua.MONITORING_DISABLED) == true)
assert(client:delete_monitored_item(
    subscription_id, monitored_basic) == true)
assert(client:delete_monitored_item(
    subscription_id, monitored_options) == true)
assert(client:delete_subscription(subscription_id) == true)

local error_subscription_id = assert(client:create_subscription(25))
local error_monitored_item = assert(client:monitor_value(
    error_subscription_id, node, 10, function()
      error("intentional opcua monitor failure")
    end))
assert(client:write(node, opcua.value_integer(subscription_value + 1)) == true)
local callback_error_observed = false
for _ = 1, 100 do
  local ok, err = client:iterate(20)
  if ok == nil then
    assert(err.dependency == "opcua")
    assert(err.message:find("intentional opcua monitor failure", 1, true))
    callback_error_observed = true
    break
  end
end
assert(callback_error_observed,
       "client monitor callback error should surface through iterate")
assert(client:delete_monitored_item(
    error_subscription_id, error_monitored_item) == true)
assert(client:delete_subscription(error_subscription_id) == true)
assert(client:write(node, opcua.value_integer(updated_value)) == true)

local event_subscription_id = assert(client:create_subscription(25))
local compact_events = {}
local selected_events = {}
local compact_event_item = assert(client:monitor_events(
    event_subscription_id, method_object, 10, function(change)
      assert(type(change.subscription_id) == "number")
      assert(type(change.monitored_item_id) == "number")
      assert(type(change.event.event_id) == "string")
      assert(type(change.event.source_name) == "string")
      assert(type(change.event.message) == "string")
      assert(type(change.event.severity) == "number")
      assert(type(change.opcua_status) == "number")
      assert(type(change.opcua_status_name) == "string")
      compact_events[#compact_events + 1] = change
    end))
local selected_event_item = assert(client:monitor_event_fields(
    event_subscription_id, method_object, 10, { "Message", "Severity" },
    function(change)
      assert(type(change.subscription_id) == "number")
      assert(type(change.monitored_item_id) == "number")
      assert(type(change.fields) == "table")
      selected_events[#selected_events + 1] = change
    end))
assert(client:set_monitoring_mode(
    event_subscription_id, compact_event_item,
    opcua.MONITORING_REPORTING) == true)
assert(client:set_monitoring_mode(
    event_subscription_id, selected_event_item,
    opcua.MONITORING_REPORTING) == true)
local observed_compact_event = false
local observed_selected_event = false
for _ = 1, 150 do
  local ok, err = client:iterate(20)
  assert(ok == true, err and err.message or "client iterate event subscription")
  for i = 1, #compact_events do
    local compact = compact_events[i]
    if compact.monitored_item_id == compact_event_item and
        compact.event.message == "Fixture periodic event" and
        compact.event.severity == 321 then
      observed_compact_event = true
    end
  end
  for i = 1, #selected_events do
    local selected = selected_events[i]
    if selected.monitored_item_id == selected_event_item then
      for field_index = 1, #selected.fields do
        local field = selected.fields[field_index]
        if field.name == "Message" and
            field.value:type() == opcua.VALUE_LOCALIZED_TEXT and
            field.value:get().text == "Fixture periodic event" then
          observed_selected_event = true
        end
      end
    end
  end
  if observed_compact_event and observed_selected_event then
    break
  end
end
assert(observed_compact_event,
       "client monitor_events callback should observe fixture event")
assert(observed_selected_event,
       "client monitor_event_fields callback should observe fixture event")
assert(client:delete_monitored_item(
    event_subscription_id, compact_event_item) == true)
assert(client:delete_monitored_item(
    event_subscription_id, selected_event_item) == true)
assert(client:delete_subscription(event_subscription_id) == true)

local async_write_result = nil
local async_write_request = assert(client:write_async(
    node, opcua.value_integer(updated_value + 2), function(result)
      assert(type(result.request_id) == "number")
      assert(type(result.result) == "number")
      assert(type(result.result_string) == "string")
      assert(type(result.opcua_status) == "number")
      assert(type(result.opcua_status_name) == "string")
      async_write_result = result
    end))
for _ = 1, 100 do
  local ok, err = client:iterate(20)
  assert(ok == true, err and err.message or "client iterate async write")
  if async_write_result ~= nil then
    break
  end
end
assert(async_write_result ~= nil, "client write_async should complete")
assert(async_write_result.request_id == async_write_request)
assert(async_write_result.ok == true)

local async_read_result = nil
local async_read_request = assert(client:read_async(node, function(result)
  assert(type(result.request_id) == "number")
  assert(type(result.result) == "number")
  assert(type(result.result_string) == "string")
  assert(type(result.opcua_status) == "number")
  assert(type(result.opcua_status_name) == "string")
  assert(result.value:type() == opcua.VALUE_INTEGER)
  async_read_result = result
end))
for _ = 1, 100 do
  local ok, err = client:iterate(20)
  assert(ok == true, err and err.message or "client iterate async read")
  if async_read_result ~= nil then
    break
  end
end
assert(async_read_result ~= nil, "client read_async should complete")
assert(async_read_result.request_id == async_read_request)
assert(async_read_result.ok == true)
assert(async_read_result.value:get() == updated_value + 2)

local async_method_result = nil
local async_method_request = assert(client:call_method_async(
    method_object, remote_method_node, { opcua.value_integer(6) }, 1,
    function(result)
      assert(type(result.request_id) == "number")
      assert(type(result.outputs) == "table")
      async_method_result = result
    end))
for _ = 1, 100 do
  local ok, err = client:iterate(20)
  assert(ok == true, err and err.message or "client iterate async method")
  if async_method_result ~= nil then
    break
  end
end
assert(async_method_result ~= nil, "client call_method_async should complete")
assert(async_method_result.request_id == async_method_request)
assert(async_method_result.ok == true)
assert(async_method_result.outputs[1]:get() == 18)

local async_callback_error_seen = false
assert(client:write_async(node, opcua.value_integer(updated_value + 3),
                          function()
                            error("intentional opcua async failure")
                          end))
for _ = 1, 100 do
  local ok, err = client:iterate(20)
  if ok == nil then
    assert(err.dependency == "opcua")
    assert(err.message:find("intentional opcua async failure", 1, true))
    async_callback_error_seen = true
    break
  end
end
assert(async_callback_error_seen,
       "client async callback error should surface through iterate")
assert(client:write(node, opcua.value_integer(updated_value)) == true)
assert(client:read_method_argument_count(
    remote_method_node, opcua.METHOD_ARGUMENT_INPUT) == 1)
assert(client:read_method_argument_count(
    remote_method_node, opcua.METHOD_ARGUMENT_OUTPUT) == 1)
local client_method_argument, client_method_argument_err =
    client:read_method_argument(
        remote_method_node, opcua.METHOD_ARGUMENT_INPUT, 1)
assert(client_method_argument ~= nil,
       client_method_argument_err and client_method_argument_err.message or
       "client read method input argument")
assert(type(tostring(client_method_argument.data_type)) == "string")
local method_value, method_value_err =
    client:call_method(method_object, remote_method_node,
                       { opcua.value_integer(9) })
assert(method_value ~= nil,
       method_value_err and method_value_err.message or "client call method")
assert(method_value:get() == 27)
local method_values, method_values_err =
    client:call_method_many(method_object, remote_method_node,
                            { opcua.value_integer(4) }, 1)
assert(method_values ~= nil,
       method_values_err and method_values_err.message or
       "client call method many")
assert(method_values[1]:get() == 12)
local client_range, client_range_err =
    client:read_integer_array_range(remote_array, "1:2")
assert(client_range ~= nil,
       client_range_err and client_range_err.message or
       "client read integer array range")
assert(client_range[1] == 2 and client_range[2] == 3)
assert(client:write_index_range(
    remote_array, "1:2", opcua.value_integer_array({ 20, 30 })) == true)
local client_array_items, client_array_items_err =
    client:read_integer_array(remote_array)
assert(client_array_items ~= nil,
       client_array_items_err and client_array_items_err.message or
       "client read integer array")
assert(client_array_items[1] == 1)
assert(client_array_items[2] == 20)
assert(client_array_items[3] == 30)
assert(client_array_items[4] == 4)
assert(client:add_variable_under({
  node_id = remote_boolean_array,
  parent_node_id = remote_object,
  browse_name = "clientBooleans",
  display_name = "Client Booleans",
  value = opcua.value_boolean_array({ true, false, true }),
}) == true, "client add boolean array")
local client_booleans =
    assert(client:read_boolean_array(remote_boolean_array),
           "client read boolean array")
assert(client_booleans[1] == true and client_booleans[2] == false,
       "client boolean array values")
local client_boolean_range, client_boolean_range_err =
    client:read_boolean_array_range(remote_boolean_array, "1:2")
assert(client_boolean_range ~= nil,
       client_boolean_range_err and client_boolean_range_err.message or
       "client read boolean array range")
assert(client_boolean_range[1] == false and client_boolean_range[2] == true,
       "client boolean array range values")
assert(client:add_variable_under({
  node_id = remote_double_array,
  parent_node_id = remote_object,
  browse_name = "clientDoubles",
  display_name = "Client Doubles",
  value = opcua.value_double_array({ 1.5, 2.5, 3.5 }),
}) == true, "client add double array")
local client_doubles =
    assert(client:read_double_array(remote_double_array),
           "client read double array")
assert(client_doubles[2] == 2.5, "client double array values")
local client_double_range, client_double_range_err =
    client:read_double_array_range(remote_double_array, "0:1")
assert(client_double_range ~= nil,
       client_double_range_err and client_double_range_err.message or
       "client read double array range")
assert(client_double_range[1] == 1.5, "client double array range values")
assert(client:add_variable_under({
  node_id = remote_string_array,
  parent_node_id = remote_object,
  browse_name = "clientStrings",
  display_name = "Client Strings",
  value = opcua.value_string_array({ "alpha", "beta", "gamma" }),
}) == true, "client add string array")
local client_strings =
    assert(client:read_string_array(remote_string_array),
           "client read string array")
assert(client_strings[2] == "beta", "client string array values")
local client_string_range, client_string_range_err =
    client:read_string_array_range(remote_string_array, "1:2")
assert(client_string_range ~= nil,
       client_string_range_err and client_string_range_err.message or
       "client read string array range")
assert(client_string_range[2] == "gamma", "client string array range values")
assert(client:add_variable_under({
  node_id = remote_byte_string_array,
  parent_node_id = remote_object,
  browse_name = "clientBytes",
  display_name = "Client Bytes",
  value = opcua.value_byte_string_array({ "aa", "bb", "cc" }),
}) == true, "client add byte string array")
local client_bytes =
    assert(client:read_byte_string_array(remote_byte_string_array),
           "client read byte string array")
assert(client_bytes[3] == "cc", "client byte string array values")
local client_byte_range, client_byte_range_err =
    client:read_byte_string_array_range(remote_byte_string_array, "0:1")
assert(client_byte_range ~= nil,
       client_byte_range_err and client_byte_range_err.message or
       "client read byte string array range")
assert(client_byte_range[2] == "bb", "client byte string array range values")
assert(client:add_variable_under({
  node_id = remote_uint64_array,
  parent_node_id = remote_object,
  browse_name = "clientUint64s",
  display_name = "Client Uint64s",
  value = opcua.value_uint64_array({
    { high32 = 0, low32 = 10 },
    { high32 = 1, low32 = 11 },
  }),
}) == true, "client add uint64 array")
local client_uint64s =
    assert(client:read_uint64_array(remote_uint64_array),
           "client read uint64 array")
assert(client_uint64s[2].high32 == 1 and client_uint64s[2].low32 == 11,
       "client uint64 array values")
local client_uint64_range, client_uint64_range_err =
    client:read_uint64_array_range(remote_uint64_array, "0:1")
assert(client_uint64_range ~= nil,
       client_uint64_range_err and client_uint64_range_err.message or
       "client read uint64 array range")
assert(client_uint64_range[2].low32 == 11, "client uint64 array range values")
assert(client:add_variable_under({
  node_id = remote_datetime_array,
  parent_node_id = remote_object,
  browse_name = "clientDateTimes",
  display_name = "Client DateTimes",
  value = opcua.value_datetime_array({
    { high32 = 2, low32 = 20 },
    { high32 = 3, low32 = 30 },
  }),
}) == true, "client add datetime array")
local client_datetimes =
    assert(client:read_datetime_array(remote_datetime_array),
           "client read datetime array")
assert(client_datetimes[1].high32 == 2 and client_datetimes[2].low32 == 30,
       "client datetime array values")
local client_datetime_range, client_datetime_range_err =
    client:read_datetime_array_range(remote_datetime_array, "0:1")
assert(client_datetime_range ~= nil,
       client_datetime_range_err and client_datetime_range_err.message or
       "client read datetime array range")
assert(client_datetime_range[2].high32 == 3,
       "client datetime array range values")
assert(client:add_variable_under({
  node_id = remote_status_array,
  parent_node_id = remote_object,
  browse_name = "clientStatuses",
  display_name = "Client Statuses",
  value = opcua.value_status_array({ 0, 1, 2 }),
}) == true, "client add status array")
local client_statuses =
    assert(client:read_status_array(remote_status_array),
           "client read status array")
assert(client_statuses[3] == 2, "client status array values")
local client_status_range, client_status_range_err =
    client:read_status_array_range(remote_status_array, "1:2")
assert(client_status_range ~= nil,
       client_status_range_err and client_status_range_err.message or
       "client read status array range")
assert(client_status_range[1] == 1, "client status array range values")
assert(client:add_variable_under({
  node_id = remote_guid_array,
  parent_node_id = remote_object,
  browse_name = "clientGuids",
  display_name = "Client Guids",
  value = opcua.value_guid_array({
    "00112233-4455-6677-8899-aabbccddeeff",
    "11112222-3333-4444-5555-666677778888",
  }),
}) == true, "client add guid array")
local client_guids =
    assert(client:read_guid_array(remote_guid_array),
           "client read guid array")
assert(client_guids[1] == "00112233-4455-6677-8899-aabbccddeeff",
       "client guid array values")
local client_guid_range, client_guid_range_err =
    client:read_guid_array_range(remote_guid_array, "0:1")
assert(client_guid_range ~= nil,
       client_guid_range_err and client_guid_range_err.message or
       "client read guid array range")
assert(client_guid_range[2] == "11112222-3333-4444-5555-666677778888",
       "client guid array range values")
assert(client:add_variable_under({
  node_id = remote_qualified_name_array,
  parent_node_id = remote_object,
  browse_name = "clientQualifiedNames",
  display_name = "Client Qualified Names",
  value = opcua.value_qualified_name_array({
    { namespace_index = 1, name = "first" },
    { namespace_index = 1, name = "second" },
  }),
}) == true, "client add qualified name array")
local client_qualified_names =
    assert(client:read_qualified_name_array(remote_qualified_name_array),
           "client read qualified name array")
assert(client_qualified_names[2].name == "second",
       "client qualified name array values")
local client_qualified_name_range, client_qualified_name_range_err =
    client:read_qualified_name_array_range(remote_qualified_name_array, "0:1")
assert(client_qualified_name_range ~= nil,
       client_qualified_name_range_err and
       client_qualified_name_range_err.message or
       "client read qualified name array range")
assert(client_qualified_name_range[1].name == "first",
       "client qualified name array range values")
assert(client:add_variable_under({
  node_id = remote_localized_text_array,
  parent_node_id = remote_object,
  browse_name = "clientLocalizedTexts",
  display_name = "Client Localized Texts",
  value = opcua.value_localized_text_array({
    { locale = "en-US", text = "First" },
    { locale = "en-US", text = "Second" },
  }),
}) == true, "client add localized text array")
local client_localized_texts =
    assert(client:read_localized_text_array(remote_localized_text_array),
           "client read localized text array")
assert(client_localized_texts[2].text == "Second",
       "client localized text array values")
local client_localized_text_range, client_localized_text_range_err =
    client:read_localized_text_array_range(remote_localized_text_array, "0:1")
assert(client_localized_text_range ~= nil,
       client_localized_text_range_err and
       client_localized_text_range_err.message or
       "client read localized text array range")
assert(client_localized_text_range[1].text == "First",
       "client localized text array range values")
local client_translated, client_translated_err =
    client:translate_browse_path(objects_folder, {
  { namespace_index = 1, name = "clientObject" },
})
assert(client_translated ~= nil,
       client_translated_err and client_translated_err.message or
       "client translate browse path")
assert(client_translated == remote_object)
local client_children, client_children_err =
    client:browse_children(remote_object)
assert(client_children ~= nil,
       client_children_err and client_children_err.message or
       "client browse children")
local found_client_child = false
for i = 1, #client_children do
  if client_children[i].target_node_id == remote_array then
    assert(client_children[i].node_class == opcua.NODE_CLASS_VARIABLE)
    assert(client_children[i].browse_name.namespace_index == 1)
    assert(client_children[i].browse_name.name == "clientArray")
    assert(client_children[i].display_name == "Client Array")
    found_client_child = true
  end
end
assert(found_client_child, "client browse children should return clientArray")
local client_variable_children, client_variable_children_err =
    client:browse_children_ex(remote_object, {
  node_class_mask = opcua.NODE_CLASS_VARIABLE,
  result_mask = opcua.BROWSE_RESULT_ALL,
})
assert(client_variable_children ~= nil,
       client_variable_children_err and
       client_variable_children_err.message or
       "client browse children ex")
assert(#client_variable_children >= 1,
       "client browse children ex should return variables")
local client_page, client_page_err =
    client:browse_children_page(remote_object, { max_references = 1 })
assert(client_page ~= nil,
       client_page_err and client_page_err.message or
       "client browse children page")
assert(type(client_page.entries) == "table")
assert(client_page.entries[1].target_node_id == remote_array)
assert(client:delete_node(remote_array, true) == true)
assert(client:delete_node(remote_boolean_array, true) == true)
assert(client:delete_node(remote_double_array, true) == true)
assert(client:delete_node(remote_string_array, true) == true)
assert(client:delete_node(remote_byte_string_array, true) == true)
assert(client:delete_node(remote_uint64_array, true) == true)
assert(client:delete_node(remote_datetime_array, true) == true)
assert(client:delete_node(remote_status_array, true) == true)
assert(client:delete_node(remote_guid_array, true) == true)
assert(client:delete_node(remote_qualified_name_array, true) == true)
assert(client:delete_node(remote_localized_text_array, true) == true)
assert(client:delete_node(remote_linked_object, true) == true)
assert(client:delete_node(remote_object, true) == true)
assert(client:delete_node(remote_object_type, true) == true)
assert(client:delete_node(remote_variable_type, true) == true)
assert(client:delete_node(remote_data_type, true) == true)
assert(client:delete_node(remote_reference_type, true) == true)
assert(client:delete_node(remote_view, true) == true)

assert(client:disconnect() == true)
assert(client:close() == true)

local manual = assert(opcua.client())
assert(manual:connect(endpoint) == true)
assert(manual:disconnect() == true)
assert(manual:close() == true)

print("lua opcua client example ok")
