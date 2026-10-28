#include "vectis_lua_xml_objects.h"
#include <lauxlib.h>
#include <string.h>
#include <unistd.h>
#include <vectis/xml.h>
#define XML_DOC "vectis.xml.document"
#define XML_NODE "vectis.xml.element"
#define XML_TEMP "vectis.xml.result"
#define XML_SAX "vectis.xml.sax"
typedef struct xml_lua_document {
  vectis_xml_document *native;
  int busy;
  pid_t process;
} xml_lua_document;
typedef struct xml_lua_node {
  vectis_xml_node *native;
  pid_t process;
} xml_lua_node;
typedef struct xml_lua_result {
  vectis_mutable_bytes bytes;
  vectis_xml_xpath_result xpath;
  vectis_xml_namespace namespaces[256];
} xml_lua_result;
typedef struct xml_lua_sax {
  vectis_xml_sax *native;
  lua_State *L;
  pid_t process;
  int self, event;
  const vectis_xml_name *name;
  const vectis_xml_attribute *attributes;
  size_t attribute_count;
  const vectis_xml_namespace *namespaces;
  size_t namespace_count;
  const void *text;
  size_t size;
} xml_lua_sax;
static int xml_lua_error(lua_State *L, const vectis_error *error,
                         const char *operation) {
  lua_pushnil(L);
  lua_createtable(L, 0, 8);
  lua_pushinteger(L, error->code);
  lua_setfield(L, -2, "status");
  lua_pushstring(L, vectis_status_string(error->code));
  lua_setfield(L, -2, "status_string");
  lua_pushstring(L, vectis_error_source_string(error->source));
  lua_setfield(L, -2, "source");
  lua_pushinteger(L, error->source);
  lua_setfield(L, -2, "source_code");
  lua_pushinteger(L, error->dependency_code);
  lua_setfield(L, -2, "dependency_code");
  lua_pushstring(L, error->message);
  lua_setfield(L, -2, "message");
  lua_pushstring(L, error->detail);
  lua_setfield(L, -2, "detail");
  lua_pushstring(L, operation);
  lua_setfield(L, -2, "operation");
  return 2;
}
static const char *xml_lua_string(lua_State *L, int index, size_t *size,
                                  int text) {
  const char *value;
  luaL_checktype(L, index, LUA_TSTRING);
  value = lua_tolstring(L, index, size);
  if (text && memchr(value, 0, *size) != NULL)
    luaL_argerror(L, index, "XML name/option contains NUL");
  return value;
}
static void xml_lua_field(lua_State *L, int index, const char *name) {
  if (lua_isnoneornil(L, index)) {
    lua_pushnil(L);
    return;
  }
  index = lua_absindex(L, index);
  lua_pushstring(L, name);
  lua_rawget(L, index);
}
static void xml_lua_fields(lua_State *L, int index, const char *const *fields) {
  size_t size;
  const char *key;
  int i;
  if (lua_isnoneornil(L, index))
    return;
  luaL_checktype(L, index, LUA_TTABLE);
  index = lua_absindex(L, index);
  lua_pushnil(L);
  while (lua_next(L, index)) {
    key = xml_lua_string(L, -2, &size, 1);
    for (i = 0; fields[i] != NULL && strcmp(fields[i], key) != 0; ++i) {
    }
    if (fields[i] == NULL)
      luaL_error(L, "unknown XML option '%s'", key);
    lua_pop(L, 1);
  }
}
static size_t xml_lua_limit(lua_State *L, int index, const char *name,
                            size_t fallback) {
  lua_Integer value;
  xml_lua_field(L, index, name);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return fallback;
  }
  value = luaL_checkinteger(L, -1);
  luaL_argcheck(L, value > 0 && (lua_Unsigned)value <= 2147483647u, index,
                "XML limit must be a positive integer <=2147483647");
  lua_pop(L, 1);
  return (size_t)value;
}
static int xml_lua_result_gc(lua_State *L) {
  xml_lua_result *owner = (xml_lua_result *)luaL_checkudata(L, 1, XML_TEMP);
  vectis_mutable_bytes_cleanup(&owner->bytes);
  vectis_xml_xpath_result_cleanup(&owner->xpath);
  return 0;
}
static xml_lua_result *xml_lua_result_new(lua_State *L) {
  xml_lua_result *result =
      (xml_lua_result *)lua_newuserdatauv(L, sizeof(*result), 0);
  memset(result, 0, sizeof(*result));
  luaL_setmetatable(L, XML_TEMP);
  lua_toclose(L, -1);
  return result;
}
static xml_lua_node *xml_lua_node_new(lua_State *L, int document) {
  xml_lua_node *owner;
  document = lua_absindex(L, document);
  owner = (xml_lua_node *)lua_newuserdatauv(L, sizeof(*owner), 1);
  memset(owner, 0, sizeof(*owner));
  owner->process = getpid();
  luaL_setmetatable(L, XML_NODE);
  lua_pushvalue(L, document);
  lua_setiuservalue(L, -2, 1);
  return owner;
}
static int xml_lua_close(lua_State *L) {
  xml_lua_document *document =
      (xml_lua_document *)luaL_testudata(L, 1, XML_DOC);
  xml_lua_node *node = (xml_lua_node *)luaL_testudata(L, 1, XML_NODE);
  vectis_error error;
  vectis_status status;
  int finalizer = lua_toboolean(L, lua_upvalueindex(1));
  if (node != NULL && node->native == NULL) {
    if (!finalizer)
      lua_pushboolean(L, 1);
    return !finalizer;
  }
  if (node != NULL) {
    lua_getiuservalue(L, 1, 1);
    document = (xml_lua_document *)luaL_checkudata(L, -1, XML_DOC);
    lua_pop(L, 1);
  }
  luaL_argcheck(L, document != NULL, 1, "XML document/element expected");
  if (document->busy || document->process != getpid()) {
    if (finalizer)
      return 0;
    return luaL_error(L, "cannot close active or foreign-process XML object");
  }
  status = node == NULL ? vectis_xml_document_close(document->native, &error)
                        : vectis_xml_node_close(node->native, &error);
  if (status != VECTIS_OK) {
    if (finalizer)
      return 0;
    return xml_lua_error(L, &error, "close");
  }
  if (node == NULL)
    document->native = NULL;
  else
    node->native = NULL;
  if (!finalizer)
    lua_pushboolean(L, 1);
  return !finalizer;
}
static int xml_lua_document_new(lua_State *L) {
  static const char *const fields[] = {"max_bytes", "max_xpath_nodes", NULL};
  vectis_xml_document_config config;
  xml_lua_document *owner;
  vectis_error error;
  const char *data;
  size_t size;
  int result;
  lua_settop(L, 2);
  luaL_checkstack(L, 12, "XML document constructor");
  data = xml_lua_string(L, 1, &size, 0);
  xml_lua_fields(L, 2, fields);
  vectis_xml_document_config_init(&config);
  config.max_bytes = xml_lua_limit(L, 2, "max_bytes", config.max_bytes);
  config.max_xpath_nodes =
      xml_lua_limit(L, 2, "max_xpath_nodes", config.max_xpath_nodes);
  owner = (xml_lua_document *)lua_newuserdatauv(L, sizeof(*owner), 0);
  memset(owner, 0, sizeof(*owner));
  owner->process = getpid();
  luaL_setmetatable(L, XML_DOC);
  result = lua_gettop(L);
  if (vectis_xml_document_parse(data, size, &config, &owner->native, &error) !=
      VECTIS_OK)
    return xml_lua_error(L, &error, "document");
  lua_pushvalue(L, result);
  return 1;
}
static xml_lua_document *xml_lua_doc(lua_State *L) {
  xml_lua_document *owner = (xml_lua_document *)luaL_checkudata(L, 1, XML_DOC);
  luaL_argcheck(L, owner->native != NULL, 1, "XML document is closed");
  return owner;
}
static xml_lua_node *xml_lua_node_check(lua_State *L) {
  xml_lua_node *owner = (xml_lua_node *)luaL_checkudata(L, 1, XML_NODE);
  luaL_argcheck(L, owner->native != NULL, 1, "XML element view is closed");
  return owner;
}
static int xml_lua_root(lua_State *L) {
  xml_lua_document *owner = xml_lua_doc(L);
  xml_lua_node *node = xml_lua_node_new(L, 1);
  vectis_error error;
  if (owner->native->root(owner->native, &node->native, &error) != VECTIS_OK)
    return xml_lua_error(L, &error, "root");
  return 1;
}
static int xml_lua_xpath(lua_State *L) {
  xml_lua_document *owner = xml_lua_doc(L);
  xml_lua_result *output;
  xml_lua_node *node;
  vectis_error error;
  const char *expression;
  size_t size, count = 0u, i;
  int index;
  lua_settop(L, 3);
  expression = xml_lua_string(L, 2, &size, 1);
  output = xml_lua_result_new(L);
  if (!lua_isnil(L, 3)) {
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 3)) {
      luaL_argcheck(L, count < 256u, 3, "at most 256 namespace bindings");
      output->namespaces[count].prefix = xml_lua_string(L, -2, &size, 1);
      output->namespaces[count].uri = xml_lua_string(L, -1, &size, 1);
      ++count;
      lua_pop(L, 1);
    }
  }
  if (owner->native->xpath(owner->native, expression, output->namespaces, count,
                           &output->xpath, &error) != VECTIS_OK)
    return xml_lua_error(L, &error, "xpath");
  if (output->xpath.kind == VECTIS_XML_XPATH_STRING)
    lua_pushlstring(L, (const char *)output->xpath.string.data,
                    output->xpath.string.size);
  else if (output->xpath.kind == VECTIS_XML_XPATH_BOOLEAN)
    lua_pushboolean(L, output->xpath.boolean);
  else if (output->xpath.kind == VECTIS_XML_XPATH_NUMBER)
    lua_pushnumber(L, output->xpath.number);
  else {
    lua_createtable(L, (int)output->xpath.count, 0);
    index = lua_gettop(L);
    for (i = 0u; i < output->xpath.count; ++i) {
      node = xml_lua_node_new(L, 1);
      node->native = output->xpath.nodes[i];
      output->xpath.nodes[i] = NULL;
      lua_rawseti(L, index, (lua_Integer)i + 1);
    }
  }
  return 1;
}
static int xml_lua_serialize(lua_State *L) {
  xml_lua_document *owner = xml_lua_doc(L);
  xml_lua_result *output;
  vectis_error error;
  int formatted = 0;
  if (!lua_isnoneornil(L, 2)) {
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    formatted = lua_toboolean(L, 2);
  }
  output = xml_lua_result_new(L);
  if (owner->native->serialize_materialized(
          owner->native, formatted, &output->bytes, &error) != VECTIS_OK)
    return xml_lua_error(L, &error, "serialize_materialized");
  lua_pushlstring(L, (const char *)output->bytes.data, output->bytes.size);
  return 1;
}
static int xml_lua_name(lua_State *L) {
  xml_lua_node *node = xml_lua_node_check(L);
  vectis_xml_name name;
  vectis_error error;
  if (node->native->name(node->native, &name, &error) != VECTIS_OK)
    return xml_lua_error(L, &error, "name");
  lua_pushstring(L, name.name);
  lua_pushstring(L, name.namespace_uri);
  lua_pushstring(L, name.prefix);
  return 3;
}
static int xml_lua_text(lua_State *L) {
  xml_lua_node *node = xml_lua_node_check(L);
  xml_lua_result *output = xml_lua_result_new(L);
  vectis_error error;
  if (node->native->text(node->native, &output->bytes, &error) != VECTIS_OK)
    return xml_lua_error(L, &error, "text");
  lua_pushlstring(L, (const char *)output->bytes.data, output->bytes.size);
  return 1;
}
static int xml_lua_attribute(lua_State *L) {
  xml_lua_node *node = xml_lua_node_check(L);
  xml_lua_result *output;
  const char *name, *uri = NULL;
  size_t size;
  int found = 0;
  vectis_error error;
  lua_settop(L, 3);
  name = xml_lua_string(L, 2, &size, 1);
  if (!lua_isnil(L, 3))
    uri = xml_lua_string(L, 3, &size, 1);
  output = xml_lua_result_new(L);
  if (node->native->attribute(node->native, name, uri, &output->bytes, &found,
                              &error) != VECTIS_OK)
    return xml_lua_error(L, &error, "attribute");
  if (found)
    lua_pushlstring(L, (const char *)output->bytes.data, output->bytes.size);
  else
    lua_pushnil(L);
  return 1;
}
static int xml_lua_set_text(lua_State *L) {
  xml_lua_node *node = xml_lua_node_check(L);
  const char *text;
  size_t size;
  vectis_error error;
  text = xml_lua_string(L, 2, &size, 0);
  if (node->native->set_text(node->native, text, size, &error) != VECTIS_OK)
    return xml_lua_error(L, &error, "set_text");
  lua_pushboolean(L, 1);
  return 1;
}
static int xml_lua_edit(lua_State *L) {
  static const char *const attr_fields[] = {"namespace", "prefix", NULL};
  static const char *const child_fields[] = {"namespace", "prefix", "text",
                                             NULL};
  int child = lua_toboolean(L, lua_upvalueindex(1)), options = child ? 3 : 4;
  xml_lua_node *node = xml_lua_node_check(L), *output = NULL;
  vectis_xml_child_config config;
  vectis_error error;
  vectis_status status;
  const char *name, *value = NULL;
  size_t size, value_size = 0u;
  lua_settop(L, options);
  memset(&config, 0, sizeof(config));
  name = xml_lua_string(L, 2, &size, 1);
  if (!child)
    value = xml_lua_string(L, 3, &value_size, 0);
  xml_lua_fields(L, options, child ? child_fields : attr_fields);
  xml_lua_field(L, options, "namespace");
  if (!lua_isnil(L, -1))
    config.namespace_uri = xml_lua_string(L, -1, &size, 1);
  xml_lua_field(L, options, "prefix");
  if (!lua_isnil(L, -1))
    config.prefix = xml_lua_string(L, -1, &size, 1);
  if (child) {
    xml_lua_field(L, options, "text");
    if (!lua_isnil(L, -1)) {
      config.text_present = 1;
      config.text = xml_lua_string(L, -1, &config.text_size, 0);
    }
    lua_getiuservalue(L, 1, 1);
    output = xml_lua_node_new(L, -1);
    status = node->native->append_child(node->native, name, &config,
                                        &output->native, &error);
  } else
    status = node->native->set_attribute(node->native, name, value, value_size,
                                         config.namespace_uri, config.prefix,
                                         &error);
  if (status != VECTIS_OK)
    return xml_lua_error(L, &error, child ? "append_child" : "set_attribute");
  if (!child)
    lua_pushboolean(L, 1);
  return 1;
}
static int xml_lua_remove(lua_State *L) {
  xml_lua_node *node = xml_lua_node_check(L);
  vectis_error error;
  if (node->native->remove(node->native, &error) != VECTIS_OK)
    return xml_lua_error(L, &error, "remove");
  lua_pushboolean(L, 1);
  return 1;
}
static int xml_lua_guard(lua_State *L) {
  xml_lua_document *document;
  int count = lua_gettop(L), status;
  luaL_checkstack(L, 24, "XML method stack");
  document = (xml_lua_document *)luaL_testudata(L, 1, XML_DOC);
  if (document == NULL) {
    luaL_checkudata(L, 1, XML_NODE);
    lua_getiuservalue(L, 1, 1);
    document = (xml_lua_document *)luaL_checkudata(L, -1, XML_DOC);
    lua_pop(L, 1);
  }
  luaL_argcheck(L, !document->busy && document->process == getpid(), 1,
                "cannot reenter active or foreign-process XML document");
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  document->busy = 1;
  status = lua_pcall(L, count, LUA_MULTRET, 0);
  document->busy = 0;
  if (status != LUA_OK)
    return lua_error(L);
  return lua_gettop(L);
}
static void xml_lua_name_fields(lua_State *L, const vectis_xml_name *name) {
  lua_pushstring(L, name->namespace_uri);
  lua_setfield(L, -2, "uri");
  lua_pushstring(L, name->prefix);
  lua_setfield(L, -2, "prefix");
}
static int xml_lua_sax_dispatch(lua_State *L) {
  static const char *const names[] = {"start_element", "end_element", "text"};
  xml_lua_sax *owner = (xml_lua_sax *)lua_touserdata(L, 1);
  size_t i;
  int attrs, namespaces;
  lua_getiuservalue(L, 2, 1);
  xml_lua_field(L, -1, names[owner->event]);
  if (lua_isnil(L, -1))
    return 0;
  if (owner->event == 2) {
    lua_pushlstring(L, (const char *)owner->text, owner->size);
    lua_call(L, 1, 2);
  } else {
    lua_pushstring(L, owner->name->name);
    if (owner->event == 0) {
      lua_createtable(L, (int)owner->attribute_count, 0);
      attrs = lua_gettop(L);
      for (i = 0u; i < owner->attribute_count; ++i) {
        lua_createtable(L, 0, 4);
        lua_pushstring(L, owner->attributes[i].name.name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, owner->attributes[i].name.namespace_uri);
        lua_setfield(L, -2, "namespace");
        lua_pushstring(L, owner->attributes[i].name.prefix);
        lua_setfield(L, -2, "prefix");
        lua_pushlstring(L, (const char *)owner->attributes[i].value,
                        owner->attributes[i].size);
        lua_setfield(L, -2, "value");
        lua_rawseti(L, attrs, (lua_Integer)i + 1);
      }
    }
    lua_createtable(L, 0, 3);
    xml_lua_name_fields(L, owner->name);
    if (owner->event == 0) {
      lua_createtable(L, (int)owner->namespace_count, 0);
      namespaces = lua_gettop(L);
      for (i = 0u; i < owner->namespace_count; ++i) {
        lua_createtable(L, 0, 2);
        lua_pushstring(L, owner->namespaces[i].prefix);
        lua_setfield(L, -2, "prefix");
        lua_pushstring(L, owner->namespaces[i].uri);
        lua_setfield(L, -2, "uri");
        lua_rawseti(L, namespaces, (lua_Integer)i + 1);
      }
      lua_setfield(L, -2, "declarations");
    }
    lua_call(L, owner->event == 0 ? 3 : 2, 2);
  }
  if (lua_isnil(L, -2) && !lua_isnil(L, -1))
    return lua_error(L);
  return 0;
}
static vectis_status xml_lua_sax_call(xml_lua_sax *owner, vectis_error *error) {
  lua_State *L = owner->L;
  int base = lua_gettop(L), status;
  if (!lua_checkstack(L, 6)) {
    memset(error, 0, sizeof(*error));
    error->code = VECTIS_ERR_NOMEM;
    error->source = VECTIS_ERROR_SOURCE_CPKT;
    strcpy(error->message, "SAX callback stack allocation failed");
    return error->code;
  }
  lua_pushcfunction(L, xml_lua_sax_dispatch);
  lua_pushlightuserdata(L, owner);
  lua_pushvalue(L, owner->self);
  status = lua_pcall(L, 2, 0, 0);
  if (status != LUA_OK) {
    lua_pushvalue(L, -1);
    lua_setiuservalue(L, owner->self, 2);
    memset(error, 0, sizeof(*error));
    error->code = status == LUA_ERRMEM ? VECTIS_ERR_NOMEM : VECTIS_ERR_STATE;
    error->source = VECTIS_ERROR_SOURCE_CPKT;
    strcpy(error->message, "SAX callback failed");
  }
  lua_settop(L, base);
  return status == LUA_OK ? VECTIS_OK : error->code;
}
static vectis_status
xml_lua_sax_start(void *context, const vectis_xml_name *name,
                  const vectis_xml_attribute *attrs, size_t count,
                  const vectis_xml_namespace *namespaces,
                  size_t namespace_count, vectis_error *error) {
  xml_lua_sax *owner = (xml_lua_sax *)context;
  owner->event = 0;
  owner->name = name;
  owner->attributes = attrs;
  owner->attribute_count = count;
  owner->namespaces = namespaces;
  owner->namespace_count = namespace_count;
  return xml_lua_sax_call(owner, error);
}
static vectis_status xml_lua_sax_end(void *context, const vectis_xml_name *name,
                                     vectis_error *error) {
  xml_lua_sax *owner = (xml_lua_sax *)context;
  owner->event = 1;
  owner->name = name;
  return xml_lua_sax_call(owner, error);
}
static vectis_status xml_lua_sax_text(void *context, const void *data,
                                      size_t size, vectis_error *error) {
  xml_lua_sax *owner = (xml_lua_sax *)context;
  owner->event = 2;
  owner->text = data;
  owner->size = size;
  return xml_lua_sax_call(owner, error);
}
static int xml_lua_sax_close(lua_State *L) {
  xml_lua_sax *owner = (xml_lua_sax *)luaL_checkudata(L, 1, XML_SAX);
  int finalizer = lua_toboolean(L, lua_upvalueindex(1));
  vectis_error error;
  if (vectis_xml_sax_close(owner->native, &error) != VECTIS_OK) {
    if (finalizer)
      return 0;
    return xml_lua_error(L, &error, "close");
  }
  owner->native = NULL;
  if (!finalizer)
    lua_pushboolean(L, 1);
  return !finalizer;
}
static int xml_lua_sax_new(lua_State *L) {
  static const char *const callbacks[] = {"start_element", "end_element",
                                          "text", NULL};
  static const char *const fields[] = {"max_metadata_bytes", NULL};
  vectis_xml_sax_callbacks native_callbacks;
  xml_lua_sax *owner;
  vectis_error error;
  size_t limit;
  int i, result, copied;
  lua_settop(L, 2);
  luaL_checkstack(L, 12, "SAX constructor stack");
  luaL_checktype(L, 1, LUA_TTABLE);
  xml_lua_fields(L, 1, callbacks);
  xml_lua_fields(L, 2, fields);
  limit =
      xml_lua_limit(L, 2, "max_metadata_bytes", VECTIS_XML_DEFAULT_MAX_BYTES);
  owner = (xml_lua_sax *)lua_newuserdatauv(L, sizeof(*owner), 2);
  memset(owner, 0, sizeof(*owner));
  owner->process = getpid();
  luaL_setmetatable(L, XML_SAX);
  result = lua_gettop(L);
  lua_createtable(L, 0, 3);
  copied = lua_gettop(L);
  for (i = 0; callbacks[i] != NULL; ++i) {
    xml_lua_field(L, 1, callbacks[i]);
    if (!lua_isnil(L, -1))
      luaL_checktype(L, -1, LUA_TFUNCTION);
    lua_setfield(L, copied, callbacks[i]);
  }
  lua_setiuservalue(L, result, 1);
  memset(&native_callbacks, 0, sizeof(native_callbacks));
  native_callbacks.start_element = xml_lua_sax_start;
  native_callbacks.end_element = xml_lua_sax_end;
  native_callbacks.text = xml_lua_sax_text;
  if (vectis_xml_sax_new(&native_callbacks, owner, limit, &owner->native,
                         &error) != VECTIS_OK)
    return xml_lua_error(L, &error, "sax");
  lua_pushvalue(L, result);
  return 1;
}
static int xml_lua_sax_process(lua_State *L) {
  xml_lua_sax *owner = (xml_lua_sax *)luaL_checkudata(L, 1, XML_SAX);
  int finish = lua_toboolean(L, lua_upvalueindex(1));
  vectis_error error;
  vectis_status status;
  const char *data = NULL;
  size_t size = 0u;
  luaL_checkstack(L, 12, "SAX feed stack");
  luaL_argcheck(L, owner->native != NULL, 1, "SAX receiver is closed");
  if (!finish)
    data = xml_lua_string(L, 2, &size, 0);
  /* A nested call is rejected by the native busy guard before it can invoke
   * callbacks. Keep the outer callback's Lua state/self unchanged. */
  if (owner->L == NULL) {
    owner->L = L;
    owner->self = 1;
    status = finish ? vectis_xml_sax_finish(owner->native, &error)
                    : vectis_xml_sax_feed(owner->native, data, size, &error);
    owner->L = NULL;
  } else
    status = finish ? vectis_xml_sax_finish(owner->native, &error)
                    : vectis_xml_sax_feed(owner->native, data, size, &error);
  if (status != VECTIS_OK) {
    lua_getiuservalue(L, 1, 2);
    if (!lua_isnil(L, -1)) {
      lua_pushnil(L);
      lua_insert(L, -2);
      return 2;
    }
    lua_pop(L, 1);
    return xml_lua_error(L, &error, finish ? "finish" : "feed");
  }
  lua_pushboolean(L, 1);
  return 1;
}
static void xml_lua_method(lua_State *L, const char *name, lua_CFunction fn) {
  lua_pushcfunction(L, fn);
  lua_pushcclosure(L, xml_lua_guard, 1);
  lua_setfield(L, -2, name);
}
static void xml_lua_metatable(lua_State *L, const char *name,
                              lua_CFunction close) {
  luaL_newmetatable(L, name);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, close, 1);
  lua_setfield(L, -2, "close");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, close, 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "__gc");
  lua_setfield(L, -2, "__close");
}
void vectis_lua_xml_objects_register(lua_State *L) {
  int module = lua_absindex(L, -1);
  xml_lua_metatable(L, XML_DOC, xml_lua_close);
  xml_lua_method(L, "root", xml_lua_root);
  xml_lua_method(L, "xpath", xml_lua_xpath);
  xml_lua_method(L, "serialize_materialized", xml_lua_serialize);
  lua_pop(L, 1);
  xml_lua_metatable(L, XML_NODE, xml_lua_close);
  xml_lua_method(L, "name", xml_lua_name);
  xml_lua_method(L, "text", xml_lua_text);
  xml_lua_method(L, "attribute", xml_lua_attribute);
  xml_lua_method(L, "set_text", xml_lua_set_text);
  xml_lua_method(L, "remove", xml_lua_remove);
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, xml_lua_edit, 1);
  lua_pushcclosure(L, xml_lua_guard, 1);
  lua_setfield(L, -2, "set_attribute");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, xml_lua_edit, 1);
  lua_pushcclosure(L, xml_lua_guard, 1);
  lua_setfield(L, -2, "append_child");
  lua_pop(L, 1);
  if (luaL_newmetatable(L, XML_TEMP)) {
    lua_pushcfunction(L, xml_lua_result_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, xml_lua_result_gc);
    lua_setfield(L, -2, "__close");
  }
  lua_pop(L, 1);
  xml_lua_metatable(L, XML_SAX, xml_lua_sax_close);
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, xml_lua_sax_process, 1);
  lua_setfield(L, -2, "feed");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, xml_lua_sax_process, 1);
  lua_setfield(L, -2, "finish");
  lua_pop(L, 1);
  lua_pushcfunction(L, xml_lua_document_new);
  lua_setfield(L, module, "document");
  lua_pushcfunction(L, xml_lua_sax_new);
  lua_setfield(L, module, "sax");
}
int luaopen_vectis_xml_objects_core(lua_State *L) {
  lua_newtable(L);
  vectis_lua_xml_objects_register(L);
  return 1;
}
