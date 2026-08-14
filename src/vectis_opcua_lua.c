#include <cpkt/opcua.h>
#include <vectis/vectis.h>

#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define VECTIS_OPCUA_NODE_ID "opcua.node_id"
#define VECTIS_OPCUA_VALUE "opcua.value"
#define VECTIS_OPCUA_CLIENT "opcua.client"
#define VECTIS_OPCUA_SERVER "opcua.server"

typedef struct vectis_opcua_lua_node_id {
  cpkt_opcua_node_id node_id;
  size_t storage_size;
  unsigned char storage[1];
} vectis_opcua_lua_node_id;

typedef struct vectis_opcua_lua_value {
  cpkt_opcua_value value;
  size_t storage_size;
  unsigned char storage[1];
} vectis_opcua_lua_value;

typedef struct vectis_opcua_lua_client {
  cpkt_opcua_client *client;
} vectis_opcua_lua_client;

typedef struct vectis_opcua_lua_server {
  cpkt_opcua_server *server;
} vectis_opcua_lua_server;

int luaopen_opcua(lua_State *lua);
static int vectis_opcua_lua_connect_client(lua_State *lua,
                                           cpkt_opcua_client *client,
                                           int endpoint_index,
                                           int options_index);

static void vectis_opcua_lua_set_field_string(lua_State *lua,
                                              const char *field,
                                              const char *value) {
  lua_pushstring(lua, value != NULL ? value : "");
  lua_setfield(lua, -2, field);
}

static const char *vectis_opcua_lua_table_string(lua_State *lua, int index,
                                                 const char *field) {
  const char *value;

  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return NULL;
  }
  value = luaL_checkstring(lua, -1);
  lua_pop(lua, 1);
  return value;
}

static int vectis_opcua_lua_table_bool(lua_State *lua, int index,
                                       const char *field, int fallback) {
  int value;

  lua_getfield(lua, index, field);
  value = lua_isnil(lua, -1) ? fallback : lua_toboolean(lua, -1);
  lua_pop(lua, 1);
  return value;
}

static unsigned short vectis_opcua_lua_table_ushort(lua_State *lua, int index,
                                                    const char *field,
                                                    unsigned short fallback) {
  lua_Integer value;

  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return fallback;
  }
  value = luaL_checkinteger(lua, -1);
  lua_pop(lua, 1);
  if (value < 0 || value > 65535) {
    return (unsigned short)luaL_error(lua, "%s must be between 0 and 65535",
                                      field);
  }
  return (unsigned short)value;
}

static unsigned long vectis_opcua_lua_table_ulong(lua_State *lua, int index,
                                                  const char *field,
                                                  unsigned long fallback) {
  lua_Integer value;

  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return fallback;
  }
  value = luaL_checkinteger(lua, -1);
  lua_pop(lua, 1);
  if (value < 0) {
    return (unsigned long)luaL_error(lua, "%s must be non-negative", field);
  }
  return (unsigned long)value;
}

static const char *vectis_opcua_lua_vectis_status_string(
    vectis_status status) {
  switch (status) {
  case VECTIS_ERR_INVALID:
    return "invalid";
  case VECTIS_ERR_NOMEM:
    return "nomem";
  case VECTIS_ERR_STATE:
    return "state";
  default:
    return "unknown";
  }
}

static int vectis_opcua_lua_push_error(lua_State *lua,
                                       cpkt_opcua_result result,
                                       cpkt_opcua_status status,
                                       const char *context) {
  const char *result_string;
  const char *status_name;
  vectis_status vectis_code;

  result_string = cpkt_opcua_result_string(result);
  status_name = cpkt_opcua_status_name(status);
  switch (result) {
  case CPKT_OPCUA_ERR_ARG:
  case CPKT_OPCUA_ERR_TYPE:
  case CPKT_OPCUA_ERR_RANGE:
    vectis_code = VECTIS_ERR_INVALID;
    break;
  case CPKT_OPCUA_ERR_ALLOC:
    vectis_code = VECTIS_ERR_NOMEM;
    break;
  default:
    vectis_code = VECTIS_ERR_STATE;
    break;
  }
  lua_pushnil(lua);
  lua_newtable(lua);
  lua_pushinteger(lua, vectis_code);
  lua_setfield(lua, -2, "status");
  vectis_opcua_lua_set_field_string(lua, "status_string",
                                    vectis_opcua_lua_vectis_status_string(
                                        vectis_code));
  lua_pushinteger(lua, VECTIS_ERROR_SOURCE_CPKT);
  lua_setfield(lua, -2, "source_code");
  vectis_opcua_lua_set_field_string(lua, "source", "cpkt");
  vectis_opcua_lua_set_field_string(lua, "dependency", "opcua");
  lua_pushinteger(lua, (lua_Integer)result);
  lua_setfield(lua, -2, "dependency_code");
  lua_pushinteger(lua, (lua_Integer)result);
  lua_setfield(lua, -2, "result");
  vectis_opcua_lua_set_field_string(lua, "result_string", result_string);
  lua_pushinteger(lua, (lua_Integer)status);
  lua_setfield(lua, -2, "opcua_status");
  vectis_opcua_lua_set_field_string(lua, "opcua_status_name", status_name);
  if (context != NULL && context[0] != '\0') {
    lua_pushfstring(lua, "%s: %s%s%s", context,
                    result_string != NULL ? result_string : "opcua error",
                    status != 0u ? " / " : "",
                    status != 0u && status_name != NULL ? status_name : "");
  } else {
    lua_pushstring(lua, result_string != NULL ? result_string : "opcua error");
  }
  lua_setfield(lua, -2, "message");
  return 2;
}

static unsigned long vectis_opcua_lua_check_ulong(lua_State *lua, int index,
                                                  const char *name) {
  lua_Integer value;

  value = luaL_checkinteger(lua, index);
  if (value < 0) {
    return (unsigned long)luaL_error(lua, "%s must be non-negative", name);
  }
  return (unsigned long)value;
}

static unsigned short vectis_opcua_lua_check_ushort(lua_State *lua, int index,
                                                    const char *name) {
  unsigned long value;

  value = vectis_opcua_lua_check_ulong(lua, index, name);
  if (value > 65535u) {
    return (unsigned short)luaL_error(lua, "%s must be <= 65535", name);
  }
  return (unsigned short)value;
}

static vectis_opcua_lua_node_id *
vectis_opcua_lua_new_node_id(lua_State *lua, size_t storage_size) {
  vectis_opcua_lua_node_id *node;
  size_t alloc_size;

  alloc_size = offsetof(vectis_opcua_lua_node_id, storage);
  if (storage_size > ((size_t)-1) - alloc_size) {
    luaL_error(lua, "opcua node id allocation size overflow");
    return NULL;
  }
  if (storage_size == 0u) {
    storage_size = 1u;
  }
  node = (vectis_opcua_lua_node_id *)lua_newuserdatauv(
      lua, alloc_size + storage_size, 0);
  node->node_id = cpkt_opcua_node_id_null();
  node->storage_size = storage_size;
  memset(node->storage, 0, storage_size);
  luaL_getmetatable(lua, VECTIS_OPCUA_NODE_ID);
  lua_setmetatable(lua, -2);
  return node;
}

static vectis_opcua_lua_node_id *
vectis_opcua_lua_check_node_id(lua_State *lua, int index) {
  return (vectis_opcua_lua_node_id *)luaL_checkudata(lua, index,
                                                     VECTIS_OPCUA_NODE_ID);
}

static cpkt_opcua_node_id vectis_opcua_lua_node_id_at(lua_State *lua,
                                                      int index) {
  if (luaL_testudata(lua, index, VECTIS_OPCUA_NODE_ID) != NULL) {
    return vectis_opcua_lua_check_node_id(lua, index)->node_id;
  }
  (void)luaL_error(lua, "opcua node id expected");
  return cpkt_opcua_node_id_null();
}

static int vectis_opcua_lua_node_id_print_to_lua(lua_State *lua,
                                                 cpkt_opcua_node_id node_id) {
  char stack_buffer[128];
  char *buffer;
  size_t required_size;
  cpkt_opcua_result result;

  buffer = stack_buffer;
  required_size = 0u;
  result = cpkt_opcua_node_id_print(node_id, buffer, sizeof(stack_buffer),
                                    &required_size);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua node id print allocation failed");
    }
    result =
        cpkt_opcua_node_id_print(node_id, buffer, required_size + 1u, NULL);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, 0u, "opcua node_id print");
  }
  lua_pushstring(lua, buffer);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_push_node_id_copy(lua_State *lua,
                                              const cpkt_opcua_node_id *input) {
  vectis_opcua_lua_node_id *node;
  size_t storage_size;

  storage_size = 0u;
  if (input->identifier_type == CPKT_OPCUA_NODE_ID_STRING) {
    storage_size = strlen(input->string) + 1u;
  } else if (input->identifier_type == CPKT_OPCUA_NODE_ID_BYTE_STRING) {
    storage_size = input->byte_string_length;
  }
  node = vectis_opcua_lua_new_node_id(lua, storage_size);
  node->node_id = *input;
  if (input->identifier_type == CPKT_OPCUA_NODE_ID_STRING) {
    memcpy(node->storage, input->string, storage_size);
    node->node_id.string = (const char *)node->storage;
  } else if (input->identifier_type == CPKT_OPCUA_NODE_ID_BYTE_STRING) {
    memcpy(node->storage, input->byte_string, input->byte_string_length);
    node->node_id.byte_string = node->storage;
  }
  return 1;
}

static int vectis_opcua_lua_node_id_tostring(lua_State *lua) {
  vectis_opcua_lua_node_id *node;

  node = vectis_opcua_lua_check_node_id(lua, 1);
  return vectis_opcua_lua_node_id_print_to_lua(lua, node->node_id);
}

static int vectis_opcua_lua_node_id_eq(lua_State *lua) {
  vectis_opcua_lua_node_id *left;
  vectis_opcua_lua_node_id *right;

  left = vectis_opcua_lua_check_node_id(lua, 1);
  right = vectis_opcua_lua_check_node_id(lua, 2);
  lua_pushboolean(lua,
                  cpkt_opcua_node_id_equal(left->node_id, right->node_id));
  return 1;
}

static int vectis_opcua_lua_node_id_type(lua_State *lua) {
  vectis_opcua_lua_node_id *node;

  node = vectis_opcua_lua_check_node_id(lua, 1);
  lua_pushinteger(lua, (lua_Integer)node->node_id.identifier_type);
  return 1;
}

static int vectis_opcua_lua_node_id_namespace(lua_State *lua) {
  vectis_opcua_lua_node_id *node;

  node = vectis_opcua_lua_check_node_id(lua, 1);
  lua_pushinteger(lua, (lua_Integer)node->node_id.namespace_index);
  return 1;
}

static int vectis_opcua_lua_node_id_parse(lua_State *lua) {
  const char *text;
  cpkt_opcua_result result;
  cpkt_opcua_node_id parsed;
  size_t required_size;
  size_t storage_size;
  vectis_opcua_lua_node_id *node;
  char stack_buffer[256];
  char *buffer;

  text = luaL_checkstring(lua, 1);
  buffer = stack_buffer;
  required_size = 0u;
  result = cpkt_opcua_node_id_parse(text, &parsed, buffer, sizeof(stack_buffer),
                                    &required_size);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua node_id parse allocation failed");
    }
    result = cpkt_opcua_node_id_parse(text, &parsed, buffer, required_size + 1u,
                                      NULL);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, 0u, "opcua node_id parse");
  }
  storage_size = 0u;
  if (parsed.identifier_type == CPKT_OPCUA_NODE_ID_STRING) {
    storage_size = strlen(parsed.string) + 1u;
  } else if (parsed.identifier_type == CPKT_OPCUA_NODE_ID_BYTE_STRING) {
    storage_size = parsed.byte_string_length;
  }
  node = vectis_opcua_lua_new_node_id(lua, storage_size);
  if (parsed.identifier_type == CPKT_OPCUA_NODE_ID_STRING) {
    memcpy(node->storage, parsed.string, strlen(parsed.string) + 1u);
    parsed.string = (const char *)node->storage;
  } else if (parsed.identifier_type == CPKT_OPCUA_NODE_ID_BYTE_STRING) {
    memcpy(node->storage, parsed.byte_string, parsed.byte_string_length);
    parsed.byte_string = node->storage;
  }
  node->node_id = parsed;
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_node_id_null(lua_State *lua) {
  vectis_opcua_lua_node_id *node;

  node = vectis_opcua_lua_new_node_id(lua, 0u);
  node->node_id = cpkt_opcua_node_id_null();
  return 1;
}

static int vectis_opcua_lua_node_id_numeric(lua_State *lua) {
  unsigned short namespace_index;
  unsigned long identifier;
  vectis_opcua_lua_node_id *node;

  namespace_index = vectis_opcua_lua_check_ushort(lua, 1, "namespace_index");
  identifier = vectis_opcua_lua_check_ulong(lua, 2, "identifier");
  node = vectis_opcua_lua_new_node_id(lua, 0u);
  node->node_id = cpkt_opcua_node_id_numeric(namespace_index, identifier);
  return 1;
}

static int vectis_opcua_lua_node_id_string(lua_State *lua) {
  unsigned short namespace_index;
  const char *identifier;
  size_t identifier_size;
  vectis_opcua_lua_node_id *node;

  namespace_index = vectis_opcua_lua_check_ushort(lua, 1, "namespace_index");
  identifier = luaL_checklstring(lua, 2, &identifier_size);
  node = vectis_opcua_lua_new_node_id(lua, identifier_size + 1u);
  memcpy(node->storage, identifier, identifier_size);
  node->storage[identifier_size] = '\0';
  node->node_id =
      cpkt_opcua_node_id_string(namespace_index, (const char *)node->storage);
  return 1;
}

static int vectis_opcua_lua_node_id_guid(lua_State *lua) {
  unsigned short namespace_index;
  const char *guid_text;
  unsigned char guid[16];
  cpkt_opcua_result result;
  vectis_opcua_lua_node_id *node;

  namespace_index = vectis_opcua_lua_check_ushort(lua, 1, "namespace_index");
  guid_text = luaL_checkstring(lua, 2);
  result = cpkt_opcua_guid_parse(guid_text, guid);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, 0u, "opcua guid parse");
  }
  node = vectis_opcua_lua_new_node_id(lua, 0u);
  node->node_id = cpkt_opcua_node_id_guid(namespace_index, guid);
  return 1;
}

static int vectis_opcua_lua_node_id_byte_string(lua_State *lua) {
  unsigned short namespace_index;
  const char *identifier;
  size_t identifier_size;
  vectis_opcua_lua_node_id *node;

  namespace_index = vectis_opcua_lua_check_ushort(lua, 1, "namespace_index");
  identifier = luaL_checklstring(lua, 2, &identifier_size);
  node = vectis_opcua_lua_new_node_id(lua, identifier_size);
  memcpy(node->storage, identifier, identifier_size);
  node->node_id = cpkt_opcua_node_id_byte_string(
      namespace_index, node->storage, identifier_size);
  return 1;
}

static vectis_opcua_lua_value *
vectis_opcua_lua_new_value(lua_State *lua, size_t storage_size) {
  vectis_opcua_lua_value *value;
  size_t alloc_size;

  alloc_size = offsetof(vectis_opcua_lua_value, storage);
  if (storage_size > ((size_t)-1) - alloc_size) {
    luaL_error(lua, "opcua value allocation size overflow");
    return NULL;
  }
  if (storage_size == 0u) {
    storage_size = 1u;
  }
  value =
      (vectis_opcua_lua_value *)lua_newuserdatauv(lua, alloc_size + storage_size,
                                                  0);
  cpkt_opcua_value_clear(&value->value);
  value->storage_size = storage_size;
  memset(value->storage, 0, storage_size);
  luaL_getmetatable(lua, VECTIS_OPCUA_VALUE);
  lua_setmetatable(lua, -2);
  return value;
}

static vectis_opcua_lua_value *vectis_opcua_lua_check_value(lua_State *lua,
                                                            int index) {
  return (vectis_opcua_lua_value *)luaL_checkudata(lua, index,
                                                  VECTIS_OPCUA_VALUE);
}

static void vectis_opcua_lua_value_from_lua(lua_State *lua, int index,
                                            cpkt_opcua_value *value_out) {
  size_t string_size;
  const char *string_value;

  if (luaL_testudata(lua, index, VECTIS_OPCUA_VALUE) != NULL) {
    *value_out = vectis_opcua_lua_check_value(lua, index)->value;
    return;
  }
  if (lua_type(lua, index) == LUA_TBOOLEAN) {
    cpkt_opcua_value_boolean(value_out, lua_toboolean(lua, index));
    return;
  }
  if (lua_type(lua, index) == LUA_TNUMBER) {
    if (lua_isinteger(lua, index)) {
      cpkt_opcua_value_integer(value_out, (long)lua_tointeger(lua, index));
    } else {
      cpkt_opcua_value_double(value_out, lua_tonumber(lua, index));
    }
    return;
  }
  if (lua_type(lua, index) == LUA_TSTRING) {
    string_value = lua_tolstring(lua, index, &string_size);
    cpkt_opcua_value_string(value_out, string_value, string_size);
    return;
  }
  (void)luaL_error(lua, "opcua scalar value expected");
}

static int vectis_opcua_lua_push_value_get(lua_State *lua,
                                           const cpkt_opcua_value *value) {
  switch (value->type) {
  case CPKT_OPCUA_VALUE_EMPTY:
    lua_pushnil(lua);
    return 1;
  case CPKT_OPCUA_VALUE_BOOLEAN:
    lua_pushboolean(lua, value->boolean_value);
    return 1;
  case CPKT_OPCUA_VALUE_INTEGER:
    lua_pushinteger(lua, (lua_Integer)value->integer_value);
    return 1;
  case CPKT_OPCUA_VALUE_DOUBLE:
    lua_pushnumber(lua, (lua_Number)value->double_value);
    return 1;
  case CPKT_OPCUA_VALUE_STRING:
    lua_pushlstring(lua, value->string_value, value->string_length);
    return 1;
  case CPKT_OPCUA_VALUE_BYTE_STRING:
    lua_pushlstring(lua, (const char *)value->bytes_value,
                    value->bytes_length);
    return 1;
  case CPKT_OPCUA_VALUE_STATUS:
    lua_pushinteger(lua, (lua_Integer)value->status_value);
    return 1;
  case CPKT_OPCUA_VALUE_UINT64:
    lua_newtable(lua);
    lua_pushinteger(lua, (lua_Integer)value->uint64_value.high32);
    lua_setfield(lua, -2, "high32");
    lua_pushinteger(lua, (lua_Integer)value->uint64_value.low32);
    lua_setfield(lua, -2, "low32");
    return 1;
  case CPKT_OPCUA_VALUE_DATETIME:
    lua_newtable(lua);
    lua_pushinteger(lua, (lua_Integer)value->datetime_value.high32);
    lua_setfield(lua, -2, "high32");
    lua_pushinteger(lua, (lua_Integer)value->datetime_value.low32);
    lua_setfield(lua, -2, "low32");
    return 1;
  case CPKT_OPCUA_VALUE_GUID: {
    char guid_buffer[40];
    size_t required_size;
    cpkt_opcua_result result;

    required_size = 0u;
    result = cpkt_opcua_guid_print(value->guid_value, guid_buffer,
                                   sizeof(guid_buffer), &required_size);
    if (result != CPKT_OPCUA_OK) {
      return vectis_opcua_lua_push_error(lua, result, 0u, "opcua guid print");
    }
    lua_pushstring(lua, guid_buffer);
    return 1;
  }
  case CPKT_OPCUA_VALUE_QUALIFIED_NAME:
    lua_newtable(lua);
    lua_pushinteger(lua,
                    (lua_Integer)value->qualified_name_namespace_index);
    lua_setfield(lua, -2, "namespace_index");
    lua_pushlstring(lua, value->qualified_name, value->qualified_name_length);
    lua_setfield(lua, -2, "name");
    return 1;
  case CPKT_OPCUA_VALUE_LOCALIZED_TEXT:
    lua_newtable(lua);
    lua_pushlstring(lua, value->localized_text_locale,
                    value->localized_text_locale_length);
    lua_setfield(lua, -2, "locale");
    lua_pushlstring(lua, value->localized_text, value->localized_text_length);
    lua_setfield(lua, -2, "text");
    return 1;
  default:
    lua_newtable(lua);
    lua_pushinteger(lua, (lua_Integer)value->type);
    lua_setfield(lua, -2, "type");
    lua_pushliteral(lua, "unsupported Lua conversion for this OPC UA value");
    lua_setfield(lua, -2, "message");
    return 1;
  }
}

static int vectis_opcua_lua_value_type(lua_State *lua) {
  vectis_opcua_lua_value *value;

  value = vectis_opcua_lua_check_value(lua, 1);
  lua_pushinteger(lua, (lua_Integer)value->value.type);
  return 1;
}

static int vectis_opcua_lua_value_get(lua_State *lua) {
  vectis_opcua_lua_value *value;

  value = vectis_opcua_lua_check_value(lua, 1);
  return vectis_opcua_lua_push_value_get(lua, &value->value);
}

static int vectis_opcua_lua_value_tostring(lua_State *lua) {
  vectis_opcua_lua_value *value;

  value = vectis_opcua_lua_check_value(lua, 1);
  lua_pushfstring(lua, "opcua.value(%d)", value->value.type);
  return 1;
}

static int vectis_opcua_lua_value_empty(lua_State *lua) {
  (void)vectis_opcua_lua_new_value(lua, 0u);
  return 1;
}

static int vectis_opcua_lua_value_boolean(lua_State *lua) {
  vectis_opcua_lua_value *value;

  value = vectis_opcua_lua_new_value(lua, 0u);
  cpkt_opcua_value_boolean(&value->value, lua_toboolean(lua, 1));
  return 1;
}

static int vectis_opcua_lua_value_integer(lua_State *lua) {
  vectis_opcua_lua_value *value;

  value = vectis_opcua_lua_new_value(lua, 0u);
  cpkt_opcua_value_integer(&value->value, (long)luaL_checkinteger(lua, 1));
  return 1;
}

static int vectis_opcua_lua_value_double(lua_State *lua) {
  vectis_opcua_lua_value *value;

  value = vectis_opcua_lua_new_value(lua, 0u);
  cpkt_opcua_value_double(&value->value, luaL_checknumber(lua, 1));
  return 1;
}

static int vectis_opcua_lua_value_string(lua_State *lua) {
  const char *string_value;
  size_t string_size;
  vectis_opcua_lua_value *value;

  string_value = luaL_checklstring(lua, 1, &string_size);
  value = vectis_opcua_lua_new_value(lua, string_size + 1u);
  memcpy(value->storage, string_value, string_size);
  value->storage[string_size] = '\0';
  cpkt_opcua_value_string(&value->value, (const char *)value->storage,
                          string_size);
  return 1;
}

static int vectis_opcua_lua_value_byte_string(lua_State *lua) {
  const char *bytes;
  size_t bytes_size;
  vectis_opcua_lua_value *value;

  bytes = luaL_checklstring(lua, 1, &bytes_size);
  value = vectis_opcua_lua_new_value(lua, bytes_size);
  memcpy(value->storage, bytes, bytes_size);
  cpkt_opcua_value_byte_string(&value->value, value->storage, bytes_size);
  return 1;
}

static int vectis_opcua_lua_value_guid(lua_State *lua) {
  const char *guid_text;
  unsigned char guid[16];
  cpkt_opcua_result result;
  vectis_opcua_lua_value *value;

  guid_text = luaL_checkstring(lua, 1);
  result = cpkt_opcua_guid_parse(guid_text, guid);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, 0u, "opcua guid parse");
  }
  value = vectis_opcua_lua_new_value(lua, 0u);
  cpkt_opcua_value_guid(&value->value, guid);
  return 1;
}

static int vectis_opcua_lua_value_status(lua_State *lua) {
  vectis_opcua_lua_value *value;

  value = vectis_opcua_lua_new_value(lua, 0u);
  cpkt_opcua_value_status(
      &value->value, vectis_opcua_lua_check_ulong(lua, 1, "status"));
  return 1;
}

static int vectis_opcua_lua_value_uint64(lua_State *lua) {
  vectis_opcua_lua_value *value;

  value = vectis_opcua_lua_new_value(lua, 0u);
  cpkt_opcua_value_uint64(&value->value,
                          vectis_opcua_lua_check_ulong(lua, 1, "high32"),
                          vectis_opcua_lua_check_ulong(lua, 2, "low32"));
  return 1;
}

static int vectis_opcua_lua_value_datetime(lua_State *lua) {
  vectis_opcua_lua_value *value;

  value = vectis_opcua_lua_new_value(lua, 0u);
  cpkt_opcua_value_datetime(&value->value, (long)luaL_checkinteger(lua, 1),
                            vectis_opcua_lua_check_ulong(lua, 2, "low32"));
  return 1;
}

static int vectis_opcua_lua_value_qualified_name(lua_State *lua) {
  unsigned short namespace_index;
  const char *name;
  size_t name_size;
  vectis_opcua_lua_value *value;

  namespace_index = vectis_opcua_lua_check_ushort(lua, 1, "namespace_index");
  name = luaL_checklstring(lua, 2, &name_size);
  value = vectis_opcua_lua_new_value(lua, name_size + 1u);
  memcpy(value->storage, name, name_size);
  value->storage[name_size] = '\0';
  cpkt_opcua_value_qualified_name(&value->value, namespace_index,
                                  (const char *)value->storage, name_size);
  return 1;
}

static int vectis_opcua_lua_value_localized_text(lua_State *lua) {
  const char *locale;
  const char *text;
  size_t locale_size;
  size_t text_size;
  vectis_opcua_lua_value *value;

  locale = luaL_checklstring(lua, 1, &locale_size);
  text = luaL_checklstring(lua, 2, &text_size);
  value = vectis_opcua_lua_new_value(lua, locale_size + text_size + 2u);
  memcpy(value->storage, locale, locale_size);
  value->storage[locale_size] = '\0';
  memcpy(value->storage + locale_size + 1u, text, text_size);
  value->storage[locale_size + 1u + text_size] = '\0';
  cpkt_opcua_value_localized_text(
      &value->value, (const char *)value->storage, locale_size,
      (const char *)(value->storage + locale_size + 1u), text_size);
  return 1;
}

static int vectis_opcua_lua_push_value_copy(lua_State *lua,
                                            const cpkt_opcua_value *input) {
  vectis_opcua_lua_value *value;
  size_t storage_size;

  storage_size = 0u;
  if (input->type == CPKT_OPCUA_VALUE_STRING) {
    storage_size = input->string_length + 1u;
  } else if (input->type == CPKT_OPCUA_VALUE_BYTE_STRING) {
    storage_size = input->bytes_length;
  } else if (input->type == CPKT_OPCUA_VALUE_QUALIFIED_NAME) {
    storage_size = input->qualified_name_length + 1u;
  } else if (input->type == CPKT_OPCUA_VALUE_LOCALIZED_TEXT) {
    storage_size = input->localized_text_locale_length +
                   input->localized_text_length + 2u;
  }
  value = vectis_opcua_lua_new_value(lua, storage_size);
  value->value = *input;
  if (input->type == CPKT_OPCUA_VALUE_STRING) {
    memcpy(value->storage, input->string_value, input->string_length);
    value->storage[input->string_length] = '\0';
    value->value.string_value = (const char *)value->storage;
  } else if (input->type == CPKT_OPCUA_VALUE_BYTE_STRING) {
    memcpy(value->storage, input->bytes_value, input->bytes_length);
    value->value.bytes_value = value->storage;
  } else if (input->type == CPKT_OPCUA_VALUE_QUALIFIED_NAME) {
    memcpy(value->storage, input->qualified_name,
           input->qualified_name_length);
    value->storage[input->qualified_name_length] = '\0';
    value->value.qualified_name = (const char *)value->storage;
  } else if (input->type == CPKT_OPCUA_VALUE_LOCALIZED_TEXT) {
    memcpy(value->storage, input->localized_text_locale,
           input->localized_text_locale_length);
    value->storage[input->localized_text_locale_length] = '\0';
    memcpy(value->storage + input->localized_text_locale_length + 1u,
           input->localized_text, input->localized_text_length);
    value->storage[input->localized_text_locale_length + 1u +
                   input->localized_text_length] = '\0';
    value->value.localized_text_locale = (const char *)value->storage;
    value->value.localized_text =
        (const char *)(value->storage + input->localized_text_locale_length +
                       1u);
  }
  return 1;
}

static vectis_opcua_lua_client *vectis_opcua_lua_check_client(lua_State *lua,
                                                              int index) {
  return (vectis_opcua_lua_client *)luaL_checkudata(lua, index,
                                                   VECTIS_OPCUA_CLIENT);
}

static cpkt_opcua_client *vectis_opcua_lua_client_handle(lua_State *lua,
                                                         int index) {
  cpkt_opcua_client *client;

  client = vectis_opcua_lua_check_client(lua, index)->client;
  if (client == NULL) {
    (void)luaL_error(lua, "opcua client is closed");
  }
  return client;
}

static int vectis_opcua_lua_client_close(lua_State *lua) {
  vectis_opcua_lua_client *client;

  client = vectis_opcua_lua_check_client(lua, 1);
  if (client->client != NULL) {
    cpkt_opcua_client_free(client->client);
    client->client = NULL;
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_new(lua_State *lua) {
  vectis_opcua_lua_client *client;
  cpkt_opcua_result result;

  client = (vectis_opcua_lua_client *)lua_newuserdatauv(
      lua, sizeof(vectis_opcua_lua_client), 0);
  client->client = NULL;
  luaL_getmetatable(lua, VECTIS_OPCUA_CLIENT);
  lua_setmetatable(lua, -2);
  result = cpkt_opcua_client_new(&client->client);
  if (result != CPKT_OPCUA_OK) {
    client->client = NULL;
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, 0u, "opcua client new");
  }
  return 1;
}

static int vectis_opcua_lua_client_connect(lua_State *lua) {
  cpkt_opcua_client *client;

  client = vectis_opcua_lua_client_handle(lua, 1);
  return vectis_opcua_lua_connect_client(lua, client, 2, 3);
}

static int vectis_opcua_lua_connect_client(lua_State *lua,
                                           cpkt_opcua_client *client,
                                           int endpoint_index,
                                           int options_index) {
  const char *endpoint_url;
  const char *username;
  const char *password;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  endpoint_url = luaL_checkstring(lua, endpoint_index);
  username = NULL;
  password = NULL;
  if (!lua_isnoneornil(lua, options_index)) {
    luaL_checktype(lua, options_index, LUA_TTABLE);
    lua_getfield(lua, options_index, "username");
    username = lua_isnil(lua, -1) ? NULL : luaL_checkstring(lua, -1);
    lua_getfield(lua, options_index, "password");
    password = lua_isnil(lua, -1) ? NULL : luaL_checkstring(lua, -1);
    lua_pop(lua, 2);
  }
  status = 0u;
  if (username != NULL || password != NULL) {
    if (username == NULL || password == NULL) {
      return luaL_error(lua,
                        "opcua connect credentials require username and "
                        "password");
    }
    result = cpkt_opcua_client_connect_username(client, endpoint_url, username,
                                                password, &status);
  } else {
    result = cpkt_opcua_client_connect(client, endpoint_url, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client connect");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_connect(lua_State *lua) {
  vectis_opcua_lua_client *client;
  int result_count;

  (void)vectis_opcua_lua_client_new(lua);
  client = vectis_opcua_lua_check_client(lua, -1);
  lua_insert(lua, 1);
  result_count = vectis_opcua_lua_connect_client(lua, client->client, 2, 3);
  if (result_count != 1 || !lua_toboolean(lua, -1)) {
    cpkt_opcua_client_free(client->client);
    client->client = NULL;
    return 2;
  }
  lua_pop(lua, 1);
  lua_settop(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_disconnect(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  status = 0u;
  result = cpkt_opcua_client_disconnect(client, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client disconnect");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_iterate(lua_State *lua) {
  cpkt_opcua_client *client;
  unsigned long timeout_ms;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  timeout_ms = lua_isnoneornil(lua, 2)
                   ? 0u
                   : vectis_opcua_lua_check_ulong(lua, 2, "timeout_ms");
  status = 0u;
  result = cpkt_opcua_client_run_iterate(client, timeout_ms, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client iterate");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_read(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_value value;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  char stack_buffer[4096];
  char *buffer;
  size_t required_size;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  buffer = stack_buffer;
  required_size = 0u;
  cpkt_opcua_value_clear(&value);
  status = 0u;
  result = cpkt_opcua_client_read(client, node_id, &value, buffer,
                                  sizeof(stack_buffer), &required_size,
                                  &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua client read allocation failed");
    }
    cpkt_opcua_value_clear(&value);
    result = cpkt_opcua_client_read(client, node_id, &value, buffer,
                                    required_size + 1u, NULL, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client read");
  }
  (void)vectis_opcua_lua_push_value_copy(lua, &value);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_client_write(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_value value;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  vectis_opcua_lua_value_from_lua(lua, 3, &value);
  status = 0u;
  result = cpkt_opcua_client_write(client, node_id, &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client write");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_namespace_index(lua_State *lua) {
  cpkt_opcua_client *client;
  const char *namespace_uri;
  unsigned short namespace_index;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  namespace_uri = luaL_checkstring(lua, 2);
  namespace_index = 0u;
  status = 0u;
  result = cpkt_opcua_client_get_namespace_index(
      client, namespace_uri, &namespace_index, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua namespace index");
  }
  lua_pushinteger(lua, (lua_Integer)namespace_index);
  return 1;
}

static int vectis_opcua_lua_client_namespace_uri(lua_State *lua) {
  cpkt_opcua_client *client;
  unsigned short namespace_index;
  char stack_buffer[512];
  char *buffer;
  size_t required_size;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  namespace_index = vectis_opcua_lua_check_ushort(lua, 2, "namespace_index");
  buffer = stack_buffer;
  required_size = 0u;
  status = 0u;
  result = cpkt_opcua_client_get_namespace_uri(
      client, namespace_index, buffer, sizeof(stack_buffer), &required_size,
      &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua namespace uri allocation failed");
    }
    result = cpkt_opcua_client_get_namespace_uri(
        client, namespace_index, buffer, required_size + 1u, NULL, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua namespace uri");
  }
  lua_pushstring(lua, buffer);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static vectis_opcua_lua_server *vectis_opcua_lua_check_server(lua_State *lua,
                                                              int index) {
  return (vectis_opcua_lua_server *)luaL_checkudata(lua, index,
                                                   VECTIS_OPCUA_SERVER);
}

static cpkt_opcua_server *vectis_opcua_lua_server_handle(lua_State *lua,
                                                         int index) {
  cpkt_opcua_server *server;

  server = vectis_opcua_lua_check_server(lua, index)->server;
  if (server == NULL) {
    (void)luaL_error(lua, "opcua server is closed");
  }
  return server;
}

static cpkt_opcua_node_id vectis_opcua_lua_node_id_field(lua_State *lua,
                                                         int index,
                                                         const char *field,
                                                         const char *context) {
  cpkt_opcua_node_id node_id;

  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    (void)luaL_error(lua, "%s requires %s", context, field);
    return cpkt_opcua_node_id_null();
  }
  node_id = vectis_opcua_lua_node_id_at(lua, -1);
  lua_pop(lua, 1);
  return node_id;
}

static void vectis_opcua_lua_value_field(lua_State *lua, int index,
                                         const char *field,
                                         const char *context,
                                         cpkt_opcua_value *value_out) {
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    (void)luaL_error(lua, "%s requires %s", context, field);
    return;
  }
  vectis_opcua_lua_value_from_lua(lua, -1, value_out);
  lua_pop(lua, 1);
}

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_ulong_fn)(
    cpkt_opcua_server *server, cpkt_opcua_node_id node_id,
    unsigned long *value_out, cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_write_ulong_fn)(
    cpkt_opcua_server *server, cpkt_opcua_node_id node_id, unsigned long value,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_long_fn)(
    cpkt_opcua_server *server, cpkt_opcua_node_id node_id, long *value_out,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_write_long_fn)(
    cpkt_opcua_server *server, cpkt_opcua_node_id node_id, long value,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_bool_fn)(
    cpkt_opcua_server *server, cpkt_opcua_node_id node_id, int *value_out,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_write_bool_fn)(
    cpkt_opcua_server *server, cpkt_opcua_node_id node_id, int value,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_double_fn)(
    cpkt_opcua_server *server, cpkt_opcua_node_id node_id, double *value_out,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_write_double_fn)(
    cpkt_opcua_server *server, cpkt_opcua_node_id node_id, double value,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_string_fn)(
    cpkt_opcua_server *server, cpkt_opcua_node_id node_id, char *buffer,
    size_t buffer_size, size_t *required_size_out,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_write_string_fn)(
    cpkt_opcua_server *server, cpkt_opcua_node_id node_id, const char *value,
    cpkt_opcua_status *status_out);

static int vectis_opcua_lua_server_read_ulong_attr(
    lua_State *lua, vectis_opcua_lua_server_read_ulong_fn fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned long value;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = 0u;
  status = 0u;
  result = fn(server, node_id, &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushinteger(lua, (lua_Integer)value);
  return 1;
}

static int vectis_opcua_lua_server_write_ulong_attr(
    lua_State *lua, vectis_opcua_lua_server_write_ulong_fn fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned long value;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = vectis_opcua_lua_check_ulong(lua, 3, "value");
  status = 0u;
  result = fn(server, node_id, value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_read_long_attr(
    lua_State *lua, vectis_opcua_lua_server_read_long_fn fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  long value;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = 0L;
  status = 0u;
  result = fn(server, node_id, &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushinteger(lua, (lua_Integer)value);
  return 1;
}

static int vectis_opcua_lua_server_write_long_attr(
    lua_State *lua, vectis_opcua_lua_server_write_long_fn fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  long value;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = (long)luaL_checkinteger(lua, 3);
  status = 0u;
  result = fn(server, node_id, value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_read_bool_attr(
    lua_State *lua, vectis_opcua_lua_server_read_bool_fn fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int value;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = 0;
  status = 0u;
  result = fn(server, node_id, &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushboolean(lua, value);
  return 1;
}

static int vectis_opcua_lua_server_write_bool_attr(
    lua_State *lua, vectis_opcua_lua_server_write_bool_fn fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int value;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = lua_toboolean(lua, 3);
  status = 0u;
  result = fn(server, node_id, value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_read_double_attr(
    lua_State *lua, vectis_opcua_lua_server_read_double_fn fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  double value;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = 0.0;
  status = 0u;
  result = fn(server, node_id, &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushnumber(lua, (lua_Number)value);
  return 1;
}

static int vectis_opcua_lua_server_write_double_attr(
    lua_State *lua, vectis_opcua_lua_server_write_double_fn fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  double value;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = luaL_checknumber(lua, 3);
  status = 0u;
  result = fn(server, node_id, value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_read_string_attr(
    lua_State *lua, vectis_opcua_lua_server_read_string_fn fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  char stack_buffer[512];
  char *buffer;
  size_t required_size;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  buffer = stack_buffer;
  required_size = 0u;
  status = 0u;
  result = fn(server, node_id, buffer, sizeof(stack_buffer), &required_size,
              &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua string attribute allocation failed");
    }
    result = fn(server, node_id, buffer, required_size + 1u, NULL, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushstring(lua, buffer);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_server_write_string_attr(
    lua_State *lua, vectis_opcua_lua_server_write_string_fn fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  const char *value;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = luaL_checkstring(lua, 3);
  status = 0u;
  result = fn(server, node_id, value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_close(lua_State *lua) {
  vectis_opcua_lua_server *server;

  server = vectis_opcua_lua_check_server(lua, 1);
  if (server->server != NULL) {
    cpkt_opcua_server_free(server->server);
    server->server = NULL;
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_new(lua_State *lua) {
  vectis_opcua_lua_server *server;
  cpkt_opcua_result result;
  cpkt_opcua_status status;
  unsigned short port;
  const char *json;
  const char *path;
  size_t json_size;

  server = (vectis_opcua_lua_server *)lua_newuserdatauv(
      lua, sizeof(vectis_opcua_lua_server), 0);
  server->server = NULL;
  luaL_getmetatable(lua, VECTIS_OPCUA_SERVER);
  lua_setmetatable(lua, -2);

  status = 0u;
  result = CPKT_OPCUA_OK;
  if (lua_istable(lua, 1)) {
    lua_getfield(lua, 1, "json");
    if (!lua_isnil(lua, -1)) {
      json = luaL_checklstring(lua, -1, &json_size);
      result = cpkt_opcua_server_new_from_json(
          &server->server, (const unsigned char *)json, json_size, &status);
      lua_pop(lua, 1);
    } else {
      lua_pop(lua, 1);
      path = vectis_opcua_lua_table_string(lua, 1, "json_path");
      if (path == NULL) {
        path = vectis_opcua_lua_table_string(lua, 1, "config_path");
      }
      if (path != NULL) {
        result =
            cpkt_opcua_server_new_from_json_file(&server->server, path,
                                                 &status);
      } else {
        port = vectis_opcua_lua_table_ushort(lua, 1, "port", 0u);
        result = cpkt_opcua_server_new(&server->server, port);
      }
    }
  } else if (lua_isnoneornil(lua, 1)) {
    result = cpkt_opcua_server_new(&server->server, 0u);
  } else {
    port = vectis_opcua_lua_check_ushort(lua, 1, "port");
    result = cpkt_opcua_server_new(&server->server, port);
  }
  if (result != CPKT_OPCUA_OK) {
    server->server = NULL;
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server new");
  }
  return 1;
}

static int vectis_opcua_lua_server_set_endpoint(lua_State *lua) {
  cpkt_opcua_server *server;
  const char *hostname;
  unsigned short port;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  if (lua_istable(lua, 2)) {
    hostname = vectis_opcua_lua_table_string(lua, 2, "hostname");
    if (hostname == NULL) {
      hostname = vectis_opcua_lua_table_string(lua, 2, "host");
    }
    port = vectis_opcua_lua_table_ushort(lua, 2, "port", 0u);
  } else {
    hostname = luaL_checkstring(lua, 2);
    port = vectis_opcua_lua_check_ushort(lua, 3, "port");
  }
  if (hostname == NULL || hostname[0] == '\0') {
    return luaL_error(lua, "opcua server endpoint hostname is required");
  }
  result = cpkt_opcua_server_set_endpoint(server, hostname, port);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, 0u,
                                       "opcua server set endpoint");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_set_application_identity(lua_State *lua) {
  cpkt_opcua_server *server;
  const char *application_uri;
  const char *product_uri;
  const char *application_name;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  application_uri = vectis_opcua_lua_table_string(lua, 2, "application_uri");
  product_uri = vectis_opcua_lua_table_string(lua, 2, "product_uri");
  application_name =
      vectis_opcua_lua_table_string(lua, 2, "application_name");
  if (application_name == NULL) {
    application_name = vectis_opcua_lua_table_string(lua, 2, "name");
  }
  result = cpkt_opcua_server_set_application_identity(
      server, application_uri, product_uri, application_name);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, 0u,
                                       "opcua server identity");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_set_access_control(lua_State *lua) {
  cpkt_opcua_server *server;
  const char *username;
  const char *password;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int allow_anonymous;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  allow_anonymous =
      vectis_opcua_lua_table_bool(lua, 2, "allow_anonymous", 1);
  username = vectis_opcua_lua_table_string(lua, 2, "username");
  password = vectis_opcua_lua_table_string(lua, 2, "password");
  if ((username == NULL) != (password == NULL)) {
    return luaL_error(lua,
                      "opcua server access control username and password must "
                      "be provided together");
  }
  status = 0u;
  result = cpkt_opcua_server_set_access_control(
      server, allow_anonymous, username, password, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server access control");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_add_namespace(lua_State *lua) {
  cpkt_opcua_server *server;
  const char *namespace_uri;
  unsigned short namespace_index;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  namespace_uri = luaL_checkstring(lua, 2);
  namespace_index = 0u;
  result =
      cpkt_opcua_server_add_namespace(server, namespace_uri, &namespace_index);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, 0u,
                                       "opcua server add namespace");
  }
  lua_pushinteger(lua, (lua_Integer)namespace_index);
  return 1;
}

static int vectis_opcua_lua_server_add_variable(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_value value;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua server add_variable");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  if (browse_name == NULL || browse_name[0] == '\0') {
    return luaL_error(lua, "opcua server add_variable requires browse_name");
  }
  if (display_name == NULL) {
    display_name = browse_name;
  }
  vectis_opcua_lua_value_field(lua, 2, "value",
                               "opcua server add_variable", &value);
  status = 0u;
  result = cpkt_opcua_server_add_variable(server, node_id, browse_name,
                                          display_name, &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server add variable");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_add_object(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua server add_object");
  parent_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "parent_node_id", "opcua server add_object");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  if (browse_name == NULL || browse_name[0] == '\0') {
    return luaL_error(lua, "opcua server add_object requires browse_name");
  }
  if (display_name == NULL) {
    display_name = browse_name;
  }
  status = 0u;
  result = cpkt_opcua_server_add_object(server, node_id, parent_node_id,
                                        browse_name, display_name, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server add object");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_add_variable_under(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  cpkt_opcua_value value;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "node_id", "opcua server add_variable_under");
  parent_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "parent_node_id", "opcua server add_variable_under");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  if (browse_name == NULL || browse_name[0] == '\0') {
    return luaL_error(lua,
                      "opcua server add_variable_under requires browse_name");
  }
  if (display_name == NULL) {
    display_name = browse_name;
  }
  vectis_opcua_lua_value_field(lua, 2, "value",
                               "opcua server add_variable_under", &value);
  status = 0u;
  result = cpkt_opcua_server_add_variable_under(
      server, node_id, parent_node_id, browse_name, display_name, &value,
      &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server add variable under");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_add_object_type(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int is_abstract;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "node_id", "opcua server add_object_type");
  parent_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "parent_node_id", "opcua server add_object_type");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  is_abstract = vectis_opcua_lua_table_bool(lua, 2, "is_abstract", 0);
  if (browse_name == NULL || browse_name[0] == '\0') {
    return luaL_error(lua, "opcua server add_object_type requires browse_name");
  }
  if (display_name == NULL) {
    display_name = browse_name;
  }
  status = 0u;
  result = cpkt_opcua_server_add_object_type(
      server, node_id, parent_node_id, browse_name, display_name, is_abstract,
      &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server add object type");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_add_variable_type(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  cpkt_opcua_value value;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int is_abstract;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "node_id", "opcua server add_variable_type");
  parent_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "parent_node_id", "opcua server add_variable_type");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  is_abstract = vectis_opcua_lua_table_bool(lua, 2, "is_abstract", 0);
  if (browse_name == NULL || browse_name[0] == '\0') {
    return luaL_error(lua,
                      "opcua server add_variable_type requires browse_name");
  }
  if (display_name == NULL) {
    display_name = browse_name;
  }
  vectis_opcua_lua_value_field(lua, 2, "value",
                               "opcua server add_variable_type", &value);
  status = 0u;
  result = cpkt_opcua_server_add_variable_type(
      server, node_id, parent_node_id, browse_name, display_name, &value,
      is_abstract, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server add variable type");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_add_reference_type(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  const char *browse_name;
  const char *display_name;
  const char *inverse_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int is_abstract;
  int symmetric;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "node_id", "opcua server add_reference_type");
  parent_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "parent_node_id", "opcua server add_reference_type");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  inverse_name = vectis_opcua_lua_table_string(lua, 2, "inverse_name");
  is_abstract = vectis_opcua_lua_table_bool(lua, 2, "is_abstract", 0);
  symmetric = vectis_opcua_lua_table_bool(lua, 2, "symmetric", 0);
  if (browse_name == NULL || browse_name[0] == '\0') {
    return luaL_error(lua,
                      "opcua server add_reference_type requires browse_name");
  }
  if (display_name == NULL) {
    display_name = browse_name;
  }
  status = 0u;
  result = cpkt_opcua_server_add_reference_type(
      server, node_id, parent_node_id, browse_name, display_name, inverse_name,
      is_abstract, symmetric, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server add reference type");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_add_data_type(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int is_abstract;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "node_id", "opcua server add_data_type");
  parent_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "parent_node_id", "opcua server add_data_type");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  is_abstract = vectis_opcua_lua_table_bool(lua, 2, "is_abstract", 0);
  if (browse_name == NULL || browse_name[0] == '\0') {
    return luaL_error(lua, "opcua server add_data_type requires browse_name");
  }
  if (display_name == NULL) {
    display_name = browse_name;
  }
  status = 0u;
  result = cpkt_opcua_server_add_data_type(
      server, node_id, parent_node_id, browse_name, display_name, is_abstract,
      &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server add data type");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_add_view(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int contains_no_loops;
  unsigned long event_notifier;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua server add_view");
  parent_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "parent_node_id", "opcua server add_view");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  contains_no_loops =
      vectis_opcua_lua_table_bool(lua, 2, "contains_no_loops", 1);
  event_notifier = vectis_opcua_lua_table_ulong(lua, 2, "event_notifier", 0u);
  if (browse_name == NULL || browse_name[0] == '\0') {
    return luaL_error(lua, "opcua server add_view requires browse_name");
  }
  if (display_name == NULL) {
    display_name = browse_name;
  }
  status = 0u;
  result = cpkt_opcua_server_add_view(
      server, node_id, parent_node_id, browse_name, display_name,
      contains_no_loops, event_notifier, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server add view");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_delete_node(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int delete_target_refs;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  delete_target_refs = lua_isnoneornil(lua, 3) ? 0 : lua_toboolean(lua, 3);
  status = 0u;
  result = cpkt_opcua_server_delete_node(server, node_id, delete_target_refs,
                                         &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server delete node");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_add_reference(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id source_node_id;
  cpkt_opcua_node_id reference_type_id;
  cpkt_opcua_node_id target_node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int is_forward;
  unsigned long target_node_class;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  source_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "source_node_id", "opcua server add_reference");
  reference_type_id = vectis_opcua_lua_node_id_field(
      lua, 2, "reference_type_id", "opcua server add_reference");
  target_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "target_node_id", "opcua server add_reference");
  is_forward = vectis_opcua_lua_table_bool(lua, 2, "is_forward", 1);
  target_node_class = vectis_opcua_lua_table_ulong(
      lua, 2, "target_node_class", CPKT_OPCUA_NODE_CLASS_UNSPECIFIED);
  status = 0u;
  result = cpkt_opcua_server_add_reference(
      server, source_node_id, reference_type_id, is_forward, target_node_id,
      target_node_class, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server add reference");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_delete_reference(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id source_node_id;
  cpkt_opcua_node_id reference_type_id;
  cpkt_opcua_node_id target_node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int is_forward;
  int delete_bidirectional;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  source_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "source_node_id", "opcua server delete_reference");
  reference_type_id = vectis_opcua_lua_node_id_field(
      lua, 2, "reference_type_id", "opcua server delete_reference");
  target_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "target_node_id", "opcua server delete_reference");
  is_forward = vectis_opcua_lua_table_bool(lua, 2, "is_forward", 1);
  delete_bidirectional =
      vectis_opcua_lua_table_bool(lua, 2, "delete_bidirectional", 0);
  status = 0u;
  result = cpkt_opcua_server_delete_reference(
      server, source_node_id, reference_type_id, is_forward, target_node_id,
      delete_bidirectional, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server delete reference");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_read_node_id(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id result_node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  char stack_buffer[256];
  char *buffer;
  size_t required_size;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  buffer = stack_buffer;
  required_size = 0u;
  status = 0u;
  result_node_id = cpkt_opcua_node_id_null();
  result = cpkt_opcua_server_read_node_id(
      server, node_id, &result_node_id, buffer, sizeof(stack_buffer),
      &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua server read node id allocation failed");
    }
    result_node_id = cpkt_opcua_node_id_null();
    result = cpkt_opcua_server_read_node_id(
        server, node_id, &result_node_id, buffer, required_size + 1u, NULL,
        &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server read node id");
  }
  (void)vectis_opcua_lua_push_node_id_copy(lua, &result_node_id);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_server_read_node_class(lua_State *lua) {
  return vectis_opcua_lua_server_read_ulong_attr(
      lua, cpkt_opcua_server_read_node_class,
      "opcua server read node class");
}

static int vectis_opcua_lua_server_read_browse_name(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned short namespace_index;
  char stack_buffer[512];
  char *buffer;
  size_t required_size;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  buffer = stack_buffer;
  required_size = 0u;
  namespace_index = 0u;
  status = 0u;
  result = cpkt_opcua_server_read_browse_name(
      server, node_id, &namespace_index, buffer, sizeof(stack_buffer),
      &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua server browse name allocation failed");
    }
    result = cpkt_opcua_server_read_browse_name(
        server, node_id, &namespace_index, buffer, required_size + 1u, NULL,
        &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server read browse name");
  }
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)namespace_index);
  lua_setfield(lua, -2, "namespace_index");
  lua_pushstring(lua, buffer);
  lua_setfield(lua, -2, "name");
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_server_read_display_name(lua_State *lua) {
  return vectis_opcua_lua_server_read_string_attr(
      lua, cpkt_opcua_server_read_display_name,
      "opcua server read display name");
}

static int vectis_opcua_lua_server_read_description(lua_State *lua) {
  return vectis_opcua_lua_server_read_string_attr(
      lua, cpkt_opcua_server_read_description,
      "opcua server read description");
}

static int vectis_opcua_lua_server_write_display_name(lua_State *lua) {
  return vectis_opcua_lua_server_write_string_attr(
      lua, cpkt_opcua_server_write_display_name,
      "opcua server write display name");
}

static int vectis_opcua_lua_server_write_description(lua_State *lua) {
  return vectis_opcua_lua_server_write_string_attr(
      lua, cpkt_opcua_server_write_description,
      "opcua server write description");
}

static int vectis_opcua_lua_server_read_write_mask(lua_State *lua) {
  return vectis_opcua_lua_server_read_ulong_attr(
      lua, cpkt_opcua_server_read_write_mask,
      "opcua server read write mask");
}

static int vectis_opcua_lua_server_read_user_write_mask(lua_State *lua) {
  return vectis_opcua_lua_server_read_ulong_attr(
      lua, cpkt_opcua_server_read_user_write_mask,
      "opcua server read user write mask");
}

static int vectis_opcua_lua_server_write_write_mask(lua_State *lua) {
  return vectis_opcua_lua_server_write_ulong_attr(
      lua, cpkt_opcua_server_write_write_mask,
      "opcua server write write mask");
}

static int vectis_opcua_lua_server_read_is_abstract(lua_State *lua) {
  return vectis_opcua_lua_server_read_bool_attr(
      lua, cpkt_opcua_server_read_is_abstract,
      "opcua server read is abstract");
}

static int vectis_opcua_lua_server_write_is_abstract(lua_State *lua) {
  return vectis_opcua_lua_server_write_bool_attr(
      lua, cpkt_opcua_server_write_is_abstract,
      "opcua server write is abstract");
}

static int vectis_opcua_lua_server_read_symmetric(lua_State *lua) {
  return vectis_opcua_lua_server_read_bool_attr(
      lua, cpkt_opcua_server_read_symmetric,
      "opcua server read symmetric");
}

static int vectis_opcua_lua_server_write_symmetric(lua_State *lua) {
  return vectis_opcua_lua_server_write_bool_attr(
      lua, cpkt_opcua_server_write_symmetric,
      "opcua server write symmetric");
}

static int vectis_opcua_lua_server_read_inverse_name(lua_State *lua) {
  return vectis_opcua_lua_server_read_string_attr(
      lua, cpkt_opcua_server_read_inverse_name,
      "opcua server read inverse name");
}

static int vectis_opcua_lua_server_write_inverse_name(lua_State *lua) {
  return vectis_opcua_lua_server_write_string_attr(
      lua, cpkt_opcua_server_write_inverse_name,
      "opcua server write inverse name");
}

static int vectis_opcua_lua_server_read_contains_no_loops(lua_State *lua) {
  return vectis_opcua_lua_server_read_bool_attr(
      lua, cpkt_opcua_server_read_contains_no_loops,
      "opcua server read contains no loops");
}

static int vectis_opcua_lua_server_write_contains_no_loops(lua_State *lua) {
  return vectis_opcua_lua_server_write_bool_attr(
      lua, cpkt_opcua_server_write_contains_no_loops,
      "opcua server write contains no loops");
}

static int vectis_opcua_lua_server_read_event_notifier(lua_State *lua) {
  return vectis_opcua_lua_server_read_ulong_attr(
      lua, cpkt_opcua_server_read_event_notifier,
      "opcua server read event notifier");
}

static int vectis_opcua_lua_server_write_event_notifier(lua_State *lua) {
  return vectis_opcua_lua_server_write_ulong_attr(
      lua, cpkt_opcua_server_write_event_notifier,
      "opcua server write event notifier");
}

static int vectis_opcua_lua_server_read_data_type(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id data_type;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  data_type = cpkt_opcua_node_id_null();
  status = 0u;
  result = cpkt_opcua_server_read_data_type(server, node_id, &data_type,
                                            &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server read data type");
  }
  return vectis_opcua_lua_push_node_id_copy(lua, &data_type);
}

static int vectis_opcua_lua_server_write_data_type(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id data_type;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  data_type = vectis_opcua_lua_node_id_at(lua, 3);
  status = 0u;
  result = cpkt_opcua_server_write_data_type(server, node_id, data_type,
                                             &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server write data type");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_read_value_rank(lua_State *lua) {
  return vectis_opcua_lua_server_read_long_attr(
      lua, cpkt_opcua_server_read_value_rank,
      "opcua server read value rank");
}

static int vectis_opcua_lua_server_write_value_rank(lua_State *lua) {
  return vectis_opcua_lua_server_write_long_attr(
      lua, cpkt_opcua_server_write_value_rank,
      "opcua server write value rank");
}

static int vectis_opcua_lua_server_read_access_level(lua_State *lua) {
  return vectis_opcua_lua_server_read_ulong_attr(
      lua, cpkt_opcua_server_read_access_level,
      "opcua server read access level");
}

static int vectis_opcua_lua_server_read_user_access_level(lua_State *lua) {
  return vectis_opcua_lua_server_read_ulong_attr(
      lua, cpkt_opcua_server_read_user_access_level,
      "opcua server read user access level");
}

static int vectis_opcua_lua_server_write_access_level(lua_State *lua) {
  return vectis_opcua_lua_server_write_ulong_attr(
      lua, cpkt_opcua_server_write_access_level,
      "opcua server write access level");
}

static int vectis_opcua_lua_server_read_access_level_ex(lua_State *lua) {
  return vectis_opcua_lua_server_read_ulong_attr(
      lua, cpkt_opcua_server_read_access_level_ex,
      "opcua server read access level ex");
}

static int vectis_opcua_lua_server_write_access_level_ex(lua_State *lua) {
  return vectis_opcua_lua_server_write_ulong_attr(
      lua, cpkt_opcua_server_write_access_level_ex,
      "opcua server write access level ex");
}

static int
vectis_opcua_lua_server_read_minimum_sampling_interval(lua_State *lua) {
  return vectis_opcua_lua_server_read_double_attr(
      lua, cpkt_opcua_server_read_minimum_sampling_interval,
      "opcua server read minimum sampling interval");
}

static int
vectis_opcua_lua_server_write_minimum_sampling_interval(lua_State *lua) {
  return vectis_opcua_lua_server_write_double_attr(
      lua, cpkt_opcua_server_write_minimum_sampling_interval,
      "opcua server write minimum sampling interval");
}

static int vectis_opcua_lua_server_read_historizing(lua_State *lua) {
  return vectis_opcua_lua_server_read_bool_attr(
      lua, cpkt_opcua_server_read_historizing,
      "opcua server read historizing");
}

static int vectis_opcua_lua_server_write_historizing(lua_State *lua) {
  return vectis_opcua_lua_server_write_bool_attr(
      lua, cpkt_opcua_server_write_historizing,
      "opcua server write historizing");
}

static int vectis_opcua_lua_server_read_executable(lua_State *lua) {
  return vectis_opcua_lua_server_read_bool_attr(
      lua, cpkt_opcua_server_read_executable,
      "opcua server read executable");
}

static int vectis_opcua_lua_server_read_user_executable(lua_State *lua) {
  return vectis_opcua_lua_server_read_bool_attr(
      lua, cpkt_opcua_server_read_user_executable,
      "opcua server read user executable");
}

static int vectis_opcua_lua_server_write_executable(lua_State *lua) {
  return vectis_opcua_lua_server_write_bool_attr(
      lua, cpkt_opcua_server_write_executable,
      "opcua server write executable");
}

static int vectis_opcua_lua_server_startup(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  status = 0u;
  result = cpkt_opcua_server_startup(server, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server startup");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_iterate(lua_State *lua) {
  cpkt_opcua_server *server;
  unsigned short wait_ms;
  cpkt_opcua_result result;
  int wait_internal;

  server = vectis_opcua_lua_server_handle(lua, 1);
  wait_internal = lua_isnoneornil(lua, 2) ? 0 : lua_toboolean(lua, 2);
  wait_ms = 0u;
  result = cpkt_opcua_server_iterate(server, wait_internal, &wait_ms);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, 0u,
                                       "opcua server iterate");
  }
  lua_pushinteger(lua, (lua_Integer)wait_ms);
  return 1;
}

static int vectis_opcua_lua_server_shutdown(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  status = 0u;
  result = cpkt_opcua_server_shutdown(server, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server shutdown");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_endpoint_url(lua_State *lua) {
  cpkt_opcua_server *server;
  char stack_buffer[256];
  char *buffer;
  size_t required_size;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  buffer = stack_buffer;
  required_size = 0u;
  result = cpkt_opcua_server_endpoint_url(server, buffer, sizeof(stack_buffer),
                                          &required_size);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua server endpoint url allocation failed");
    }
    result = cpkt_opcua_server_endpoint_url(server, buffer, required_size + 1u,
                                            NULL);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, 0u,
                                       "opcua server endpoint url");
  }
  lua_pushstring(lua, buffer);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_server_read(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_value value;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  char stack_buffer[4096];
  char *buffer;
  size_t required_size;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  buffer = stack_buffer;
  required_size = 0u;
  cpkt_opcua_value_clear(&value);
  status = 0u;
  result = cpkt_opcua_server_read(server, node_id, &value, buffer,
                                  sizeof(stack_buffer), &required_size,
                                  &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua server read allocation failed");
    }
    cpkt_opcua_value_clear(&value);
    result = cpkt_opcua_server_read(server, node_id, &value, buffer,
                                    required_size + 1u, NULL, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server read");
  }
  (void)vectis_opcua_lua_push_value_copy(lua, &value);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_server_write(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_value value;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  vectis_opcua_lua_value_from_lua(lua, 3, &value);
  status = 0u;
  result = cpkt_opcua_server_write(server, node_id, &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server write");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_open62541_version(lua_State *lua) {
  lua_pushstring(lua, cpkt_opcua_open62541_version());
  return 1;
}

static int vectis_opcua_lua_facade_version(lua_State *lua) {
  lua_pushstring(lua, cpkt_opcua_facade_version());
  return 1;
}

static int vectis_opcua_lua_status_name(lua_State *lua) {
  lua_pushstring(lua, cpkt_opcua_status_name(
                          vectis_opcua_lua_check_ulong(lua, 1, "status")));
  return 1;
}

static int vectis_opcua_lua_result_string(lua_State *lua) {
  lua_pushstring(lua, cpkt_opcua_result_string(
                          (cpkt_opcua_result)luaL_checkinteger(lua, 1)));
  return 1;
}

static void vectis_opcua_lua_register_node_id(lua_State *lua) {
  luaL_Reg methods[] = {{"type", vectis_opcua_lua_node_id_type},
                        {"namespace", vectis_opcua_lua_node_id_namespace},
                        {NULL, NULL}};
  luaL_Reg metamethods[] = {{"__tostring", vectis_opcua_lua_node_id_tostring},
                            {"__eq", vectis_opcua_lua_node_id_eq},
                            {NULL, NULL}};

  if (luaL_newmetatable(lua, VECTIS_OPCUA_NODE_ID) != 0) {
    luaL_setfuncs(lua, metamethods, 0);
    lua_newtable(lua);
    luaL_setfuncs(lua, methods, 0);
    lua_setfield(lua, -2, "__index");
  }
  lua_pop(lua, 1);
}

static void vectis_opcua_lua_register_value(lua_State *lua) {
  luaL_Reg methods[] = {{"type", vectis_opcua_lua_value_type},
                        {"get", vectis_opcua_lua_value_get},
                        {NULL, NULL}};
  luaL_Reg metamethods[] = {{"__tostring", vectis_opcua_lua_value_tostring},
                            {NULL, NULL}};

  if (luaL_newmetatable(lua, VECTIS_OPCUA_VALUE) != 0) {
    luaL_setfuncs(lua, metamethods, 0);
    lua_newtable(lua);
    luaL_setfuncs(lua, methods, 0);
    lua_setfield(lua, -2, "__index");
  }
  lua_pop(lua, 1);
}

static void vectis_opcua_lua_register_client(lua_State *lua) {
  luaL_Reg methods[] = {{"connect", vectis_opcua_lua_client_connect},
                        {"disconnect", vectis_opcua_lua_client_disconnect},
                        {"iterate", vectis_opcua_lua_client_iterate},
                        {"read", vectis_opcua_lua_client_read},
                        {"write", vectis_opcua_lua_client_write},
                        {"namespace_index",
                         vectis_opcua_lua_client_namespace_index},
                        {"namespace_uri",
                         vectis_opcua_lua_client_namespace_uri},
                        {"close", vectis_opcua_lua_client_close},
                        {NULL, NULL}};
  luaL_Reg metamethods[] = {{"__gc", vectis_opcua_lua_client_close},
                            {"__close", vectis_opcua_lua_client_close},
                            {NULL, NULL}};

  if (luaL_newmetatable(lua, VECTIS_OPCUA_CLIENT) != 0) {
    luaL_setfuncs(lua, metamethods, 0);
    lua_newtable(lua);
    luaL_setfuncs(lua, methods, 0);
    lua_setfield(lua, -2, "__index");
  }
  lua_pop(lua, 1);
}

static void vectis_opcua_lua_register_server(lua_State *lua) {
  luaL_Reg methods[] = {
      {"set_endpoint", vectis_opcua_lua_server_set_endpoint},
      {"set_application_identity",
       vectis_opcua_lua_server_set_application_identity},
      {"set_access_control", vectis_opcua_lua_server_set_access_control},
      {"add_namespace", vectis_opcua_lua_server_add_namespace},
      {"add_variable", vectis_opcua_lua_server_add_variable},
      {"add_object", vectis_opcua_lua_server_add_object},
      {"add_variable_under", vectis_opcua_lua_server_add_variable_under},
      {"add_object_type", vectis_opcua_lua_server_add_object_type},
      {"add_variable_type", vectis_opcua_lua_server_add_variable_type},
      {"add_reference_type", vectis_opcua_lua_server_add_reference_type},
      {"add_data_type", vectis_opcua_lua_server_add_data_type},
      {"add_view", vectis_opcua_lua_server_add_view},
      {"delete_node", vectis_opcua_lua_server_delete_node},
      {"add_reference", vectis_opcua_lua_server_add_reference},
      {"delete_reference", vectis_opcua_lua_server_delete_reference},
      {"startup", vectis_opcua_lua_server_startup},
      {"iterate", vectis_opcua_lua_server_iterate},
      {"shutdown", vectis_opcua_lua_server_shutdown},
      {"endpoint_url", vectis_opcua_lua_server_endpoint_url},
      {"read", vectis_opcua_lua_server_read},
      {"write", vectis_opcua_lua_server_write},
      {"read_node_id", vectis_opcua_lua_server_read_node_id},
      {"read_node_class", vectis_opcua_lua_server_read_node_class},
      {"read_browse_name", vectis_opcua_lua_server_read_browse_name},
      {"read_display_name", vectis_opcua_lua_server_read_display_name},
      {"read_description", vectis_opcua_lua_server_read_description},
      {"write_display_name", vectis_opcua_lua_server_write_display_name},
      {"write_description", vectis_opcua_lua_server_write_description},
      {"read_write_mask", vectis_opcua_lua_server_read_write_mask},
      {"read_user_write_mask", vectis_opcua_lua_server_read_user_write_mask},
      {"write_write_mask", vectis_opcua_lua_server_write_write_mask},
      {"read_is_abstract", vectis_opcua_lua_server_read_is_abstract},
      {"write_is_abstract", vectis_opcua_lua_server_write_is_abstract},
      {"read_symmetric", vectis_opcua_lua_server_read_symmetric},
      {"write_symmetric", vectis_opcua_lua_server_write_symmetric},
      {"read_inverse_name", vectis_opcua_lua_server_read_inverse_name},
      {"write_inverse_name", vectis_opcua_lua_server_write_inverse_name},
      {"read_contains_no_loops",
       vectis_opcua_lua_server_read_contains_no_loops},
      {"write_contains_no_loops",
       vectis_opcua_lua_server_write_contains_no_loops},
      {"read_event_notifier", vectis_opcua_lua_server_read_event_notifier},
      {"write_event_notifier", vectis_opcua_lua_server_write_event_notifier},
      {"read_data_type", vectis_opcua_lua_server_read_data_type},
      {"write_data_type", vectis_opcua_lua_server_write_data_type},
      {"read_value_rank", vectis_opcua_lua_server_read_value_rank},
      {"write_value_rank", vectis_opcua_lua_server_write_value_rank},
      {"read_access_level", vectis_opcua_lua_server_read_access_level},
      {"read_user_access_level",
       vectis_opcua_lua_server_read_user_access_level},
      {"write_access_level", vectis_opcua_lua_server_write_access_level},
      {"read_access_level_ex", vectis_opcua_lua_server_read_access_level_ex},
      {"write_access_level_ex", vectis_opcua_lua_server_write_access_level_ex},
      {"read_minimum_sampling_interval",
       vectis_opcua_lua_server_read_minimum_sampling_interval},
      {"write_minimum_sampling_interval",
       vectis_opcua_lua_server_write_minimum_sampling_interval},
      {"read_historizing", vectis_opcua_lua_server_read_historizing},
      {"write_historizing", vectis_opcua_lua_server_write_historizing},
      {"read_executable", vectis_opcua_lua_server_read_executable},
      {"read_user_executable", vectis_opcua_lua_server_read_user_executable},
      {"write_executable", vectis_opcua_lua_server_write_executable},
      {"close", vectis_opcua_lua_server_close},
      {NULL, NULL}};
  luaL_Reg metamethods[] = {{"__gc", vectis_opcua_lua_server_close},
                            {"__close", vectis_opcua_lua_server_close},
                            {NULL, NULL}};

  if (luaL_newmetatable(lua, VECTIS_OPCUA_SERVER) != 0) {
    luaL_setfuncs(lua, metamethods, 0);
    lua_newtable(lua);
    luaL_setfuncs(lua, methods, 0);
    lua_setfield(lua, -2, "__index");
  }
  lua_pop(lua, 1);
}

static void vectis_opcua_lua_set_const(lua_State *lua, const char *name,
                                       lua_Integer value) {
  lua_pushinteger(lua, value);
  lua_setfield(lua, -2, name);
}

int luaopen_opcua(lua_State *lua) {
  luaL_Reg functions[] = {
      {"open62541_version", vectis_opcua_lua_open62541_version},
      {"facade_version", vectis_opcua_lua_facade_version},
      {"status_name", vectis_opcua_lua_status_name},
      {"result_string", vectis_opcua_lua_result_string},
      {"client", vectis_opcua_lua_client_new},
      {"connect", vectis_opcua_lua_connect},
      {"server", vectis_opcua_lua_server_new},
      {"node_id", vectis_opcua_lua_node_id_parse},
      {"node_id_parse", vectis_opcua_lua_node_id_parse},
      {"node_id_null", vectis_opcua_lua_node_id_null},
      {"node_id_numeric", vectis_opcua_lua_node_id_numeric},
      {"node_id_string", vectis_opcua_lua_node_id_string},
      {"node_id_guid", vectis_opcua_lua_node_id_guid},
      {"node_id_byte_string", vectis_opcua_lua_node_id_byte_string},
      {"value_empty", vectis_opcua_lua_value_empty},
      {"value_boolean", vectis_opcua_lua_value_boolean},
      {"value_integer", vectis_opcua_lua_value_integer},
      {"value_double", vectis_opcua_lua_value_double},
      {"value_string", vectis_opcua_lua_value_string},
      {"value_byte_string", vectis_opcua_lua_value_byte_string},
      {"value_guid", vectis_opcua_lua_value_guid},
      {"value_status", vectis_opcua_lua_value_status},
      {"value_uint64", vectis_opcua_lua_value_uint64},
      {"value_datetime", vectis_opcua_lua_value_datetime},
      {"value_qualified_name", vectis_opcua_lua_value_qualified_name},
      {"value_localized_text", vectis_opcua_lua_value_localized_text},
      {NULL, NULL}};

  vectis_opcua_lua_register_node_id(lua);
  vectis_opcua_lua_register_value(lua);
  vectis_opcua_lua_register_client(lua);
  vectis_opcua_lua_register_server(lua);
  luaL_newlib(lua, functions);

  vectis_opcua_lua_set_const(lua, "OK", CPKT_OPCUA_OK);
  vectis_opcua_lua_set_const(lua, "ERR_ARG", CPKT_OPCUA_ERR_ARG);
  vectis_opcua_lua_set_const(lua, "ERR_ALLOC", CPKT_OPCUA_ERR_ALLOC);
  vectis_opcua_lua_set_const(lua, "ERR_UPSTREAM", CPKT_OPCUA_ERR_UPSTREAM);
  vectis_opcua_lua_set_const(lua, "ERR_TYPE", CPKT_OPCUA_ERR_TYPE);
  vectis_opcua_lua_set_const(lua, "ERR_RANGE", CPKT_OPCUA_ERR_RANGE);
  vectis_opcua_lua_set_const(lua, "ERR_CALLBACK", CPKT_OPCUA_ERR_CALLBACK);

  vectis_opcua_lua_set_const(lua, "NODE_ID_NULL",
                             CPKT_OPCUA_NODE_ID_NULL);
  vectis_opcua_lua_set_const(lua, "NODE_ID_NUMERIC",
                             CPKT_OPCUA_NODE_ID_NUMERIC);
  vectis_opcua_lua_set_const(lua, "NODE_ID_STRING",
                             CPKT_OPCUA_NODE_ID_STRING);
  vectis_opcua_lua_set_const(lua, "NODE_ID_GUID",
                             CPKT_OPCUA_NODE_ID_GUID);
  vectis_opcua_lua_set_const(lua, "NODE_ID_BYTE_STRING",
                             CPKT_OPCUA_NODE_ID_BYTE_STRING);

  vectis_opcua_lua_set_const(lua, "VALUE_EMPTY", CPKT_OPCUA_VALUE_EMPTY);
  vectis_opcua_lua_set_const(lua, "VALUE_BOOLEAN", CPKT_OPCUA_VALUE_BOOLEAN);
  vectis_opcua_lua_set_const(lua, "VALUE_INTEGER", CPKT_OPCUA_VALUE_INTEGER);
  vectis_opcua_lua_set_const(lua, "VALUE_DOUBLE", CPKT_OPCUA_VALUE_DOUBLE);
  vectis_opcua_lua_set_const(lua, "VALUE_STRING", CPKT_OPCUA_VALUE_STRING);
  vectis_opcua_lua_set_const(lua, "VALUE_BYTE_STRING",
                             CPKT_OPCUA_VALUE_BYTE_STRING);
  vectis_opcua_lua_set_const(lua, "VALUE_GUID", CPKT_OPCUA_VALUE_GUID);
  vectis_opcua_lua_set_const(lua, "VALUE_STATUS", CPKT_OPCUA_VALUE_STATUS);
  vectis_opcua_lua_set_const(lua, "VALUE_QUALIFIED_NAME",
                             CPKT_OPCUA_VALUE_QUALIFIED_NAME);
  vectis_opcua_lua_set_const(lua, "VALUE_LOCALIZED_TEXT",
                             CPKT_OPCUA_VALUE_LOCALIZED_TEXT);
  vectis_opcua_lua_set_const(lua, "VALUE_UINT64", CPKT_OPCUA_VALUE_UINT64);
  vectis_opcua_lua_set_const(lua, "VALUE_DATETIME",
                             CPKT_OPCUA_VALUE_DATETIME);

  vectis_opcua_lua_set_const(lua, "STANDARD_DATA_TYPE_BOOLEAN",
                             CPKT_OPCUA_STANDARD_DATA_TYPE_BOOLEAN);
  vectis_opcua_lua_set_const(lua, "STANDARD_DATA_TYPE_INTEGER",
                             CPKT_OPCUA_STANDARD_DATA_TYPE_INTEGER);
  vectis_opcua_lua_set_const(lua, "STANDARD_DATA_TYPE_UINT64",
                             CPKT_OPCUA_STANDARD_DATA_TYPE_UINT64);
  vectis_opcua_lua_set_const(lua, "STANDARD_DATA_TYPE_DOUBLE",
                             CPKT_OPCUA_STANDARD_DATA_TYPE_DOUBLE);
  vectis_opcua_lua_set_const(lua, "STANDARD_DATA_TYPE_DATETIME",
                             CPKT_OPCUA_STANDARD_DATA_TYPE_DATETIME);
  vectis_opcua_lua_set_const(lua, "STANDARD_DATA_TYPE_STRING",
                             CPKT_OPCUA_STANDARD_DATA_TYPE_STRING);
  vectis_opcua_lua_set_const(lua, "STANDARD_DATA_TYPE_BYTE_STRING",
                             CPKT_OPCUA_STANDARD_DATA_TYPE_BYTE_STRING);
  vectis_opcua_lua_set_const(lua, "STANDARD_DATA_TYPE_GUID",
                             CPKT_OPCUA_STANDARD_DATA_TYPE_GUID);
  vectis_opcua_lua_set_const(lua, "STANDARD_DATA_TYPE_STATUS",
                             CPKT_OPCUA_STANDARD_DATA_TYPE_STATUS);
  vectis_opcua_lua_set_const(lua, "STANDARD_DATA_TYPE_QUALIFIED_NAME",
                             CPKT_OPCUA_STANDARD_DATA_TYPE_QUALIFIED_NAME);
  vectis_opcua_lua_set_const(lua, "STANDARD_DATA_TYPE_LOCALIZED_TEXT",
                             CPKT_OPCUA_STANDARD_DATA_TYPE_LOCALIZED_TEXT);

  vectis_opcua_lua_set_const(lua, "NODE_CLASS_UNSPECIFIED",
                             CPKT_OPCUA_NODE_CLASS_UNSPECIFIED);
  vectis_opcua_lua_set_const(lua, "NODE_CLASS_OBJECT",
                             CPKT_OPCUA_NODE_CLASS_OBJECT);
  vectis_opcua_lua_set_const(lua, "NODE_CLASS_VARIABLE",
                             CPKT_OPCUA_NODE_CLASS_VARIABLE);
  vectis_opcua_lua_set_const(lua, "NODE_CLASS_METHOD",
                             CPKT_OPCUA_NODE_CLASS_METHOD);
  vectis_opcua_lua_set_const(lua, "NODE_CLASS_OBJECT_TYPE",
                             CPKT_OPCUA_NODE_CLASS_OBJECT_TYPE);
  vectis_opcua_lua_set_const(lua, "NODE_CLASS_VARIABLE_TYPE",
                             CPKT_OPCUA_NODE_CLASS_VARIABLE_TYPE);
  vectis_opcua_lua_set_const(lua, "NODE_CLASS_REFERENCE_TYPE",
                             CPKT_OPCUA_NODE_CLASS_REFERENCE_TYPE);
  vectis_opcua_lua_set_const(lua, "NODE_CLASS_DATA_TYPE",
                             CPKT_OPCUA_NODE_CLASS_DATA_TYPE);
  vectis_opcua_lua_set_const(lua, "NODE_CLASS_VIEW",
                             CPKT_OPCUA_NODE_CLASS_VIEW);

  vectis_opcua_lua_set_const(lua, "NODE_OBJECTS_FOLDER", 85);
  vectis_opcua_lua_set_const(lua, "NODE_VIEWS_FOLDER", 87);
  vectis_opcua_lua_set_const(lua, "NODE_BASE_OBJECT_TYPE", 58);
  vectis_opcua_lua_set_const(lua, "NODE_BASE_DATA_VARIABLE_TYPE", 63);
  vectis_opcua_lua_set_const(lua, "NODE_BASE_DATA_TYPE", 24);
  vectis_opcua_lua_set_const(lua, "NODE_REFERENCES", 31);
  vectis_opcua_lua_set_const(lua, "REFERENCE_ORGANIZES", 35);
  vectis_opcua_lua_set_const(lua, "REFERENCE_HAS_COMPONENT", 47);

  return 1;
}
