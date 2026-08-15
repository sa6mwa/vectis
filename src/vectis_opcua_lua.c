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
#define VECTIS_OPCUA_EVENT "opcua.event"

typedef union vectis_opcua_lua_value_storage_align {
  void *pointer_value;
  double double_value;
  long long_value;
  cpkt_opcua_uint64 uint64_value;
  cpkt_opcua_datetime datetime_value;
  cpkt_opcua_status status_value;
  cpkt_opcua_guid guid_value;
  cpkt_opcua_string_view string_view;
  cpkt_opcua_byte_string_view byte_string_view;
  cpkt_opcua_qualified_name_view qualified_name_view;
  cpkt_opcua_localized_text_view localized_text_view;
} vectis_opcua_lua_value_storage_align;

typedef struct vectis_opcua_lua_node_id {
  cpkt_opcua_node_id node_id;
  size_t storage_size;
  unsigned char storage[1];
} vectis_opcua_lua_node_id;

typedef struct vectis_opcua_lua_value {
  cpkt_opcua_value value;
  size_t storage_size;
  vectis_opcua_lua_value_storage_align align;
  unsigned char storage[1];
} vectis_opcua_lua_value;

typedef struct vectis_opcua_lua_client {
  cpkt_opcua_client *client;
} vectis_opcua_lua_client;

typedef struct vectis_opcua_lua_method_output_slot {
  unsigned char *storage;
  size_t storage_size;
} vectis_opcua_lua_method_output_slot;

typedef struct vectis_opcua_lua_method_callback {
  lua_State *owner;
  int callback_ref;
  size_t output_count;
  vectis_opcua_lua_method_output_slot *outputs;
  struct vectis_opcua_lua_method_callback *next;
} vectis_opcua_lua_method_callback;

typedef struct vectis_opcua_lua_server {
  cpkt_opcua_server *server;
  lua_State *owner;
  vectis_opcua_lua_method_callback *methods;
} vectis_opcua_lua_server;

typedef struct vectis_opcua_lua_event {
  cpkt_opcua_server_event *event;
} vectis_opcua_lua_event;

typedef struct vectis_opcua_lua_browse_collect {
  lua_State *lua;
  int table_index;
  size_t count;
} vectis_opcua_lua_browse_collect;

int luaopen_opcua(lua_State *lua);
static int vectis_opcua_lua_connect_client(lua_State *lua,
                                           cpkt_opcua_client *client,
                                           int endpoint_index,
                                           int options_index);

static void vectis_opcua_lua_set_field_string(lua_State *lua, const char *field,
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

static int vectis_opcua_lua_table_int(lua_State *lua, int index,
                                      const char *field, int fallback) {
  lua_Integer value;

  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return fallback;
  }
  value = luaL_checkinteger(lua, -1);
  lua_pop(lua, 1);
  if (value < INT_MIN || value > INT_MAX) {
    return (int)luaL_error(lua, "%s must fit in an int", field);
  }
  return (int)value;
}

static const char *vectis_opcua_lua_vectis_status_string(vectis_status status) {
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

static int vectis_opcua_lua_push_error(lua_State *lua, cpkt_opcua_result result,
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
  vectis_opcua_lua_set_field_string(
      lua, "status_string", vectis_opcua_lua_vectis_status_string(vectis_code));
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

static size_t vectis_opcua_lua_checked_size_add(lua_State *lua, size_t left,
                                                size_t right,
                                                const char *context) {
  if (right > ((size_t)-1) - left) {
    luaL_error(lua, "%s size overflow", context);
    return 0u;
  }
  return left + right;
}

static size_t vectis_opcua_lua_checked_size_mul(lua_State *lua, size_t left,
                                                size_t right,
                                                const char *context) {
  if (left != 0u && right > ((size_t)-1) / left) {
    luaL_error(lua, "%s size overflow", context);
    return 0u;
  }
  return left * right;
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

static vectis_opcua_lua_node_id *vectis_opcua_lua_check_node_id(lua_State *lua,
                                                                int index) {
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
  lua_pushboolean(lua, cpkt_opcua_node_id_equal(left->node_id, right->node_id));
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
  node->node_id = cpkt_opcua_node_id_byte_string(namespace_index, node->storage,
                                                 identifier_size);
  return 1;
}

static vectis_opcua_lua_value *vectis_opcua_lua_new_value(lua_State *lua,
                                                          size_t storage_size) {
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
  value = (vectis_opcua_lua_value *)lua_newuserdatauv(
      lua, alloc_size + storage_size, 0);
  cpkt_opcua_value_clear(&value->value);
  value->storage_size = storage_size;
  memset(value->storage, 0, storage_size);
  luaL_getmetatable(lua, VECTIS_OPCUA_VALUE);
  lua_setmetatable(lua, -2);
  return value;
}

static size_t vectis_opcua_lua_array_len(lua_State *lua, int index,
                                         const char *name) {
  lua_Unsigned length;

  luaL_checktype(lua, index, LUA_TTABLE);
  length = (lua_Unsigned)lua_rawlen(lua, index);
  if ((size_t)length != length) {
    (void)luaL_error(lua, "%s length overflow", name);
  }
  return (size_t)length;
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

static int vectis_opcua_lua_try_value_from_lua(lua_State *lua, int index,
                                               cpkt_opcua_value *value_out) {
  size_t string_size;
  const char *string_value;

  if (luaL_testudata(lua, index, VECTIS_OPCUA_VALUE) != NULL) {
    *value_out = vectis_opcua_lua_check_value(lua, index)->value;
    return 1;
  }
  if (lua_type(lua, index) == LUA_TBOOLEAN) {
    cpkt_opcua_value_boolean(value_out, lua_toboolean(lua, index));
    return 1;
  }
  if (lua_type(lua, index) == LUA_TNUMBER) {
    if (lua_isinteger(lua, index)) {
      cpkt_opcua_value_integer(value_out, (long)lua_tointeger(lua, index));
    } else {
      cpkt_opcua_value_double(value_out, lua_tonumber(lua, index));
    }
    return 1;
  }
  if (lua_type(lua, index) == LUA_TSTRING) {
    string_value = lua_tolstring(lua, index, &string_size);
    cpkt_opcua_value_string(value_out, string_value, string_size);
    return 1;
  }
  return 0;
}

static cpkt_opcua_result vectis_opcua_lua_method_output_slot_reserve(
    vectis_opcua_lua_method_output_slot *slot, size_t size) {
  unsigned char *storage;

  if (size == 0u) {
    size = 1u;
  }
  if (slot->storage_size >= size) {
    return CPKT_OPCUA_OK;
  }
  storage = (unsigned char *)realloc(slot->storage, size);
  if (storage == NULL) {
    return CPKT_OPCUA_ERR_ALLOC;
  }
  slot->storage = storage;
  slot->storage_size = size;
  return CPKT_OPCUA_OK;
}

static cpkt_opcua_result
vectis_opcua_lua_copy_method_output(lua_State *lua, int index,
                                    cpkt_opcua_value *output,
                                    vectis_opcua_lua_method_output_slot *slot) {
  cpkt_opcua_value value;
  cpkt_opcua_result result;
  size_t storage_size;

  cpkt_opcua_value_clear(&value);
  if (!vectis_opcua_lua_try_value_from_lua(lua, index, &value)) {
    return CPKT_OPCUA_ERR_TYPE;
  }
  switch (value.type) {
  case CPKT_OPCUA_VALUE_EMPTY:
  case CPKT_OPCUA_VALUE_BOOLEAN:
  case CPKT_OPCUA_VALUE_INTEGER:
  case CPKT_OPCUA_VALUE_DOUBLE:
  case CPKT_OPCUA_VALUE_GUID:
  case CPKT_OPCUA_VALUE_STATUS:
  case CPKT_OPCUA_VALUE_UINT64:
  case CPKT_OPCUA_VALUE_DATETIME:
    *output = value;
    return CPKT_OPCUA_OK;
  case CPKT_OPCUA_VALUE_STRING:
    storage_size = value.string_length + 1u;
    result = vectis_opcua_lua_method_output_slot_reserve(slot, storage_size);
    if (result != CPKT_OPCUA_OK) {
      return result;
    }
    memcpy(slot->storage, value.string_value, value.string_length);
    slot->storage[value.string_length] = '\0';
    *output = value;
    output->string_value = (const char *)slot->storage;
    return CPKT_OPCUA_OK;
  case CPKT_OPCUA_VALUE_BYTE_STRING:
    storage_size = value.bytes_length;
    result = vectis_opcua_lua_method_output_slot_reserve(slot, storage_size);
    if (result != CPKT_OPCUA_OK) {
      return result;
    }
    memcpy(slot->storage, value.bytes_value, value.bytes_length);
    *output = value;
    output->bytes_value = slot->storage;
    return CPKT_OPCUA_OK;
  case CPKT_OPCUA_VALUE_QUALIFIED_NAME:
    storage_size = value.qualified_name_length + 1u;
    result = vectis_opcua_lua_method_output_slot_reserve(slot, storage_size);
    if (result != CPKT_OPCUA_OK) {
      return result;
    }
    memcpy(slot->storage, value.qualified_name, value.qualified_name_length);
    slot->storage[value.qualified_name_length] = '\0';
    *output = value;
    output->qualified_name = (const char *)slot->storage;
    return CPKT_OPCUA_OK;
  case CPKT_OPCUA_VALUE_LOCALIZED_TEXT:
    storage_size =
        value.localized_text_locale_length + value.localized_text_length + 2u;
    result = vectis_opcua_lua_method_output_slot_reserve(slot, storage_size);
    if (result != CPKT_OPCUA_OK) {
      return result;
    }
    memcpy(slot->storage, value.localized_text_locale,
           value.localized_text_locale_length);
    slot->storage[value.localized_text_locale_length] = '\0';
    memcpy(slot->storage + value.localized_text_locale_length + 1u,
           value.localized_text, value.localized_text_length);
    slot->storage[value.localized_text_locale_length + 1u +
                  value.localized_text_length] = '\0';
    *output = value;
    output->localized_text_locale = (const char *)slot->storage;
    output->localized_text =
        (const char *)(slot->storage + value.localized_text_locale_length + 1u);
    return CPKT_OPCUA_OK;
  default:
    return CPKT_OPCUA_ERR_TYPE;
  }
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
    lua_pushlstring(lua, (const char *)value->bytes_value, value->bytes_length);
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
    lua_pushinteger(lua, (lua_Integer)value->qualified_name_namespace_index);
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
  case CPKT_OPCUA_VALUE_BOOLEAN_ARRAY: {
    size_t i;

    lua_newtable(lua);
    for (i = 0u; i < value->boolean_array_length; ++i) {
      lua_pushboolean(lua, value->boolean_array_values[i]);
      lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
  }
  case CPKT_OPCUA_VALUE_INTEGER_ARRAY: {
    size_t i;

    lua_newtable(lua);
    for (i = 0u; i < value->integer_array_length; ++i) {
      lua_pushinteger(lua, (lua_Integer)value->integer_array_values[i]);
      lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
  }
  case CPKT_OPCUA_VALUE_DOUBLE_ARRAY: {
    size_t i;

    lua_newtable(lua);
    for (i = 0u; i < value->double_array_length; ++i) {
      lua_pushnumber(lua, (lua_Number)value->double_array_values[i]);
      lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
  }
  case CPKT_OPCUA_VALUE_STRING_ARRAY: {
    size_t i;

    lua_newtable(lua);
    for (i = 0u; i < value->string_array_length; ++i) {
      lua_pushlstring(lua, value->string_array_values[i].data,
                      value->string_array_values[i].length);
      lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
  }
  case CPKT_OPCUA_VALUE_BYTE_STRING_ARRAY: {
    size_t i;

    lua_newtable(lua);
    for (i = 0u; i < value->byte_string_array_length; ++i) {
      lua_pushlstring(lua,
                      (const char *)value->byte_string_array_values[i].data,
                      value->byte_string_array_values[i].length);
      lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
  }
  case CPKT_OPCUA_VALUE_UINT64_ARRAY: {
    size_t i;

    lua_newtable(lua);
    for (i = 0u; i < value->uint64_array_length; ++i) {
      lua_newtable(lua);
      lua_pushinteger(lua, (lua_Integer)value->uint64_array_values[i].high32);
      lua_setfield(lua, -2, "high32");
      lua_pushinteger(lua, (lua_Integer)value->uint64_array_values[i].low32);
      lua_setfield(lua, -2, "low32");
      lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
  }
  case CPKT_OPCUA_VALUE_DATETIME_ARRAY: {
    size_t i;

    lua_newtable(lua);
    for (i = 0u; i < value->datetime_array_length; ++i) {
      lua_newtable(lua);
      lua_pushinteger(lua, (lua_Integer)value->datetime_array_values[i].high32);
      lua_setfield(lua, -2, "high32");
      lua_pushinteger(lua, (lua_Integer)value->datetime_array_values[i].low32);
      lua_setfield(lua, -2, "low32");
      lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
  }
  case CPKT_OPCUA_VALUE_STATUS_ARRAY: {
    size_t i;

    lua_newtable(lua);
    for (i = 0u; i < value->status_array_length; ++i) {
      lua_pushinteger(lua, (lua_Integer)value->status_array_values[i]);
      lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
  }
  case CPKT_OPCUA_VALUE_GUID_ARRAY: {
    char guid_buffer[40];
    size_t required_size;
    cpkt_opcua_result result;
    size_t i;

    lua_newtable(lua);
    for (i = 0u; i < value->guid_array_length; ++i) {
      required_size = 0u;
      result =
          cpkt_opcua_guid_print(value->guid_array_values[i].bytes, guid_buffer,
                                sizeof(guid_buffer), &required_size);
      if (result != CPKT_OPCUA_OK) {
        lua_pop(lua, 1);
        return vectis_opcua_lua_push_error(lua, result, 0u,
                                           "opcua guid array print");
      }
      lua_pushstring(lua, guid_buffer);
      lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
  }
  case CPKT_OPCUA_VALUE_QUALIFIED_NAME_ARRAY: {
    size_t i;

    lua_newtable(lua);
    for (i = 0u; i < value->qualified_name_array_length; ++i) {
      lua_newtable(lua);
      lua_pushinteger(
          lua,
          (lua_Integer)value->qualified_name_array_values[i].namespace_index);
      lua_setfield(lua, -2, "namespace_index");
      lua_pushlstring(lua, value->qualified_name_array_values[i].name,
                      value->qualified_name_array_values[i].name_length);
      lua_setfield(lua, -2, "name");
      lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
  }
  case CPKT_OPCUA_VALUE_LOCALIZED_TEXT_ARRAY: {
    size_t i;

    lua_newtable(lua);
    for (i = 0u; i < value->localized_text_array_length; ++i) {
      lua_newtable(lua);
      lua_pushlstring(lua, value->localized_text_array_values[i].locale,
                      value->localized_text_array_values[i].locale_length);
      lua_setfield(lua, -2, "locale");
      lua_pushlstring(lua, value->localized_text_array_values[i].text,
                      value->localized_text_array_values[i].text_length);
      lua_setfield(lua, -2, "text");
      lua_rawseti(lua, -2, (lua_Integer)i + 1);
    }
    return 1;
  }
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
  cpkt_opcua_value_status(&value->value,
                          vectis_opcua_lua_check_ulong(lua, 1, "status"));
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

static int vectis_opcua_lua_value_boolean_array(lua_State *lua) {
  vectis_opcua_lua_value *value;
  int *values;
  size_t count;
  size_t i;

  count = vectis_opcua_lua_array_len(lua, 1, "boolean array");
  value = vectis_opcua_lua_new_value(lua, sizeof(int) * count);
  values = (int *)value->storage;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    values[i] = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
  }
  cpkt_opcua_value_boolean_array(&value->value, values, count);
  return 1;
}

static int vectis_opcua_lua_value_integer_array(lua_State *lua) {
  vectis_opcua_lua_value *value;
  long *values;
  size_t count;
  size_t i;

  count = vectis_opcua_lua_array_len(lua, 1, "integer array");
  value = vectis_opcua_lua_new_value(lua, sizeof(long) * count);
  values = (long *)value->storage;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    values[i] = (long)luaL_checkinteger(lua, -1);
    lua_pop(lua, 1);
  }
  cpkt_opcua_value_integer_array(&value->value, values, count);
  return 1;
}

static int vectis_opcua_lua_value_double_array(lua_State *lua) {
  vectis_opcua_lua_value *value;
  double *values;
  size_t count;
  size_t i;

  count = vectis_opcua_lua_array_len(lua, 1, "double array");
  value = vectis_opcua_lua_new_value(lua, sizeof(double) * count);
  values = (double *)value->storage;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    values[i] = luaL_checknumber(lua, -1);
    lua_pop(lua, 1);
  }
  cpkt_opcua_value_double_array(&value->value, values, count);
  return 1;
}

static int vectis_opcua_lua_value_string_array(lua_State *lua) {
  vectis_opcua_lua_value *value;
  cpkt_opcua_string_view *values;
  unsigned char *bytes;
  size_t descriptor_size;
  size_t storage_size;
  size_t string_size;
  size_t count;
  size_t i;
  const char *string_value;

  count = vectis_opcua_lua_array_len(lua, 1, "string array");
  descriptor_size = vectis_opcua_lua_checked_size_mul(
      lua, sizeof(cpkt_opcua_string_view), count, "string array");
  storage_size = descriptor_size;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    (void)luaL_checklstring(lua, -1, &string_size);
    storage_size = vectis_opcua_lua_checked_size_add(
        lua, storage_size, string_size, "string array");
    lua_pop(lua, 1);
  }
  value = vectis_opcua_lua_new_value(lua, storage_size);
  values = (cpkt_opcua_string_view *)value->storage;
  bytes = value->storage + descriptor_size;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    string_value = luaL_checklstring(lua, -1, &string_size);
    memcpy(bytes, string_value, string_size);
    values[i].data = (const char *)bytes;
    values[i].length = string_size;
    bytes += string_size;
    lua_pop(lua, 1);
  }
  cpkt_opcua_value_string_array(&value->value, values, count);
  return 1;
}

static int vectis_opcua_lua_value_byte_string_array(lua_State *lua) {
  vectis_opcua_lua_value *value;
  cpkt_opcua_byte_string_view *values;
  unsigned char *bytes;
  size_t descriptor_size;
  size_t storage_size;
  size_t bytes_size;
  size_t count;
  size_t i;
  const char *bytes_value;

  count = vectis_opcua_lua_array_len(lua, 1, "byte string array");
  descriptor_size = vectis_opcua_lua_checked_size_mul(
      lua, sizeof(cpkt_opcua_byte_string_view), count, "byte string array");
  storage_size = descriptor_size;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    (void)luaL_checklstring(lua, -1, &bytes_size);
    storage_size = vectis_opcua_lua_checked_size_add(
        lua, storage_size, bytes_size, "byte string array");
    lua_pop(lua, 1);
  }
  value = vectis_opcua_lua_new_value(lua, storage_size);
  values = (cpkt_opcua_byte_string_view *)value->storage;
  bytes = value->storage + descriptor_size;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    bytes_value = luaL_checklstring(lua, -1, &bytes_size);
    memcpy(bytes, bytes_value, bytes_size);
    values[i].data = bytes;
    values[i].length = bytes_size;
    bytes += bytes_size;
    lua_pop(lua, 1);
  }
  cpkt_opcua_value_byte_string_array(&value->value, values, count);
  return 1;
}

static int vectis_opcua_lua_value_uint64_array(lua_State *lua) {
  vectis_opcua_lua_value *value;
  cpkt_opcua_uint64 *values;
  size_t count;
  size_t i;

  count = vectis_opcua_lua_array_len(lua, 1, "uint64 array");
  value = vectis_opcua_lua_new_value(lua, sizeof(cpkt_opcua_uint64) * count);
  values = (cpkt_opcua_uint64 *)value->storage;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    luaL_checktype(lua, -1, LUA_TTABLE);
    values[i].high32 = vectis_opcua_lua_table_ulong(lua, -1, "high32", 0u);
    values[i].low32 = vectis_opcua_lua_table_ulong(lua, -1, "low32", 0u);
    lua_pop(lua, 1);
  }
  cpkt_opcua_value_uint64_array(&value->value, values, count);
  return 1;
}

static int vectis_opcua_lua_value_datetime_array(lua_State *lua) {
  vectis_opcua_lua_value *value;
  cpkt_opcua_datetime *values;
  size_t count;
  size_t i;

  count = vectis_opcua_lua_array_len(lua, 1, "datetime array");
  value = vectis_opcua_lua_new_value(lua, sizeof(cpkt_opcua_datetime) * count);
  values = (cpkt_opcua_datetime *)value->storage;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    luaL_checktype(lua, -1, LUA_TTABLE);
    lua_getfield(lua, -1, "high32");
    values[i].high32 = (long)luaL_checkinteger(lua, -1);
    lua_pop(lua, 1);
    values[i].low32 = vectis_opcua_lua_table_ulong(lua, -1, "low32", 0u);
    lua_pop(lua, 1);
  }
  cpkt_opcua_value_datetime_array(&value->value, values, count);
  return 1;
}

static int vectis_opcua_lua_value_status_array(lua_State *lua) {
  vectis_opcua_lua_value *value;
  cpkt_opcua_status *values;
  size_t count;
  size_t i;

  count = vectis_opcua_lua_array_len(lua, 1, "status array");
  value = vectis_opcua_lua_new_value(lua, sizeof(cpkt_opcua_status) * count);
  values = (cpkt_opcua_status *)value->storage;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    values[i] = vectis_opcua_lua_check_ulong(lua, -1, "status");
    lua_pop(lua, 1);
  }
  cpkt_opcua_value_status_array(&value->value, values, count);
  return 1;
}

static int vectis_opcua_lua_value_guid_array(lua_State *lua) {
  vectis_opcua_lua_value *value;
  cpkt_opcua_guid *values;
  cpkt_opcua_result result;
  size_t count;
  size_t i;
  const char *guid_text;

  count = vectis_opcua_lua_array_len(lua, 1, "guid array");
  value = vectis_opcua_lua_new_value(
      lua, vectis_opcua_lua_checked_size_mul(lua, sizeof(cpkt_opcua_guid),
                                             count, "guid array"));
  values = (cpkt_opcua_guid *)value->storage;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    guid_text = luaL_checkstring(lua, -1);
    result = cpkt_opcua_guid_parse(guid_text, values[i].bytes);
    lua_pop(lua, 1);
    if (result != CPKT_OPCUA_OK) {
      return vectis_opcua_lua_push_error(lua, result, 0u,
                                         "opcua guid array parse");
    }
  }
  cpkt_opcua_value_guid_array(&value->value, values, count);
  return 1;
}

static int vectis_opcua_lua_value_qualified_name_array(lua_State *lua) {
  vectis_opcua_lua_value *value;
  cpkt_opcua_qualified_name_view *values;
  unsigned char *bytes;
  size_t descriptor_size;
  size_t storage_size;
  size_t name_size;
  size_t count;
  size_t i;
  const char *name;

  count = vectis_opcua_lua_array_len(lua, 1, "qualified name array");
  descriptor_size = vectis_opcua_lua_checked_size_mul(
      lua, sizeof(cpkt_opcua_qualified_name_view), count,
      "qualified name array");
  storage_size = descriptor_size;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    luaL_checktype(lua, -1, LUA_TTABLE);
    name = vectis_opcua_lua_table_string(lua, -1, "name");
    if (name == NULL) {
      return luaL_error(lua, "qualified name array item requires name");
    }
    storage_size = vectis_opcua_lua_checked_size_add(
        lua, storage_size, strlen(name), "qualified name array");
    lua_pop(lua, 1);
  }
  value = vectis_opcua_lua_new_value(lua, storage_size);
  values = (cpkt_opcua_qualified_name_view *)value->storage;
  bytes = value->storage + descriptor_size;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    luaL_checktype(lua, -1, LUA_TTABLE);
    name = vectis_opcua_lua_table_string(lua, -1, "name");
    name_size = strlen(name);
    memcpy(bytes, name, name_size);
    values[i].namespace_index =
        vectis_opcua_lua_table_ushort(lua, -1, "namespace_index", 0u);
    values[i].name = (const char *)bytes;
    values[i].name_length = name_size;
    bytes += name_size;
    lua_pop(lua, 1);
  }
  cpkt_opcua_value_qualified_name_array(&value->value, values, count);
  return 1;
}

static int vectis_opcua_lua_value_localized_text_array(lua_State *lua) {
  vectis_opcua_lua_value *value;
  cpkt_opcua_localized_text_view *values;
  unsigned char *bytes;
  size_t descriptor_size;
  size_t storage_size;
  size_t locale_size;
  size_t text_size;
  size_t count;
  size_t i;
  const char *locale;
  const char *text;

  count = vectis_opcua_lua_array_len(lua, 1, "localized text array");
  descriptor_size = vectis_opcua_lua_checked_size_mul(
      lua, sizeof(cpkt_opcua_localized_text_view), count,
      "localized text array");
  storage_size = descriptor_size;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    luaL_checktype(lua, -1, LUA_TTABLE);
    locale = vectis_opcua_lua_table_string(lua, -1, "locale");
    text = vectis_opcua_lua_table_string(lua, -1, "text");
    if (locale == NULL || text == NULL) {
      return luaL_error(lua,
                        "localized text array item requires locale and text");
    }
    storage_size = vectis_opcua_lua_checked_size_add(
        lua, storage_size, strlen(locale), "localized text array");
    storage_size = vectis_opcua_lua_checked_size_add(
        lua, storage_size, strlen(text), "localized text array");
    lua_pop(lua, 1);
  }
  value = vectis_opcua_lua_new_value(lua, storage_size);
  values = (cpkt_opcua_localized_text_view *)value->storage;
  bytes = value->storage + descriptor_size;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 1, (lua_Integer)i + 1);
    luaL_checktype(lua, -1, LUA_TTABLE);
    locale = vectis_opcua_lua_table_string(lua, -1, "locale");
    text = vectis_opcua_lua_table_string(lua, -1, "text");
    locale_size = strlen(locale);
    text_size = strlen(text);
    memcpy(bytes, locale, locale_size);
    values[i].locale = (const char *)bytes;
    values[i].locale_length = locale_size;
    bytes += locale_size;
    memcpy(bytes, text, text_size);
    values[i].text = (const char *)bytes;
    values[i].text_length = text_size;
    bytes += text_size;
    lua_pop(lua, 1);
  }
  cpkt_opcua_value_localized_text_array(&value->value, values, count);
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
    storage_size =
        input->localized_text_locale_length + input->localized_text_length + 2u;
  } else if (input->type == CPKT_OPCUA_VALUE_BOOLEAN_ARRAY) {
    storage_size = sizeof(int) * input->boolean_array_length;
  } else if (input->type == CPKT_OPCUA_VALUE_INTEGER_ARRAY) {
    storage_size = sizeof(long) * input->integer_array_length;
  } else if (input->type == CPKT_OPCUA_VALUE_DOUBLE_ARRAY) {
    storage_size = sizeof(double) * input->double_array_length;
  } else if (input->type == CPKT_OPCUA_VALUE_STRING_ARRAY) {
    size_t i;

    storage_size = vectis_opcua_lua_checked_size_mul(
        lua, sizeof(cpkt_opcua_string_view), input->string_array_length,
        "string array copy");
    for (i = 0u; i < input->string_array_length; ++i) {
      storage_size = vectis_opcua_lua_checked_size_add(
          lua, storage_size, input->string_array_values[i].length,
          "string array copy");
    }
  } else if (input->type == CPKT_OPCUA_VALUE_BYTE_STRING_ARRAY) {
    size_t i;

    storage_size = vectis_opcua_lua_checked_size_mul(
        lua, sizeof(cpkt_opcua_byte_string_view),
        input->byte_string_array_length, "byte string array copy");
    for (i = 0u; i < input->byte_string_array_length; ++i) {
      storage_size = vectis_opcua_lua_checked_size_add(
          lua, storage_size, input->byte_string_array_values[i].length,
          "byte string array copy");
    }
  } else if (input->type == CPKT_OPCUA_VALUE_UINT64_ARRAY) {
    storage_size = sizeof(cpkt_opcua_uint64) * input->uint64_array_length;
  } else if (input->type == CPKT_OPCUA_VALUE_DATETIME_ARRAY) {
    storage_size = sizeof(cpkt_opcua_datetime) * input->datetime_array_length;
  } else if (input->type == CPKT_OPCUA_VALUE_STATUS_ARRAY) {
    storage_size = sizeof(cpkt_opcua_status) * input->status_array_length;
  } else if (input->type == CPKT_OPCUA_VALUE_GUID_ARRAY) {
    storage_size = sizeof(cpkt_opcua_guid) * input->guid_array_length;
  } else if (input->type == CPKT_OPCUA_VALUE_QUALIFIED_NAME_ARRAY) {
    size_t i;

    storage_size = vectis_opcua_lua_checked_size_mul(
        lua, sizeof(cpkt_opcua_qualified_name_view),
        input->qualified_name_array_length, "qualified name array copy");
    for (i = 0u; i < input->qualified_name_array_length; ++i) {
      storage_size = vectis_opcua_lua_checked_size_add(
          lua, storage_size, input->qualified_name_array_values[i].name_length,
          "qualified name array copy");
    }
  } else if (input->type == CPKT_OPCUA_VALUE_LOCALIZED_TEXT_ARRAY) {
    size_t i;

    storage_size = vectis_opcua_lua_checked_size_mul(
        lua, sizeof(cpkt_opcua_localized_text_view),
        input->localized_text_array_length, "localized text array copy");
    for (i = 0u; i < input->localized_text_array_length; ++i) {
      storage_size = vectis_opcua_lua_checked_size_add(
          lua, storage_size,
          input->localized_text_array_values[i].locale_length,
          "localized text array copy");
      storage_size = vectis_opcua_lua_checked_size_add(
          lua, storage_size, input->localized_text_array_values[i].text_length,
          "localized text array copy");
    }
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
    memcpy(value->storage, input->qualified_name, input->qualified_name_length);
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
  } else if (input->type == CPKT_OPCUA_VALUE_BOOLEAN_ARRAY) {
    memcpy(value->storage, input->boolean_array_values, storage_size);
    value->value.boolean_array_values = (const int *)value->storage;
  } else if (input->type == CPKT_OPCUA_VALUE_INTEGER_ARRAY) {
    memcpy(value->storage, input->integer_array_values, storage_size);
    value->value.integer_array_values = (const long *)value->storage;
  } else if (input->type == CPKT_OPCUA_VALUE_DOUBLE_ARRAY) {
    memcpy(value->storage, input->double_array_values, storage_size);
    value->value.double_array_values = (const double *)value->storage;
  } else if (input->type == CPKT_OPCUA_VALUE_STRING_ARRAY) {
    cpkt_opcua_string_view *views;
    unsigned char *bytes;
    size_t descriptor_size;
    size_t i;

    descriptor_size =
        sizeof(cpkt_opcua_string_view) * input->string_array_length;
    views = (cpkt_opcua_string_view *)value->storage;
    bytes = value->storage + descriptor_size;
    for (i = 0u; i < input->string_array_length; ++i) {
      memcpy(bytes, input->string_array_values[i].data,
             input->string_array_values[i].length);
      views[i].data = (const char *)bytes;
      views[i].length = input->string_array_values[i].length;
      bytes += input->string_array_values[i].length;
    }
    value->value.string_array_values = views;
  } else if (input->type == CPKT_OPCUA_VALUE_BYTE_STRING_ARRAY) {
    cpkt_opcua_byte_string_view *views;
    unsigned char *bytes;
    size_t descriptor_size;
    size_t i;

    descriptor_size =
        sizeof(cpkt_opcua_byte_string_view) * input->byte_string_array_length;
    views = (cpkt_opcua_byte_string_view *)value->storage;
    bytes = value->storage + descriptor_size;
    for (i = 0u; i < input->byte_string_array_length; ++i) {
      memcpy(bytes, input->byte_string_array_values[i].data,
             input->byte_string_array_values[i].length);
      views[i].data = bytes;
      views[i].length = input->byte_string_array_values[i].length;
      bytes += input->byte_string_array_values[i].length;
    }
    value->value.byte_string_array_values = views;
  } else if (input->type == CPKT_OPCUA_VALUE_UINT64_ARRAY) {
    memcpy(value->storage, input->uint64_array_values, storage_size);
    value->value.uint64_array_values =
        (const cpkt_opcua_uint64 *)value->storage;
  } else if (input->type == CPKT_OPCUA_VALUE_DATETIME_ARRAY) {
    memcpy(value->storage, input->datetime_array_values, storage_size);
    value->value.datetime_array_values =
        (const cpkt_opcua_datetime *)value->storage;
  } else if (input->type == CPKT_OPCUA_VALUE_STATUS_ARRAY) {
    memcpy(value->storage, input->status_array_values, storage_size);
    value->value.status_array_values =
        (const cpkt_opcua_status *)value->storage;
  } else if (input->type == CPKT_OPCUA_VALUE_GUID_ARRAY) {
    memcpy(value->storage, input->guid_array_values, storage_size);
    value->value.guid_array_values = (const cpkt_opcua_guid *)value->storage;
  } else if (input->type == CPKT_OPCUA_VALUE_QUALIFIED_NAME_ARRAY) {
    cpkt_opcua_qualified_name_view *views;
    unsigned char *bytes;
    size_t descriptor_size;
    size_t i;

    descriptor_size = sizeof(cpkt_opcua_qualified_name_view) *
                      input->qualified_name_array_length;
    views = (cpkt_opcua_qualified_name_view *)value->storage;
    bytes = value->storage + descriptor_size;
    for (i = 0u; i < input->qualified_name_array_length; ++i) {
      memcpy(bytes, input->qualified_name_array_values[i].name,
             input->qualified_name_array_values[i].name_length);
      views[i].namespace_index =
          input->qualified_name_array_values[i].namespace_index;
      views[i].name = (const char *)bytes;
      views[i].name_length = input->qualified_name_array_values[i].name_length;
      bytes += input->qualified_name_array_values[i].name_length;
    }
    value->value.qualified_name_array_values = views;
  } else if (input->type == CPKT_OPCUA_VALUE_LOCALIZED_TEXT_ARRAY) {
    cpkt_opcua_localized_text_view *views;
    unsigned char *bytes;
    size_t descriptor_size;
    size_t i;

    descriptor_size = sizeof(cpkt_opcua_localized_text_view) *
                      input->localized_text_array_length;
    views = (cpkt_opcua_localized_text_view *)value->storage;
    bytes = value->storage + descriptor_size;
    for (i = 0u; i < input->localized_text_array_length; ++i) {
      memcpy(bytes, input->localized_text_array_values[i].locale,
             input->localized_text_array_values[i].locale_length);
      views[i].locale = (const char *)bytes;
      views[i].locale_length =
          input->localized_text_array_values[i].locale_length;
      bytes += input->localized_text_array_values[i].locale_length;
      memcpy(bytes, input->localized_text_array_values[i].text,
             input->localized_text_array_values[i].text_length);
      views[i].text = (const char *)bytes;
      views[i].text_length = input->localized_text_array_values[i].text_length;
      bytes += input->localized_text_array_values[i].text_length;
    }
    value->value.localized_text_array_values = views;
  }
  return 1;
}

static void vectis_opcua_lua_method_callback_free(
    vectis_opcua_lua_method_callback *callback) {
  size_t i;

  if (callback == NULL) {
    return;
  }
  if (callback->owner != NULL && callback->callback_ref != LUA_NOREF) {
    luaL_unref(callback->owner, LUA_REGISTRYINDEX, callback->callback_ref);
  }
  for (i = 0u; i < callback->output_count; ++i) {
    free(callback->outputs[i].storage);
  }
  free(callback->outputs);
  free(callback);
}

static void
vectis_opcua_lua_server_free_method_callbacks(vectis_opcua_lua_server *server) {
  vectis_opcua_lua_method_callback *callback;
  vectis_opcua_lua_method_callback *next;

  callback = server->methods;
  server->methods = NULL;
  while (callback != NULL) {
    next = callback->next;
    vectis_opcua_lua_method_callback_free(callback);
    callback = next;
  }
}

static vectis_opcua_lua_method_callback *
vectis_opcua_lua_method_callback_new(lua_State *lua, int callback_index,
                                     size_t output_count) {
  vectis_opcua_lua_method_callback *callback;

  callback = (vectis_opcua_lua_method_callback *)calloc(1u, sizeof(*callback));
  if (callback == NULL) {
    (void)luaL_error(lua, "opcua method callback allocation failed");
    return NULL;
  }
  callback->owner = lua;
  callback->callback_ref = LUA_NOREF;
  callback->output_count = output_count;
  if (output_count > 0u) {
    callback->outputs = (vectis_opcua_lua_method_output_slot *)calloc(
        output_count, sizeof(*callback->outputs));
    if (callback->outputs == NULL) {
      free(callback);
      (void)luaL_error(lua, "opcua method output allocation failed");
      return NULL;
    }
  }
  lua_pushvalue(lua, callback_index);
  callback->callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  return callback;
}

static void vectis_opcua_lua_server_keep_method_callback(
    vectis_opcua_lua_server *server,
    vectis_opcua_lua_method_callback *callback) {
  callback->next = server->methods;
  server->methods = callback;
}

static cpkt_opcua_result vectis_opcua_lua_method_callback_call(
    vectis_opcua_lua_method_callback *callback, const cpkt_opcua_value *inputs,
    size_t input_count, cpkt_opcua_value *outputs, size_t output_count) {
  lua_State *lua;
  size_t i;
  cpkt_opcua_result result;

  if (callback == NULL || callback->owner == NULL ||
      callback->callback_ref == LUA_NOREF || outputs == NULL ||
      output_count != callback->output_count) {
    return CPKT_OPCUA_ERR_ARG;
  }
  lua = callback->owner;
  lua_rawgeti(lua, LUA_REGISTRYINDEX, callback->callback_ref);
  lua_newtable(lua);
  for (i = 0u; i < input_count; ++i) {
    (void)vectis_opcua_lua_push_value_copy(lua, &inputs[i]);
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    lua_pop(lua, 1);
    return CPKT_OPCUA_ERR_CALLBACK;
  }
  result = CPKT_OPCUA_OK;
  if (output_count == 1u) {
    result = vectis_opcua_lua_copy_method_output(lua, -1, &outputs[0],
                                                 &callback->outputs[0]);
  } else if (!lua_istable(lua, -1)) {
    result = CPKT_OPCUA_ERR_TYPE;
  } else {
    for (i = 0u; i < output_count && result == CPKT_OPCUA_OK; ++i) {
      lua_rawgeti(lua, -1, (lua_Integer)i + 1);
      result = vectis_opcua_lua_copy_method_output(lua, -1, &outputs[i],
                                                   &callback->outputs[i]);
      lua_pop(lua, 1);
    }
  }
  lua_pop(lua, 1);
  return result;
}

static cpkt_opcua_result
vectis_opcua_lua_method_cb(const cpkt_opcua_value *inputs, size_t input_count,
                           cpkt_opcua_value *output, void *user) {
  return vectis_opcua_lua_method_callback_call(
      (vectis_opcua_lua_method_callback *)user, inputs, input_count, output,
      1u);
}

static cpkt_opcua_result
vectis_opcua_lua_method_many_cb(const cpkt_opcua_value *inputs,
                                size_t input_count, cpkt_opcua_value *outputs,
                                size_t output_count, void *user) {
  return vectis_opcua_lua_method_callback_call(
      (vectis_opcua_lua_method_callback *)user, inputs, input_count, outputs,
      output_count);
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
      return luaL_error(lua, "opcua connect credentials require username and "
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
  result =
      cpkt_opcua_client_read(client, node_id, &value, buffer,
                             sizeof(stack_buffer), &required_size, &status);
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
  result = cpkt_opcua_client_get_namespace_index(client, namespace_uri,
                                                 &namespace_index, &status);
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
  result = cpkt_opcua_client_get_namespace_uri(client, namespace_index, buffer,
                                               sizeof(stack_buffer),
                                               &required_size, &status);
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
                                         const char *field, const char *context,
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

static int *vectis_opcua_lua_int_array_field(lua_State *lua, int index,
                                             const char *field,
                                             size_t *count_out) {
  int *values;
  size_t count;
  size_t i;

  *count_out = 0u;
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return NULL;
  }
  count = vectis_opcua_lua_array_len(lua, -1, field);
  if (count > ((size_t)-1) / sizeof(int)) {
    lua_pop(lua, 1);
    (void)luaL_error(lua, "%s size overflow", field);
    return NULL;
  }
  values = (int *)malloc(sizeof(int) * (count == 0u ? 1u : count));
  if (values == NULL) {
    lua_pop(lua, 1);
    (void)luaL_error(lua, "%s allocation failed", field);
    return NULL;
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, -1, (lua_Integer)i + 1);
    values[i] = (int)luaL_checkinteger(lua, -1);
    lua_pop(lua, 1);
  }
  lua_pop(lua, 1);
  *count_out = count;
  return values;
}

static cpkt_opcua_value *
vectis_opcua_lua_value_array_from_lua(lua_State *lua, int index,
                                      size_t *count_out, const char *context) {
  cpkt_opcua_value *values;
  size_t count;
  size_t i;

  *count_out = 0u;
  if (lua_isnoneornil(lua, index)) {
    return NULL;
  }
  count = vectis_opcua_lua_array_len(lua, index, context);
  if (count > ((size_t)-1) / sizeof(cpkt_opcua_value)) {
    (void)luaL_error(lua, "%s size overflow", context);
    return NULL;
  }
  values = (cpkt_opcua_value *)malloc(sizeof(cpkt_opcua_value) *
                                      (count == 0u ? 1u : count));
  if (values == NULL) {
    (void)luaL_error(lua, "%s allocation failed", context);
    return NULL;
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, index, (lua_Integer)i + 1);
    vectis_opcua_lua_value_from_lua(lua, -1, &values[i]);
    lua_pop(lua, 1);
  }
  *count_out = count;
  return values;
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

static int
vectis_opcua_lua_server_read_long_attr(lua_State *lua,
                                       vectis_opcua_lua_server_read_long_fn fn,
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

static int
vectis_opcua_lua_server_read_bool_attr(lua_State *lua,
                                       vectis_opcua_lua_server_read_bool_fn fn,
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
  vectis_opcua_lua_server_free_method_callbacks(server);
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
  server->owner = lua;
  server->methods = NULL;
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
        result = cpkt_opcua_server_new_from_json_file(&server->server, path,
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
    return vectis_opcua_lua_push_error(lua, result, status, "opcua server new");
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
  application_name = vectis_opcua_lua_table_string(lua, 2, "application_name");
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
  allow_anonymous = vectis_opcua_lua_table_bool(lua, 2, "allow_anonymous", 1);
  username = vectis_opcua_lua_table_string(lua, 2, "username");
  password = vectis_opcua_lua_table_string(lua, 2, "password");
  if ((username == NULL) != (password == NULL)) {
    return luaL_error(lua,
                      "opcua server access control username and password must "
                      "be provided together");
  }
  status = 0u;
  result = cpkt_opcua_server_set_access_control(server, allow_anonymous,
                                                username, password, &status);
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
  vectis_opcua_lua_value_field(lua, 2, "value", "opcua server add_variable",
                               &value);
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
  parent_node_id = vectis_opcua_lua_node_id_field(lua, 2, "parent_node_id",
                                                  "opcua server add_object");
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
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua server add_variable_under");
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
  result = cpkt_opcua_server_add_variable_under(server, node_id, parent_node_id,
                                                browse_name, display_name,
                                                &value, &status);
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
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua server add_object_type");
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
  result = cpkt_opcua_server_add_object_type(server, node_id, parent_node_id,
                                             browse_name, display_name,
                                             is_abstract, &status);
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
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua server add_variable_type");
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
  result = cpkt_opcua_server_add_variable_type(server, node_id, parent_node_id,
                                               browse_name, display_name,
                                               &value, is_abstract, &status);
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
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua server add_reference_type");
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
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua server add_data_type");
  parent_node_id = vectis_opcua_lua_node_id_field(lua, 2, "parent_node_id",
                                                  "opcua server add_data_type");
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
  result = cpkt_opcua_server_add_data_type(server, node_id, parent_node_id,
                                           browse_name, display_name,
                                           is_abstract, &status);
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
  parent_node_id = vectis_opcua_lua_node_id_field(lua, 2, "parent_node_id",
                                                  "opcua server add_view");
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

static int vectis_opcua_lua_server_add_method(lua_State *lua) {
  vectis_opcua_lua_server *server_ud;
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  const char *browse_name;
  const char *display_name;
  int *input_types;
  size_t input_count;
  int output_type;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  vectis_opcua_lua_method_callback *callback;

  server_ud = vectis_opcua_lua_check_server(lua, 1);
  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua server add_method");
  parent_node_id = vectis_opcua_lua_node_id_field(lua, 2, "parent_node_id",
                                                  "opcua server add_method");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  if (browse_name == NULL || browse_name[0] == '\0') {
    return luaL_error(lua, "opcua server add_method requires browse_name");
  }
  if (display_name == NULL) {
    display_name = browse_name;
  }
  lua_getfield(lua, 2, "callback");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_getfield(lua, 2, "handler");
  }
  luaL_checktype(lua, -1, LUA_TFUNCTION);
  output_type =
      vectis_opcua_lua_table_int(lua, 2, "output_type", CPKT_OPCUA_VALUE_EMPTY);
  input_types =
      vectis_opcua_lua_int_array_field(lua, 2, "input_types", &input_count);
  callback = vectis_opcua_lua_method_callback_new(lua, -1, 1u);
  lua_pop(lua, 1);
  status = 0u;
  result = cpkt_opcua_server_add_method(
      server, node_id, parent_node_id, browse_name, display_name, input_types,
      input_count, output_type, vectis_opcua_lua_method_cb, callback, &status);
  free(input_types);
  if (result != CPKT_OPCUA_OK) {
    vectis_opcua_lua_method_callback_free(callback);
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server add method");
  }
  vectis_opcua_lua_server_keep_method_callback(server_ud, callback);
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_add_method_many(lua_State *lua) {
  vectis_opcua_lua_server *server_ud;
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  const char *browse_name;
  const char *display_name;
  int *input_types;
  int *output_types;
  size_t input_count;
  size_t output_count;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  vectis_opcua_lua_method_callback *callback;

  server_ud = vectis_opcua_lua_check_server(lua, 1);
  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua server add_method_many");
  parent_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "parent_node_id", "opcua server add_method_many");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  if (browse_name == NULL || browse_name[0] == '\0') {
    return luaL_error(lua, "opcua server add_method_many requires browse_name");
  }
  if (display_name == NULL) {
    display_name = browse_name;
  }
  lua_getfield(lua, 2, "callback");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_getfield(lua, 2, "handler");
  }
  luaL_checktype(lua, -1, LUA_TFUNCTION);
  input_types =
      vectis_opcua_lua_int_array_field(lua, 2, "input_types", &input_count);
  output_types =
      vectis_opcua_lua_int_array_field(lua, 2, "output_types", &output_count);
  if (output_count == 0u) {
    free(input_types);
    free(output_types);
    return luaL_error(lua,
                      "opcua server add_method_many requires output_types");
  }
  callback = vectis_opcua_lua_method_callback_new(lua, -1, output_count);
  lua_pop(lua, 1);
  status = 0u;
  result = cpkt_opcua_server_add_method_many(
      server, node_id, parent_node_id, browse_name, display_name, input_types,
      input_count, output_types, output_count, vectis_opcua_lua_method_many_cb,
      callback, &status);
  free(input_types);
  free(output_types);
  if (result != CPKT_OPCUA_OK) {
    vectis_opcua_lua_method_callback_free(callback);
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server add method many");
  }
  vectis_opcua_lua_server_keep_method_callback(server_ud, callback);
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
  source_node_id = vectis_opcua_lua_node_id_field(lua, 2, "source_node_id",
                                                  "opcua server add_reference");
  reference_type_id = vectis_opcua_lua_node_id_field(
      lua, 2, "reference_type_id", "opcua server add_reference");
  target_node_id = vectis_opcua_lua_node_id_field(lua, 2, "target_node_id",
                                                  "opcua server add_reference");
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
  result = cpkt_opcua_server_read_node_id(server, node_id, &result_node_id,
                                          buffer, sizeof(stack_buffer),
                                          &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua server read node id allocation failed");
    }
    result_node_id = cpkt_opcua_node_id_null();
    result =
        cpkt_opcua_server_read_node_id(server, node_id, &result_node_id, buffer,
                                       required_size + 1u, NULL, &status);
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
      lua, cpkt_opcua_server_read_node_class, "opcua server read node class");
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
  result = cpkt_opcua_server_read_browse_name(server, node_id, &namespace_index,
                                              buffer, sizeof(stack_buffer),
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
      lua, cpkt_opcua_server_read_description, "opcua server read description");
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
      lua, cpkt_opcua_server_read_write_mask, "opcua server read write mask");
}

static int vectis_opcua_lua_server_read_user_write_mask(lua_State *lua) {
  return vectis_opcua_lua_server_read_ulong_attr(
      lua, cpkt_opcua_server_read_user_write_mask,
      "opcua server read user write mask");
}

static int vectis_opcua_lua_server_write_write_mask(lua_State *lua) {
  return vectis_opcua_lua_server_write_ulong_attr(
      lua, cpkt_opcua_server_write_write_mask, "opcua server write write mask");
}

static int vectis_opcua_lua_server_read_is_abstract(lua_State *lua) {
  return vectis_opcua_lua_server_read_bool_attr(
      lua, cpkt_opcua_server_read_is_abstract, "opcua server read is abstract");
}

static int vectis_opcua_lua_server_write_is_abstract(lua_State *lua) {
  return vectis_opcua_lua_server_write_bool_attr(
      lua, cpkt_opcua_server_write_is_abstract,
      "opcua server write is abstract");
}

static int vectis_opcua_lua_server_read_symmetric(lua_State *lua) {
  return vectis_opcua_lua_server_read_bool_attr(
      lua, cpkt_opcua_server_read_symmetric, "opcua server read symmetric");
}

static int vectis_opcua_lua_server_write_symmetric(lua_State *lua) {
  return vectis_opcua_lua_server_write_bool_attr(
      lua, cpkt_opcua_server_write_symmetric, "opcua server write symmetric");
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
  result =
      cpkt_opcua_server_read_data_type(server, node_id, &data_type, &status);
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
  result =
      cpkt_opcua_server_write_data_type(server, node_id, data_type, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server write data type");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_read_value_rank(lua_State *lua) {
  return vectis_opcua_lua_server_read_long_attr(
      lua, cpkt_opcua_server_read_value_rank, "opcua server read value rank");
}

static int vectis_opcua_lua_server_write_value_rank(lua_State *lua) {
  return vectis_opcua_lua_server_write_long_attr(
      lua, cpkt_opcua_server_write_value_rank, "opcua server write value rank");
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
      lua, cpkt_opcua_server_read_historizing, "opcua server read historizing");
}

static int vectis_opcua_lua_server_write_historizing(lua_State *lua) {
  return vectis_opcua_lua_server_write_bool_attr(
      lua, cpkt_opcua_server_write_historizing,
      "opcua server write historizing");
}

static int vectis_opcua_lua_server_read_executable(lua_State *lua) {
  return vectis_opcua_lua_server_read_bool_attr(
      lua, cpkt_opcua_server_read_executable, "opcua server read executable");
}

static int vectis_opcua_lua_server_read_user_executable(lua_State *lua) {
  return vectis_opcua_lua_server_read_bool_attr(
      lua, cpkt_opcua_server_read_user_executable,
      "opcua server read user executable");
}

static int vectis_opcua_lua_server_write_executable(lua_State *lua) {
  return vectis_opcua_lua_server_write_bool_attr(
      lua, cpkt_opcua_server_write_executable, "opcua server write executable");
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
    return vectis_opcua_lua_push_error(lua, result, 0u, "opcua server iterate");
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
  result =
      cpkt_opcua_server_read(server, node_id, &value, buffer,
                             sizeof(stack_buffer), &required_size, &status);
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

static int
vectis_opcua_lua_push_data_value(lua_State *lua,
                                 const cpkt_opcua_data_value *data_value) {
  lua_newtable(lua);
  lua_pushboolean(lua, data_value->has_value);
  lua_setfield(lua, -2, "has_value");
  if (data_value->has_value) {
    (void)vectis_opcua_lua_push_value_copy(lua, &data_value->value);
    lua_setfield(lua, -2, "value");
  }
  lua_pushboolean(lua, data_value->has_status);
  lua_setfield(lua, -2, "has_status");
  if (data_value->has_status) {
    lua_pushinteger(lua, (lua_Integer)data_value->status);
    lua_setfield(lua, -2, "status");
  }
  lua_pushboolean(lua, data_value->has_source_timestamp);
  lua_setfield(lua, -2, "has_source_timestamp");
  if (data_value->has_source_timestamp) {
    lua_newtable(lua);
    lua_pushinteger(lua, (lua_Integer)data_value->source_timestamp.high32);
    lua_setfield(lua, -2, "high32");
    lua_pushinteger(lua, (lua_Integer)data_value->source_timestamp.low32);
    lua_setfield(lua, -2, "low32");
    lua_setfield(lua, -2, "source_timestamp");
  }
  lua_pushboolean(lua, data_value->has_server_timestamp);
  lua_setfield(lua, -2, "has_server_timestamp");
  if (data_value->has_server_timestamp) {
    lua_newtable(lua);
    lua_pushinteger(lua, (lua_Integer)data_value->server_timestamp.high32);
    lua_setfield(lua, -2, "high32");
    lua_pushinteger(lua, (lua_Integer)data_value->server_timestamp.low32);
    lua_setfield(lua, -2, "low32");
    lua_setfield(lua, -2, "server_timestamp");
  }
  return 1;
}

static int vectis_opcua_lua_server_read_data_value(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_data_value data_value;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  char stack_buffer[4096];
  char *buffer;
  size_t required_size;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  buffer = stack_buffer;
  required_size = 0u;
  cpkt_opcua_data_value_clear(&data_value);
  status = 0u;
  result = cpkt_opcua_server_read_data_value(server, node_id, &data_value,
                                             buffer, sizeof(stack_buffer),
                                             &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua server read data value allocation failed");
    }
    cpkt_opcua_data_value_clear(&data_value);
    result =
        cpkt_opcua_server_read_data_value(server, node_id, &data_value, buffer,
                                          required_size + 1u, NULL, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server read data value");
  }
  (void)vectis_opcua_lua_push_data_value(lua, &data_value);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_push_int_array(lua_State *lua, const int *values,
                                           size_t count) {
  size_t i;

  lua_newtable(lua);
  for (i = 0u; i < count; ++i) {
    lua_pushboolean(lua, values[i]);
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int vectis_opcua_lua_push_long_array(lua_State *lua, const long *values,
                                            size_t count) {
  size_t i;

  lua_newtable(lua);
  for (i = 0u; i < count; ++i) {
    lua_pushinteger(lua, (lua_Integer)values[i]);
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int vectis_opcua_lua_push_double_array(lua_State *lua,
                                              const double *values,
                                              size_t count) {
  size_t i;

  lua_newtable(lua);
  for (i = 0u; i < count; ++i) {
    lua_pushnumber(lua, (lua_Number)values[i]);
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int vectis_opcua_lua_push_uint64_array(lua_State *lua,
                                              const cpkt_opcua_uint64 *values,
                                              size_t count) {
  size_t i;

  lua_newtable(lua);
  for (i = 0u; i < count; ++i) {
    lua_newtable(lua);
    lua_pushinteger(lua, (lua_Integer)values[i].high32);
    lua_setfield(lua, -2, "high32");
    lua_pushinteger(lua, (lua_Integer)values[i].low32);
    lua_setfield(lua, -2, "low32");
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int vectis_opcua_lua_push_datetime_array(
    lua_State *lua, const cpkt_opcua_datetime *values, size_t count) {
  size_t i;

  lua_newtable(lua);
  for (i = 0u; i < count; ++i) {
    lua_newtable(lua);
    lua_pushinteger(lua, (lua_Integer)values[i].high32);
    lua_setfield(lua, -2, "high32");
    lua_pushinteger(lua, (lua_Integer)values[i].low32);
    lua_setfield(lua, -2, "low32");
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int vectis_opcua_lua_push_status_array(lua_State *lua,
                                              const cpkt_opcua_status *values,
                                              size_t count) {
  size_t i;

  lua_newtable(lua);
  for (i = 0u; i < count; ++i) {
    lua_pushinteger(lua, (lua_Integer)values[i]);
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  return 1;
}

static int vectis_opcua_lua_push_guid_array(lua_State *lua,
                                            const cpkt_opcua_guid *values,
                                            size_t count) {
  char guid_buffer[40];
  size_t required_size;
  cpkt_opcua_result result;
  size_t i;

  lua_newtable(lua);
  for (i = 0u; i < count; ++i) {
    required_size = 0u;
    result = cpkt_opcua_guid_print(values[i].bytes, guid_buffer,
                                   sizeof(guid_buffer), &required_size);
    if (result != CPKT_OPCUA_OK) {
      lua_pop(lua, 1);
      return vectis_opcua_lua_push_error(lua, result, 0u,
                                         "opcua guid array print");
    }
    lua_pushstring(lua, guid_buffer);
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  return 1;
}

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_int_array_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, int *, size_t, size_t *,
    cpkt_opcua_status *);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_int_array_range_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, const char *, int *, size_t,
    size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_server_read_int_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_server_read_int_array_fn read_fn,
    vectis_opcua_lua_server_read_int_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int stack_values[16];
  int *values;
  size_t required_count;
  const char *index_range;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(server, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(server, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values = (int *)malloc(sizeof(int) * required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua server int array allocation failed");
    }
    result = has_range ? range_fn(server, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(server, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  (void)vectis_opcua_lua_push_int_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return 1;
}

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_long_array_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, long *, size_t, size_t *,
    cpkt_opcua_status *);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_long_array_range_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, const char *, long *, size_t,
    size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_server_read_long_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_server_read_long_array_fn read_fn,
    vectis_opcua_lua_server_read_long_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  long stack_values[16];
  long *values;
  size_t required_count;
  const char *index_range;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(server, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(server, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values = (long *)malloc(sizeof(long) * required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua server long array allocation failed");
    }
    result = has_range ? range_fn(server, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(server, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  (void)vectis_opcua_lua_push_long_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return 1;
}

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_double_array_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, double *, size_t, size_t *,
    cpkt_opcua_status *);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_double_array_range_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, const char *, double *, size_t,
    size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_server_read_double_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_server_read_double_array_fn read_fn,
    vectis_opcua_lua_server_read_double_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  double stack_values[16];
  double *values;
  size_t required_count;
  const char *index_range;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(server, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(server, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values = (double *)malloc(sizeof(double) * required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua server double array allocation failed");
    }
    result = has_range ? range_fn(server, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(server, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  (void)vectis_opcua_lua_push_double_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return 1;
}

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_uint64_array_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, cpkt_opcua_uint64 *, size_t,
    size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_uint64_array_range_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, const char *, cpkt_opcua_uint64 *,
    size_t, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_server_read_uint64_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_server_read_uint64_array_fn read_fn,
    vectis_opcua_lua_server_read_uint64_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  cpkt_opcua_uint64 stack_values[16];
  cpkt_opcua_uint64 *values;
  size_t required_count;
  const char *index_range;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(server, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(server, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values =
        (cpkt_opcua_uint64 *)malloc(sizeof(cpkt_opcua_uint64) * required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua server uint64 array allocation failed");
    }
    result = has_range ? range_fn(server, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(server, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  (void)vectis_opcua_lua_push_uint64_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return 1;
}

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_datetime_array_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, cpkt_opcua_datetime *, size_t,
    size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (
    *vectis_opcua_lua_server_read_datetime_array_range_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, const char *,
    cpkt_opcua_datetime *, size_t, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_server_read_datetime_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_server_read_datetime_array_fn read_fn,
    vectis_opcua_lua_server_read_datetime_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  cpkt_opcua_datetime stack_values[16];
  cpkt_opcua_datetime *values;
  size_t required_count;
  const char *index_range;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(server, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(server, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values = (cpkt_opcua_datetime *)malloc(sizeof(cpkt_opcua_datetime) *
                                           required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua server datetime array allocation failed");
    }
    result = has_range ? range_fn(server, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(server, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  (void)vectis_opcua_lua_push_datetime_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return 1;
}

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_status_array_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, cpkt_opcua_status *, size_t,
    size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_status_array_range_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, const char *, cpkt_opcua_status *,
    size_t, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_server_read_status_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_server_read_status_array_fn read_fn,
    vectis_opcua_lua_server_read_status_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  cpkt_opcua_status stack_values[16];
  cpkt_opcua_status *values;
  size_t required_count;
  const char *index_range;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(server, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(server, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values =
        (cpkt_opcua_status *)malloc(sizeof(cpkt_opcua_status) * required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua server status array allocation failed");
    }
    result = has_range ? range_fn(server, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(server, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  (void)vectis_opcua_lua_push_status_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return 1;
}

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_guid_array_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, cpkt_opcua_guid *, size_t,
    size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_guid_array_range_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, const char *, cpkt_opcua_guid *,
    size_t, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_server_read_guid_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_server_read_guid_array_fn read_fn,
    vectis_opcua_lua_server_read_guid_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  cpkt_opcua_guid stack_values[16];
  cpkt_opcua_guid *values;
  size_t required_count;
  const char *index_range;
  int pushed;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(server, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(server, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values =
        (cpkt_opcua_guid *)malloc(sizeof(cpkt_opcua_guid) * required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua server guid array allocation failed");
    }
    result = has_range ? range_fn(server, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(server, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  pushed = vectis_opcua_lua_push_guid_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return pushed;
}

static int vectis_opcua_lua_string_array_cb(size_t index, const char *data,
                                            size_t length, void *user) {
  lua_State *lua;

  lua = (lua_State *)user;
  lua_pushlstring(lua, data != NULL ? data : "", length);
  lua_rawseti(lua, -2, (lua_Integer)index + 1);
  return 0;
}

static int vectis_opcua_lua_byte_string_array_cb(size_t index,
                                                 const unsigned char *data,
                                                 size_t length, void *user) {
  lua_State *lua;

  lua = (lua_State *)user;
  lua_pushlstring(lua, data != NULL ? (const char *)data : "", length);
  lua_rawseti(lua, -2, (lua_Integer)index + 1);
  return 0;
}

static int vectis_opcua_lua_qualified_name_array_cb(
    size_t index, unsigned short namespace_index, const char *name,
    size_t name_length, void *user) {
  lua_State *lua;

  lua = (lua_State *)user;
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)namespace_index);
  lua_setfield(lua, -2, "namespace_index");
  lua_pushlstring(lua, name != NULL ? name : "", name_length);
  lua_setfield(lua, -2, "name");
  lua_rawseti(lua, -2, (lua_Integer)index + 1);
  return 0;
}

static int
vectis_opcua_lua_localized_text_array_cb(size_t index, const char *locale,
                                         size_t locale_length, const char *text,
                                         size_t text_length, void *user) {
  lua_State *lua;

  lua = (lua_State *)user;
  lua_newtable(lua);
  lua_pushlstring(lua, locale != NULL ? locale : "", locale_length);
  lua_setfield(lua, -2, "locale");
  lua_pushlstring(lua, text != NULL ? text : "", text_length);
  lua_setfield(lua, -2, "text");
  lua_rawseti(lua, -2, (lua_Integer)index + 1);
  return 0;
}

typedef cpkt_opcua_result (*vectis_opcua_lua_server_read_string_cb_array_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, cpkt_opcua_string_array_fn, void *,
    size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (
    *vectis_opcua_lua_server_read_string_cb_array_range_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, const char *,
    cpkt_opcua_string_array_fn, void *, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_server_read_string_cb_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_server_read_string_cb_array_fn read_fn,
    vectis_opcua_lua_server_read_string_cb_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  const char *index_range;
  size_t value_count;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  status = 0u;
  value_count = 0u;
  lua_newtable(lua);
  result = has_range
               ? range_fn(server, node_id, index_range,
                          vectis_opcua_lua_string_array_cb, lua, &value_count,
                          &status)
               : read_fn(server, node_id, vectis_opcua_lua_string_array_cb, lua,
                         &value_count, &status);
  if (result != CPKT_OPCUA_OK) {
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  return 1;
}

typedef cpkt_opcua_result (
    *vectis_opcua_lua_server_read_byte_string_cb_array_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, cpkt_opcua_byte_string_array_fn,
    void *, size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (
    *vectis_opcua_lua_server_read_byte_string_cb_array_range_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, const char *,
    cpkt_opcua_byte_string_array_fn, void *, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_server_read_byte_string_cb_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_server_read_byte_string_cb_array_fn read_fn,
    vectis_opcua_lua_server_read_byte_string_cb_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  const char *index_range;
  size_t value_count;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  status = 0u;
  value_count = 0u;
  lua_newtable(lua);
  result = has_range
               ? range_fn(server, node_id, index_range,
                          vectis_opcua_lua_byte_string_array_cb, lua,
                          &value_count, &status)
               : read_fn(server, node_id, vectis_opcua_lua_byte_string_array_cb,
                         lua, &value_count, &status);
  if (result != CPKT_OPCUA_OK) {
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  return 1;
}

typedef cpkt_opcua_result (
    *vectis_opcua_lua_server_read_qualified_name_cb_array_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, cpkt_opcua_qualified_name_array_fn,
    void *, size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (
    *vectis_opcua_lua_server_read_qualified_name_cb_array_range_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, const char *,
    cpkt_opcua_qualified_name_array_fn, void *, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_server_read_qualified_name_cb_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_server_read_qualified_name_cb_array_fn read_fn,
    vectis_opcua_lua_server_read_qualified_name_cb_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  const char *index_range;
  size_t value_count;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  status = 0u;
  value_count = 0u;
  lua_newtable(lua);
  result = has_range ? range_fn(server, node_id, index_range,
                                vectis_opcua_lua_qualified_name_array_cb, lua,
                                &value_count, &status)
                     : read_fn(server, node_id,
                               vectis_opcua_lua_qualified_name_array_cb, lua,
                               &value_count, &status);
  if (result != CPKT_OPCUA_OK) {
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  return 1;
}

typedef cpkt_opcua_result (
    *vectis_opcua_lua_server_read_localized_text_cb_array_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, cpkt_opcua_localized_text_array_fn,
    void *, size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (
    *vectis_opcua_lua_server_read_localized_text_cb_array_range_fn)(
    cpkt_opcua_server *, cpkt_opcua_node_id, const char *,
    cpkt_opcua_localized_text_array_fn, void *, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_server_read_localized_text_cb_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_server_read_localized_text_cb_array_fn read_fn,
    vectis_opcua_lua_server_read_localized_text_cb_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  const char *index_range;
  size_t value_count;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  status = 0u;
  value_count = 0u;
  lua_newtable(lua);
  result = has_range ? range_fn(server, node_id, index_range,
                                vectis_opcua_lua_localized_text_array_cb, lua,
                                &value_count, &status)
                     : read_fn(server, node_id,
                               vectis_opcua_lua_localized_text_array_cb, lua,
                               &value_count, &status);
  if (result != CPKT_OPCUA_OK) {
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  return 1;
}

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_ulong_fn)(
    cpkt_opcua_client *client, cpkt_opcua_node_id node_id,
    unsigned long *value_out, cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_write_ulong_fn)(
    cpkt_opcua_client *client, cpkt_opcua_node_id node_id, unsigned long value,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_long_fn)(
    cpkt_opcua_client *client, cpkt_opcua_node_id node_id, long *value_out,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_write_long_fn)(
    cpkt_opcua_client *client, cpkt_opcua_node_id node_id, long value,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_bool_fn)(
    cpkt_opcua_client *client, cpkt_opcua_node_id node_id, int *value_out,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_write_bool_fn)(
    cpkt_opcua_client *client, cpkt_opcua_node_id node_id, int value,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_double_fn)(
    cpkt_opcua_client *client, cpkt_opcua_node_id node_id, double *value_out,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_write_double_fn)(
    cpkt_opcua_client *client, cpkt_opcua_node_id node_id, double value,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_string_fn)(
    cpkt_opcua_client *client, cpkt_opcua_node_id node_id, char *buffer,
    size_t buffer_size, size_t *required_size_out,
    cpkt_opcua_status *status_out);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_write_string_fn)(
    cpkt_opcua_client *client, cpkt_opcua_node_id node_id, const char *value,
    cpkt_opcua_status *status_out);

static int vectis_opcua_lua_client_read_ulong_attr(
    lua_State *lua, vectis_opcua_lua_client_read_ulong_fn fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned long value;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = 0u;
  status = 0u;
  result = fn(client, node_id, &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushinteger(lua, (lua_Integer)value);
  return 1;
}

static int vectis_opcua_lua_client_write_ulong_attr(
    lua_State *lua, vectis_opcua_lua_client_write_ulong_fn fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned long value;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = vectis_opcua_lua_check_ulong(lua, 3, "value");
  status = 0u;
  result = fn(client, node_id, value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int
vectis_opcua_lua_client_read_long_attr(lua_State *lua,
                                       vectis_opcua_lua_client_read_long_fn fn,
                                       const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  long value;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = 0L;
  status = 0u;
  result = fn(client, node_id, &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushinteger(lua, (lua_Integer)value);
  return 1;
}

static int vectis_opcua_lua_client_write_long_attr(
    lua_State *lua, vectis_opcua_lua_client_write_long_fn fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  long value;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = (long)luaL_checkinteger(lua, 3);
  status = 0u;
  result = fn(client, node_id, value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int
vectis_opcua_lua_client_read_bool_attr(lua_State *lua,
                                       vectis_opcua_lua_client_read_bool_fn fn,
                                       const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int value;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = 0;
  status = 0u;
  result = fn(client, node_id, &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushboolean(lua, value);
  return 1;
}

static int vectis_opcua_lua_client_write_bool_attr(
    lua_State *lua, vectis_opcua_lua_client_write_bool_fn fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int value;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = lua_toboolean(lua, 3);
  status = 0u;
  result = fn(client, node_id, value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_read_double_attr(
    lua_State *lua, vectis_opcua_lua_client_read_double_fn fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  double value;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = 0.0;
  status = 0u;
  result = fn(client, node_id, &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushnumber(lua, (lua_Number)value);
  return 1;
}

static int vectis_opcua_lua_client_write_double_attr(
    lua_State *lua, vectis_opcua_lua_client_write_double_fn fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  double value;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = luaL_checknumber(lua, 3);
  status = 0u;
  result = fn(client, node_id, value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_read_string_attr(
    lua_State *lua, vectis_opcua_lua_client_read_string_fn fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  char stack_buffer[512];
  char *buffer;
  size_t required_size;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  buffer = stack_buffer;
  required_size = 0u;
  status = 0u;
  result = fn(client, node_id, buffer, sizeof(stack_buffer), &required_size,
              &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua client string attribute allocation failed");
    }
    result = fn(client, node_id, buffer, required_size + 1u, NULL, &status);
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

static int vectis_opcua_lua_client_write_string_attr(
    lua_State *lua, vectis_opcua_lua_client_write_string_fn fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  const char *value;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  value = luaL_checkstring(lua, 3);
  status = 0u;
  result = fn(client, node_id, value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_add_object(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua client add_object");
  parent_node_id = vectis_opcua_lua_node_id_field(lua, 2, "parent_node_id",
                                                  "opcua client add_object");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  status = 0u;
  result = cpkt_opcua_client_add_object(client, node_id, parent_node_id,
                                        browse_name, display_name, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client add object");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_add_variable(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_value value;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua client add_variable");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  vectis_opcua_lua_value_field(lua, 2, "value", "opcua client add_variable",
                               &value);
  status = 0u;
  result = cpkt_opcua_client_add_variable(client, node_id, browse_name,
                                          display_name, &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client add variable");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_add_variable_under(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  cpkt_opcua_value value;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua client add_variable_under");
  parent_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "parent_node_id", "opcua client add_variable_under");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  vectis_opcua_lua_value_field(lua, 2, "value",
                               "opcua client add_variable_under", &value);
  status = 0u;
  result = cpkt_opcua_client_add_variable_under(client, node_id, parent_node_id,
                                                browse_name, display_name,
                                                &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client add variable under");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_add_object_type(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int is_abstract;

  client = vectis_opcua_lua_client_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua client add_object_type");
  parent_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "parent_node_id", "opcua client add_object_type");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  is_abstract = vectis_opcua_lua_table_bool(lua, 2, "is_abstract", 0);
  status = 0u;
  result = cpkt_opcua_client_add_object_type(client, node_id, parent_node_id,
                                             browse_name, display_name,
                                             is_abstract, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client add object type");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_add_variable_type(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  cpkt_opcua_value value;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int is_abstract;

  client = vectis_opcua_lua_client_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua client add_variable_type");
  parent_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "parent_node_id", "opcua client add_variable_type");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  vectis_opcua_lua_value_field(lua, 2, "value",
                               "opcua client add_variable_type", &value);
  is_abstract = vectis_opcua_lua_table_bool(lua, 2, "is_abstract", 0);
  status = 0u;
  result = cpkt_opcua_client_add_variable_type(client, node_id, parent_node_id,
                                               browse_name, display_name,
                                               &value, is_abstract, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client add variable type");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_add_reference_type(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  const char *browse_name;
  const char *display_name;
  const char *inverse_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int is_abstract;
  int symmetric;

  client = vectis_opcua_lua_client_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua client add_reference_type");
  parent_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "parent_node_id", "opcua client add_reference_type");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  inverse_name = vectis_opcua_lua_table_string(lua, 2, "inverse_name");
  is_abstract = vectis_opcua_lua_table_bool(lua, 2, "is_abstract", 0);
  symmetric = vectis_opcua_lua_table_bool(lua, 2, "symmetric", 0);
  status = 0u;
  result = cpkt_opcua_client_add_reference_type(
      client, node_id, parent_node_id, browse_name, display_name, inverse_name,
      is_abstract, symmetric, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client add reference type");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_add_data_type(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int is_abstract;

  client = vectis_opcua_lua_client_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua client add_data_type");
  parent_node_id = vectis_opcua_lua_node_id_field(lua, 2, "parent_node_id",
                                                  "opcua client add_data_type");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  is_abstract = vectis_opcua_lua_table_bool(lua, 2, "is_abstract", 0);
  status = 0u;
  result = cpkt_opcua_client_add_data_type(client, node_id, parent_node_id,
                                           browse_name, display_name,
                                           is_abstract, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client add data type");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_add_view(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id parent_node_id;
  const char *browse_name;
  const char *display_name;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int contains_no_loops;
  unsigned long event_notifier;

  client = vectis_opcua_lua_client_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  node_id = vectis_opcua_lua_node_id_field(lua, 2, "node_id",
                                           "opcua client add_view");
  parent_node_id = vectis_opcua_lua_node_id_field(lua, 2, "parent_node_id",
                                                  "opcua client add_view");
  browse_name = vectis_opcua_lua_table_string(lua, 2, "browse_name");
  display_name = vectis_opcua_lua_table_string(lua, 2, "display_name");
  contains_no_loops =
      vectis_opcua_lua_table_bool(lua, 2, "contains_no_loops", 0);
  event_notifier = vectis_opcua_lua_table_ulong(lua, 2, "event_notifier", 0u);
  status = 0u;
  result = cpkt_opcua_client_add_view(
      client, node_id, parent_node_id, browse_name, display_name,
      contains_no_loops, event_notifier, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client add view");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_delete_node(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  int delete_target_refs;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  delete_target_refs = lua_isnoneornil(lua, 3) ? 1 : lua_toboolean(lua, 3);
  status = 0u;
  result = cpkt_opcua_client_delete_node(client, node_id, delete_target_refs,
                                         &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client delete node");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_add_reference(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id source_node_id;
  cpkt_opcua_node_id reference_type_id;
  cpkt_opcua_node_id target_node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int is_forward;
  unsigned long target_node_class;

  client = vectis_opcua_lua_client_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  source_node_id = vectis_opcua_lua_node_id_field(lua, 2, "source_node_id",
                                                  "opcua client add_reference");
  reference_type_id = vectis_opcua_lua_node_id_field(
      lua, 2, "reference_type_id", "opcua client add_reference");
  target_node_id = vectis_opcua_lua_node_id_field(lua, 2, "target_node_id",
                                                  "opcua client add_reference");
  is_forward = vectis_opcua_lua_table_bool(lua, 2, "is_forward", 1);
  target_node_class = vectis_opcua_lua_table_ulong(
      lua, 2, "target_node_class", CPKT_OPCUA_NODE_CLASS_UNSPECIFIED);
  status = 0u;
  result = cpkt_opcua_client_add_reference(
      client, source_node_id, reference_type_id, is_forward, target_node_id,
      target_node_class, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client add reference");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_delete_reference(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id source_node_id;
  cpkt_opcua_node_id reference_type_id;
  cpkt_opcua_node_id target_node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int is_forward;
  int delete_bidirectional;

  client = vectis_opcua_lua_client_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  source_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "source_node_id", "opcua client delete_reference");
  reference_type_id = vectis_opcua_lua_node_id_field(
      lua, 2, "reference_type_id", "opcua client delete_reference");
  target_node_id = vectis_opcua_lua_node_id_field(
      lua, 2, "target_node_id", "opcua client delete_reference");
  is_forward = vectis_opcua_lua_table_bool(lua, 2, "is_forward", 1);
  delete_bidirectional =
      vectis_opcua_lua_table_bool(lua, 2, "delete_bidirectional", 0);
  status = 0u;
  result = cpkt_opcua_client_delete_reference(
      client, source_node_id, reference_type_id, is_forward, target_node_id,
      delete_bidirectional, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client delete reference");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_read_node_id(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id result_node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  char stack_buffer[256];
  char *buffer;
  size_t required_size;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  buffer = stack_buffer;
  required_size = 0u;
  status = 0u;
  result_node_id = cpkt_opcua_node_id_null();
  result = cpkt_opcua_client_read_node_id(client, node_id, &result_node_id,
                                          buffer, sizeof(stack_buffer),
                                          &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua client read node id allocation failed");
    }
    result =
        cpkt_opcua_client_read_node_id(client, node_id, &result_node_id, buffer,
                                       required_size + 1u, NULL, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client read node id");
  }
  (void)vectis_opcua_lua_push_node_id_copy(lua, &result_node_id);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_client_read_node_class(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  unsigned long node_class;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  status = 0u;
  node_class = 0u;
  result =
      cpkt_opcua_client_read_node_class(client, node_id, &node_class, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client read node class");
  }
  lua_pushinteger(lua, (lua_Integer)node_class);
  return 1;
}

static int vectis_opcua_lua_client_read_browse_name(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned short namespace_index;
  char stack_buffer[512];
  char *buffer;
  size_t required_size;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  buffer = stack_buffer;
  required_size = 0u;
  namespace_index = 0u;
  status = 0u;
  result = cpkt_opcua_client_read_browse_name(client, node_id, &namespace_index,
                                              buffer, sizeof(stack_buffer),
                                              &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua client browse name allocation failed");
    }
    result = cpkt_opcua_client_read_browse_name(
        client, node_id, &namespace_index, buffer, required_size + 1u, NULL,
        &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client read browse name");
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

static int vectis_opcua_lua_client_read_display_name(lua_State *lua) {
  return vectis_opcua_lua_client_read_string_attr(
      lua, cpkt_opcua_client_read_display_name,
      "opcua client read display name");
}

static int vectis_opcua_lua_client_read_description(lua_State *lua) {
  return vectis_opcua_lua_client_read_string_attr(
      lua, cpkt_opcua_client_read_description, "opcua client read description");
}

static int vectis_opcua_lua_client_write_display_name(lua_State *lua) {
  return vectis_opcua_lua_client_write_string_attr(
      lua, cpkt_opcua_client_write_display_name,
      "opcua client write display name");
}

static int vectis_opcua_lua_client_write_description(lua_State *lua) {
  return vectis_opcua_lua_client_write_string_attr(
      lua, cpkt_opcua_client_write_description,
      "opcua client write description");
}

static int vectis_opcua_lua_client_read_write_mask(lua_State *lua) {
  return vectis_opcua_lua_client_read_ulong_attr(
      lua, cpkt_opcua_client_read_write_mask, "opcua client read write mask");
}

static int vectis_opcua_lua_client_read_user_write_mask(lua_State *lua) {
  return vectis_opcua_lua_client_read_ulong_attr(
      lua, cpkt_opcua_client_read_user_write_mask,
      "opcua client read user write mask");
}

static int vectis_opcua_lua_client_write_write_mask(lua_State *lua) {
  return vectis_opcua_lua_client_write_ulong_attr(
      lua, cpkt_opcua_client_write_write_mask, "opcua client write write mask");
}

static int vectis_opcua_lua_client_read_is_abstract(lua_State *lua) {
  return vectis_opcua_lua_client_read_bool_attr(
      lua, cpkt_opcua_client_read_is_abstract, "opcua client read is abstract");
}

static int vectis_opcua_lua_client_write_is_abstract(lua_State *lua) {
  return vectis_opcua_lua_client_write_bool_attr(
      lua, cpkt_opcua_client_write_is_abstract,
      "opcua client write is abstract");
}

static int vectis_opcua_lua_client_read_symmetric(lua_State *lua) {
  return vectis_opcua_lua_client_read_bool_attr(
      lua, cpkt_opcua_client_read_symmetric, "opcua client read symmetric");
}

static int vectis_opcua_lua_client_write_symmetric(lua_State *lua) {
  return vectis_opcua_lua_client_write_bool_attr(
      lua, cpkt_opcua_client_write_symmetric, "opcua client write symmetric");
}

static int vectis_opcua_lua_client_read_inverse_name(lua_State *lua) {
  return vectis_opcua_lua_client_read_string_attr(
      lua, cpkt_opcua_client_read_inverse_name,
      "opcua client read inverse name");
}

static int vectis_opcua_lua_client_write_inverse_name(lua_State *lua) {
  return vectis_opcua_lua_client_write_string_attr(
      lua, cpkt_opcua_client_write_inverse_name,
      "opcua client write inverse name");
}

static int vectis_opcua_lua_client_read_contains_no_loops(lua_State *lua) {
  return vectis_opcua_lua_client_read_bool_attr(
      lua, cpkt_opcua_client_read_contains_no_loops,
      "opcua client read contains no loops");
}

static int vectis_opcua_lua_client_write_contains_no_loops(lua_State *lua) {
  return vectis_opcua_lua_client_write_bool_attr(
      lua, cpkt_opcua_client_write_contains_no_loops,
      "opcua client write contains no loops");
}

static int vectis_opcua_lua_client_read_event_notifier(lua_State *lua) {
  return vectis_opcua_lua_client_read_ulong_attr(
      lua, cpkt_opcua_client_read_event_notifier,
      "opcua client read event notifier");
}

static int vectis_opcua_lua_client_write_event_notifier(lua_State *lua) {
  return vectis_opcua_lua_client_write_ulong_attr(
      lua, cpkt_opcua_client_write_event_notifier,
      "opcua client write event notifier");
}

static int vectis_opcua_lua_client_read_data_type(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id data_type;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  data_type = cpkt_opcua_node_id_null();
  status = 0u;
  result =
      cpkt_opcua_client_read_data_type(client, node_id, &data_type, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client read data type");
  }
  return vectis_opcua_lua_push_node_id_copy(lua, &data_type);
}

static int vectis_opcua_lua_client_write_data_type(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_node_id data_type;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  data_type = vectis_opcua_lua_node_id_at(lua, 3);
  status = 0u;
  result =
      cpkt_opcua_client_write_data_type(client, node_id, data_type, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client write data type");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_client_read_value_rank(lua_State *lua) {
  return vectis_opcua_lua_client_read_long_attr(
      lua, cpkt_opcua_client_read_value_rank, "opcua client read value rank");
}

static int vectis_opcua_lua_client_write_value_rank(lua_State *lua) {
  return vectis_opcua_lua_client_write_long_attr(
      lua, cpkt_opcua_client_write_value_rank, "opcua client write value rank");
}

static int vectis_opcua_lua_client_read_access_level(lua_State *lua) {
  return vectis_opcua_lua_client_read_ulong_attr(
      lua, cpkt_opcua_client_read_access_level,
      "opcua client read access level");
}

static int vectis_opcua_lua_client_read_user_access_level(lua_State *lua) {
  return vectis_opcua_lua_client_read_ulong_attr(
      lua, cpkt_opcua_client_read_user_access_level,
      "opcua client read user access level");
}

static int vectis_opcua_lua_client_write_access_level(lua_State *lua) {
  return vectis_opcua_lua_client_write_ulong_attr(
      lua, cpkt_opcua_client_write_access_level,
      "opcua client write access level");
}

static int vectis_opcua_lua_client_read_access_level_ex(lua_State *lua) {
  return vectis_opcua_lua_client_read_ulong_attr(
      lua, cpkt_opcua_client_read_access_level_ex,
      "opcua client read access level ex");
}

static int vectis_opcua_lua_client_write_access_level_ex(lua_State *lua) {
  return vectis_opcua_lua_client_write_ulong_attr(
      lua, cpkt_opcua_client_write_access_level_ex,
      "opcua client write access level ex");
}

static int
vectis_opcua_lua_client_read_minimum_sampling_interval(lua_State *lua) {
  return vectis_opcua_lua_client_read_double_attr(
      lua, cpkt_opcua_client_read_minimum_sampling_interval,
      "opcua client read minimum sampling interval");
}

static int
vectis_opcua_lua_client_write_minimum_sampling_interval(lua_State *lua) {
  return vectis_opcua_lua_client_write_double_attr(
      lua, cpkt_opcua_client_write_minimum_sampling_interval,
      "opcua client write minimum sampling interval");
}

static int vectis_opcua_lua_client_read_historizing(lua_State *lua) {
  return vectis_opcua_lua_client_read_bool_attr(
      lua, cpkt_opcua_client_read_historizing, "opcua client read historizing");
}

static int vectis_opcua_lua_client_write_historizing(lua_State *lua) {
  return vectis_opcua_lua_client_write_bool_attr(
      lua, cpkt_opcua_client_write_historizing,
      "opcua client write historizing");
}

static int vectis_opcua_lua_client_read_executable(lua_State *lua) {
  return vectis_opcua_lua_client_read_bool_attr(
      lua, cpkt_opcua_client_read_executable, "opcua client read executable");
}

static int vectis_opcua_lua_client_read_user_executable(lua_State *lua) {
  return vectis_opcua_lua_client_read_bool_attr(
      lua, cpkt_opcua_client_read_user_executable,
      "opcua client read user executable");
}

static int vectis_opcua_lua_client_write_executable(lua_State *lua) {
  return vectis_opcua_lua_client_write_bool_attr(
      lua, cpkt_opcua_client_write_executable, "opcua client write executable");
}

static int vectis_opcua_lua_client_read_data_value(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_data_value data_value;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  char stack_buffer[4096];
  char *buffer;
  size_t required_size;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  buffer = stack_buffer;
  required_size = 0u;
  cpkt_opcua_data_value_clear(&data_value);
  status = 0u;
  result = cpkt_opcua_client_read_data_value(client, node_id, &data_value,
                                             buffer, sizeof(stack_buffer),
                                             &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua client read data value allocation failed");
    }
    cpkt_opcua_data_value_clear(&data_value);
    result =
        cpkt_opcua_client_read_data_value(client, node_id, &data_value, buffer,
                                          required_size + 1u, NULL, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client read data value");
  }
  (void)vectis_opcua_lua_push_data_value(lua, &data_value);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_client_read_integer_array_common(lua_State *lua,
                                                             int has_range) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  long stack_values[16];
  long *values;
  size_t required_count;
  const char *index_range;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  values = stack_values;
  required_count = 0u;
  status = 0u;
  if (has_range) {
    index_range = luaL_checkstring(lua, 3);
    result = cpkt_opcua_client_read_integer_array_range(
        client, node_id, index_range, values, 16u, &required_count, &status);
  } else {
    result = cpkt_opcua_client_read_integer_array(client, node_id, values, 16u,
                                                  &required_count, &status);
  }
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values = (long *)malloc(sizeof(long) * required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua client integer array allocation failed");
    }
    if (has_range) {
      index_range = luaL_checkstring(lua, 3);
      result = cpkt_opcua_client_read_integer_array_range(
          client, node_id, index_range, values, required_count, NULL, &status);
    } else {
      result = cpkt_opcua_client_read_integer_array(
          client, node_id, values, required_count, NULL, &status);
    }
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(
        lua, result, status,
        has_range ? "opcua client read integer array range"
                  : "opcua client read integer array");
  }
  (void)vectis_opcua_lua_push_long_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return 1;
}

static int vectis_opcua_lua_client_read_integer_array(lua_State *lua) {
  return vectis_opcua_lua_client_read_integer_array_common(lua, 0);
}

static int vectis_opcua_lua_client_read_integer_array_range(lua_State *lua) {
  return vectis_opcua_lua_client_read_integer_array_common(lua, 1);
}

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_int_array_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, int *, size_t, size_t *,
    cpkt_opcua_status *);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_int_array_range_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, const char *, int *, size_t,
    size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_client_read_int_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_client_read_int_array_fn read_fn,
    vectis_opcua_lua_client_read_int_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int stack_values[16];
  int *values;
  size_t required_count;
  const char *index_range;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(client, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(client, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values = (int *)malloc(sizeof(int) * required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua client int array allocation failed");
    }
    result = has_range ? range_fn(client, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(client, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  (void)vectis_opcua_lua_push_int_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return 1;
}

static int vectis_opcua_lua_client_read_boolean_array(lua_State *lua) {
  return vectis_opcua_lua_client_read_int_array_common(
      lua, 0, cpkt_opcua_client_read_boolean_array,
      cpkt_opcua_client_read_boolean_array_range,
      "opcua client read boolean array");
}

static int vectis_opcua_lua_client_read_boolean_array_range(lua_State *lua) {
  return vectis_opcua_lua_client_read_int_array_common(
      lua, 1, cpkt_opcua_client_read_boolean_array,
      cpkt_opcua_client_read_boolean_array_range,
      "opcua client read boolean array range");
}

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_double_array_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, double *, size_t, size_t *,
    cpkt_opcua_status *);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_double_array_range_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, const char *, double *, size_t,
    size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_client_read_double_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_client_read_double_array_fn read_fn,
    vectis_opcua_lua_client_read_double_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  double stack_values[16];
  double *values;
  size_t required_count;
  const char *index_range;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(client, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(client, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values = (double *)malloc(sizeof(double) * required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua client double array allocation failed");
    }
    result = has_range ? range_fn(client, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(client, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  (void)vectis_opcua_lua_push_double_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return 1;
}

static int vectis_opcua_lua_client_read_double_array(lua_State *lua) {
  return vectis_opcua_lua_client_read_double_array_common(
      lua, 0, cpkt_opcua_client_read_double_array,
      cpkt_opcua_client_read_double_array_range,
      "opcua client read double array");
}

static int vectis_opcua_lua_client_read_double_array_range(lua_State *lua) {
  return vectis_opcua_lua_client_read_double_array_common(
      lua, 1, cpkt_opcua_client_read_double_array,
      cpkt_opcua_client_read_double_array_range,
      "opcua client read double array range");
}

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_uint64_array_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, cpkt_opcua_uint64 *, size_t,
    size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_uint64_array_range_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, const char *, cpkt_opcua_uint64 *,
    size_t, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_client_read_uint64_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_client_read_uint64_array_fn read_fn,
    vectis_opcua_lua_client_read_uint64_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  cpkt_opcua_uint64 stack_values[16];
  cpkt_opcua_uint64 *values;
  size_t required_count;
  const char *index_range;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(client, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(client, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values =
        (cpkt_opcua_uint64 *)malloc(sizeof(cpkt_opcua_uint64) * required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua client uint64 array allocation failed");
    }
    result = has_range ? range_fn(client, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(client, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  (void)vectis_opcua_lua_push_uint64_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return 1;
}

static int vectis_opcua_lua_client_read_uint64_array(lua_State *lua) {
  return vectis_opcua_lua_client_read_uint64_array_common(
      lua, 0, cpkt_opcua_client_read_uint64_array,
      cpkt_opcua_client_read_uint64_array_range,
      "opcua client read uint64 array");
}

static int vectis_opcua_lua_client_read_uint64_array_range(lua_State *lua) {
  return vectis_opcua_lua_client_read_uint64_array_common(
      lua, 1, cpkt_opcua_client_read_uint64_array,
      cpkt_opcua_client_read_uint64_array_range,
      "opcua client read uint64 array range");
}

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_datetime_array_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, cpkt_opcua_datetime *, size_t,
    size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (
    *vectis_opcua_lua_client_read_datetime_array_range_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, const char *,
    cpkt_opcua_datetime *, size_t, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_client_read_datetime_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_client_read_datetime_array_fn read_fn,
    vectis_opcua_lua_client_read_datetime_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  cpkt_opcua_datetime stack_values[16];
  cpkt_opcua_datetime *values;
  size_t required_count;
  const char *index_range;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(client, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(client, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values = (cpkt_opcua_datetime *)malloc(sizeof(cpkt_opcua_datetime) *
                                           required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua client datetime array allocation failed");
    }
    result = has_range ? range_fn(client, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(client, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  (void)vectis_opcua_lua_push_datetime_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return 1;
}

static int vectis_opcua_lua_client_read_datetime_array(lua_State *lua) {
  return vectis_opcua_lua_client_read_datetime_array_common(
      lua, 0, cpkt_opcua_client_read_datetime_array,
      cpkt_opcua_client_read_datetime_array_range,
      "opcua client read datetime array");
}

static int vectis_opcua_lua_client_read_datetime_array_range(lua_State *lua) {
  return vectis_opcua_lua_client_read_datetime_array_common(
      lua, 1, cpkt_opcua_client_read_datetime_array,
      cpkt_opcua_client_read_datetime_array_range,
      "opcua client read datetime array range");
}

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_status_array_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, cpkt_opcua_status *, size_t,
    size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_status_array_range_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, const char *, cpkt_opcua_status *,
    size_t, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_client_read_status_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_client_read_status_array_fn read_fn,
    vectis_opcua_lua_client_read_status_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  cpkt_opcua_status stack_values[16];
  cpkt_opcua_status *values;
  size_t required_count;
  const char *index_range;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(client, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(client, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values =
        (cpkt_opcua_status *)malloc(sizeof(cpkt_opcua_status) * required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua client status array allocation failed");
    }
    result = has_range ? range_fn(client, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(client, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  (void)vectis_opcua_lua_push_status_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return 1;
}

static int vectis_opcua_lua_client_read_status_array(lua_State *lua) {
  return vectis_opcua_lua_client_read_status_array_common(
      lua, 0, cpkt_opcua_client_read_status_array,
      cpkt_opcua_client_read_status_array_range,
      "opcua client read status array");
}

static int vectis_opcua_lua_client_read_status_array_range(lua_State *lua) {
  return vectis_opcua_lua_client_read_status_array_common(
      lua, 1, cpkt_opcua_client_read_status_array,
      cpkt_opcua_client_read_status_array_range,
      "opcua client read status array range");
}

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_guid_array_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, cpkt_opcua_guid *, size_t,
    size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_guid_array_range_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, const char *, cpkt_opcua_guid *,
    size_t, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_client_read_guid_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_client_read_guid_array_fn read_fn,
    vectis_opcua_lua_client_read_guid_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  cpkt_opcua_guid stack_values[16];
  cpkt_opcua_guid *values;
  size_t required_count;
  const char *index_range;
  int pushed;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  values = stack_values;
  required_count = 0u;
  status = 0u;
  result = has_range ? range_fn(client, node_id, index_range, values, 16u,
                                &required_count, &status)
                     : read_fn(client, node_id, values, 16u, &required_count,
                               &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_count > 16u) {
    values =
        (cpkt_opcua_guid *)malloc(sizeof(cpkt_opcua_guid) * required_count);
    if (values == NULL) {
      return luaL_error(lua, "opcua client guid array allocation failed");
    }
    result = has_range ? range_fn(client, node_id, index_range, values,
                                  required_count, NULL, &status)
                       : read_fn(client, node_id, values, required_count, NULL,
                                 &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (values != stack_values) {
      free(values);
    }
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  pushed = vectis_opcua_lua_push_guid_array(lua, values, required_count);
  if (values != stack_values) {
    free(values);
  }
  return pushed;
}

static int vectis_opcua_lua_client_read_guid_array(lua_State *lua) {
  return vectis_opcua_lua_client_read_guid_array_common(
      lua, 0, cpkt_opcua_client_read_guid_array,
      cpkt_opcua_client_read_guid_array_range, "opcua client read guid array");
}

static int vectis_opcua_lua_client_read_guid_array_range(lua_State *lua) {
  return vectis_opcua_lua_client_read_guid_array_common(
      lua, 1, cpkt_opcua_client_read_guid_array,
      cpkt_opcua_client_read_guid_array_range,
      "opcua client read guid array range");
}

typedef cpkt_opcua_result (*vectis_opcua_lua_client_read_string_cb_array_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, cpkt_opcua_string_array_fn, void *,
    size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (
    *vectis_opcua_lua_client_read_string_cb_array_range_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, const char *,
    cpkt_opcua_string_array_fn, void *, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_client_read_string_cb_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_client_read_string_cb_array_fn read_fn,
    vectis_opcua_lua_client_read_string_cb_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  const char *index_range;
  size_t value_count;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  status = 0u;
  value_count = 0u;
  lua_newtable(lua);
  result = has_range
               ? range_fn(client, node_id, index_range,
                          vectis_opcua_lua_string_array_cb, lua, &value_count,
                          &status)
               : read_fn(client, node_id, vectis_opcua_lua_string_array_cb, lua,
                         &value_count, &status);
  if (result != CPKT_OPCUA_OK) {
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  return 1;
}

static int vectis_opcua_lua_client_read_string_array(lua_State *lua) {
  return vectis_opcua_lua_client_read_string_cb_array_common(
      lua, 0, cpkt_opcua_client_read_string_array,
      cpkt_opcua_client_read_string_array_range,
      "opcua client read string array");
}

static int vectis_opcua_lua_client_read_string_array_range(lua_State *lua) {
  return vectis_opcua_lua_client_read_string_cb_array_common(
      lua, 1, cpkt_opcua_client_read_string_array,
      cpkt_opcua_client_read_string_array_range,
      "opcua client read string array range");
}

typedef cpkt_opcua_result (
    *vectis_opcua_lua_client_read_byte_string_cb_array_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, cpkt_opcua_byte_string_array_fn,
    void *, size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (
    *vectis_opcua_lua_client_read_byte_string_cb_array_range_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, const char *,
    cpkt_opcua_byte_string_array_fn, void *, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_client_read_byte_string_cb_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_client_read_byte_string_cb_array_fn read_fn,
    vectis_opcua_lua_client_read_byte_string_cb_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  const char *index_range;
  size_t value_count;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  status = 0u;
  value_count = 0u;
  lua_newtable(lua);
  result = has_range
               ? range_fn(client, node_id, index_range,
                          vectis_opcua_lua_byte_string_array_cb, lua,
                          &value_count, &status)
               : read_fn(client, node_id, vectis_opcua_lua_byte_string_array_cb,
                         lua, &value_count, &status);
  if (result != CPKT_OPCUA_OK) {
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  return 1;
}

static int vectis_opcua_lua_client_read_byte_string_array(lua_State *lua) {
  return vectis_opcua_lua_client_read_byte_string_cb_array_common(
      lua, 0, cpkt_opcua_client_read_byte_string_array,
      cpkt_opcua_client_read_byte_string_array_range,
      "opcua client read byte string array");
}

static int
vectis_opcua_lua_client_read_byte_string_array_range(lua_State *lua) {
  return vectis_opcua_lua_client_read_byte_string_cb_array_common(
      lua, 1, cpkt_opcua_client_read_byte_string_array,
      cpkt_opcua_client_read_byte_string_array_range,
      "opcua client read byte string array range");
}

typedef cpkt_opcua_result (
    *vectis_opcua_lua_client_read_qualified_name_cb_array_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, cpkt_opcua_qualified_name_array_fn,
    void *, size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (
    *vectis_opcua_lua_client_read_qualified_name_cb_array_range_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, const char *,
    cpkt_opcua_qualified_name_array_fn, void *, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_client_read_qualified_name_cb_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_client_read_qualified_name_cb_array_fn read_fn,
    vectis_opcua_lua_client_read_qualified_name_cb_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  const char *index_range;
  size_t value_count;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  status = 0u;
  value_count = 0u;
  lua_newtable(lua);
  result = has_range ? range_fn(client, node_id, index_range,
                                vectis_opcua_lua_qualified_name_array_cb, lua,
                                &value_count, &status)
                     : read_fn(client, node_id,
                               vectis_opcua_lua_qualified_name_array_cb, lua,
                               &value_count, &status);
  if (result != CPKT_OPCUA_OK) {
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  return 1;
}

static int vectis_opcua_lua_client_read_qualified_name_array(lua_State *lua) {
  return vectis_opcua_lua_client_read_qualified_name_cb_array_common(
      lua, 0, cpkt_opcua_client_read_qualified_name_array,
      cpkt_opcua_client_read_qualified_name_array_range,
      "opcua client read qualified name array");
}

static int
vectis_opcua_lua_client_read_qualified_name_array_range(lua_State *lua) {
  return vectis_opcua_lua_client_read_qualified_name_cb_array_common(
      lua, 1, cpkt_opcua_client_read_qualified_name_array,
      cpkt_opcua_client_read_qualified_name_array_range,
      "opcua client read qualified name array range");
}

typedef cpkt_opcua_result (
    *vectis_opcua_lua_client_read_localized_text_cb_array_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, cpkt_opcua_localized_text_array_fn,
    void *, size_t *, cpkt_opcua_status *);

typedef cpkt_opcua_result (
    *vectis_opcua_lua_client_read_localized_text_cb_array_range_fn)(
    cpkt_opcua_client *, cpkt_opcua_node_id, const char *,
    cpkt_opcua_localized_text_array_fn, void *, size_t *, cpkt_opcua_status *);

static int vectis_opcua_lua_client_read_localized_text_cb_array_common(
    lua_State *lua, int has_range,
    vectis_opcua_lua_client_read_localized_text_cb_array_fn read_fn,
    vectis_opcua_lua_client_read_localized_text_cb_array_range_fn range_fn,
    const char *context) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  const char *index_range;
  size_t value_count;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = has_range ? luaL_checkstring(lua, 3) : NULL;
  status = 0u;
  value_count = 0u;
  lua_newtable(lua);
  result = has_range ? range_fn(client, node_id, index_range,
                                vectis_opcua_lua_localized_text_array_cb, lua,
                                &value_count, &status)
                     : read_fn(client, node_id,
                               vectis_opcua_lua_localized_text_array_cb, lua,
                               &value_count, &status);
  if (result != CPKT_OPCUA_OK) {
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  return 1;
}

static int vectis_opcua_lua_client_read_localized_text_array(lua_State *lua) {
  return vectis_opcua_lua_client_read_localized_text_cb_array_common(
      lua, 0, cpkt_opcua_client_read_localized_text_array,
      cpkt_opcua_client_read_localized_text_array_range,
      "opcua client read localized text array");
}

static int
vectis_opcua_lua_client_read_localized_text_array_range(lua_State *lua) {
  return vectis_opcua_lua_client_read_localized_text_cb_array_common(
      lua, 1, cpkt_opcua_client_read_localized_text_array,
      cpkt_opcua_client_read_localized_text_array_range,
      "opcua client read localized text array range");
}

static int vectis_opcua_lua_client_write_index_range(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_value value;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  const char *index_range;

  client = vectis_opcua_lua_client_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = luaL_checkstring(lua, 3);
  vectis_opcua_lua_value_from_lua(lua, 4, &value);
  status = 0u;
  result = cpkt_opcua_client_write_index_range(client, node_id, index_range,
                                               &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client write index range");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static void
vectis_opcua_lua_browse_options_from_lua(lua_State *lua, int index,
                                         cpkt_opcua_browse_options *options) {
  int abs_index;

  cpkt_opcua_browse_options_default(options);
  if (lua_isnoneornil(lua, index)) {
    return;
  }
  luaL_checktype(lua, index, LUA_TTABLE);
  abs_index = lua_absindex(lua, index);
  options->browse_direction = vectis_opcua_lua_table_int(
      lua, abs_index, "browse_direction", options->browse_direction);
  options->browse_direction = vectis_opcua_lua_table_int(
      lua, abs_index, "direction", options->browse_direction);
  options->include_subtypes = vectis_opcua_lua_table_bool(
      lua, abs_index, "include_subtypes", options->include_subtypes);
  options->node_class_mask = vectis_opcua_lua_table_ulong(
      lua, abs_index, "node_class_mask", options->node_class_mask);
  options->result_mask = vectis_opcua_lua_table_ulong(
      lua, abs_index, "result_mask", options->result_mask);
  options->max_references = vectis_opcua_lua_table_ulong(
      lua, abs_index, "max_references", options->max_references);

  lua_getfield(lua, abs_index, "reference_type_id");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_getfield(lua, abs_index, "reference_type");
  }
  if (!lua_isnil(lua, -1)) {
    options->reference_type_id = vectis_opcua_lua_node_id_at(lua, -1);
    options->has_reference_type = 1;
  }
  lua_pop(lua, 1);
}

static void vectis_opcua_lua_push_optional_string(lua_State *lua,
                                                  const char *value) {
  if (value != NULL) {
    lua_pushstring(lua, value);
  } else {
    lua_pushnil(lua);
  }
}

static int
vectis_opcua_lua_browse_collect_cb(const cpkt_opcua_browse_entry *entry,
                                   void *user) {
  vectis_opcua_lua_browse_collect *collect;
  lua_State *lua;

  collect = (vectis_opcua_lua_browse_collect *)user;
  lua = collect->lua;
  lua_newtable(lua);
  (void)vectis_opcua_lua_push_node_id_copy(lua, &entry->target_node_id);
  lua_setfield(lua, -2, "target_node_id");
  lua_pushinteger(lua, (lua_Integer)entry->node_class);
  lua_setfield(lua, -2, "node_class");
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)entry->browse_name_namespace_index);
  lua_setfield(lua, -2, "namespace_index");
  vectis_opcua_lua_push_optional_string(lua, entry->browse_name);
  lua_setfield(lua, -2, "name");
  lua_setfield(lua, -2, "browse_name");
  vectis_opcua_lua_push_optional_string(lua, entry->display_name);
  lua_setfield(lua, -2, "display_name");
  lua_pushboolean(lua, entry->is_forward != 0);
  lua_setfield(lua, -2, "is_forward");
  ++collect->count;
  lua_rawseti(lua, collect->table_index, (lua_Integer)collect->count);
  return 0;
}

static int vectis_opcua_lua_push_browse_page_result(
    lua_State *lua, int entries_index, const unsigned char *continuation_point,
    size_t continuation_point_size) {
  entries_index = lua_absindex(lua, entries_index);
  lua_newtable(lua);
  lua_pushvalue(lua, entries_index);
  lua_setfield(lua, -2, "entries");
  if (continuation_point_size > 0u) {
    lua_pushlstring(lua, (const char *)continuation_point,
                    continuation_point_size);
  } else {
    lua_pushnil(lua);
  }
  lua_setfield(lua, -2, "continuation_point");
  lua_remove(lua, entries_index);
  return 1;
}

static int vectis_opcua_lua_client_browse_children_common(lua_State *lua,
                                                          int force_ex) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id parent_node_id;
  cpkt_opcua_browse_options options;
  vectis_opcua_lua_browse_collect collect;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int has_options;

  client = vectis_opcua_lua_client_handle(lua, 1);
  parent_node_id = vectis_opcua_lua_node_id_at(lua, 2);
  has_options = !lua_isnoneornil(lua, 3);
  lua_newtable(lua);
  collect.lua = lua;
  collect.table_index = lua_absindex(lua, -1);
  collect.count = 0u;
  status = 0u;
  if (force_ex || has_options) {
    vectis_opcua_lua_browse_options_from_lua(lua, 3, &options);
    result = cpkt_opcua_client_browse_children_ex(
        client, parent_node_id, &options, vectis_opcua_lua_browse_collect_cb,
        &collect, &status);
  } else {
    result = cpkt_opcua_client_browse_children(
        client, parent_node_id, vectis_opcua_lua_browse_collect_cb, &collect,
        &status);
  }
  if (result != CPKT_OPCUA_OK) {
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status,
                                       force_ex || has_options
                                           ? "opcua client browse children ex"
                                           : "opcua client browse children");
  }
  return 1;
}

static int vectis_opcua_lua_client_browse_children(lua_State *lua) {
  return vectis_opcua_lua_client_browse_children_common(lua, 0);
}

static int vectis_opcua_lua_client_browse_children_ex(lua_State *lua) {
  return vectis_opcua_lua_client_browse_children_common(lua, 1);
}

static int vectis_opcua_lua_client_browse_children_page(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id parent_node_id;
  cpkt_opcua_browse_options options;
  vectis_opcua_lua_browse_collect collect;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned char stack_buffer[256];
  unsigned char *buffer;
  size_t buffer_size;
  size_t required_size;

  client = vectis_opcua_lua_client_handle(lua, 1);
  parent_node_id = vectis_opcua_lua_node_id_at(lua, 2);
  vectis_opcua_lua_browse_options_from_lua(lua, 3, &options);
  lua_newtable(lua);
  collect.lua = lua;
  collect.table_index = lua_absindex(lua, -1);
  collect.count = 0u;
  buffer = stack_buffer;
  buffer_size = sizeof(stack_buffer);
  required_size = 0u;
  status = 0u;
  result = cpkt_opcua_client_browse_children_page(
      client, parent_node_id, &options, vectis_opcua_lua_browse_collect_cb,
      &collect, buffer, buffer_size, &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (unsigned char *)malloc(required_size);
    if (buffer == NULL) {
      lua_pop(lua, 1);
      return luaL_error(lua,
                        "opcua client browse continuation allocation failed");
    }
    buffer_size = required_size;
    lua_pop(lua, 1);
    lua_newtable(lua);
    collect.table_index = lua_absindex(lua, -1);
    collect.count = 0u;
    required_size = 0u;
    result = cpkt_opcua_client_browse_children_page(
        client, parent_node_id, &options, vectis_opcua_lua_browse_collect_cb,
        &collect, buffer, buffer_size, &required_size, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client browse children page");
  }
  result =
      vectis_opcua_lua_push_browse_page_result(lua, -1, buffer, required_size);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return result;
}

static int vectis_opcua_lua_client_browse_next(lua_State *lua) {
  cpkt_opcua_client *client;
  const char *continuation_point;
  size_t continuation_point_size;
  int release_continuation_point;
  vectis_opcua_lua_browse_collect collect;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned char stack_buffer[256];
  unsigned char *buffer;
  size_t buffer_size;
  size_t required_size;

  client = vectis_opcua_lua_client_handle(lua, 1);
  continuation_point = luaL_checklstring(lua, 2, &continuation_point_size);
  release_continuation_point =
      lua_isnoneornil(lua, 3) ? 0 : lua_toboolean(lua, 3);
  lua_newtable(lua);
  collect.lua = lua;
  collect.table_index = lua_absindex(lua, -1);
  collect.count = 0u;
  buffer = stack_buffer;
  buffer_size = sizeof(stack_buffer);
  required_size = 0u;
  status = 0u;
  result = cpkt_opcua_client_browse_next(
      client, (const unsigned char *)continuation_point,
      continuation_point_size, release_continuation_point,
      vectis_opcua_lua_browse_collect_cb, &collect, buffer, buffer_size,
      &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (unsigned char *)malloc(required_size);
    if (buffer == NULL) {
      lua_pop(lua, 1);
      return luaL_error(lua,
                        "opcua client browse continuation allocation failed");
    }
    buffer_size = required_size;
    lua_pop(lua, 1);
    lua_newtable(lua);
    collect.table_index = lua_absindex(lua, -1);
    collect.count = 0u;
    required_size = 0u;
    result = cpkt_opcua_client_browse_next(
        client, (const unsigned char *)continuation_point,
        continuation_point_size, release_continuation_point,
        vectis_opcua_lua_browse_collect_cb, &collect, buffer, buffer_size,
        &required_size, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client browse next");
  }
  result =
      vectis_opcua_lua_push_browse_page_result(lua, -1, buffer, required_size);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return result;
}

static int vectis_opcua_lua_client_read_method_argument_count(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id method_node_id;
  int direction;
  size_t count;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  method_node_id = vectis_opcua_lua_node_id_at(lua, 2);
  direction = (int)luaL_checkinteger(lua, 3);
  count = 0u;
  status = 0u;
  result = cpkt_opcua_client_read_method_argument_count(
      client, method_node_id, direction, &count, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(
        lua, result, status, "opcua client read method argument count");
  }
  lua_pushinteger(lua, (lua_Integer)count);
  return 1;
}

static int vectis_opcua_lua_client_read_method_argument(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id method_node_id;
  cpkt_opcua_node_id data_type;
  int direction;
  lua_Integer argument_index;
  long value_rank;
  char stack_buffer[256];
  char *buffer;
  size_t required_size;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  method_node_id = vectis_opcua_lua_node_id_at(lua, 2);
  direction = (int)luaL_checkinteger(lua, 3);
  argument_index = luaL_checkinteger(lua, 4);
  if (argument_index < 1) {
    return luaL_error(lua, "argument_index must be >= 1");
  }
  buffer = stack_buffer;
  required_size = 0u;
  value_rank = 0;
  data_type = cpkt_opcua_node_id_null();
  status = 0u;
  result = cpkt_opcua_client_read_method_argument(
      client, method_node_id, direction, (size_t)(argument_index - 1),
      &data_type, &value_rank, buffer, sizeof(stack_buffer), &required_size,
      &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua client method argument allocation failed");
    }
    data_type = cpkt_opcua_node_id_null();
    result = cpkt_opcua_client_read_method_argument(
        client, method_node_id, direction, (size_t)(argument_index - 1),
        &data_type, &value_rank, buffer, required_size + 1u, NULL, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client read method argument");
  }
  lua_newtable(lua);
  (void)vectis_opcua_lua_push_node_id_copy(lua, &data_type);
  lua_setfield(lua, -2, "data_type");
  lua_pushinteger(lua, (lua_Integer)value_rank);
  lua_setfield(lua, -2, "value_rank");
  lua_pushstring(lua, buffer);
  lua_setfield(lua, -2, "name");
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_client_call_method(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id object_node_id;
  cpkt_opcua_node_id method_node_id;
  cpkt_opcua_value *inputs;
  size_t input_count;
  cpkt_opcua_value output;
  char stack_buffer[512];
  char *buffer;
  size_t required_size;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  object_node_id = vectis_opcua_lua_node_id_at(lua, 2);
  method_node_id = vectis_opcua_lua_node_id_at(lua, 3);
  inputs = vectis_opcua_lua_value_array_from_lua(lua, 4, &input_count,
                                                 "opcua client method inputs");
  buffer = stack_buffer;
  required_size = 0u;
  cpkt_opcua_value_clear(&output);
  status = 0u;
  result = cpkt_opcua_client_call_method(
      client, object_node_id, method_node_id, inputs, input_count, &output,
      buffer, sizeof(stack_buffer), &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      free(inputs);
      return luaL_error(lua, "opcua client method output allocation failed");
    }
    cpkt_opcua_value_clear(&output);
    result = cpkt_opcua_client_call_method(
        client, object_node_id, method_node_id, inputs, input_count, &output,
        buffer, required_size + 1u, NULL, &status);
  }
  free(inputs);
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client call method");
  }
  (void)vectis_opcua_lua_push_value_copy(lua, &output);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static void vectis_opcua_lua_free_method_many_buffers(char **buffers,
                                                      size_t count) {
  size_t i;

  if (buffers == NULL) {
    return;
  }
  for (i = 0u; i < count; ++i) {
    free(buffers[i]);
  }
  free(buffers);
}

static int vectis_opcua_lua_client_call_method_many(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id object_node_id;
  cpkt_opcua_node_id method_node_id;
  cpkt_opcua_value *inputs;
  cpkt_opcua_value *outputs;
  char **buffers;
  size_t *buffer_sizes;
  size_t *required_sizes;
  size_t input_count;
  size_t output_count;
  size_t i;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  client = vectis_opcua_lua_client_handle(lua, 1);
  object_node_id = vectis_opcua_lua_node_id_at(lua, 2);
  method_node_id = vectis_opcua_lua_node_id_at(lua, 3);
  inputs = vectis_opcua_lua_value_array_from_lua(lua, 4, &input_count,
                                                 "opcua client method inputs");
  output_count = (size_t)vectis_opcua_lua_check_ulong(lua, 5, "output_count");
  if (output_count == 0u) {
    free(inputs);
    return luaL_error(lua, "output_count must be non-zero");
  }
  outputs = (cpkt_opcua_value *)calloc(output_count, sizeof(*outputs));
  buffers = (char **)calloc(output_count, sizeof(*buffers));
  buffer_sizes = (size_t *)calloc(output_count, sizeof(*buffer_sizes));
  required_sizes = (size_t *)calloc(output_count, sizeof(*required_sizes));
  if (outputs == NULL || buffers == NULL || buffer_sizes == NULL ||
      required_sizes == NULL) {
    free(inputs);
    free(outputs);
    vectis_opcua_lua_free_method_many_buffers(buffers, output_count);
    free(buffer_sizes);
    free(required_sizes);
    return luaL_error(lua, "opcua client method many allocation failed");
  }
  for (i = 0u; i < output_count; ++i) {
    buffer_sizes[i] = 512u;
    buffers[i] = (char *)malloc(buffer_sizes[i]);
    if (buffers[i] == NULL) {
      free(inputs);
      free(outputs);
      vectis_opcua_lua_free_method_many_buffers(buffers, output_count);
      free(buffer_sizes);
      free(required_sizes);
      return luaL_error(lua,
                        "opcua client method many buffer allocation failed");
    }
  }
  status = 0u;
  result = cpkt_opcua_client_call_method_many(
      client, object_node_id, method_node_id, inputs, input_count, outputs,
      output_count, buffers, buffer_sizes, required_sizes, &status);
  if (result == CPKT_OPCUA_ERR_RANGE) {
    for (i = 0u; i < output_count; ++i) {
      if (required_sizes[i] > buffer_sizes[i]) {
        char *new_buffer;

        new_buffer = (char *)realloc(buffers[i], required_sizes[i] + 1u);
        if (new_buffer == NULL) {
          free(inputs);
          free(outputs);
          vectis_opcua_lua_free_method_many_buffers(buffers, output_count);
          free(buffer_sizes);
          free(required_sizes);
          return luaL_error(
              lua, "opcua client method many output allocation failed");
        }
        buffers[i] = new_buffer;
        buffer_sizes[i] = required_sizes[i] + 1u;
      }
      required_sizes[i] = 0u;
      cpkt_opcua_value_clear(&outputs[i]);
    }
    result = cpkt_opcua_client_call_method_many(
        client, object_node_id, method_node_id, inputs, input_count, outputs,
        output_count, buffers, buffer_sizes, required_sizes, &status);
  }
  free(inputs);
  if (result != CPKT_OPCUA_OK) {
    free(outputs);
    vectis_opcua_lua_free_method_many_buffers(buffers, output_count);
    free(buffer_sizes);
    free(required_sizes);
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client call method many");
  }
  lua_newtable(lua);
  for (i = 0u; i < output_count; ++i) {
    (void)vectis_opcua_lua_push_value_copy(lua, &outputs[i]);
    lua_rawseti(lua, -2, (lua_Integer)i + 1);
  }
  free(outputs);
  vectis_opcua_lua_free_method_many_buffers(buffers, output_count);
  free(buffer_sizes);
  free(required_sizes);
  return 1;
}

static int vectis_opcua_lua_client_translate_browse_path(lua_State *lua) {
  cpkt_opcua_client *client;
  cpkt_opcua_node_id start_node_id;
  cpkt_opcua_node_id target_node_id;
  cpkt_opcua_browse_path_element *elements;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  char stack_buffer[256];
  char *buffer;
  size_t required_size;
  size_t count;
  size_t i;

  client = vectis_opcua_lua_client_handle(lua, 1);
  start_node_id = vectis_opcua_lua_node_id_at(lua, 2);
  count = vectis_opcua_lua_array_len(lua, 3, "browse path");
  elements = (cpkt_opcua_browse_path_element *)malloc(
      sizeof(cpkt_opcua_browse_path_element) * (count == 0u ? 1u : count));
  if (elements == NULL) {
    return luaL_error(lua, "opcua client browse path allocation failed");
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 3, (lua_Integer)i + 1);
    luaL_checktype(lua, -1, LUA_TTABLE);
    elements[i].namespace_index =
        vectis_opcua_lua_table_ushort(lua, -1, "namespace_index", 0u);
    elements[i].browse_name = vectis_opcua_lua_table_string(lua, -1, "name");
    if (elements[i].browse_name == NULL) {
      elements[i].browse_name =
          vectis_opcua_lua_table_string(lua, -1, "browse_name");
    }
    if (elements[i].browse_name == NULL || elements[i].browse_name[0] == '\0') {
      free(elements);
      return luaL_error(lua, "opcua client browse path element requires name");
    }
    lua_pop(lua, 1);
  }
  buffer = stack_buffer;
  required_size = 0u;
  target_node_id = cpkt_opcua_node_id_null();
  status = 0u;
  result = cpkt_opcua_client_translate_browse_path(
      client, start_node_id, elements, count, &target_node_id, buffer,
      sizeof(stack_buffer), &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      free(elements);
      return luaL_error(lua,
                        "opcua client browse path target allocation failed");
    }
    target_node_id = cpkt_opcua_node_id_null();
    result = cpkt_opcua_client_translate_browse_path(
        client, start_node_id, elements, count, &target_node_id, buffer,
        required_size + 1u, NULL, &status);
  }
  free(elements);
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua client translate browse path");
  }
  (void)vectis_opcua_lua_push_node_id_copy(lua, &target_node_id);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_server_read_integer_array_common(lua_State *lua,
                                                             int has_range) {
  return vectis_opcua_lua_server_read_long_array_common(
      lua, has_range, cpkt_opcua_server_read_integer_array,
      cpkt_opcua_server_read_integer_array_range,
      has_range ? "opcua server read integer array range"
                : "opcua server read integer array");
}

static int vectis_opcua_lua_server_read_integer_array(lua_State *lua) {
  return vectis_opcua_lua_server_read_integer_array_common(lua, 0);
}

static int vectis_opcua_lua_server_read_integer_array_range(lua_State *lua) {
  return vectis_opcua_lua_server_read_integer_array_common(lua, 1);
}

static int vectis_opcua_lua_server_read_boolean_array(lua_State *lua) {
  return vectis_opcua_lua_server_read_int_array_common(
      lua, 0, cpkt_opcua_server_read_boolean_array,
      cpkt_opcua_server_read_boolean_array_range,
      "opcua server read boolean array");
}

static int vectis_opcua_lua_server_read_boolean_array_range(lua_State *lua) {
  return vectis_opcua_lua_server_read_int_array_common(
      lua, 1, cpkt_opcua_server_read_boolean_array,
      cpkt_opcua_server_read_boolean_array_range,
      "opcua server read boolean array range");
}

static int vectis_opcua_lua_server_read_double_array(lua_State *lua) {
  return vectis_opcua_lua_server_read_double_array_common(
      lua, 0, cpkt_opcua_server_read_double_array,
      cpkt_opcua_server_read_double_array_range,
      "opcua server read double array");
}

static int vectis_opcua_lua_server_read_double_array_range(lua_State *lua) {
  return vectis_opcua_lua_server_read_double_array_common(
      lua, 1, cpkt_opcua_server_read_double_array,
      cpkt_opcua_server_read_double_array_range,
      "opcua server read double array range");
}

static int vectis_opcua_lua_server_read_string_array(lua_State *lua) {
  return vectis_opcua_lua_server_read_string_cb_array_common(
      lua, 0, cpkt_opcua_server_read_string_array,
      cpkt_opcua_server_read_string_array_range,
      "opcua server read string array");
}

static int vectis_opcua_lua_server_read_string_array_range(lua_State *lua) {
  return vectis_opcua_lua_server_read_string_cb_array_common(
      lua, 1, cpkt_opcua_server_read_string_array,
      cpkt_opcua_server_read_string_array_range,
      "opcua server read string array range");
}

static int vectis_opcua_lua_server_read_byte_string_array(lua_State *lua) {
  return vectis_opcua_lua_server_read_byte_string_cb_array_common(
      lua, 0, cpkt_opcua_server_read_byte_string_array,
      cpkt_opcua_server_read_byte_string_array_range,
      "opcua server read byte string array");
}

static int
vectis_opcua_lua_server_read_byte_string_array_range(lua_State *lua) {
  return vectis_opcua_lua_server_read_byte_string_cb_array_common(
      lua, 1, cpkt_opcua_server_read_byte_string_array,
      cpkt_opcua_server_read_byte_string_array_range,
      "opcua server read byte string array range");
}

static int vectis_opcua_lua_server_read_uint64_array(lua_State *lua) {
  return vectis_opcua_lua_server_read_uint64_array_common(
      lua, 0, cpkt_opcua_server_read_uint64_array,
      cpkt_opcua_server_read_uint64_array_range,
      "opcua server read uint64 array");
}

static int vectis_opcua_lua_server_read_uint64_array_range(lua_State *lua) {
  return vectis_opcua_lua_server_read_uint64_array_common(
      lua, 1, cpkt_opcua_server_read_uint64_array,
      cpkt_opcua_server_read_uint64_array_range,
      "opcua server read uint64 array range");
}

static int vectis_opcua_lua_server_read_datetime_array(lua_State *lua) {
  return vectis_opcua_lua_server_read_datetime_array_common(
      lua, 0, cpkt_opcua_server_read_datetime_array,
      cpkt_opcua_server_read_datetime_array_range,
      "opcua server read datetime array");
}

static int vectis_opcua_lua_server_read_datetime_array_range(lua_State *lua) {
  return vectis_opcua_lua_server_read_datetime_array_common(
      lua, 1, cpkt_opcua_server_read_datetime_array,
      cpkt_opcua_server_read_datetime_array_range,
      "opcua server read datetime array range");
}

static int vectis_opcua_lua_server_read_status_array(lua_State *lua) {
  return vectis_opcua_lua_server_read_status_array_common(
      lua, 0, cpkt_opcua_server_read_status_array,
      cpkt_opcua_server_read_status_array_range,
      "opcua server read status array");
}

static int vectis_opcua_lua_server_read_status_array_range(lua_State *lua) {
  return vectis_opcua_lua_server_read_status_array_common(
      lua, 1, cpkt_opcua_server_read_status_array,
      cpkt_opcua_server_read_status_array_range,
      "opcua server read status array range");
}

static int vectis_opcua_lua_server_read_guid_array(lua_State *lua) {
  return vectis_opcua_lua_server_read_guid_array_common(
      lua, 0, cpkt_opcua_server_read_guid_array,
      cpkt_opcua_server_read_guid_array_range, "opcua server read guid array");
}

static int vectis_opcua_lua_server_read_guid_array_range(lua_State *lua) {
  return vectis_opcua_lua_server_read_guid_array_common(
      lua, 1, cpkt_opcua_server_read_guid_array,
      cpkt_opcua_server_read_guid_array_range,
      "opcua server read guid array range");
}

static int vectis_opcua_lua_server_read_qualified_name_array(lua_State *lua) {
  return vectis_opcua_lua_server_read_qualified_name_cb_array_common(
      lua, 0, cpkt_opcua_server_read_qualified_name_array,
      cpkt_opcua_server_read_qualified_name_array_range,
      "opcua server read qualified name array");
}

static int
vectis_opcua_lua_server_read_qualified_name_array_range(lua_State *lua) {
  return vectis_opcua_lua_server_read_qualified_name_cb_array_common(
      lua, 1, cpkt_opcua_server_read_qualified_name_array,
      cpkt_opcua_server_read_qualified_name_array_range,
      "opcua server read qualified name array range");
}

static int vectis_opcua_lua_server_read_localized_text_array(lua_State *lua) {
  return vectis_opcua_lua_server_read_localized_text_cb_array_common(
      lua, 0, cpkt_opcua_server_read_localized_text_array,
      cpkt_opcua_server_read_localized_text_array_range,
      "opcua server read localized text array");
}

static int
vectis_opcua_lua_server_read_localized_text_array_range(lua_State *lua) {
  return vectis_opcua_lua_server_read_localized_text_cb_array_common(
      lua, 1, cpkt_opcua_server_read_localized_text_array,
      cpkt_opcua_server_read_localized_text_array_range,
      "opcua server read localized text array range");
}

static int vectis_opcua_lua_server_write_index_range(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id node_id;
  cpkt_opcua_value value;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  const char *index_range;

  server = vectis_opcua_lua_server_handle(lua, 1);
  node_id = vectis_opcua_lua_node_id_at(lua, 2);
  index_range = luaL_checkstring(lua, 3);
  vectis_opcua_lua_value_from_lua(lua, 4, &value);
  status = 0u;
  result = cpkt_opcua_server_write_index_range(server, node_id, index_range,
                                               &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server write index range");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_browse_children_common(lua_State *lua,
                                                          int force_ex) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id parent_node_id;
  cpkt_opcua_browse_options options;
  vectis_opcua_lua_browse_collect collect;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  int has_options;

  server = vectis_opcua_lua_server_handle(lua, 1);
  parent_node_id = vectis_opcua_lua_node_id_at(lua, 2);
  has_options = !lua_isnoneornil(lua, 3);
  lua_newtable(lua);
  collect.lua = lua;
  collect.table_index = lua_absindex(lua, -1);
  collect.count = 0u;
  status = 0u;
  if (force_ex || has_options) {
    vectis_opcua_lua_browse_options_from_lua(lua, 3, &options);
    result = cpkt_opcua_server_browse_children_ex(
        server, parent_node_id, &options, vectis_opcua_lua_browse_collect_cb,
        &collect, &status);
  } else {
    result = cpkt_opcua_server_browse_children(
        server, parent_node_id, vectis_opcua_lua_browse_collect_cb, &collect,
        &status);
  }
  if (result != CPKT_OPCUA_OK) {
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status,
                                       force_ex || has_options
                                           ? "opcua server browse children ex"
                                           : "opcua server browse children");
  }
  return 1;
}

static int vectis_opcua_lua_server_browse_children(lua_State *lua) {
  return vectis_opcua_lua_server_browse_children_common(lua, 0);
}

static int vectis_opcua_lua_server_browse_children_ex(lua_State *lua) {
  return vectis_opcua_lua_server_browse_children_common(lua, 1);
}

static int vectis_opcua_lua_server_browse_children_page(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id parent_node_id;
  cpkt_opcua_browse_options options;
  vectis_opcua_lua_browse_collect collect;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned char stack_buffer[256];
  unsigned char *buffer;
  size_t buffer_size;
  size_t required_size;

  server = vectis_opcua_lua_server_handle(lua, 1);
  parent_node_id = vectis_opcua_lua_node_id_at(lua, 2);
  vectis_opcua_lua_browse_options_from_lua(lua, 3, &options);
  lua_newtable(lua);
  collect.lua = lua;
  collect.table_index = lua_absindex(lua, -1);
  collect.count = 0u;
  buffer = stack_buffer;
  buffer_size = sizeof(stack_buffer);
  required_size = 0u;
  status = 0u;
  result = cpkt_opcua_server_browse_children_page(
      server, parent_node_id, &options, vectis_opcua_lua_browse_collect_cb,
      &collect, buffer, buffer_size, &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (unsigned char *)malloc(required_size);
    if (buffer == NULL) {
      lua_pop(lua, 1);
      return luaL_error(lua,
                        "opcua server browse continuation allocation failed");
    }
    buffer_size = required_size;
    lua_pop(lua, 1);
    lua_newtable(lua);
    collect.table_index = lua_absindex(lua, -1);
    collect.count = 0u;
    required_size = 0u;
    result = cpkt_opcua_server_browse_children_page(
        server, parent_node_id, &options, vectis_opcua_lua_browse_collect_cb,
        &collect, buffer, buffer_size, &required_size, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server browse children page");
  }
  result =
      vectis_opcua_lua_push_browse_page_result(lua, -1, buffer, required_size);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return result;
}

static int vectis_opcua_lua_server_browse_next(lua_State *lua) {
  cpkt_opcua_server *server;
  const char *continuation_point;
  size_t continuation_point_size;
  int release_continuation_point;
  vectis_opcua_lua_browse_collect collect;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned char stack_buffer[256];
  unsigned char *buffer;
  size_t buffer_size;
  size_t required_size;

  server = vectis_opcua_lua_server_handle(lua, 1);
  continuation_point = luaL_checklstring(lua, 2, &continuation_point_size);
  release_continuation_point =
      lua_isnoneornil(lua, 3) ? 0 : lua_toboolean(lua, 3);
  lua_newtable(lua);
  collect.lua = lua;
  collect.table_index = lua_absindex(lua, -1);
  collect.count = 0u;
  buffer = stack_buffer;
  buffer_size = sizeof(stack_buffer);
  required_size = 0u;
  status = 0u;
  result = cpkt_opcua_server_browse_next(
      server, (const unsigned char *)continuation_point,
      continuation_point_size, release_continuation_point,
      vectis_opcua_lua_browse_collect_cb, &collect, buffer, buffer_size,
      &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (unsigned char *)malloc(required_size);
    if (buffer == NULL) {
      lua_pop(lua, 1);
      return luaL_error(lua,
                        "opcua server browse continuation allocation failed");
    }
    buffer_size = required_size;
    lua_pop(lua, 1);
    lua_newtable(lua);
    collect.table_index = lua_absindex(lua, -1);
    collect.count = 0u;
    required_size = 0u;
    result = cpkt_opcua_server_browse_next(
        server, (const unsigned char *)continuation_point,
        continuation_point_size, release_continuation_point,
        vectis_opcua_lua_browse_collect_cb, &collect, buffer, buffer_size,
        &required_size, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server browse next");
  }
  result =
      vectis_opcua_lua_push_browse_page_result(lua, -1, buffer, required_size);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return result;
}

static int vectis_opcua_lua_server_read_method_argument_count(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id method_node_id;
  int direction;
  size_t count;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  method_node_id = vectis_opcua_lua_node_id_at(lua, 2);
  direction = (int)luaL_checkinteger(lua, 3);
  count = 0u;
  status = 0u;
  result = cpkt_opcua_server_read_method_argument_count(
      server, method_node_id, direction, &count, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(
        lua, result, status, "opcua server read method argument count");
  }
  lua_pushinteger(lua, (lua_Integer)count);
  return 1;
}

static int vectis_opcua_lua_server_read_method_argument(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id method_node_id;
  cpkt_opcua_node_id data_type;
  int direction;
  lua_Integer argument_index;
  long value_rank;
  char stack_buffer[256];
  char *buffer;
  size_t required_size;
  cpkt_opcua_status status;
  cpkt_opcua_result result;

  server = vectis_opcua_lua_server_handle(lua, 1);
  method_node_id = vectis_opcua_lua_node_id_at(lua, 2);
  direction = (int)luaL_checkinteger(lua, 3);
  argument_index = luaL_checkinteger(lua, 4);
  if (argument_index < 1) {
    return luaL_error(lua, "argument_index must be >= 1");
  }
  buffer = stack_buffer;
  required_size = 0u;
  value_rank = 0;
  data_type = cpkt_opcua_node_id_null();
  status = 0u;
  result = cpkt_opcua_server_read_method_argument(
      server, method_node_id, direction, (size_t)(argument_index - 1),
      &data_type, &value_rank, buffer, sizeof(stack_buffer), &required_size,
      &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      return luaL_error(lua, "opcua server method argument allocation failed");
    }
    data_type = cpkt_opcua_node_id_null();
    result = cpkt_opcua_server_read_method_argument(
        server, method_node_id, direction, (size_t)(argument_index - 1),
        &data_type, &value_rank, buffer, required_size + 1u, NULL, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server read method argument");
  }
  lua_newtable(lua);
  (void)vectis_opcua_lua_push_node_id_copy(lua, &data_type);
  lua_setfield(lua, -2, "data_type");
  lua_pushinteger(lua, (lua_Integer)value_rank);
  lua_setfield(lua, -2, "value_rank");
  lua_pushstring(lua, buffer);
  lua_setfield(lua, -2, "name");
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static int vectis_opcua_lua_server_translate_browse_path(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id start_node_id;
  cpkt_opcua_node_id target_node_id;
  cpkt_opcua_browse_path_element *elements;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  char stack_buffer[256];
  char *buffer;
  size_t required_size;
  size_t count;
  size_t i;

  server = vectis_opcua_lua_server_handle(lua, 1);
  start_node_id = vectis_opcua_lua_node_id_at(lua, 2);
  count = vectis_opcua_lua_array_len(lua, 3, "browse path");
  elements = (cpkt_opcua_browse_path_element *)malloc(
      sizeof(cpkt_opcua_browse_path_element) * (count == 0u ? 1u : count));
  if (elements == NULL) {
    return luaL_error(lua, "opcua browse path allocation failed");
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, 3, (lua_Integer)i + 1);
    luaL_checktype(lua, -1, LUA_TTABLE);
    elements[i].namespace_index =
        vectis_opcua_lua_table_ushort(lua, -1, "namespace_index", 0u);
    elements[i].browse_name = vectis_opcua_lua_table_string(lua, -1, "name");
    if (elements[i].browse_name == NULL) {
      elements[i].browse_name =
          vectis_opcua_lua_table_string(lua, -1, "browse_name");
    }
    if (elements[i].browse_name == NULL || elements[i].browse_name[0] == '\0') {
      free(elements);
      return luaL_error(lua, "opcua browse path element requires name");
    }
    lua_pop(lua, 1);
  }
  buffer = stack_buffer;
  required_size = 0u;
  target_node_id = cpkt_opcua_node_id_null();
  status = 0u;
  result = cpkt_opcua_server_translate_browse_path(
      server, start_node_id, elements, count, &target_node_id, buffer,
      sizeof(stack_buffer), &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE && required_size > sizeof(stack_buffer)) {
    buffer = (char *)malloc(required_size + 1u);
    if (buffer == NULL) {
      free(elements);
      return luaL_error(lua, "opcua browse path target allocation failed");
    }
    target_node_id = cpkt_opcua_node_id_null();
    result = cpkt_opcua_server_translate_browse_path(
        server, start_node_id, elements, count, &target_node_id, buffer,
        required_size + 1u, NULL, &status);
  }
  free(elements);
  if (result != CPKT_OPCUA_OK) {
    if (buffer != stack_buffer) {
      free(buffer);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server translate browse path");
  }
  (void)vectis_opcua_lua_push_node_id_copy(lua, &target_node_id);
  if (buffer != stack_buffer) {
    free(buffer);
  }
  return 1;
}

static vectis_opcua_lua_event *vectis_opcua_lua_check_event(lua_State *lua,
                                                            int index) {
  return (vectis_opcua_lua_event *)luaL_checkudata(lua, index,
                                                   VECTIS_OPCUA_EVENT);
}

static cpkt_opcua_server_event *vectis_opcua_lua_event_handle(lua_State *lua,
                                                              int index) {
  cpkt_opcua_server_event *event;

  event = vectis_opcua_lua_check_event(lua, index)->event;
  if (event == NULL) {
    (void)luaL_error(lua, "opcua event is closed");
  }
  return event;
}

static int vectis_opcua_lua_event_close(lua_State *lua) {
  vectis_opcua_lua_event *event;

  event = vectis_opcua_lua_check_event(lua, 1);
  if (event->event != NULL) {
    cpkt_opcua_server_event_free(event->event);
    event->event = NULL;
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_opcua_lua_server_create_event(lua_State *lua) {
  cpkt_opcua_node_id source_node_id;
  cpkt_opcua_node_id event_type_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  vectis_opcua_lua_event *event;
  const char *message;
  unsigned long severity;

  (void)vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  source_node_id = vectis_opcua_lua_node_id_field(lua, 2, "source_node_id",
                                                  "opcua server create_event");
  event_type_id = vectis_opcua_lua_node_id_field(lua, 2, "event_type_id",
                                                 "opcua server create_event");
  severity = vectis_opcua_lua_table_ulong(lua, 2, "severity", 0u);
  message = vectis_opcua_lua_table_string(lua, 2, "message");
  event = (vectis_opcua_lua_event *)lua_newuserdatauv(lua, sizeof(*event), 0);
  event->event = NULL;
  luaL_getmetatable(lua, VECTIS_OPCUA_EVENT);
  lua_setmetatable(lua, -2);
  status = 0u;
  result = cpkt_opcua_server_create_event(
      source_node_id, event_type_id, severity, message, &event->event, &status);
  if (result != CPKT_OPCUA_OK) {
    event->event = NULL;
    lua_pop(lua, 1);
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server create event");
  }
  return 1;
}

static int vectis_opcua_lua_event_set_field(lua_State *lua) {
  cpkt_opcua_server_event *event;
  cpkt_opcua_value value;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned short namespace_index;
  const char *field_name;

  event = vectis_opcua_lua_event_handle(lua, 1);
  namespace_index = vectis_opcua_lua_check_ushort(lua, 2, "namespace_index");
  field_name = luaL_checkstring(lua, 3);
  vectis_opcua_lua_value_from_lua(lua, 4, &value);
  status = 0u;
  result = cpkt_opcua_server_event_set_field(event, namespace_index, field_name,
                                             &value, &status);
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua event set field");
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int
vectis_opcua_lua_push_event_id(lua_State *lua, cpkt_opcua_result result,
                               cpkt_opcua_status status, const char *context,
                               unsigned char *event_id, size_t event_id_size,
                               size_t required_size) {
  if (result != CPKT_OPCUA_OK) {
    return vectis_opcua_lua_push_error(lua, result, status, context);
  }
  lua_pushlstring(lua, (const char *)event_id,
                  required_size <= event_id_size ? required_size
                                                 : event_id_size);
  return 1;
}

static int vectis_opcua_lua_event_trigger(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_server_event *event;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned char stack_event_id[64];
  unsigned char *event_id;
  size_t required_size;

  event = vectis_opcua_lua_event_handle(lua, 1);
  server = vectis_opcua_lua_server_handle(lua, 2);
  event_id = stack_event_id;
  required_size = 0u;
  status = 0u;
  result = cpkt_opcua_server_event_trigger(
      server, event, event_id, sizeof(stack_event_id), &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE &&
      required_size > sizeof(stack_event_id)) {
    event_id = (unsigned char *)malloc(required_size);
    if (event_id == NULL) {
      return luaL_error(lua, "opcua event id allocation failed");
    }
    result = cpkt_opcua_server_event_trigger(server, event, event_id,
                                             required_size, NULL, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (event_id != stack_event_id) {
      free(event_id);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua event trigger");
  }
  (void)vectis_opcua_lua_push_event_id(
      lua, result, status, "opcua event trigger", event_id,
      event_id == stack_event_id ? sizeof(stack_event_id) : required_size,
      required_size);
  if (event_id != stack_event_id) {
    free(event_id);
  }
  return 1;
}

static int vectis_opcua_lua_server_trigger_event(lua_State *lua) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id source_node_id;
  cpkt_opcua_node_id event_type_id;
  cpkt_opcua_status status;
  cpkt_opcua_result result;
  unsigned char stack_event_id[64];
  unsigned char *event_id;
  size_t required_size;
  const char *message;
  unsigned long severity;

  server = vectis_opcua_lua_server_handle(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  source_node_id = vectis_opcua_lua_node_id_field(lua, 2, "source_node_id",
                                                  "opcua server trigger_event");
  event_type_id = vectis_opcua_lua_node_id_field(lua, 2, "event_type_id",
                                                 "opcua server trigger_event");
  severity = vectis_opcua_lua_table_ulong(lua, 2, "severity", 0u);
  message = vectis_opcua_lua_table_string(lua, 2, "message");
  event_id = stack_event_id;
  required_size = 0u;
  status = 0u;
  result = cpkt_opcua_server_trigger_event(
      server, source_node_id, event_type_id, severity, message, event_id,
      sizeof(stack_event_id), &required_size, &status);
  if (result == CPKT_OPCUA_ERR_RANGE &&
      required_size > sizeof(stack_event_id)) {
    event_id = (unsigned char *)malloc(required_size);
    if (event_id == NULL) {
      return luaL_error(lua, "opcua event id allocation failed");
    }
    result = cpkt_opcua_server_trigger_event(
        server, source_node_id, event_type_id, severity, message, event_id,
        required_size, NULL, &status);
  }
  if (result != CPKT_OPCUA_OK) {
    if (event_id != stack_event_id) {
      free(event_id);
    }
    return vectis_opcua_lua_push_error(lua, result, status,
                                       "opcua server trigger event");
  }
  (void)vectis_opcua_lua_push_event_id(
      lua, result, status, "opcua server trigger event", event_id,
      event_id == stack_event_id ? sizeof(stack_event_id) : required_size,
      required_size);
  if (event_id != stack_event_id) {
    free(event_id);
  }
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
  luaL_Reg methods[] = {
      {"connect", vectis_opcua_lua_client_connect},
      {"disconnect", vectis_opcua_lua_client_disconnect},
      {"iterate", vectis_opcua_lua_client_iterate},
      {"read", vectis_opcua_lua_client_read},
      {"write", vectis_opcua_lua_client_write},
      {"add_object", vectis_opcua_lua_client_add_object},
      {"add_variable", vectis_opcua_lua_client_add_variable},
      {"add_variable_under", vectis_opcua_lua_client_add_variable_under},
      {"add_object_type", vectis_opcua_lua_client_add_object_type},
      {"add_variable_type", vectis_opcua_lua_client_add_variable_type},
      {"add_reference_type", vectis_opcua_lua_client_add_reference_type},
      {"add_data_type", vectis_opcua_lua_client_add_data_type},
      {"add_view", vectis_opcua_lua_client_add_view},
      {"delete_node", vectis_opcua_lua_client_delete_node},
      {"add_reference", vectis_opcua_lua_client_add_reference},
      {"delete_reference", vectis_opcua_lua_client_delete_reference},
      {"read_node_id", vectis_opcua_lua_client_read_node_id},
      {"read_node_class", vectis_opcua_lua_client_read_node_class},
      {"read_browse_name", vectis_opcua_lua_client_read_browse_name},
      {"read_display_name", vectis_opcua_lua_client_read_display_name},
      {"read_description", vectis_opcua_lua_client_read_description},
      {"write_display_name", vectis_opcua_lua_client_write_display_name},
      {"write_description", vectis_opcua_lua_client_write_description},
      {"read_write_mask", vectis_opcua_lua_client_read_write_mask},
      {"read_user_write_mask", vectis_opcua_lua_client_read_user_write_mask},
      {"write_write_mask", vectis_opcua_lua_client_write_write_mask},
      {"read_is_abstract", vectis_opcua_lua_client_read_is_abstract},
      {"write_is_abstract", vectis_opcua_lua_client_write_is_abstract},
      {"read_symmetric", vectis_opcua_lua_client_read_symmetric},
      {"write_symmetric", vectis_opcua_lua_client_write_symmetric},
      {"read_inverse_name", vectis_opcua_lua_client_read_inverse_name},
      {"write_inverse_name", vectis_opcua_lua_client_write_inverse_name},
      {"read_contains_no_loops",
       vectis_opcua_lua_client_read_contains_no_loops},
      {"write_contains_no_loops",
       vectis_opcua_lua_client_write_contains_no_loops},
      {"read_event_notifier", vectis_opcua_lua_client_read_event_notifier},
      {"write_event_notifier", vectis_opcua_lua_client_write_event_notifier},
      {"read_data_type", vectis_opcua_lua_client_read_data_type},
      {"write_data_type", vectis_opcua_lua_client_write_data_type},
      {"read_value_rank", vectis_opcua_lua_client_read_value_rank},
      {"write_value_rank", vectis_opcua_lua_client_write_value_rank},
      {"read_access_level", vectis_opcua_lua_client_read_access_level},
      {"read_user_access_level",
       vectis_opcua_lua_client_read_user_access_level},
      {"write_access_level", vectis_opcua_lua_client_write_access_level},
      {"read_access_level_ex", vectis_opcua_lua_client_read_access_level_ex},
      {"write_access_level_ex", vectis_opcua_lua_client_write_access_level_ex},
      {"read_minimum_sampling_interval",
       vectis_opcua_lua_client_read_minimum_sampling_interval},
      {"write_minimum_sampling_interval",
       vectis_opcua_lua_client_write_minimum_sampling_interval},
      {"read_historizing", vectis_opcua_lua_client_read_historizing},
      {"write_historizing", vectis_opcua_lua_client_write_historizing},
      {"read_executable", vectis_opcua_lua_client_read_executable},
      {"read_user_executable", vectis_opcua_lua_client_read_user_executable},
      {"write_executable", vectis_opcua_lua_client_write_executable},
      {"read_data_value", vectis_opcua_lua_client_read_data_value},
      {"read_boolean_array", vectis_opcua_lua_client_read_boolean_array},
      {"read_boolean_array_range",
       vectis_opcua_lua_client_read_boolean_array_range},
      {"read_integer_array", vectis_opcua_lua_client_read_integer_array},
      {"read_integer_array_range",
       vectis_opcua_lua_client_read_integer_array_range},
      {"read_double_array", vectis_opcua_lua_client_read_double_array},
      {"read_double_array_range",
       vectis_opcua_lua_client_read_double_array_range},
      {"read_string_array", vectis_opcua_lua_client_read_string_array},
      {"read_string_array_range",
       vectis_opcua_lua_client_read_string_array_range},
      {"read_byte_string_array",
       vectis_opcua_lua_client_read_byte_string_array},
      {"read_byte_string_array_range",
       vectis_opcua_lua_client_read_byte_string_array_range},
      {"read_uint64_array", vectis_opcua_lua_client_read_uint64_array},
      {"read_uint64_array_range",
       vectis_opcua_lua_client_read_uint64_array_range},
      {"read_datetime_array", vectis_opcua_lua_client_read_datetime_array},
      {"read_datetime_array_range",
       vectis_opcua_lua_client_read_datetime_array_range},
      {"read_status_array", vectis_opcua_lua_client_read_status_array},
      {"read_status_array_range",
       vectis_opcua_lua_client_read_status_array_range},
      {"read_guid_array", vectis_opcua_lua_client_read_guid_array},
      {"read_guid_array_range", vectis_opcua_lua_client_read_guid_array_range},
      {"read_qualified_name_array",
       vectis_opcua_lua_client_read_qualified_name_array},
      {"read_qualified_name_array_range",
       vectis_opcua_lua_client_read_qualified_name_array_range},
      {"read_localized_text_array",
       vectis_opcua_lua_client_read_localized_text_array},
      {"read_localized_text_array_range",
       vectis_opcua_lua_client_read_localized_text_array_range},
      {"write_index_range", vectis_opcua_lua_client_write_index_range},
      {"browse_children", vectis_opcua_lua_client_browse_children},
      {"browse_children_ex", vectis_opcua_lua_client_browse_children_ex},
      {"browse_children_page", vectis_opcua_lua_client_browse_children_page},
      {"browse_next", vectis_opcua_lua_client_browse_next},
      {"read_method_argument_count",
       vectis_opcua_lua_client_read_method_argument_count},
      {"read_method_argument", vectis_opcua_lua_client_read_method_argument},
      {"call_method", vectis_opcua_lua_client_call_method},
      {"call_method_many", vectis_opcua_lua_client_call_method_many},
      {"translate_browse_path", vectis_opcua_lua_client_translate_browse_path},
      {"namespace_index", vectis_opcua_lua_client_namespace_index},
      {"namespace_uri", vectis_opcua_lua_client_namespace_uri},
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

static void vectis_opcua_lua_register_event(lua_State *lua) {
  luaL_Reg methods[] = {{"set_field", vectis_opcua_lua_event_set_field},
                        {"trigger", vectis_opcua_lua_event_trigger},
                        {"close", vectis_opcua_lua_event_close},
                        {NULL, NULL}};
  luaL_Reg metamethods[] = {{"__gc", vectis_opcua_lua_event_close},
                            {"__close", vectis_opcua_lua_event_close},
                            {NULL, NULL}};

  if (luaL_newmetatable(lua, VECTIS_OPCUA_EVENT) != 0) {
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
      {"add_method", vectis_opcua_lua_server_add_method},
      {"add_method_many", vectis_opcua_lua_server_add_method_many},
      {"delete_node", vectis_opcua_lua_server_delete_node},
      {"add_reference", vectis_opcua_lua_server_add_reference},
      {"delete_reference", vectis_opcua_lua_server_delete_reference},
      {"startup", vectis_opcua_lua_server_startup},
      {"iterate", vectis_opcua_lua_server_iterate},
      {"shutdown", vectis_opcua_lua_server_shutdown},
      {"endpoint_url", vectis_opcua_lua_server_endpoint_url},
      {"read", vectis_opcua_lua_server_read},
      {"write", vectis_opcua_lua_server_write},
      {"read_data_value", vectis_opcua_lua_server_read_data_value},
      {"read_boolean_array", vectis_opcua_lua_server_read_boolean_array},
      {"read_boolean_array_range",
       vectis_opcua_lua_server_read_boolean_array_range},
      {"read_integer_array", vectis_opcua_lua_server_read_integer_array},
      {"read_integer_array_range",
       vectis_opcua_lua_server_read_integer_array_range},
      {"read_double_array", vectis_opcua_lua_server_read_double_array},
      {"read_double_array_range",
       vectis_opcua_lua_server_read_double_array_range},
      {"read_string_array", vectis_opcua_lua_server_read_string_array},
      {"read_string_array_range",
       vectis_opcua_lua_server_read_string_array_range},
      {"read_byte_string_array",
       vectis_opcua_lua_server_read_byte_string_array},
      {"read_byte_string_array_range",
       vectis_opcua_lua_server_read_byte_string_array_range},
      {"read_uint64_array", vectis_opcua_lua_server_read_uint64_array},
      {"read_uint64_array_range",
       vectis_opcua_lua_server_read_uint64_array_range},
      {"read_datetime_array", vectis_opcua_lua_server_read_datetime_array},
      {"read_datetime_array_range",
       vectis_opcua_lua_server_read_datetime_array_range},
      {"read_status_array", vectis_opcua_lua_server_read_status_array},
      {"read_status_array_range",
       vectis_opcua_lua_server_read_status_array_range},
      {"read_guid_array", vectis_opcua_lua_server_read_guid_array},
      {"read_guid_array_range", vectis_opcua_lua_server_read_guid_array_range},
      {"read_qualified_name_array",
       vectis_opcua_lua_server_read_qualified_name_array},
      {"read_qualified_name_array_range",
       vectis_opcua_lua_server_read_qualified_name_array_range},
      {"read_localized_text_array",
       vectis_opcua_lua_server_read_localized_text_array},
      {"read_localized_text_array_range",
       vectis_opcua_lua_server_read_localized_text_array_range},
      {"write_index_range", vectis_opcua_lua_server_write_index_range},
      {"browse_children", vectis_opcua_lua_server_browse_children},
      {"browse_children_ex", vectis_opcua_lua_server_browse_children_ex},
      {"browse_children_page", vectis_opcua_lua_server_browse_children_page},
      {"browse_next", vectis_opcua_lua_server_browse_next},
      {"read_method_argument_count",
       vectis_opcua_lua_server_read_method_argument_count},
      {"read_method_argument", vectis_opcua_lua_server_read_method_argument},
      {"translate_browse_path", vectis_opcua_lua_server_translate_browse_path},
      {"create_event", vectis_opcua_lua_server_create_event},
      {"trigger_event", vectis_opcua_lua_server_trigger_event},
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
      {"value_boolean_array", vectis_opcua_lua_value_boolean_array},
      {"value_integer_array", vectis_opcua_lua_value_integer_array},
      {"value_double_array", vectis_opcua_lua_value_double_array},
      {"value_string_array", vectis_opcua_lua_value_string_array},
      {"value_byte_string_array", vectis_opcua_lua_value_byte_string_array},
      {"value_uint64_array", vectis_opcua_lua_value_uint64_array},
      {"value_datetime_array", vectis_opcua_lua_value_datetime_array},
      {"value_status_array", vectis_opcua_lua_value_status_array},
      {"value_guid_array", vectis_opcua_lua_value_guid_array},
      {"value_qualified_name", vectis_opcua_lua_value_qualified_name},
      {"value_qualified_name_array",
       vectis_opcua_lua_value_qualified_name_array},
      {"value_localized_text", vectis_opcua_lua_value_localized_text},
      {"value_localized_text_array",
       vectis_opcua_lua_value_localized_text_array},
      {NULL, NULL}};

  vectis_opcua_lua_register_node_id(lua);
  vectis_opcua_lua_register_value(lua);
  vectis_opcua_lua_register_client(lua);
  vectis_opcua_lua_register_event(lua);
  vectis_opcua_lua_register_server(lua);
  luaL_newlib(lua, functions);

  vectis_opcua_lua_set_const(lua, "OK", CPKT_OPCUA_OK);
  vectis_opcua_lua_set_const(lua, "ERR_ARG", CPKT_OPCUA_ERR_ARG);
  vectis_opcua_lua_set_const(lua, "ERR_ALLOC", CPKT_OPCUA_ERR_ALLOC);
  vectis_opcua_lua_set_const(lua, "ERR_UPSTREAM", CPKT_OPCUA_ERR_UPSTREAM);
  vectis_opcua_lua_set_const(lua, "ERR_TYPE", CPKT_OPCUA_ERR_TYPE);
  vectis_opcua_lua_set_const(lua, "ERR_RANGE", CPKT_OPCUA_ERR_RANGE);
  vectis_opcua_lua_set_const(lua, "ERR_CALLBACK", CPKT_OPCUA_ERR_CALLBACK);

  vectis_opcua_lua_set_const(lua, "NODE_ID_NULL", CPKT_OPCUA_NODE_ID_NULL);
  vectis_opcua_lua_set_const(lua, "NODE_ID_NUMERIC",
                             CPKT_OPCUA_NODE_ID_NUMERIC);
  vectis_opcua_lua_set_const(lua, "NODE_ID_STRING", CPKT_OPCUA_NODE_ID_STRING);
  vectis_opcua_lua_set_const(lua, "NODE_ID_GUID", CPKT_OPCUA_NODE_ID_GUID);
  vectis_opcua_lua_set_const(lua, "NODE_ID_BYTE_STRING",
                             CPKT_OPCUA_NODE_ID_BYTE_STRING);

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

  vectis_opcua_lua_set_const(lua, "BROWSE_FORWARD", CPKT_OPCUA_BROWSE_FORWARD);
  vectis_opcua_lua_set_const(lua, "BROWSE_INVERSE", CPKT_OPCUA_BROWSE_INVERSE);
  vectis_opcua_lua_set_const(lua, "BROWSE_BOTH", CPKT_OPCUA_BROWSE_BOTH);
  vectis_opcua_lua_set_const(lua, "BROWSE_RESULT_REFERENCE_TYPE",
                             CPKT_OPCUA_BROWSE_RESULT_REFERENCE_TYPE);
  vectis_opcua_lua_set_const(lua, "BROWSE_RESULT_IS_FORWARD",
                             CPKT_OPCUA_BROWSE_RESULT_IS_FORWARD);
  vectis_opcua_lua_set_const(lua, "BROWSE_RESULT_NODE_CLASS",
                             CPKT_OPCUA_BROWSE_RESULT_NODE_CLASS);
  vectis_opcua_lua_set_const(lua, "BROWSE_RESULT_BROWSE_NAME",
                             CPKT_OPCUA_BROWSE_RESULT_BROWSE_NAME);
  vectis_opcua_lua_set_const(lua, "BROWSE_RESULT_DISPLAY_NAME",
                             CPKT_OPCUA_BROWSE_RESULT_DISPLAY_NAME);
  vectis_opcua_lua_set_const(lua, "BROWSE_RESULT_TYPE_DEFINITION",
                             CPKT_OPCUA_BROWSE_RESULT_TYPE_DEFINITION);
  vectis_opcua_lua_set_const(lua, "BROWSE_RESULT_ALL",
                             CPKT_OPCUA_BROWSE_RESULT_ALL);

  vectis_opcua_lua_set_const(lua, "METHOD_ARGUMENT_INPUT",
                             CPKT_OPCUA_METHOD_ARGUMENT_INPUT);
  vectis_opcua_lua_set_const(lua, "METHOD_ARGUMENT_OUTPUT",
                             CPKT_OPCUA_METHOD_ARGUMENT_OUTPUT);

  vectis_opcua_lua_set_const(lua, "VALUE_EMPTY", CPKT_OPCUA_VALUE_EMPTY);
  vectis_opcua_lua_set_const(lua, "VALUE_BOOLEAN", CPKT_OPCUA_VALUE_BOOLEAN);
  vectis_opcua_lua_set_const(lua, "VALUE_INTEGER", CPKT_OPCUA_VALUE_INTEGER);
  vectis_opcua_lua_set_const(lua, "VALUE_DOUBLE", CPKT_OPCUA_VALUE_DOUBLE);
  vectis_opcua_lua_set_const(lua, "VALUE_STRING", CPKT_OPCUA_VALUE_STRING);
  vectis_opcua_lua_set_const(lua, "VALUE_BYTE_STRING",
                             CPKT_OPCUA_VALUE_BYTE_STRING);
  vectis_opcua_lua_set_const(lua, "VALUE_BOOLEAN_ARRAY",
                             CPKT_OPCUA_VALUE_BOOLEAN_ARRAY);
  vectis_opcua_lua_set_const(lua, "VALUE_INTEGER_ARRAY",
                             CPKT_OPCUA_VALUE_INTEGER_ARRAY);
  vectis_opcua_lua_set_const(lua, "VALUE_DOUBLE_ARRAY",
                             CPKT_OPCUA_VALUE_DOUBLE_ARRAY);
  vectis_opcua_lua_set_const(lua, "VALUE_STRING_ARRAY",
                             CPKT_OPCUA_VALUE_STRING_ARRAY);
  vectis_opcua_lua_set_const(lua, "VALUE_BYTE_STRING_ARRAY",
                             CPKT_OPCUA_VALUE_BYTE_STRING_ARRAY);
  vectis_opcua_lua_set_const(lua, "VALUE_GUID", CPKT_OPCUA_VALUE_GUID);
  vectis_opcua_lua_set_const(lua, "VALUE_STATUS", CPKT_OPCUA_VALUE_STATUS);
  vectis_opcua_lua_set_const(lua, "VALUE_QUALIFIED_NAME",
                             CPKT_OPCUA_VALUE_QUALIFIED_NAME);
  vectis_opcua_lua_set_const(lua, "VALUE_LOCALIZED_TEXT",
                             CPKT_OPCUA_VALUE_LOCALIZED_TEXT);
  vectis_opcua_lua_set_const(lua, "VALUE_UINT64", CPKT_OPCUA_VALUE_UINT64);
  vectis_opcua_lua_set_const(lua, "VALUE_DATETIME", CPKT_OPCUA_VALUE_DATETIME);
  vectis_opcua_lua_set_const(lua, "VALUE_UINT64_ARRAY",
                             CPKT_OPCUA_VALUE_UINT64_ARRAY);
  vectis_opcua_lua_set_const(lua, "VALUE_DATETIME_ARRAY",
                             CPKT_OPCUA_VALUE_DATETIME_ARRAY);
  vectis_opcua_lua_set_const(lua, "VALUE_STATUS_ARRAY",
                             CPKT_OPCUA_VALUE_STATUS_ARRAY);
  vectis_opcua_lua_set_const(lua, "VALUE_GUID_ARRAY",
                             CPKT_OPCUA_VALUE_GUID_ARRAY);
  vectis_opcua_lua_set_const(lua, "VALUE_QUALIFIED_NAME_ARRAY",
                             CPKT_OPCUA_VALUE_QUALIFIED_NAME_ARRAY);
  vectis_opcua_lua_set_const(lua, "VALUE_LOCALIZED_TEXT_ARRAY",
                             CPKT_OPCUA_VALUE_LOCALIZED_TEXT_ARRAY);

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
  vectis_opcua_lua_set_const(lua, "NODE_BASE_EVENT_TYPE", 2041);
  vectis_opcua_lua_set_const(lua, "NODE_REFERENCES", 31);
  vectis_opcua_lua_set_const(lua, "REFERENCE_ORGANIZES", 35);
  vectis_opcua_lua_set_const(lua, "REFERENCE_HAS_COMPONENT", 47);

  return 1;
}
