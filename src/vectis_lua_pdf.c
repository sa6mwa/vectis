#include "vectis_lua_pdf.h"
#include <lauxlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <vectis/pdf.h>

#define PDF_DOC "vectis.pdf.document"
#define PDF_PAGE "vectis.pdf.page"
#define PDF_FONT "vectis.pdf.font"
#define PDF_IMAGE "vectis.pdf.image"
#define PDF_BYTES "vectis.pdf.bytes"
typedef struct pdf_lua_doc {
  vectis_pdf *value;
  pid_t process;
  int busy;
} pdf_lua_doc;
typedef struct pdf_lua_child {
  void *value;
  pdf_lua_doc *document;
  int busy;
} pdf_lua_child;

static int pdf_push_error(lua_State *L, const vectis_error *error,
                          const char *operation) {
  lua_pushnil(L);
  lua_createtable(L, 0, 8);
  lua_pushinteger(L, error->code);
  lua_setfield(L, -2, "status");
  lua_pushstring(L, vectis_status_string(error->code));
  lua_setfield(L, -2, "status_string");
  lua_pushinteger(L, error->source);
  lua_setfield(L, -2, "source_code");
  lua_pushstring(L, vectis_error_source_string(error->source));
  lua_setfield(L, -2, "source");
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
static void pdf_fields(lua_State *L, int index, const char *const *allowed) {
  if (lua_isnoneornil(L, index))
    return;
  luaL_checktype(L, index, LUA_TTABLE);
  lua_pushnil(L);
  while (lua_next(L, index)) {
    const char *key;
    size_t size;
    int i;
    luaL_argcheck(L, lua_type(L, -2) == LUA_TSTRING, index,
                  "PDF option names must be strings");
    key = lua_tolstring(L, -2, &size);
    luaL_argcheck(L, memchr(key, 0, size) == NULL, index,
                  "PDF option names cannot contain NUL");
    for (i = 0; allowed[i] != NULL && strcmp(key, allowed[i]) != 0; ++i) {
    }
    if (allowed[i] == NULL)
      (void)luaL_error(L, "unknown PDF option '%s'", key);
    lua_pop(L, 1);
  }
}
static void pdf_field(lua_State *L, int index, const char *name) {
  if (lua_isnoneornil(L, index)) {
    lua_pushnil(L);
    return;
  }
  index = lua_absindex(L, index);
  lua_pushstring(L, name);
  lua_rawget(L, index);
}
static const char *pdf_string(lua_State *L, int index) {
  size_t size;
  const char *value;
  luaL_checktype(L, index, LUA_TSTRING);
  value = lua_tolstring(L, index, &size);
  luaL_argcheck(L, memchr(value, 0, size) == NULL, index,
                "PDF strings cannot contain NUL");
  return value;
}
/* Leave optional strings rooted on the Lua stack until the native call copies
 * them; another finalizer may mutate the original options table. */
static const char *pdf_string_field(lua_State *L, int index, const char *name,
                                    const char *fallback) {
  pdf_field(L, index, name);
  if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TSTRING)
    (void)luaL_error(L, "PDF option '%s' must be a string", name);
  return lua_isnil(L, -1) ? fallback : pdf_string(L, lua_gettop(L));
}
static double pdf_number(lua_State *L, int index) {
  luaL_checktype(L, index, LUA_TNUMBER);
  return lua_tonumber(L, index);
}
static double pdf_number_field(lua_State *L, int index, const char *name,
                               double fallback) {
  double value;
  pdf_field(L, index, name);
  value = lua_isnil(L, -1) ? fallback : pdf_number(L, lua_gettop(L));
  lua_pop(L, 1);
  return value;
}
static int pdf_bool_field(lua_State *L, int index, const char *name,
                          int fallback) {
  int value;
  pdf_field(L, index, name);
  if (lua_isnil(L, -1))
    value = fallback;
  else {
    luaL_checktype(L, -1, LUA_TBOOLEAN);
    value = lua_toboolean(L, -1);
  }
  lua_pop(L, 1);
  return value;
}
static void pdf_color(lua_State *L, int index, double *color) {
  int i;
  pdf_field(L, index, "color");
  if (!lua_isnil(L, -1)) {
    luaL_checktype(L, -1, LUA_TTABLE);
    luaL_argcheck(L, lua_rawlen(L, -1) == 3u, index,
                  "RGB color requires three numbers");
    for (i = 0; i < 3; ++i) {
      lua_rawgeti(L, -1, i + 1);
      color[i] = pdf_number(L, lua_gettop(L));
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
}
static pdf_lua_doc *pdf_document(lua_State *L, int index) {
  pdf_lua_doc *owner = (pdf_lua_doc *)luaL_checkudata(L, index, PDF_DOC);
  luaL_argcheck(L, owner->value != NULL, index, "PDF document is closed");
  luaL_argcheck(L, owner->process == getpid(), index,
                "foreign-process PDF document");
  return owner;
}
static pdf_lua_child *pdf_child(lua_State *L, int index,
                                const char *metatable) {
  pdf_lua_child *owner = (pdf_lua_child *)luaL_checkudata(L, index, metatable);
  luaL_argcheck(L, owner->value != NULL, index, "PDF view is closed");
  luaL_argcheck(L, owner->document->value != NULL, index,
                "PDF document is closed");
  luaL_argcheck(L, owner->document->process == getpid(), index,
                "foreign-process PDF view");
  return owner;
}
static pdf_lua_child *pdf_new_child(lua_State *L, pdf_lua_doc *document,
                                    const char *metatable) {
  pdf_lua_child *owner =
      (pdf_lua_child *)lua_newuserdatauv(L, sizeof(*owner), 1);
  memset(owner, 0, sizeof(*owner));
  owner->document = document;
  luaL_setmetatable(L, metatable);
  lua_pushvalue(L, 1);
  lua_setiuservalue(L, -2, 1);
  return owner;
}
static int pdf_doc_close(lua_State *L) {
  pdf_lua_doc *owner = (pdf_lua_doc *)luaL_checkudata(L, 1, PDF_DOC);
  vectis_error error;
  int finalizer = lua_toboolean(L, lua_upvalueindex(1));
  if (owner->busy || owner->process != getpid()) {
    if (finalizer)
      return 0;
    return luaL_error(L, "cannot close active or foreign-process PDF document");
  }
  if (vectis_pdf_close(owner->value, &error) != VECTIS_OK) {
    if (finalizer)
      return 0;
    return pdf_push_error(L, &error, "close");
  }
  owner->value = NULL;
  lua_pushboolean(L, 1);
  return 1;
}
static int pdf_view_close(lua_State *L) {
  const char *meta = lua_tostring(L, lua_upvalueindex(1));
  int finalizer = lua_toboolean(L, lua_upvalueindex(2));
  pdf_lua_child *owner = (pdf_lua_child *)luaL_checkudata(L, 1, meta);
  if (owner->busy || owner->document->process != getpid()) {
    if (finalizer)
      return 0;
    return luaL_error(L, "cannot close active or foreign-process PDF view");
  }
  if (strcmp(meta, PDF_PAGE) == 0)
    vectis_pdf_page_close((vectis_pdf_page *)owner->value);
  else if (strcmp(meta, PDF_FONT) == 0)
    vectis_pdf_font_close((vectis_pdf_font *)owner->value);
  else
    vectis_pdf_image_close((vectis_pdf_image *)owner->value);
  owner->value = NULL;
  lua_pushboolean(L, 1);
  return 1;
}
static int pdf_new(lua_State *L) {
  static const char *const options[] = {"title", "author", "subject", NULL};
  vectis_pdf_config config;
  pdf_lua_doc *owner;
  vectis_error error;
  lua_settop(L, 1);
  owner = (pdf_lua_doc *)lua_newuserdatauv(L, sizeof(*owner), 0);
  memset(owner, 0, sizeof(*owner));
  owner->process = getpid();
  luaL_setmetatable(L, PDF_DOC);
  pdf_fields(L, 1, options);
  vectis_pdf_config_init(&config);
  config.title = pdf_string_field(L, 1, "title", NULL);
  config.author = pdf_string_field(L, 1, "author", NULL);
  config.subject = pdf_string_field(L, 1, "subject", NULL);
  if (vectis_pdf_new(&config, &owner->value, &error) != VECTIS_OK)
    return pdf_push_error(L, &error, "new");
  lua_pushvalue(L, 2);
  return 1;
}
static int pdf_add_page(lua_State *L) {
  static const char *const options[] = {"width", "height", NULL};
  pdf_lua_doc *document = pdf_document(L, 1);
  pdf_lua_child *owner;
  vectis_pdf_page_config config;
  vectis_pdf_page *value = NULL;
  vectis_error error;
  lua_settop(L, 2);
  owner = pdf_new_child(L, document, PDF_PAGE);
  pdf_fields(L, 2, options);
  vectis_pdf_page_config_init(&config);
  config.width = pdf_number_field(L, 2, "width", config.width);
  config.height = pdf_number_field(L, 2, "height", config.height);
  if (vectis_pdf_add_page(document->value, &config, &value, &error) !=
      VECTIS_OK)
    return pdf_push_error(L, &error, "add_page");
  owner->value = value;
  return 1;
}
static int pdf_font_inner(lua_State *L, int file) {
  static const char *const standard_options[] = {"encoding", NULL};
  static const char *const file_options[] = {"encoding", "embed", NULL};
  pdf_lua_doc *document = pdf_document(L, 1);
  pdf_lua_child *owner;
  vectis_pdf_font *value = NULL;
  vectis_error error;
  const char *name = pdf_string(L, 2), *encoding;
  int embed;
  vectis_status status;
  lua_settop(L, 3);
  owner = pdf_new_child(L, document, PDF_FONT);
  pdf_fields(L, 3, file ? file_options : standard_options);
  embed = file ? pdf_bool_field(L, 3, "embed", 1) : 0;
  encoding = pdf_string_field(L, 3, "encoding", NULL);
  status = file ? vectis_pdf_load_font(document->value, name, encoding, embed,
                                       &value, &error)
                : vectis_pdf_get_font(document->value, name, encoding, &value,
                                      &error);
  if (status != VECTIS_OK)
    return pdf_push_error(L, &error, file ? "load_font" : "font");
  owner->value = value;
  lua_pushvalue(L, 4);
  return 1;
}
static int pdf_get_font(lua_State *L) { return pdf_font_inner(L, 0); }
static int pdf_load_font(lua_State *L) { return pdf_font_inner(L, 1); }
static int pdf_load_image_inner(lua_State *L, int memory) {
  static const char *const options[] = {"format", NULL};
  pdf_lua_doc *document = pdf_document(L, 1);
  pdf_lua_child *owner;
  vectis_pdf_image *value = NULL;
  vectis_error error;
  const char *input, *format;
  size_t size = 0u;
  vectis_status status;
  luaL_checktype(L, 2, LUA_TSTRING);
  input = memory ? lua_tolstring(L, 2, &size) : pdf_string(L, 2);
  lua_settop(L, 3);
  owner = pdf_new_child(L, document, PDF_IMAGE);
  pdf_fields(L, 3, options);
  format = pdf_string_field(L, 3, "format", "png");
  status = memory ? vectis_pdf_load_image_materialized(
                        document->value, input, size, format, &value, &error)
                  : vectis_pdf_load_image(document->value, input, format,
                                          &value, &error);
  if (status != VECTIS_OK)
    return pdf_push_error(L, &error,
                          memory ? "load_image_materialized" : "load_image");
  owner->value = value;
  lua_pushvalue(L, 4);
  return 1;
}
static int pdf_load_image(lua_State *L) { return pdf_load_image_inner(L, 0); }
static int pdf_load_image_materialized(lua_State *L) {
  return pdf_load_image_inner(L, 1);
}
static int pdf_save(lua_State *L) {
  pdf_lua_doc *document = pdf_document(L, 1);
  const char *path = pdf_string(L, 2);
  vectis_error error;
  if (vectis_pdf_save(document->value, path, &error) != VECTIS_OK)
    return pdf_push_error(L, &error, "save");
  lua_pushboolean(L, 1);
  return 1;
}
static int pdf_bytes_close(lua_State *L) {
  vectis_mutable_bytes_cleanup((vectis_mutable_bytes *)lua_touserdata(L, 1));
  return 0;
}
static int pdf_bytes(lua_State *L) {
  static const char *const options[] = {"max_bytes", NULL};
  pdf_lua_doc *document = pdf_document(L, 1);
  vectis_mutable_bytes *bytes;
  vectis_error error;
  size_t limit = VECTIS_PDF_DEFAULT_MAX_BYTES;
  lua_settop(L, 2);
  pdf_fields(L, 2, options);
  pdf_field(L, 2, "max_bytes");
  if (!lua_isnil(L, -1)) {
    lua_Integer value = luaL_checkinteger(L, -1);
    luaL_argcheck(L, value > 0 && (lua_Unsigned)value <= UINT_MAX, 2,
                  "invalid PDF byte limit");
    limit = (size_t)value;
  }
  lua_pop(L, 1);
  bytes = (vectis_mutable_bytes *)lua_newuserdatauv(L, sizeof(*bytes), 0);
  memset(bytes, 0, sizeof(*bytes));
  luaL_setmetatable(L, PDF_BYTES);
  lua_toclose(L, lua_gettop(L));
  if (vectis_pdf_bytes_materialized(document->value, limit, bytes, &error) !=
      VECTIS_OK)
    return pdf_push_error(L, &error, "bytes_materialized");
  lua_pushlstring(L, (const char *)bytes->data, bytes->size);
  return 1;
}
static int pdf_reset_error(lua_State *L) {
  pdf_lua_doc *document = pdf_document(L, 1);
  vectis_error error;
  if (vectis_pdf_reset_error(document->value, &error) != VECTIS_OK)
    return pdf_push_error(L, &error, "reset_error");
  lua_pushboolean(L, 1);
  return 1;
}
static int pdf_page_text(lua_State *L) {
  static const char *const options[] = {"font", "size", "color", NULL};
  pdf_lua_child *page = pdf_child(L, 1, PDF_PAGE);
  vectis_pdf_text_config config;
  vectis_error error;
  double x = pdf_number(L, 2), y = pdf_number(L, 3);
  const char *text = pdf_string(L, 4);
  lua_settop(L, 5);
  pdf_fields(L, 5, options);
  vectis_pdf_text_config_init(&config);
  config.size = pdf_number_field(L, 5, "size", config.size);
  pdf_color(L, 5, config.color);
  pdf_field(L, 5, "font");
  if (!lua_isnil(L, -1))
    config.font =
        (vectis_pdf_font *)pdf_child(L, lua_gettop(L), PDF_FONT)->value;
  if (vectis_pdf_page_text((vectis_pdf_page *)page->value, x, y, text, &config,
                           &error) != VECTIS_OK)
    return pdf_push_error(L, &error, "text");
  lua_pushboolean(L, 1);
  return 1;
}
static int pdf_shape(lua_State *L, int rectangle) {
  static const char *const options[] = {"color", "line_width", "fill", "stroke",
                                        NULL};
  pdf_lua_child *page = pdf_child(L, 1, PDF_PAGE);
  vectis_pdf_draw_config config;
  vectis_error error;
  vectis_status status;
  double x = pdf_number(L, 2), y = pdf_number(L, 3);
  double a = pdf_number(L, 4), b = pdf_number(L, 5);
  lua_settop(L, 6);
  pdf_fields(L, 6, options);
  vectis_pdf_draw_config_init(&config);
  config.line_width = pdf_number_field(L, 6, "line_width", config.line_width);
  config.fill = pdf_bool_field(L, 6, "fill", config.fill);
  config.stroke = pdf_bool_field(L, 6, "stroke", config.stroke);
  pdf_color(L, 6, config.color);
  status = rectangle ? vectis_pdf_page_rectangle((vectis_pdf_page *)page->value,
                                                 x, y, a, b, &config, &error)
                     : vectis_pdf_page_line((vectis_pdf_page *)page->value, x,
                                            y, a, b, &config, &error);
  if (status != VECTIS_OK)
    return pdf_push_error(L, &error, rectangle ? "rectangle" : "line");
  lua_pushboolean(L, 1);
  return 1;
}
static int pdf_line(lua_State *L) { return pdf_shape(L, 0); }
static int pdf_rectangle(lua_State *L) { return pdf_shape(L, 1); }
static int pdf_page_image(lua_State *L) {
  pdf_lua_child *page = pdf_child(L, 1, PDF_PAGE), *image;
  vectis_error error;
  double x = pdf_number(L, 3), y = pdf_number(L, 4);
  double width = pdf_number(L, 5), height = pdf_number(L, 6);
  image = pdf_child(L, 2, PDF_IMAGE);
  if (vectis_pdf_page_image((vectis_pdf_page *)page->value,
                            (vectis_pdf_image *)image->value, x, y, width,
                            height, &error) != VECTIS_OK)
    return pdf_push_error(L, &error, "image");
  lua_pushboolean(L, 1);
  return 1;
}
static int pdf_image_info(lua_State *L) {
  pdf_lua_child *owner = pdf_child(L, 1, PDF_IMAGE);
  vectis_pdf_image *image = (vectis_pdf_image *)owner->value;
  lua_createtable(L, 0, 2);
  lua_pushinteger(L, image->width);
  lua_setfield(L, -2, "width");
  lua_pushinteger(L, image->height);
  lua_setfield(L, -2, "height");
  return 1;
}
static int pdf_guarded(lua_State *L) {
  static const lua_CFunction methods[] = {pdf_add_page,
                                          pdf_get_font,
                                          pdf_load_font,
                                          pdf_load_image,
                                          pdf_save,
                                          pdf_bytes,
                                          pdf_reset_error,
                                          pdf_page_text,
                                          pdf_line,
                                          pdf_rectangle,
                                          pdf_page_image,
                                          pdf_image_info,
                                          pdf_load_image_materialized};
  int operation = (int)lua_tointeger(L, lua_upvalueindex(1));
  int args = lua_gettop(L), status;
  pdf_lua_doc *document = (pdf_lua_doc *)luaL_testudata(L, 1, PDF_DOC);
  pdf_lua_child *child = NULL;
  luaL_argcheck(L,
                operation >= 0 &&
                    (size_t)operation < sizeof(methods) / sizeof(methods[0]),
                1, "invalid PDF operation");
  if (document == NULL) {
    child = pdf_child(L, 1, operation == 11 ? PDF_IMAGE : PDF_PAGE);
    document = child->document;
  }
  luaL_argcheck(L, !document->busy && (child == NULL || !child->busy), 1,
                "PDF document is active");
  luaL_checkstack(L, args + 2, "PDF operation stack");
  lua_pushcfunction(L, methods[operation]);
  lua_insert(L, 1);
  document->busy = 1;
  if (child != NULL)
    child->busy = 1;
  status = lua_pcall(L, args, LUA_MULTRET, 0);
  document->busy = 0;
  if (child != NULL)
    child->busy = 0;
  if (status != LUA_OK)
    return lua_error(L);
  return lua_gettop(L);
}
static void pdf_method(lua_State *L, const char *name, int operation) {
  lua_pushinteger(L, operation);
  lua_pushcclosure(L, pdf_guarded, 1);
  lua_setfield(L, -2, name);
}
static void pdf_metatable(lua_State *L, const char *name) {
  luaL_newmetatable(L, name);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushstring(L, name);
  lua_setfield(L, -2, "__metatable");
  if (strcmp(name, PDF_DOC) == 0) {
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, pdf_doc_close, 1);
    lua_setfield(L, -2, "close");
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, pdf_doc_close, 1);
  } else {
    lua_pushstring(L, name);
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, pdf_view_close, 2);
    lua_setfield(L, -2, "close");
    lua_pushstring(L, name);
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, pdf_view_close, 2);
  }
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "__gc");
  lua_setfield(L, -2, "__close");
}
int luaopen_vectis_pdf_core(lua_State *L) {
  luaL_newmetatable(L, PDF_BYTES);
  lua_pushcfunction(L, pdf_bytes_close);
  lua_setfield(L, -2, "__close");
  lua_pushcfunction(L, pdf_bytes_close);
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);
  pdf_metatable(L, PDF_DOC);
  pdf_method(L, "add_page", 0);
  pdf_method(L, "font", 1);
  pdf_method(L, "load_font", 2);
  pdf_method(L, "load_image", 3);
  pdf_method(L, "load_image_materialized", 12);
  pdf_method(L, "save", 4);
  pdf_method(L, "bytes_materialized", 5);
  pdf_method(L, "reset_error", 6);
  lua_pop(L, 1);
  pdf_metatable(L, PDF_PAGE);
  pdf_method(L, "text", 7);
  pdf_method(L, "line", 8);
  pdf_method(L, "rectangle", 9);
  pdf_method(L, "image", 10);
  lua_pop(L, 1);
  pdf_metatable(L, PDF_FONT);
  lua_pop(L, 1);
  pdf_metatable(L, PDF_IMAGE);
  pdf_method(L, "info", 11);
  lua_pop(L, 1);
  lua_newtable(L);
  lua_pushcfunction(L, pdf_new);
  lua_setfield(L, -2, "new");
  lua_pushinteger(L, VECTIS_PDF_DEFAULT_MAX_BYTES);
  lua_setfield(L, -2, "DEFAULT_MAX_BYTES");
  return 1;
}
