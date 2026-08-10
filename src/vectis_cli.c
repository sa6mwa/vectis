#include "vectis_cli.h"

#include <cpkt/lua_runtime.h>
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <lauxlib.h>
#ifndef LONEJSON_WITH_CURL
#define LONEJSON_WITH_CURL 1
#endif
#include <lonejson.h>
#include <lonejson_lua.h>
#include <lua.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vectis/auth.h>
#include <vectis/embedded_fs.h>
#include <vectis/totp_qr.h>
#include <vectis/vectis.h>
#include <vectis/vectis_version.h>

#include "vectis_curl_lua_init.h"
#include "vectis_libmdf_lua_init.h"
#include "vectis_lockdc_lua_init.h"
#include "vectis_pslog_lua_init.h"

#define VECTIS_PACK_FOOTER_SIZE 256u
#define VECTIS_PACK_MAGIC "VECTIS_PACK"
#define VECTIS_PACK_MAGIC_SIZE 11u
#define VECTIS_LUA_CURL_RESPONSE_BODY_LIMIT (8u * 1024u * 1024u)
#define VECTIS_LUA_CURL_RESPONSE_HEADER_LIMIT (64u * 1024u)

typedef struct vectis_lua_runtime_context {
  const unsigned char *embedded_lockd_bundle;
  size_t embedded_lockd_bundle_size;
  const unsigned char *embedded_asset_payload;
  size_t embedded_asset_payload_size;
  const unsigned char *embedded_manifest;
  size_t embedded_manifest_size;
  vectis_embedded_fs *embedded_fs;
} vectis_lua_runtime_context;

typedef struct vectis_pack_asset {
  char *source_path;
  char *logical_path;
  char *content_type;
  unsigned char *data;
  size_t size;
  size_t offset;
  unsigned char sha[SHA256_DIGEST_LENGTH];
} vectis_pack_asset;

typedef struct vectis_pack_asset_list {
  vectis_pack_asset *items;
  size_t count;
  size_t capacity;
} vectis_pack_asset_list;

typedef struct vectis_pack_content_type_map_entry {
  char *extension;
  char *content_type;
} vectis_pack_content_type_map_entry;

typedef struct vectis_pack_content_type_map {
  vectis_pack_content_type_map_entry *items;
  size_t count;
  size_t capacity;
} vectis_pack_content_type_map;

typedef struct vectis_pack_content_type_map_doc_item {
  char *extension;
  char *content_type;
} vectis_pack_content_type_map_doc_item;

typedef struct vectis_pack_content_type_map_doc {
  lonejson_mapped_array_stream types;
} vectis_pack_content_type_map_doc;

typedef struct vectis_pack_content_type_map_state {
  vectis_pack_content_type_map *map;
} vectis_pack_content_type_map_state;

typedef struct vectis_pack_asset_manifest_item {
  char *source;
  char *path;
  char *content_type;
} vectis_pack_asset_manifest_item;

typedef struct vectis_pack_asset_manifest {
  lonejson_mapped_array_stream assets;
} vectis_pack_asset_manifest;

typedef struct vectis_pack_asset_manifest_state {
  vectis_pack_asset_list *assets;
  const vectis_pack_content_type_map *content_types;
} vectis_pack_asset_manifest_state;

static const lonejson_field vectis_pack_content_type_map_doc_item_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(vectis_pack_content_type_map_doc_item,
                                    extension, "extension"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(vectis_pack_content_type_map_doc_item,
                                    content_type, "content_type")};

LONEJSON_MAP_DEFINE(vectis_pack_content_type_map_doc_item_map,
                    vectis_pack_content_type_map_doc_item,
                    vectis_pack_content_type_map_doc_item_fields);

static const lonejson_field vectis_pack_content_type_map_doc_fields[] = {
    LONEJSON_FIELD_MAPPED_ARRAY_STREAM_REQ(vectis_pack_content_type_map_doc,
                                           types, "types")};

LONEJSON_MAP_DEFINE(vectis_pack_content_type_map_doc_map,
                    vectis_pack_content_type_map_doc,
                    vectis_pack_content_type_map_doc_fields);

static const lonejson_field vectis_pack_asset_manifest_item_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(vectis_pack_asset_manifest_item, source,
                                    "source"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(vectis_pack_asset_manifest_item, path,
                                    "path"),
    LONEJSON_FIELD_STRING_ALLOC(vectis_pack_asset_manifest_item, content_type,
                                "content_type")};

LONEJSON_MAP_DEFINE(vectis_pack_asset_manifest_item_map,
                    vectis_pack_asset_manifest_item,
                    vectis_pack_asset_manifest_item_fields);

static const lonejson_field vectis_pack_asset_manifest_fields[] = {
    LONEJSON_FIELD_MAPPED_ARRAY_STREAM_REQ(vectis_pack_asset_manifest, assets,
                                           "assets")};

LONEJSON_MAP_DEFINE(vectis_pack_asset_manifest_map, vectis_pack_asset_manifest,
                    vectis_pack_asset_manifest_fields);

typedef struct vectis_lua_totp {
  vectis_totp value;
} vectis_lua_totp;

typedef struct vectis_lua_qr {
  vectis_qr value;
} vectis_lua_qr;

typedef struct vectis_lua_curl_buffer {
  char *data;
  size_t size;
  size_t capacity;
  size_t offset;
  size_t limit;
  int limit_exceeded;
} vectis_lua_curl_buffer;

typedef struct vectis_lua_curl_stream_response {
  lonejson_curl_parse *parser;
  size_t size;
  size_t limit;
  int limit_exceeded;
} vectis_lua_curl_stream_response;

typedef struct vectis_lua_embedded_chunks_state {
  const char *data;
  size_t size;
  size_t offset;
  size_t chunk_size;
} vectis_lua_embedded_chunks_state;

typedef struct vectis_lua_auth_oauth2_transport {
  lua_State *lua;
  int callback_ref;
} vectis_lua_auth_oauth2_transport;

#define VECTIS_LUA_TOTP "vectis.totp"
#define VECTIS_LUA_QR "vectis.qr"

extern int luaopen_lonejson_core(lua_State *lua);
extern int luaopen_lockdc_core(lua_State *lua);
extern int luaopen_cai(lua_State *lua);
extern int luaopen_libmdf_core(lua_State *lua);
extern int luaopen_pslog_core(lua_State *lua);
extern int luaopen_softline(lua_State *lua);

static pthread_once_t vectis_lua_curl_once = PTHREAD_ONCE_INIT;

static char *vectis_cli_strdup(const char *value) {
  char *copy;
  size_t size;

  if (value == NULL) {
    return NULL;
  }
  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, size);
  return copy;
}

static void *vectis_cli_memdup(const void *data, size_t size) {
  void *copy;

  if (data == NULL || size == 0u) {
    return NULL;
  }
  copy = malloc(size);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, data, size);
  return copy;
}

static const char vectis_lonejson_lua_init[] =
    "local core = require(\"lonejson.core\")\n"
    "local M = {}\n"
    "local function field(kind, opts)\n"
    "  opts = opts or {}\n"
    "  opts.kind = kind\n"
    "  return opts\n"
    "end\n"
    "function M.field(name, spec)\n"
    "  spec = spec or {}\n"
    "  spec.name = name\n"
    "  return spec\n"
    "end\n"
    "function M.string(opts) return field(\"string\", opts) end\n"
    "function M.spooled_text(opts) return field(\"spooled_text\", opts) end\n"
    "function M.spooled_bytes(opts) return field(\"spooled_bytes\", opts) end\n"
    "function M.json_value(opts) return field(\"json_value\", opts) end\n"
    "function M.i64(opts) return field(\"i64\", opts) end\n"
    "function M.u64(opts) return field(\"u64\", opts) end\n"
    "function M.f64(opts) return field(\"f64\", opts) end\n"
    "function M.boolean(opts) return field(\"boolean\", opts) end\n"
    "M.bool = M.boolean\n"
    "function M.object(opts) return field(\"object\", opts) end\n"
    "function M.string_array(opts) return field(\"string_array\", opts) end\n"
    "function M.i64_array(opts) return field(\"i64_array\", opts) end\n"
    "function M.u64_array(opts) return field(\"u64_array\", opts) end\n"
    "function M.f64_array(opts) return field(\"f64_array\", opts) end\n"
    "function M.boolean_array(opts) return field(\"boolean_array\", opts) end\n"
    "function M.object_array(opts) return field(\"object_array\", opts) end\n"
    "function M.json_array(value)\n"
    "  value = value or {}\n"
    "  return setmetatable(value, { __lonejson_json_kind = \"array\" })\n"
    "end\n"
    "function M.json_object(value)\n"
    "  value = value or {}\n"
    "  return setmetatable(value, { __lonejson_json_kind = \"object\" })\n"
    "end\n"
    "function M.schema(name, fields) return core.new():schema(name, fields) "
    "end\n"
    "function M.chunks(spool, chunk_size)\n"
    "  spool:rewind()\n"
    "  return function() return spool:read(chunk_size or 4096) end\n"
    "end\n"
    "M.array_rewrite_string = core.array_rewrite_string\n"
    "M.array_rewrite_path = core.array_rewrite_path\n"
    "M.encode_json = core.encode_json\n"
    "M.encode_json_to_sink = core.encode_json_to_sink\n"
    "M.encode_value = core.encode_json\n"
    "M.encode_value_to_sink = core.encode_json_to_sink\n"
    "M.decode_json = core.decode_json\n"
    "M.decode_value = core.decode_json\n"
    "M.core = core\n"
    "M.json_null = core.json_null()\n"
    "return M\n";

static void vectis_cli_usage(FILE *stream) {
  fputs("usage: vectis [--version] [--help] [-x] script.lua [args...]\n"
        "       -x traces Lua line execution to stderr\n"
        "       vectis -a|--action pack --script script.lua --output output "
        "[--lockd-bundle bundle.pem] [--asset source=/path] "
        "[--asset-dir /mount:dir] [--asset-manifest assets.json] "
        "[--content-type-map types.json]\n"
        "       vectis -a|--action credentials [--store credentials.json] "
        "(--init | --issue --subject user [--purpose name] "
        "[--basic] [--bearer] | --verify authorization | "
        "--revoke client_id)\n"
        "       vectis -a|--action users [--store credentials.json] "
        "(--add username [--password value] [--totp] | "
        "--login username --password value [--totp-code code] | "
        "--webdav-key username --password value [--totp-code code])\n"
        "       vectis -a|--action oauth2 [--store credentials.json] "
        "(--authorize --authorization-endpoint url --client-id id "
        "--redirect-uri uri | --exchange-callback flow_id --subject user "
        "--token-endpoint url --client-id id --redirect-uri uri "
        "--code-verifier value --callback-query query --expected-state state | "
        "--client-credentials flow_id --subject user --token-endpoint url "
        "--client-id id --client-secret value | --upsert-flow flow_id "
        "--subject user --access-token token | --load-flow flow_id | "
        "--ensure-flow flow_id [--now seconds] | "
        "--webdav-key flow_id --subject user)\n",
        stream);
}

static void vectis_pack_write_u64(unsigned char *out,
                                  unsigned long long value) {
  int i;

  for (i = 0; i < 8; ++i) {
    out[i] = (unsigned char)((value >> (8 * i)) & 0xffu);
  }
}

static unsigned long long vectis_pack_read_u64(const unsigned char *in) {
  unsigned long long value;
  int i;

  value = 0u;
  for (i = 0; i < 8; ++i) {
    value |= ((unsigned long long)in[i]) << (8 * i);
  }
  return value;
}

static int vectis_read_all(const char *path, unsigned char **out,
                           size_t *out_size) {
  FILE *fp;
  long length;
  unsigned char *buffer;
  size_t nread;

  if (path == NULL || out == NULL || out_size == NULL) {
    return -1;
  }
  *out = NULL;
  *out_size = 0u;
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return -1;
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    (void)fclose(fp);
    return -1;
  }
  length = ftell(fp);
  if (length < 0L || fseek(fp, 0L, SEEK_SET) != 0) {
    (void)fclose(fp);
    return -1;
  }
  buffer = NULL;
  if (length > 0L) {
    buffer = (unsigned char *)malloc((size_t)length);
    if (buffer == NULL) {
      (void)fclose(fp);
      return -1;
    }
    nread = fread(buffer, 1u, (size_t)length, fp);
    if (nread != (size_t)length) {
      free(buffer);
      (void)fclose(fp);
      return -1;
    }
  }
  if (fclose(fp) != 0) {
    free(buffer);
    return -1;
  }
  *out = buffer;
  *out_size = (size_t)length;
  return 0;
}

static int vectis_write_all(FILE *fp, const void *data, size_t size) {
  if (size == 0u) {
    return 0;
  }
  return fwrite(data, 1u, size, fp) == size ? 0 : -1;
}

static void vectis_sha256_hex(const unsigned char sha[SHA256_DIGEST_LENGTH],
                              char out[SHA256_DIGEST_LENGTH * 2u + 1u]) {
  static const char hex[] = "0123456789abcdef";
  size_t i;

  for (i = 0u; i < SHA256_DIGEST_LENGTH; ++i) {
    out[i * 2u] = hex[(sha[i] >> 4) & 0x0f];
    out[i * 2u + 1u] = hex[sha[i] & 0x0f];
  }
  out[SHA256_DIGEST_LENGTH * 2u] = '\0';
}

static int vectis_pack_logical_path_valid(const char *path) {
  size_t i;
  char c;

  if (path == NULL || path[0] != '/' || path[1] == '\0') {
    return 0;
  }
  for (i = 0u; path[i] != '\0'; ++i) {
    c = path[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' ||
        c == '-') {
      continue;
    }
    return 0;
  }
  if (strstr(path, "/../") != NULL || strcmp(path, "/..") == 0 ||
      strstr(path, "/./") != NULL || strcmp(path, "/.") == 0 ||
      strstr(path, "//") != NULL) {
    return 0;
  }
  return 1;
}

static int vectis_ascii_equal_ci(const char *left, const char *right) {
  unsigned char a;
  unsigned char b;

  if (left == NULL || right == NULL) {
    return 0;
  }
  while (*left != '\0' && *right != '\0') {
    a = (unsigned char)*left++;
    b = (unsigned char)*right++;
    if (a >= 'A' && a <= 'Z') {
      a = (unsigned char)(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
      b = (unsigned char)(b - 'A' + 'a');
    }
    if (a != b) {
      return 0;
    }
  }
  return *left == '\0' && *right == '\0';
}

static const char *vectis_pack_content_type_for_path(const char *path) {
  const char *slash;
  const char *dot;

  if (path == NULL) {
    return NULL;
  }
  slash = strrchr(path, '/');
  dot = strrchr(path, '.');
  if (dot == NULL || (slash != NULL && dot < slash) || dot[1] == '\0') {
    return NULL;
  }
  if (vectis_ascii_equal_ci(dot, ".html") ||
      vectis_ascii_equal_ci(dot, ".htm")) {
    return "text/html";
  }
  if (vectis_ascii_equal_ci(dot, ".css")) {
    return "text/css";
  }
  if (vectis_ascii_equal_ci(dot, ".js") || vectis_ascii_equal_ci(dot, ".mjs")) {
    return "application/javascript";
  }
  if (vectis_ascii_equal_ci(dot, ".json") ||
      vectis_ascii_equal_ci(dot, ".map")) {
    return "application/json";
  }
  if (vectis_ascii_equal_ci(dot, ".txt") ||
      vectis_ascii_equal_ci(dot, ".text")) {
    return "text/plain";
  }
  if (vectis_ascii_equal_ci(dot, ".svg")) {
    return "image/svg+xml";
  }
  if (vectis_ascii_equal_ci(dot, ".png")) {
    return "image/png";
  }
  if (vectis_ascii_equal_ci(dot, ".jpg") ||
      vectis_ascii_equal_ci(dot, ".jpeg")) {
    return "image/jpeg";
  }
  if (vectis_ascii_equal_ci(dot, ".gif")) {
    return "image/gif";
  }
  if (vectis_ascii_equal_ci(dot, ".webp")) {
    return "image/webp";
  }
  if (vectis_ascii_equal_ci(dot, ".ico")) {
    return "image/x-icon";
  }
  if (vectis_ascii_equal_ci(dot, ".woff")) {
    return "font/woff";
  }
  if (vectis_ascii_equal_ci(dot, ".woff2")) {
    return "font/woff2";
  }
  if (vectis_ascii_equal_ci(dot, ".wasm")) {
    return "application/wasm";
  }
  return NULL;
}

static int vectis_pack_content_type_valid(const char *content_type) {
  const unsigned char *cursor;

  if (content_type == NULL || content_type[0] == '\0') {
    return 0;
  }
  cursor = (const unsigned char *)content_type;
  while (*cursor != '\0') {
    if (*cursor < 0x20u || *cursor == 0x7fu) {
      return 0;
    }
    cursor++;
  }
  return 1;
}

static int vectis_pack_extension_valid(const char *extension) {
  const unsigned char *cursor;

  if (extension == NULL || extension[0] == '\0') {
    return 0;
  }
  cursor = (const unsigned char *)extension;
  if (*cursor == '.') {
    cursor++;
  }
  if (*cursor == '\0') {
    return 0;
  }
  while (*cursor != '\0') {
    if (*cursor <= 0x20u || *cursor == 0x7fu || *cursor == '/' ||
        *cursor == '\\') {
      return 0;
    }
    cursor++;
  }
  return 1;
}

static int
vectis_pack_content_type_map_reserve(vectis_pack_content_type_map *map,
                                     size_t capacity) {
  vectis_pack_content_type_map_entry *grown;
  size_t next_capacity;

  if (capacity <= map->capacity) {
    return 0;
  }
  next_capacity = map->capacity == 0u ? 8u : map->capacity;
  while (next_capacity < capacity) {
    if (next_capacity > ((size_t)-1) / 2u) {
      return -1;
    }
    next_capacity *= 2u;
  }
  grown = (vectis_pack_content_type_map_entry *)realloc(
      map->items, next_capacity * sizeof(map->items[0]));
  if (grown == NULL) {
    return -1;
  }
  map->items = grown;
  map->capacity = next_capacity;
  return 0;
}

static void
vectis_pack_content_type_map_cleanup(vectis_pack_content_type_map *map) {
  size_t i;

  if (map == NULL) {
    return;
  }
  for (i = 0u; i < map->count; ++i) {
    free(map->items[i].extension);
    free(map->items[i].content_type);
  }
  free(map->items);
  memset(map, 0, sizeof(*map));
}

static int vectis_pack_content_type_map_add(vectis_pack_content_type_map *map,
                                            const char *extension,
                                            const char *content_type) {
  vectis_pack_content_type_map_entry *entry;
  char *normalized_extension;
  char *content_type_copy;
  size_t extension_size;

  if (!vectis_pack_extension_valid(extension)) {
    fprintf(stderr, "vectis: invalid content-type map extension: %s\n",
            extension != NULL ? extension : "(null)");
    return -1;
  }
  if (!vectis_pack_content_type_valid(content_type)) {
    fprintf(stderr, "vectis: invalid content-type map content type: %s\n",
            content_type != NULL ? content_type : "(null)");
    return -1;
  }
  extension_size = strlen(extension);
  if (extension[0] == '.') {
    normalized_extension = vectis_cli_strdup(extension);
  } else {
    normalized_extension = (char *)malloc(extension_size + 2u);
    if (normalized_extension != NULL) {
      normalized_extension[0] = '.';
      memcpy(normalized_extension + 1, extension, extension_size + 1u);
    }
  }
  content_type_copy = vectis_cli_strdup(content_type);
  if (normalized_extension == NULL || content_type_copy == NULL ||
      vectis_pack_content_type_map_reserve(map, map->count + 1u) != 0) {
    free(normalized_extension);
    free(content_type_copy);
    return -1;
  }
  entry = map->items + map->count;
  entry->extension = normalized_extension;
  entry->content_type = content_type_copy;
  map->count++;
  return 0;
}

static int vectis_pack_has_suffix_ci(const char *value, const char *suffix) {
  size_t value_len;
  size_t suffix_len;

  if (value == NULL || suffix == NULL) {
    return 0;
  }
  value_len = strlen(value);
  suffix_len = strlen(suffix);
  if (suffix_len > value_len) {
    return 0;
  }
  return vectis_ascii_equal_ci(value + value_len - suffix_len, suffix);
}

static const char *
vectis_pack_content_type_map_lookup(const vectis_pack_content_type_map *map,
                                    const char *path) {
  size_t i;

  if (map == NULL || path == NULL) {
    return NULL;
  }
  for (i = map->count; i > 0u; --i) {
    if (vectis_pack_has_suffix_ci(path, map->items[i - 1u].extension)) {
      return map->items[i - 1u].content_type;
    }
  }
  return NULL;
}

static char *vectis_pack_join_path(const char *left, const char *right,
                                   char separator) {
  size_t left_size;
  size_t right_size;
  size_t need_separator;
  char *out;

  if (left == NULL || right == NULL) {
    return NULL;
  }
  left_size = strlen(left);
  right_size = strlen(right);
  need_separator =
      left_size > 0u && left[left_size - 1u] != separator ? 1u : 0u;
  out = (char *)malloc(left_size + need_separator + right_size + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, left, left_size);
  if (need_separator) {
    out[left_size] = separator;
  }
  memcpy(out + left_size + need_separator, right, right_size + 1u);
  return out;
}

static int vectis_pack_asset_list_reserve(vectis_pack_asset_list *list,
                                          size_t capacity) {
  vectis_pack_asset *grown;
  size_t next_capacity;

  if (capacity <= list->capacity) {
    return 0;
  }
  next_capacity = list->capacity == 0u ? 8u : list->capacity;
  while (next_capacity < capacity) {
    if (next_capacity > ((size_t)-1) / 2u) {
      return -1;
    }
    next_capacity *= 2u;
  }
  grown = (vectis_pack_asset *)realloc(list->items,
                                       next_capacity * sizeof(list->items[0]));
  if (grown == NULL) {
    return -1;
  }
  list->items = grown;
  list->capacity = next_capacity;
  return 0;
}

static void vectis_pack_asset_list_cleanup(vectis_pack_asset_list *list) {
  size_t i;

  if (list == NULL) {
    return;
  }
  for (i = 0u; i < list->count; ++i) {
    free(list->items[i].source_path);
    free(list->items[i].logical_path);
    free(list->items[i].content_type);
    free(list->items[i].data);
  }
  free(list->items);
  memset(list, 0, sizeof(*list));
}

static int vectis_pack_asset_add(vectis_pack_asset_list *list,
                                 const char *source_path,
                                 const char *logical_path,
                                 const char *content_type_override,
                                 const vectis_pack_content_type_map *map) {
  vectis_pack_asset *asset;
  unsigned char *data;
  const char *content_type;
  size_t size;

  if (!vectis_pack_logical_path_valid(logical_path)) {
    fprintf(stderr, "vectis: invalid embedded asset path: %s\n",
            logical_path != NULL ? logical_path : "(null)");
    return -1;
  }
  if (content_type_override != NULL &&
      !vectis_pack_content_type_valid(content_type_override)) {
    fprintf(stderr, "vectis: invalid embedded asset content type: %s\n",
            content_type_override);
    return -1;
  }
  if (vectis_read_all(source_path, &data, &size) != 0) {
    fprintf(stderr, "vectis: failed to read embedded asset: %s\n",
            source_path != NULL ? source_path : "(null)");
    return -1;
  }
  if (vectis_pack_asset_list_reserve(list, list->count + 1u) != 0) {
    free(data);
    return -1;
  }
  asset = list->items + list->count;
  memset(asset, 0, sizeof(*asset));
  asset->source_path = vectis_cli_strdup(source_path);
  asset->logical_path = vectis_cli_strdup(logical_path);
  content_type =
      content_type_override != NULL ? content_type_override
      : vectis_pack_content_type_map_lookup(map, logical_path) != NULL
          ? vectis_pack_content_type_map_lookup(map, logical_path)
          : vectis_pack_content_type_for_path(logical_path);
  if (content_type != NULL) {
    asset->content_type = vectis_cli_strdup(content_type);
  }
  if (asset->source_path == NULL || asset->logical_path == NULL ||
      (content_type != NULL && asset->content_type == NULL)) {
    free(asset->source_path);
    free(asset->logical_path);
    free(asset->content_type);
    free(data);
    memset(asset, 0, sizeof(*asset));
    return -1;
  }
  asset->data = data;
  asset->size = size;
  SHA256(data, size, asset->sha);
  list->count++;
  return 0;
}

static lonejson_status
vectis_pack_content_type_map_item_cb(void *user, void *item,
                                     lonejson_error *json_error) {
  vectis_pack_content_type_map_state *state;
  const vectis_pack_content_type_map_doc_item *entry;

  (void)json_error;
  state = (vectis_pack_content_type_map_state *)user;
  entry = (const vectis_pack_content_type_map_doc_item *)item;
  if (vectis_pack_content_type_map_add(state->map, entry->extension,
                                       entry->content_type) != 0) {
    return LONEJSON_STATUS_INVALID_JSON;
  }
  return LONEJSON_STATUS_OK;
}

static int vectis_pack_read_content_type_map(vectis_pack_content_type_map *map,
                                             const char *path) {
  unsigned char *json;
  size_t json_size;
  lonejson *runtime;
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson_mapped_array_stream_handler handler;
  vectis_pack_content_type_map_doc doc;
  vectis_pack_content_type_map_doc_item item;
  vectis_pack_content_type_map_state state;

  json = NULL;
  json_size = 0u;
  if (vectis_read_all(path, &json, &json_size) != 0) {
    fprintf(stderr, "vectis: failed to read content-type map: %s\n",
            path != NULL ? path : "(null)");
    return -1;
  }
  lonejson_error_init(&json_error);
  runtime = lonejson_new(NULL, &json_error);
  if (runtime == NULL) {
    free(json);
    fprintf(stderr,
            "vectis: failed to initialize content-type map parser: %s\n",
            json_error.message[0] != '\0'
                ? json_error.message
                : lonejson_status_string(LONEJSON_STATUS_ALLOCATION_FAILED));
    return -1;
  }
  memset(&doc, 0, sizeof(doc));
  memset(&item, 0, sizeof(item));
  memset(&handler, 0, sizeof(handler));
  doc.types = (lonejson_mapped_array_stream)LONEJSON_MAPPED_ARRAY_STREAM_INIT;
  state.map = map;
  handler.item_map = &vectis_pack_content_type_map_doc_item_map;
  handler.item_dst = &item;
  handler.item = vectis_pack_content_type_map_item_cb;
  handler.user = &state;
  lonejson_error_init(&json_error);
  json_status = lonejson_mapped_array_stream_set_handler(&doc.types, &handler,
                                                         &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    json_status =
        lonejson_parse_buffer(runtime, &vectis_pack_content_type_map_doc_map,
                              &doc, json, json_size, &json_error);
  }
  lonejson_cleanup(&vectis_pack_content_type_map_doc_map, &doc);
  lonejson_cleanup(&vectis_pack_content_type_map_doc_item_map, &item);
  lonejson_free(runtime);
  free(json);
  if (json_status != LONEJSON_STATUS_OK) {
    fprintf(stderr, "vectis: failed to parse content-type map %s: %s\n",
            path != NULL ? path : "(null)",
            json_error.message[0] != '\0'
                ? json_error.message
                : lonejson_status_string(json_status));
    return -1;
  }
  return 0;
}

static lonejson_status
vectis_pack_asset_manifest_item_cb(void *user, void *item,
                                   lonejson_error *json_error) {
  vectis_pack_asset_manifest_state *state;
  const vectis_pack_asset_manifest_item *asset;

  (void)json_error;
  state = (vectis_pack_asset_manifest_state *)user;
  asset = (const vectis_pack_asset_manifest_item *)item;
  if (vectis_pack_asset_add(state->assets, asset->source, asset->path,
                            asset->content_type, state->content_types) != 0) {
    return LONEJSON_STATUS_INVALID_JSON;
  }
  return LONEJSON_STATUS_OK;
}

static int vectis_pack_read_asset_manifest(
    vectis_pack_asset_list *assets, const char *path,
    const vectis_pack_content_type_map *content_types) {
  unsigned char *json;
  size_t json_size;
  lonejson *runtime;
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson_mapped_array_stream_handler handler;
  vectis_pack_asset_manifest doc;
  vectis_pack_asset_manifest_item item;
  vectis_pack_asset_manifest_state state;

  json = NULL;
  json_size = 0u;
  if (vectis_read_all(path, &json, &json_size) != 0) {
    fprintf(stderr, "vectis: failed to read embedded asset manifest: %s\n",
            path != NULL ? path : "(null)");
    return -1;
  }
  lonejson_error_init(&json_error);
  runtime = lonejson_new(NULL, &json_error);
  if (runtime == NULL) {
    free(json);
    fprintf(stderr,
            "vectis: failed to initialize embedded asset manifest "
            "parser: %s\n",
            json_error.message[0] != '\0'
                ? json_error.message
                : lonejson_status_string(LONEJSON_STATUS_ALLOCATION_FAILED));
    return -1;
  }
  memset(&doc, 0, sizeof(doc));
  memset(&item, 0, sizeof(item));
  memset(&handler, 0, sizeof(handler));
  doc.assets = (lonejson_mapped_array_stream)LONEJSON_MAPPED_ARRAY_STREAM_INIT;
  state.assets = assets;
  state.content_types = content_types;
  handler.item_map = &vectis_pack_asset_manifest_item_map;
  handler.item_dst = &item;
  handler.item = vectis_pack_asset_manifest_item_cb;
  handler.user = &state;
  lonejson_error_init(&json_error);
  json_status = lonejson_mapped_array_stream_set_handler(&doc.assets, &handler,
                                                         &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    json_status =
        lonejson_parse_buffer(runtime, &vectis_pack_asset_manifest_map, &doc,
                              json, json_size, &json_error);
  }
  lonejson_cleanup(&vectis_pack_asset_manifest_map, &doc);
  lonejson_cleanup(&vectis_pack_asset_manifest_item_map, &item);
  lonejson_free(runtime);
  free(json);
  if (json_status != LONEJSON_STATUS_OK) {
    fprintf(stderr, "vectis: failed to parse embedded asset manifest %s: %s\n",
            path != NULL ? path : "(null)",
            json_error.message[0] != '\0'
                ? json_error.message
                : lonejson_status_string(json_status));
    return -1;
  }
  return 0;
}

static int vectis_pack_asset_compare(const void *a, const void *b) {
  const vectis_pack_asset *left;
  const vectis_pack_asset *right;

  left = (const vectis_pack_asset *)a;
  right = (const vectis_pack_asset *)b;
  return strcmp(left->logical_path, right->logical_path);
}

static int vectis_pack_collect_dir(vectis_pack_asset_list *list,
                                   const char *source_dir,
                                   const char *logical_root,
                                   const vectis_pack_content_type_map *map) {
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  char *source_child;
  char *logical_child;
  int rc;

  dir = opendir(source_dir);
  if (dir == NULL) {
    fprintf(stderr, "vectis: failed to open embedded asset directory: %s\n",
            source_dir);
    return -1;
  }
  rc = 0;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    source_child = vectis_pack_join_path(source_dir, entry->d_name, '/');
    logical_child = vectis_pack_join_path(logical_root, entry->d_name, '/');
    if (source_child == NULL || logical_child == NULL) {
      free(source_child);
      free(logical_child);
      rc = -1;
      break;
    }
    if (lstat(source_child, &st) != 0) {
      fprintf(stderr, "vectis: failed to stat embedded asset: %s\n",
              source_child);
      free(source_child);
      free(logical_child);
      rc = -1;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      rc = vectis_pack_collect_dir(list, source_child, logical_child, map);
    } else if (S_ISREG(st.st_mode)) {
      rc = vectis_pack_asset_add(list, source_child, logical_child, NULL, map);
    } else {
      fprintf(stderr, "vectis: unsupported embedded asset type: %s\n",
              source_child);
      rc = -1;
    }
    free(source_child);
    free(logical_child);
    if (rc != 0) {
      break;
    }
  }
  if (closedir(dir) != 0 && rc == 0) {
    rc = -1;
  }
  return rc;
}

static int vectis_pack_build_manifest(vectis_pack_asset_list *assets,
                                      unsigned char **out, size_t *out_size) {
  lonejson *runtime;
  lonejson_writer writer;
  lonejson_owned_buffer buffer;
  lonejson_error error;
  lonejson_status status;
  unsigned char *copy;
  size_t i;
  char sha_hex[SHA256_DIGEST_LENGTH * 2u + 1u];

  *out = NULL;
  *out_size = 0u;
  if (assets->count == 0u) {
    return 0;
  }
  lonejson_error_init(&error);
  runtime = lonejson_new(NULL, &error);
  if (runtime == NULL) {
    return -1;
  }
  lonejson_owned_buffer_init(&buffer);
  status = lonejson_writer_init_sink(
      runtime, &writer, lonejson_owned_buffer_sink, &buffer, &error);
  if (status != LONEJSON_STATUS_OK) {
    lonejson_free(runtime);
    return -1;
  }
  status = lonejson_writer_begin_object(&writer, &error);
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "format", 6u, &error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, "vectis-pack", 11u, &error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "assets", 6u, &error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(&writer, &error);
  }
  for (i = 0u; i < assets->count; ++i) {
    vectis_sha256_hex(assets->items[i].sha, sha_hex);
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_begin_object(&writer, &error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_key(&writer, "path", 4u, &error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status =
          lonejson_writer_string(&writer, assets->items[i].logical_path,
                                 strlen(assets->items[i].logical_path), &error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_key(&writer, "offset", 6u, &error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_u64(
          &writer, (lonejson_uint64)assets->items[i].offset, &error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_key(&writer, "size", 4u, &error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_u64(
          &writer, (lonejson_uint64)assets->items[i].size, &error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_key(&writer, "sha256", 6u, &error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status =
          lonejson_writer_string(&writer, sha_hex, strlen(sha_hex), &error);
    }
    if (status == LONEJSON_STATUS_OK && assets->items[i].content_type != NULL) {
      status = lonejson_writer_key(&writer, "content_type", 12u, &error);
    }
    if (status == LONEJSON_STATUS_OK && assets->items[i].content_type != NULL) {
      status =
          lonejson_writer_string(&writer, assets->items[i].content_type,
                                 strlen(assets->items[i].content_type), &error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_end_object(&writer, &error);
    }
    if (status != LONEJSON_STATUS_OK) {
      break;
    }
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_array(&writer, &error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_end_object(&writer, &error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_finish(&writer, &error);
  }
  lonejson_writer_cleanup(&writer);
  if (status != LONEJSON_STATUS_OK || buffer.data == NULL) {
    lonejson_owned_buffer_free(&buffer);
    lonejson_free(runtime);
    return -1;
  }
  copy = (unsigned char *)malloc(buffer.len);
  if (copy == NULL) {
    lonejson_owned_buffer_free(&buffer);
    lonejson_free(runtime);
    return -1;
  }
  memcpy(copy, buffer.data, buffer.len);
  *out = copy;
  *out_size = buffer.len;
  lonejson_owned_buffer_free(&buffer);
  lonejson_free(runtime);
  return 0;
}

static int vectis_pack_hash_assets(vectis_pack_asset_list *assets,
                                   unsigned char out[SHA256_DIGEST_LENGTH]) {
  EVP_MD_CTX *ctx;
  unsigned int digest_size;
  size_t i;

  ctx = EVP_MD_CTX_new();
  if (ctx == NULL) {
    return -1;
  }
  if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
    EVP_MD_CTX_free(ctx);
    return -1;
  }
  for (i = 0u; i < assets->count; ++i) {
    if (EVP_DigestUpdate(ctx, assets->items[i].data, assets->items[i].size) !=
        1) {
      EVP_MD_CTX_free(ctx);
      return -1;
    }
  }
  digest_size = 0u;
  if (EVP_DigestFinal_ex(ctx, out, &digest_size) != 1 ||
      digest_size != SHA256_DIGEST_LENGTH) {
    EVP_MD_CTX_free(ctx);
    return -1;
  }
  EVP_MD_CTX_free(ctx);
  return 0;
}

static const char *vectis_cli_auth_mode_name(unsigned mode) {
  if ((mode & VECTIS_AUTH_MODE_BASIC) != 0u) {
    return "basic";
  }
  if ((mode & VECTIS_AUTH_MODE_BEARER) != 0u) {
    return "bearer";
  }
  return "default";
}

static int vectis_cli_credentials_default_path(char *out, size_t out_size) {
  const char *config_dir;
  const char *home;
  int written;

  config_dir = getenv("VECTIS_CONFIG_DIR");
  if (config_dir != NULL && config_dir[0] != '\0') {
    written = snprintf(out, out_size, "%s/credentials.json", config_dir);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
  }
  config_dir = getenv("XDG_CONFIG_HOME");
  if (config_dir != NULL && config_dir[0] != '\0') {
    written = snprintf(out, out_size, "%s/vectis/credentials.json", config_dir);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
  }
  home = getenv("HOME");
  if (home == NULL || home[0] == '\0') {
    return -1;
  }
  written = snprintf(out, out_size, "%s/.config/vectis/credentials.json", home);
  return written > 0 && (size_t)written < out_size ? 0 : -1;
}

static int vectis_cli_auth_status(vectis_status status,
                                  const vectis_error *error) {
  const char *message;

  message = error != NULL && error->message[0] != '\0'
                ? error->message
                : vectis_status_string(status);
  fprintf(stderr, "vectis: %s\n", message != NULL ? message : "auth failed");
  return status == VECTIS_ERR_NOMEM ? 70 : 1;
}

static int vectis_cli_credentials_command(int argc, char **argv, int index) {
  vectis_auth_store_config store;
  vectis_auth_issue_config issue;
  vectis_auth_issued_credential credential;
  vectis_auth_result result;
  vectis_error error;
  vectis_status status;
  char default_path[4096];
  const char *action;
  const char *authorization;
  const char *revoke_client_id;
  unsigned explicit_modes;

  if (vectis_cli_credentials_default_path(default_path, sizeof(default_path)) !=
      0) {
    fputs("vectis: unable to resolve default credentials path\n", stderr);
    return 1;
  }
  vectis_auth_store_config_init(&store);
  store.credentials_path = default_path;
  vectis_auth_issue_config_init(&issue);
  action = NULL;
  authorization = NULL;
  revoke_client_id = NULL;
  explicit_modes = 0u;
  while (index < argc) {
    if (strcmp(argv[index], "--store") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --store requires a path\n", stderr);
        return 64;
      }
      store.credentials_path = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--init") == 0) {
      action = "init";
      index++;
    } else if (strcmp(argv[index], "--issue") == 0) {
      action = "issue";
      index++;
    } else if (strcmp(argv[index], "--verify") == 0 ||
               strcmp(argv[index], "--authorization") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --verify requires an Authorization header value\n",
              stderr);
        return 64;
      }
      action = "verify";
      authorization = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--revoke") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --revoke requires a client id\n", stderr);
        return 64;
      }
      action = "revoke";
      revoke_client_id = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--subject") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --subject requires a value\n", stderr);
        return 64;
      }
      issue.subject = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--purpose") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --purpose requires a value\n", stderr);
        return 64;
      }
      issue.purpose = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--basic") == 0) {
      explicit_modes |= VECTIS_AUTH_MODE_BASIC;
      index++;
    } else if (strcmp(argv[index], "--bearer") == 0) {
      explicit_modes |= VECTIS_AUTH_MODE_BEARER;
      index++;
    } else {
      fprintf(stderr, "vectis: unknown credentials option: %s\n", argv[index]);
      return 64;
    }
  }
  vectis_error_clear(&error);
  if (action == NULL) {
    fputs("vectis: credentials requires --init, --issue, --verify, or "
          "--revoke\n",
          stderr);
    return 64;
  }
  if (strcmp(action, "init") == 0) {
    status = vectis_auth_store_init(&store, &error);
    if (status != VECTIS_OK) {
      return vectis_cli_auth_status(status, &error);
    }
    printf("initialized=%s\n", store.credentials_path);
    return 0;
  }
  if (strcmp(action, "issue") == 0) {
    if (explicit_modes != 0u) {
      issue.auth_modes = explicit_modes;
    }
    vectis_auth_issued_credential_init(&credential);
    status = vectis_auth_issue_credential(&store, &issue, &credential, &error);
    if (status != VECTIS_OK) {
      return vectis_cli_auth_status(status, &error);
    }
    if (credential.client_id != NULL) {
      printf("client_id=%s\n", credential.client_id);
    }
    if (credential.client_secret != NULL) {
      printf("client_secret=%s\n", credential.client_secret);
    }
    if (credential.api_key != NULL) {
      printf("api_key=%s\n", credential.api_key);
    }
    if (credential.claim_json != NULL) {
      printf("claim_json=%s\n", credential.claim_json);
    }
    vectis_auth_issued_credential_cleanup(&credential);
    return 0;
  }
  if (strcmp(action, "verify") == 0) {
    vectis_auth_result_init(&result);
    status = vectis_auth_verify_authorization(
        &store, authorization,
        explicit_modes != 0u ? explicit_modes : VECTIS_AUTH_MODE_DEFAULT,
        &result, &error);
    if (status != VECTIS_OK) {
      return vectis_cli_auth_status(status, &error);
    }
    printf("authenticated=%s\n", result.authenticated ? "true" : "false");
    printf("auth_mode=%s\n", vectis_cli_auth_mode_name(result.auth_mode));
    if (result.client_id != NULL) {
      printf("client_id=%s\n", result.client_id);
    }
    if (result.claim_json != NULL) {
      printf("claim_json=%s\n", result.claim_json);
    }
    vectis_auth_result_cleanup(&result);
    return 0;
  }
  status = vectis_auth_revoke_client(&store, revoke_client_id, &error);
  if (status != VECTIS_OK) {
    return vectis_cli_auth_status(status, &error);
  }
  printf("revoked=%s\n", revoke_client_id);
  return 0;
}

static int vectis_cli_users_time_arg(const char *value, uint64_t *out) {
  char *end;
  unsigned long long parsed;

  if (value == NULL || out == NULL) {
    return -1;
  }
  errno = 0;
  end = NULL;
  parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || end == NULL || *end != '\0') {
    return -1;
  }
  *out = (uint64_t)parsed;
  return 0;
}

static int vectis_cli_i64_arg(const char *value, int64_t *out) {
  char *end;
  long long parsed;

  if (value == NULL || out == NULL) {
    return -1;
  }
  errno = 0;
  end = NULL;
  parsed = strtoll(value, &end, 10);
  if (errno != 0 || end == value || end == NULL || *end != '\0') {
    return -1;
  }
  *out = (int64_t)parsed;
  return 0;
}

static int
vectis_cli_print_credential(const vectis_auth_issued_credential *credential) {
  if (credential->client_id != NULL) {
    printf("client_id=%s\n", credential->client_id);
  }
  if (credential->client_secret != NULL) {
    printf("client_secret=%s\n", credential->client_secret);
  }
  if (credential->api_key != NULL) {
    printf("api_key=%s\n", credential->api_key);
  }
  if (credential->claim_json != NULL) {
    printf("claim_json=%s\n", credential->claim_json);
  }
  return 0;
}

static int vectis_cli_token_flow_copy_arg(const char *value, char **out);

static void
vectis_cli_print_token_flow(const vectis_auth_oauth2_token_flow *flow) {
  if (flow->access_token != NULL) {
    printf("access_token=%s\n", flow->access_token);
  }
  if (flow->token_type != NULL) {
    printf("token_type=%s\n", flow->token_type);
  }
  if (flow->refresh_token != NULL) {
    printf("refresh_token=%s\n", flow->refresh_token);
  }
  if (flow->scope != NULL) {
    printf("scope=%s\n", flow->scope);
  }
  if (flow->id_token != NULL) {
    printf("id_token=%s\n", flow->id_token);
  }
  if (flow->has_expires_at) {
    printf("expires_at=%lld\n", (long long)flow->expires_at);
  }
  printf("has_expires_at=%s\n", flow->has_expires_at ? "true" : "false");
}

static int vectis_cli_token_flow_from_response(
    const vectis_auth_oauth2_token_response *response, int64_t now,
    vectis_auth_oauth2_token_flow *flow) {
  if (response == NULL || flow == NULL) {
    return -1;
  }
  vectis_auth_oauth2_token_flow_init(flow);
  if (vectis_cli_token_flow_copy_arg(response->access_token,
                                     &flow->access_token) != 0 ||
      vectis_cli_token_flow_copy_arg(response->token_type, &flow->token_type) !=
          0 ||
      vectis_cli_token_flow_copy_arg(response->refresh_token,
                                     &flow->refresh_token) != 0 ||
      vectis_cli_token_flow_copy_arg(response->scope, &flow->scope) != 0 ||
      vectis_cli_token_flow_copy_arg(response->id_token, &flow->id_token) !=
          0) {
    vectis_auth_oauth2_token_flow_cleanup(flow);
    return -1;
  }
  if (response->has_expires_in) {
    flow->expires_at = now + response->expires_in;
    flow->has_expires_at = 1;
  }
  return 0;
}

static const char *
vectis_cli_oauth2_flow_state_name(vectis_auth_oauth2_token_flow_state state) {
  switch (state) {
  case VECTIS_AUTH_OAUTH2_TOKEN_FLOW_REFRESHED:
    return "refreshed";
  case VECTIS_AUTH_OAUTH2_TOKEN_FLOW_NEEDS_INTERACTION:
    return "needs_interaction";
  case VECTIS_AUTH_OAUTH2_TOKEN_FLOW_FAILED:
    return "failed";
  case VECTIS_AUTH_OAUTH2_TOKEN_FLOW_READY:
  default:
    return "ready";
  }
}

static int vectis_cli_token_flow_copy_arg(const char *value, char **out) {
  if (value == NULL) {
    *out = NULL;
    return 0;
  }
  *out = vectis_cli_strdup(value);
  return *out == NULL ? -1 : 0;
}

static int vectis_cli_users_command(int argc, char **argv, int index) {
  vectis_auth_store_config store;
  vectis_auth_user_config user;
  vectis_auth_user_enrollment enrollment;
  vectis_auth_login_config login;
  vectis_auth_result result;
  vectis_auth_issued_credential credential;
  vectis_error error;
  vectis_status status;
  char default_path[4096];
  const char *action;

  if (vectis_cli_credentials_default_path(default_path, sizeof(default_path)) !=
      0) {
    fputs("vectis: unable to resolve default credentials path\n", stderr);
    return 1;
  }
  vectis_auth_store_config_init(&store);
  store.credentials_path = default_path;
  vectis_auth_user_config_init(&user);
  vectis_auth_login_config_init(&login);
  action = NULL;
  while (index < argc) {
    if (strcmp(argv[index], "--store") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --store requires a path\n", stderr);
        return 64;
      }
      store.credentials_path = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--add") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --add requires a username\n", stderr);
        return 64;
      }
      action = "add";
      user.username = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--login") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --login requires a username\n", stderr);
        return 64;
      }
      action = "login";
      login.username = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--webdav-key") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --webdav-key requires a username\n", stderr);
        return 64;
      }
      action = "webdav-key";
      login.username = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--password") == 0 ||
               strcmp(argv[index], "-p") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --password requires a value\n", stderr);
        return 64;
      }
      user.password = argv[index + 1];
      login.password = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--totp") == 0) {
      user.enable_totp = 1;
      index++;
    } else if (strcmp(argv[index], "--totp-secret") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --totp-secret requires a value\n", stderr);
        return 64;
      }
      user.enable_totp = 1;
      user.totp_secret = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--totp-code") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --totp-code requires a value\n", stderr);
        return 64;
      }
      login.totp_code = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--issuer") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --issuer requires a value\n", stderr);
        return 64;
      }
      user.totp_issuer = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--label") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --label requires a value\n", stderr);
        return 64;
      }
      user.totp_label = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--time") == 0) {
      if (index + 1 >= argc || vectis_cli_users_time_arg(
                                   argv[index + 1], &login.unix_seconds) != 0) {
        fputs("vectis: --time requires a non-negative integer\n", stderr);
        return 64;
      }
      index += 2;
    } else if (strcmp(argv[index], "--window") == 0) {
      uint64_t parsed;

      if (index + 1 >= argc ||
          vectis_cli_users_time_arg(argv[index + 1], &parsed) != 0 ||
          parsed > 10u) {
        fputs("vectis: --window requires an integer from 0 to 10\n", stderr);
        return 64;
      }
      login.totp_window = (unsigned int)parsed;
      index += 2;
    } else {
      fprintf(stderr, "vectis: unknown users option: %s\n", argv[index]);
      return 64;
    }
  }
  vectis_error_clear(&error);
  if (action == NULL) {
    fputs("vectis: users requires --add, --login, or --webdav-key\n", stderr);
    return 64;
  }
  if (strcmp(action, "add") == 0) {
    vectis_auth_user_enrollment_init(&enrollment);
    status = vectis_auth_user_add_or_update(&store, &user, &enrollment, &error);
    if (status != VECTIS_OK) {
      return vectis_cli_auth_status(status, &error);
    }
    printf("username=%s\n", enrollment.username);
    if (enrollment.generated_password != NULL) {
      printf("password=%s\n", enrollment.generated_password);
    }
    if (enrollment.totp_secret != NULL) {
      printf("totp_secret=%s\n", enrollment.totp_secret);
    }
    if (enrollment.totp_uri != NULL) {
      printf("totp_uri=%s\n", enrollment.totp_uri);
    }
    if (enrollment.totp_qr_ansi != NULL) {
      fputs("totp_qr:\n", stdout);
      fputs(enrollment.totp_qr_ansi, stdout);
    }
    vectis_auth_user_enrollment_cleanup(&enrollment);
    return 0;
  }
  if (strcmp(action, "login") == 0) {
    vectis_auth_result_init(&result);
    status = vectis_auth_user_login(&store, &login, &result, &error);
    if (status != VECTIS_OK) {
      return vectis_cli_auth_status(status, &error);
    }
    printf("authenticated=%s\n", result.authenticated ? "true" : "false");
    if (result.claim_json != NULL) {
      printf("claim_json=%s\n", result.claim_json);
    }
    vectis_auth_result_cleanup(&result);
    return 0;
  }
  vectis_auth_issued_credential_init(&credential);
  status = vectis_auth_issue_webdav_key_for_login(&store, &login, &credential,
                                                  &error);
  if (status != VECTIS_OK) {
    return vectis_cli_auth_status(status, &error);
  }
  (void)vectis_cli_print_credential(&credential);
  vectis_auth_issued_credential_cleanup(&credential);
  return 0;
}

static int vectis_cli_oauth2_command(int argc, char **argv, int index) {
  vectis_auth_store_config store;
  vectis_auth_oidc_authorization_config authorization_config;
  vectis_auth_oidc_authorization authorization;
  vectis_auth_oidc_token_exchange_config exchange_config;
  vectis_auth_oidc_token_exchange exchange;
  vectis_auth_oauth2_client_credentials_config client_credentials_config;
  vectis_auth_oauth2_token_response token_response;
  vectis_auth_oauth2_token_flow_store_config flow_config;
  vectis_auth_oauth2_stored_token_flow loaded_flow;
  vectis_auth_oauth2_stored_token_flow_policy stored_policy;
  vectis_auth_oauth2_token_flow_result flow_result;
  vectis_auth_oauth2_webdav_key_config webdav_config;
  vectis_auth_issued_credential credential;
  vectis_error error;
  vectis_status status;
  char default_path[4096];
  const char *action;
  const char *flow_id;
  const char *subject;
  const char *webdav_client_id;
  const char *access_token;
  const char *token_endpoint;
  const char *client_secret;
  const char *callback_query;
  const char *expected_state;
  const char *token_type;
  const char *refresh_token;
  const char *scope;
  const char *id_token;
  uint64_t parsed_size;
  int64_t now;
  int now_set;
  int64_t refresh_skew_seconds;
  unsigned max_retries;
  int disable_refresh;
  int disable_retry;
  int revoke_webdav_keys_on_failure;
  int expires_at_set;
  int64_t expires_at;
  size_t max_record_bytes;
  size_t max_response_bytes;
  size_t max_body_bytes;
  size_t max_query_bytes;

  if (vectis_cli_credentials_default_path(default_path, sizeof(default_path)) !=
      0) {
    fputs("vectis: unable to resolve default credentials path\n", stderr);
    return 1;
  }
  vectis_auth_store_config_init(&store);
  store.credentials_path = default_path;
  vectis_auth_oidc_authorization_config_init(&authorization_config);
  action = NULL;
  flow_id = NULL;
  subject = NULL;
  webdav_client_id = NULL;
  access_token = NULL;
  token_endpoint = NULL;
  client_secret = NULL;
  callback_query = NULL;
  expected_state = NULL;
  token_type = NULL;
  refresh_token = NULL;
  scope = NULL;
  id_token = NULL;
  now = 0;
  now_set = 0;
  refresh_skew_seconds = 0;
  max_retries = 0u;
  disable_refresh = 0;
  disable_retry = 0;
  revoke_webdav_keys_on_failure = 1;
  expires_at_set = 0;
  expires_at = 0;
  max_record_bytes = 0u;
  max_response_bytes = 0u;
  max_body_bytes = 0u;
  max_query_bytes = 0u;
  while (index < argc) {
    if (strcmp(argv[index], "--store") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --store requires a path\n", stderr);
        return 64;
      }
      store.credentials_path = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--authorize") == 0) {
      action = "authorize";
      index++;
    } else if (strcmp(argv[index], "--upsert-flow") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --upsert-flow requires a flow id\n", stderr);
        return 64;
      }
      action = "upsert-flow";
      flow_id = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--exchange-callback") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --exchange-callback requires a flow id\n", stderr);
        return 64;
      }
      action = "exchange-callback";
      flow_id = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--client-credentials") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --client-credentials requires a flow id\n", stderr);
        return 64;
      }
      action = "client-credentials";
      flow_id = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--load-flow") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --load-flow requires a flow id\n", stderr);
        return 64;
      }
      action = "load-flow";
      flow_id = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--ensure-flow") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --ensure-flow requires a flow id\n", stderr);
        return 64;
      }
      action = "ensure-flow";
      flow_id = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--webdav-key") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --webdav-key requires a flow id\n", stderr);
        return 64;
      }
      action = "webdav-key";
      flow_id = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--flow-id") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --flow-id requires a value\n", stderr);
        return 64;
      }
      flow_id = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--subject") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --subject requires a value\n", stderr);
        return 64;
      }
      subject = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--webdav-client-id") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --webdav-client-id requires a value\n", stderr);
        return 64;
      }
      webdav_client_id = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--authorization-endpoint") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --authorization-endpoint requires a URL\n", stderr);
        return 64;
      }
      authorization_config.authorization_endpoint = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--client-id") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --client-id requires a value\n", stderr);
        return 64;
      }
      authorization_config.client_id = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--client-secret") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --client-secret requires a value\n", stderr);
        return 64;
      }
      client_secret = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--token-endpoint") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --token-endpoint requires a URL\n", stderr);
        return 64;
      }
      token_endpoint = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--callback-query") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --callback-query requires a value\n", stderr);
        return 64;
      }
      callback_query = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--expected-state") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --expected-state requires a value\n", stderr);
        return 64;
      }
      expected_state = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--redirect-uri") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --redirect-uri requires a value\n", stderr);
        return 64;
      }
      authorization_config.redirect_uri = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--scope") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --scope requires a value\n", stderr);
        return 64;
      }
      scope = argv[index + 1];
      authorization_config.scope = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--state") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --state requires a value\n", stderr);
        return 64;
      }
      authorization_config.state = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--nonce") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --nonce requires a value\n", stderr);
        return 64;
      }
      authorization_config.nonce = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--code-verifier") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --code-verifier requires a value\n", stderr);
        return 64;
      }
      authorization_config.code_verifier = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--code-challenge") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --code-challenge requires a value\n", stderr);
        return 64;
      }
      authorization_config.code_challenge = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--audience") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --audience requires a value\n", stderr);
        return 64;
      }
      authorization_config.audience = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--resource") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --resource requires a value\n", stderr);
        return 64;
      }
      authorization_config.resource = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--verifier-bytes") == 0) {
      if (index + 1 >= argc ||
          vectis_cli_users_time_arg(argv[index + 1], &parsed_size) != 0) {
        fputs("vectis: --verifier-bytes requires a non-negative integer\n",
              stderr);
        return 64;
      }
      authorization_config.verifier_bytes = (size_t)parsed_size;
      index += 2;
    } else if (strcmp(argv[index], "--max-url-bytes") == 0) {
      if (index + 1 >= argc ||
          vectis_cli_users_time_arg(argv[index + 1], &parsed_size) != 0) {
        fputs("vectis: --max-url-bytes requires a non-negative integer\n",
              stderr);
        return 64;
      }
      authorization_config.max_url_bytes = (size_t)parsed_size;
      index += 2;
    } else if (strcmp(argv[index], "--access-token") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --access-token requires a value\n", stderr);
        return 64;
      }
      access_token = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--token-type") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --token-type requires a value\n", stderr);
        return 64;
      }
      token_type = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--refresh-token") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --refresh-token requires a value\n", stderr);
        return 64;
      }
      refresh_token = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--id-token") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --id-token requires a value\n", stderr);
        return 64;
      }
      id_token = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--expires-at") == 0) {
      if (index + 1 >= argc ||
          vectis_cli_i64_arg(argv[index + 1], &expires_at) != 0) {
        fputs("vectis: --expires-at requires an integer\n", stderr);
        return 64;
      }
      expires_at_set = 1;
      index += 2;
    } else if (strcmp(argv[index], "--now") == 0) {
      if (index + 1 >= argc || vectis_cli_i64_arg(argv[index + 1], &now) != 0) {
        fputs("vectis: --now requires an integer\n", stderr);
        return 64;
      }
      now_set = 1;
      index += 2;
    } else if (strcmp(argv[index], "--refresh-skew-seconds") == 0) {
      if (index + 1 >= argc ||
          vectis_cli_i64_arg(argv[index + 1], &refresh_skew_seconds) != 0) {
        fputs("vectis: --refresh-skew-seconds requires an integer\n", stderr);
        return 64;
      }
      index += 2;
    } else if (strcmp(argv[index], "--max-response-bytes") == 0) {
      if (index + 1 >= argc ||
          vectis_cli_users_time_arg(argv[index + 1], &parsed_size) != 0) {
        fputs("vectis: --max-response-bytes requires a non-negative integer\n",
              stderr);
        return 64;
      }
      max_response_bytes = (size_t)parsed_size;
      index += 2;
    } else if (strcmp(argv[index], "--max-body-bytes") == 0) {
      if (index + 1 >= argc ||
          vectis_cli_users_time_arg(argv[index + 1], &parsed_size) != 0) {
        fputs("vectis: --max-body-bytes requires a non-negative integer\n",
              stderr);
        return 64;
      }
      max_body_bytes = (size_t)parsed_size;
      index += 2;
    } else if (strcmp(argv[index], "--max-query-bytes") == 0) {
      if (index + 1 >= argc ||
          vectis_cli_users_time_arg(argv[index + 1], &parsed_size) != 0) {
        fputs("vectis: --max-query-bytes requires a non-negative integer\n",
              stderr);
        return 64;
      }
      max_query_bytes = (size_t)parsed_size;
      index += 2;
    } else if (strcmp(argv[index], "--max-retries") == 0) {
      if (index + 1 >= argc ||
          vectis_cli_users_time_arg(argv[index + 1], &parsed_size) != 0) {
        fputs("vectis: --max-retries requires a non-negative integer\n",
              stderr);
        return 64;
      }
      max_retries = (unsigned)parsed_size;
      index += 2;
    } else if (strcmp(argv[index], "--disable-refresh") == 0) {
      disable_refresh = 1;
      index++;
    } else if (strcmp(argv[index], "--disable-retry") == 0) {
      disable_retry = 1;
      index++;
    } else if (strcmp(argv[index], "--keep-webdav-keys-on-failure") == 0) {
      revoke_webdav_keys_on_failure = 0;
      index++;
    } else if (strcmp(argv[index], "--max-record-bytes") == 0) {
      if (index + 1 >= argc ||
          vectis_cli_users_time_arg(argv[index + 1], &parsed_size) != 0) {
        fputs("vectis: --max-record-bytes requires a non-negative integer\n",
              stderr);
        return 64;
      }
      max_record_bytes = (size_t)parsed_size;
      index += 2;
    } else {
      fprintf(stderr, "vectis: unknown oauth2 option: %s\n", argv[index]);
      return 64;
    }
  }
  vectis_error_clear(&error);
  if (action == NULL) {
    fputs("vectis: oauth2 requires --authorize, --exchange-callback, "
          "--client-credentials, --upsert-flow, --load-flow, --ensure-flow, "
          "or --webdav-key\n",
          stderr);
    return 64;
  }
  if (strcmp(action, "authorize") == 0) {
    vectis_auth_oidc_authorization_init(&authorization);
    status = vectis_auth_oidc_authorization_start(&authorization_config,
                                                  &authorization, &error);
    if (status != VECTIS_OK) {
      vectis_auth_oidc_authorization_cleanup(&authorization);
      return vectis_cli_auth_status(status, &error);
    }
    if (authorization.authorization_url != NULL) {
      printf("authorization_url=%s\n", authorization.authorization_url);
    }
    if (authorization.code_verifier != NULL) {
      printf("code_verifier=%s\n", authorization.code_verifier);
    }
    if (authorization.code_challenge != NULL) {
      printf("code_challenge=%s\n", authorization.code_challenge);
    }
    if (authorization.state != NULL) {
      printf("state=%s\n", authorization.state);
    }
    if (authorization.nonce != NULL) {
      printf("nonce=%s\n", authorization.nonce);
    }
    vectis_auth_oidc_authorization_cleanup(&authorization);
    return 0;
  }
  if (strcmp(action, "exchange-callback") == 0) {
    vectis_auth_oidc_token_exchange_config_init(&exchange_config);
    exchange_config.token_endpoint = token_endpoint;
    exchange_config.client_id = authorization_config.client_id;
    exchange_config.client_secret = client_secret;
    exchange_config.redirect_uri = authorization_config.redirect_uri;
    exchange_config.code_verifier = authorization_config.code_verifier;
    exchange_config.callback_query = callback_query;
    exchange_config.expected_state = expected_state;
    exchange_config.now = now_set ? now : 0;
    exchange_config.max_query_bytes = max_query_bytes;
    exchange_config.max_response_bytes = max_response_bytes;
    exchange_config.max_body_bytes = max_body_bytes;
    vectis_auth_oidc_token_exchange_init(&exchange);
    status =
        vectis_auth_oidc_exchange_callback(&exchange_config, &exchange, &error);
    if (status != VECTIS_OK) {
      vectis_auth_oidc_token_exchange_cleanup(&exchange);
      return vectis_cli_auth_status(status, &error);
    }
    vectis_auth_oauth2_token_flow_store_config_init(&flow_config);
    flow_config.store = store;
    flow_config.flow_id = flow_id;
    flow_config.subject = subject;
    flow_config.webdav_client_id = webdav_client_id;
    flow_config.flow = exchange.flow;
    status = vectis_auth_oauth2_token_flow_upsert(&flow_config, &error);
    if (status != VECTIS_OK) {
      vectis_auth_oidc_token_exchange_cleanup(&exchange);
      return vectis_cli_auth_status(status, &error);
    }
    printf("stored_flow=%s\n", flow_id);
    if (exchange.code != NULL) {
      printf("code=%s\n", exchange.code);
    }
    if (exchange.state != NULL) {
      printf("state=%s\n", exchange.state);
    }
    vectis_cli_print_token_flow(&exchange.flow);
    vectis_auth_oidc_token_exchange_cleanup(&exchange);
    return 0;
  }
  if (strcmp(action, "client-credentials") == 0) {
    vectis_auth_oauth2_client_credentials_config_init(
        &client_credentials_config);
    client_credentials_config.token_endpoint = token_endpoint;
    client_credentials_config.client_id = authorization_config.client_id;
    client_credentials_config.client_secret = client_secret;
    client_credentials_config.scope = scope;
    client_credentials_config.audience = authorization_config.audience;
    client_credentials_config.resource = authorization_config.resource;
    client_credentials_config.max_response_bytes = max_response_bytes;
    client_credentials_config.max_body_bytes = max_body_bytes;
    vectis_auth_oauth2_token_response_init(&token_response);
    status = vectis_auth_oauth2_client_credentials_request(
        &client_credentials_config, &token_response, &error);
    if (status != VECTIS_OK) {
      vectis_auth_oauth2_token_response_cleanup(&token_response);
      return vectis_cli_auth_status(status, &error);
    }
    vectis_auth_oauth2_token_flow_store_config_init(&flow_config);
    flow_config.store = store;
    flow_config.flow_id = flow_id;
    flow_config.subject = subject;
    flow_config.webdav_client_id = webdav_client_id;
    if (vectis_cli_token_flow_from_response(&token_response,
                                            now_set ? now : (int64_t)time(NULL),
                                            &flow_config.flow) != 0) {
      vectis_auth_oauth2_token_response_cleanup(&token_response);
      return vectis_cli_auth_status(VECTIS_ERR_NOMEM, &error);
    }
    status = vectis_auth_oauth2_token_flow_upsert(&flow_config, &error);
    if (status != VECTIS_OK) {
      vectis_auth_oauth2_token_flow_cleanup(&flow_config.flow);
      vectis_auth_oauth2_token_response_cleanup(&token_response);
      return vectis_cli_auth_status(status, &error);
    }
    printf("stored_flow=%s\n", flow_id);
    vectis_cli_print_token_flow(&flow_config.flow);
    vectis_auth_oauth2_token_flow_cleanup(&flow_config.flow);
    vectis_auth_oauth2_token_response_cleanup(&token_response);
    return 0;
  }
  if (strcmp(action, "upsert-flow") == 0) {
    vectis_auth_oauth2_token_flow_store_config_init(&flow_config);
    flow_config.store = store;
    flow_config.flow_id = flow_id;
    flow_config.subject = subject;
    flow_config.webdav_client_id = webdav_client_id;
    if (vectis_cli_token_flow_copy_arg(access_token,
                                       &flow_config.flow.access_token) != 0 ||
        vectis_cli_token_flow_copy_arg(token_type,
                                       &flow_config.flow.token_type) != 0 ||
        vectis_cli_token_flow_copy_arg(refresh_token,
                                       &flow_config.flow.refresh_token) != 0 ||
        vectis_cli_token_flow_copy_arg(scope, &flow_config.flow.scope) != 0 ||
        vectis_cli_token_flow_copy_arg(id_token, &flow_config.flow.id_token) !=
            0) {
      vectis_auth_oauth2_token_flow_cleanup(&flow_config.flow);
      return vectis_cli_auth_status(VECTIS_ERR_NOMEM, &error);
    }
    flow_config.flow.expires_at = expires_at;
    flow_config.flow.has_expires_at = expires_at_set;
    status = vectis_auth_oauth2_token_flow_upsert(&flow_config, &error);
    vectis_auth_oauth2_token_flow_cleanup(&flow_config.flow);
    if (status != VECTIS_OK) {
      return vectis_cli_auth_status(status, &error);
    }
    printf("stored_flow=%s\n", flow_id);
    return 0;
  }
  if (strcmp(action, "load-flow") == 0) {
    vectis_auth_oauth2_stored_token_flow_init(&loaded_flow);
    status = vectis_auth_oauth2_token_flow_load(&store, flow_id, &loaded_flow,
                                                &error);
    if (status != VECTIS_OK) {
      vectis_auth_oauth2_stored_token_flow_cleanup(&loaded_flow);
      return vectis_cli_auth_status(status, &error);
    }
    printf("found=%s\n", loaded_flow.found ? "true" : "false");
    if (loaded_flow.flow_id != NULL) {
      printf("flow_id=%s\n", loaded_flow.flow_id);
    }
    if (loaded_flow.subject != NULL) {
      printf("subject=%s\n", loaded_flow.subject);
    }
    if (loaded_flow.webdav_client_id != NULL) {
      printf("webdav_client_id=%s\n", loaded_flow.webdav_client_id);
    }
    vectis_cli_print_token_flow(&loaded_flow.flow);
    vectis_auth_oauth2_stored_token_flow_cleanup(&loaded_flow);
    return 0;
  }
  if (strcmp(action, "ensure-flow") == 0) {
    vectis_auth_oauth2_stored_token_flow_policy_init(&stored_policy);
    stored_policy.store = store;
    stored_policy.flow_id = flow_id;
    stored_policy.flow_policy.token_endpoint = token_endpoint;
    stored_policy.flow_policy.client_id = authorization_config.client_id;
    stored_policy.flow_policy.client_secret = client_secret;
    stored_policy.flow_policy.scope = scope;
    stored_policy.flow_policy.now = now_set ? now : 0;
    stored_policy.flow_policy.refresh_skew_seconds = refresh_skew_seconds;
    stored_policy.flow_policy.max_response_bytes = max_response_bytes;
    stored_policy.flow_policy.max_retries = max_retries;
    stored_policy.flow_policy.disable_refresh = disable_refresh;
    stored_policy.flow_policy.disable_retry = disable_retry;
    stored_policy.revoke_webdav_keys_on_failure = revoke_webdav_keys_on_failure;
    vectis_auth_oauth2_stored_token_flow_init(&loaded_flow);
    vectis_auth_oauth2_token_flow_result_init(&flow_result);
    status = vectis_auth_oauth2_stored_token_flow_ensure(
        &stored_policy, &loaded_flow, &flow_result, &error);
    if (status != VECTIS_OK) {
      vectis_auth_oauth2_stored_token_flow_cleanup(&loaded_flow);
      return vectis_cli_auth_status(status, &error);
    }
    printf("flow_state=%s\n",
           vectis_cli_oauth2_flow_state_name(flow_result.state));
    printf("attempts=%u\n", flow_result.attempts);
    printf("refreshed=%s\n", flow_result.refreshed ? "true" : "false");
    printf("found=%s\n", loaded_flow.found ? "true" : "false");
    if (loaded_flow.flow_id != NULL) {
      printf("flow_id=%s\n", loaded_flow.flow_id);
    }
    if (loaded_flow.subject != NULL) {
      printf("subject=%s\n", loaded_flow.subject);
    }
    if (loaded_flow.webdav_client_id != NULL) {
      printf("webdav_client_id=%s\n", loaded_flow.webdav_client_id);
    }
    vectis_cli_print_token_flow(&loaded_flow.flow);
    vectis_auth_oauth2_stored_token_flow_cleanup(&loaded_flow);
    return 0;
  }
  vectis_auth_oauth2_webdav_key_config_init(&webdav_config);
  webdav_config.store = store;
  webdav_config.flow_id = flow_id;
  webdav_config.subject = subject;
  webdav_config.max_record_bytes = max_record_bytes;
  vectis_auth_issued_credential_init(&credential);
  status = vectis_auth_issue_webdav_key_for_oauth2_flow(&webdav_config,
                                                        &credential, &error);
  if (status != VECTIS_OK) {
    vectis_auth_issued_credential_cleanup(&credential);
    return vectis_cli_auth_status(status, &error);
  }
  (void)vectis_cli_print_credential(&credential);
  vectis_auth_issued_credential_cleanup(&credential);
  return 0;
}

static int vectis_pack_command(int argc, char **argv, int index);

static int vectis_action_command(int argc, char **argv, int index) {
  const char *action;

  if (index >= argc) {
    fputs("vectis: -a/--action requires an action\n", stderr);
    return 64;
  }
  action = argv[index];
  index++;
  if (strcmp(action, "pack") == 0) {
    return vectis_pack_command(argc, argv, index);
  }
  if (strcmp(action, "credentials") == 0) {
    return vectis_cli_credentials_command(argc, argv, index);
  }
  if (strcmp(action, "users") == 0) {
    return vectis_cli_users_command(argc, argv, index);
  }
  if (strcmp(action, "oauth2") == 0) {
    return vectis_cli_oauth2_command(argc, argv, index);
  }
  fprintf(stderr, "vectis: unknown action: %s\n", action);
  return 64;
}

static void vectis_pack_make_footer(
    unsigned char *footer, unsigned long long script_offset,
    unsigned long long script_size, const unsigned char *script_sha,
    unsigned long long bundle_offset, unsigned long long bundle_size,
    const unsigned char *bundle_sha, unsigned long long asset_offset,
    unsigned long long asset_size, const unsigned char *asset_sha,
    unsigned long long manifest_offset, unsigned long long manifest_size,
    const unsigned char *manifest_sha) {
  memset(footer, 0, VECTIS_PACK_FOOTER_SIZE);
  memcpy(footer, VECTIS_PACK_MAGIC, VECTIS_PACK_MAGIC_SIZE);
  vectis_pack_write_u64(footer + 16u, script_offset);
  vectis_pack_write_u64(footer + 24u, script_size);
  vectis_pack_write_u64(footer + 32u, bundle_offset);
  vectis_pack_write_u64(footer + 40u, bundle_size);
  vectis_pack_write_u64(footer + 48u, asset_offset);
  vectis_pack_write_u64(footer + 56u, asset_size);
  vectis_pack_write_u64(footer + 64u, manifest_offset);
  vectis_pack_write_u64(footer + 72u, manifest_size);
  memcpy(footer + 80u, script_sha, SHA256_DIGEST_LENGTH);
  if (bundle_sha != NULL) {
    memcpy(footer + 112u, bundle_sha, SHA256_DIGEST_LENGTH);
  }
  if (asset_sha != NULL) {
    memcpy(footer + 144u, asset_sha, SHA256_DIGEST_LENGTH);
  }
  if (manifest_sha != NULL) {
    memcpy(footer + 176u, manifest_sha, SHA256_DIGEST_LENGTH);
  }
}

static int vectis_pack_footer_valid(const unsigned char *footer) {
  return memcmp(footer, VECTIS_PACK_MAGIC, VECTIS_PACK_MAGIC_SIZE) == 0;
}

static int vectis_self_path(const char *argv0, char *path, size_t path_size) {
#ifdef __linux__
  ssize_t nread;

  nread = readlink("/proc/self/exe", path, path_size - 1u);
  if (nread > 0 && (size_t)nread < path_size) {
    path[nread] = '\0';
    return 0;
  }
#endif
  if (argv0 == NULL || strlen(argv0) + 1u > path_size) {
    return -1;
  }
  memcpy(path, argv0, strlen(argv0) + 1u);
  return 0;
}

static void
vectis_pack_command_cleanup(vectis_pack_asset_list *assets,
                            vectis_pack_content_type_map *content_types) {
  vectis_pack_asset_list_cleanup(assets);
  vectis_pack_content_type_map_cleanup(content_types);
}

static int vectis_pack_command(int argc, char **argv, int index) {
  const char *script_path;
  const char *output_path;
  const char *bundle_path;
  const char *asset_arg;
  const char *separator;
  char *asset_source;
  char *asset_logical;
  unsigned char *self;
  unsigned char *script;
  unsigned char *bundle;
  unsigned char *manifest;
  size_t self_size;
  size_t script_size;
  size_t bundle_size;
  size_t manifest_size;
  size_t asset_size;
  size_t offset;
  unsigned char script_sha[SHA256_DIGEST_LENGTH];
  unsigned char bundle_sha[SHA256_DIGEST_LENGTH];
  unsigned char asset_sha[SHA256_DIGEST_LENGTH];
  unsigned char manifest_sha[SHA256_DIGEST_LENGTH];
  unsigned char footer[VECTIS_PACK_FOOTER_SIZE];
  vectis_pack_asset_list assets;
  vectis_pack_content_type_map content_types;
  char self_path[4096];
  FILE *out;
  int i;

  script_path = NULL;
  output_path = NULL;
  bundle_path = NULL;
  memset(&assets, 0, sizeof(assets));
  memset(&content_types, 0, sizeof(content_types));
  for (i = index; i < argc; ++i) {
    if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
      script_path = argv[++i];
    } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      output_path = argv[++i];
    } else if (strcmp(argv[i], "--lockd-bundle") == 0 && i + 1 < argc) {
      bundle_path = argv[++i];
    } else if (strcmp(argv[i], "--content-type-map") == 0 && i + 1 < argc) {
      if (vectis_pack_read_content_type_map(&content_types, argv[++i]) != 0) {
        vectis_pack_command_cleanup(&assets, &content_types);
        return 1;
      }
    } else if ((strcmp(argv[i], "--asset") == 0 ||
                strcmp(argv[i], "--asset-dir") == 0 ||
                strcmp(argv[i], "--asset-manifest") == 0) &&
               i + 1 < argc) {
      i++;
    }
  }
  for (i = index; i < argc; ++i) {
    if ((strcmp(argv[i], "--script") == 0 || strcmp(argv[i], "--output") == 0 ||
         strcmp(argv[i], "--lockd-bundle") == 0 ||
         strcmp(argv[i], "--content-type-map") == 0) &&
        i + 1 < argc) {
      i++;
    } else if (strcmp(argv[i], "--asset") == 0 && i + 1 < argc) {
      asset_arg = argv[++i];
      separator = strchr(asset_arg, '=');
      if (separator == NULL || separator == asset_arg || separator[1] == '\0') {
        fputs("vectis: --asset requires source=logical-path\n", stderr);
        vectis_pack_command_cleanup(&assets, &content_types);
        return 64;
      }
      asset_source =
          vectis_cli_memdup(asset_arg, (size_t)(separator - asset_arg) + 1u);
      asset_logical = vectis_cli_strdup(separator + 1);
      if (asset_source == NULL || asset_logical == NULL) {
        free(asset_source);
        free(asset_logical);
        vectis_pack_command_cleanup(&assets, &content_types);
        return 70;
      }
      asset_source[separator - asset_arg] = '\0';
      if (vectis_pack_asset_add(&assets, asset_source, asset_logical, NULL,
                                &content_types) != 0) {
        free(asset_source);
        free(asset_logical);
        vectis_pack_command_cleanup(&assets, &content_types);
        return 1;
      }
      free(asset_source);
      free(asset_logical);
    } else if (strcmp(argv[i], "--asset-dir") == 0 && i + 1 < argc) {
      asset_arg = argv[++i];
      separator = strchr(asset_arg, ':');
      if (separator == NULL || separator == asset_arg || separator[1] == '\0') {
        fputs("vectis: --asset-dir requires logical-root:source-dir\n", stderr);
        vectis_pack_command_cleanup(&assets, &content_types);
        return 64;
      }
      asset_logical =
          vectis_cli_memdup(asset_arg, (size_t)(separator - asset_arg) + 1u);
      asset_source = vectis_cli_strdup(separator + 1);
      if (asset_source == NULL || asset_logical == NULL) {
        free(asset_source);
        free(asset_logical);
        vectis_pack_command_cleanup(&assets, &content_types);
        return 70;
      }
      asset_logical[separator - asset_arg] = '\0';
      if (vectis_pack_collect_dir(&assets, asset_source, asset_logical,
                                  &content_types) != 0) {
        free(asset_source);
        free(asset_logical);
        vectis_pack_command_cleanup(&assets, &content_types);
        return 1;
      }
      free(asset_source);
      free(asset_logical);
    } else if (strcmp(argv[i], "--asset-manifest") == 0 && i + 1 < argc) {
      if (vectis_pack_read_asset_manifest(&assets, argv[++i], &content_types) !=
          0) {
        vectis_pack_command_cleanup(&assets, &content_types);
        return 1;
      }
    } else {
      fprintf(stderr, "vectis: unknown pack argument: %s\n", argv[i]);
      vectis_pack_command_cleanup(&assets, &content_types);
      return 64;
    }
  }
  if (script_path == NULL || output_path == NULL) {
    fputs("vectis: pack requires --script and --output\n", stderr);
    vectis_pack_command_cleanup(&assets, &content_types);
    return 64;
  }
  if (vectis_self_path(argv[0], self_path, sizeof(self_path)) != 0) {
    fputs("vectis: failed to resolve current executable path\n", stderr);
    vectis_pack_command_cleanup(&assets, &content_types);
    return 1;
  }
  self = NULL;
  script = NULL;
  bundle = NULL;
  manifest = NULL;
  bundle_size = 0u;
  if (vectis_read_all(self_path, &self, &self_size) != 0 ||
      vectis_read_all(script_path, &script, &script_size) != 0) {
    fprintf(stderr, "vectis: failed to read pack input: %s\n", strerror(errno));
    free(self);
    free(script);
    vectis_pack_command_cleanup(&assets, &content_types);
    return 1;
  }
  if (bundle_path != NULL &&
      vectis_read_all(bundle_path, &bundle, &bundle_size) != 0) {
    fprintf(stderr, "vectis: failed to read lockd bundle: %s\n",
            strerror(errno));
    free(self);
    free(script);
    vectis_pack_command_cleanup(&assets, &content_types);
    return 1;
  }
  if (assets.count > 1u) {
    qsort(assets.items, assets.count, sizeof(assets.items[0]),
          vectis_pack_asset_compare);
    for (i = 1; i < (int)assets.count; ++i) {
      if (strcmp(assets.items[i - 1].logical_path,
                 assets.items[i].logical_path) == 0) {
        fprintf(stderr, "vectis: duplicate embedded asset path: %s\n",
                assets.items[i].logical_path);
        free(bundle);
        free(script);
        free(self);
        vectis_pack_command_cleanup(&assets, &content_types);
        return 1;
      }
    }
  }
  asset_size = 0u;
  for (i = 0; i < (int)assets.count; ++i) {
    assets.items[i].offset = asset_size;
    if (assets.items[i].size > ((size_t)-1) - asset_size) {
      fputs("vectis: embedded assets are too large\n", stderr);
      free(bundle);
      free(script);
      free(self);
      vectis_pack_command_cleanup(&assets, &content_types);
      return 1;
    }
    asset_size += assets.items[i].size;
  }
  if (vectis_pack_hash_assets(&assets, asset_sha) != 0) {
    fputs("vectis: failed to hash embedded assets\n", stderr);
    free(bundle);
    free(script);
    free(self);
    vectis_pack_command_cleanup(&assets, &content_types);
    return 1;
  }
  if (vectis_pack_build_manifest(&assets, &manifest, &manifest_size) != 0) {
    fprintf(stderr, "vectis: failed to build embedded asset manifest\n");
    free(bundle);
    free(script);
    free(self);
    vectis_pack_command_cleanup(&assets, &content_types);
    return 1;
  }
  SHA256(script, script_size, script_sha);
  memset(bundle_sha, 0, sizeof(bundle_sha));
  if (bundle != NULL) {
    SHA256(bundle, bundle_size, bundle_sha);
  }
  memset(manifest_sha, 0, sizeof(manifest_sha));
  if (manifest != NULL) {
    SHA256(manifest, manifest_size, manifest_sha);
  }
  offset = self_size;
  vectis_pack_make_footer(
      footer, (unsigned long long)offset, (unsigned long long)script_size,
      script_sha, (unsigned long long)(offset + script_size),
      (unsigned long long)bundle_size, bundle != NULL ? bundle_sha : NULL,
      (unsigned long long)(offset + script_size + bundle_size),
      (unsigned long long)asset_size, assets.count > 0u ? asset_sha : NULL,
      (unsigned long long)(offset + script_size + bundle_size + asset_size),
      (unsigned long long)manifest_size,
      manifest != NULL ? manifest_sha : NULL);
  out = fopen(output_path, "wb");
  if (out == NULL) {
    fprintf(stderr, "vectis: failed to create packed output: %s\n",
            output_path);
    free(manifest);
    free(bundle);
    free(script);
    free(self);
    vectis_pack_command_cleanup(&assets, &content_types);
    return 1;
  }
  if (vectis_write_all(out, self, self_size) != 0 ||
      vectis_write_all(out, script, script_size) != 0 ||
      vectis_write_all(out, bundle, bundle_size) != 0) {
    fprintf(stderr, "vectis: failed to write packed output: %s\n", output_path);
    (void)fclose(out);
    free(manifest);
    free(bundle);
    free(script);
    free(self);
    vectis_pack_command_cleanup(&assets, &content_types);
    return 1;
  }
  for (i = 0; i < (int)assets.count; ++i) {
    if (vectis_write_all(out, assets.items[i].data, assets.items[i].size) !=
        0) {
      fprintf(stderr, "vectis: failed to write packed output: %s\n",
              output_path);
      (void)fclose(out);
      free(manifest);
      free(bundle);
      free(script);
      free(self);
      vectis_pack_command_cleanup(&assets, &content_types);
      return 1;
    }
  }
  if (vectis_write_all(out, manifest, manifest_size) != 0 ||
      vectis_write_all(out, footer, sizeof(footer)) != 0 || fclose(out) != 0) {
    fprintf(stderr, "vectis: failed to write packed output: %s\n", output_path);
    free(manifest);
    free(bundle);
    free(script);
    free(self);
    vectis_pack_command_cleanup(&assets, &content_types);
    return 1;
  }
  if (chmod(output_path, 0755) != 0) {
    fprintf(stderr, "vectis: failed to chmod packed output: %s\n", output_path);
    free(manifest);
    free(bundle);
    free(script);
    free(self);
    vectis_pack_command_cleanup(&assets, &content_types);
    return 1;
  }
  free(manifest);
  free(bundle);
  free(script);
  free(self);
  vectis_pack_command_cleanup(&assets, &content_types);
  return 0;
}

static int vectis_lua_status_string(lua_State *lua) {
  lua_Integer status;
  const char *name;

  status = luaL_checkinteger(lua, 1);
  name = vectis_status_string((vectis_status)status);
  if (name == NULL) {
    lua_pushnil(lua);
  } else {
    lua_pushstring(lua, name);
  }
  return 1;
}

static int vectis_lua_has_embedded_lockd_bundle(lua_State *lua) {
  vectis_lua_runtime_context *context;

  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  lua_pushboolean(lua, context != NULL &&
                           context->embedded_lockd_bundle != NULL &&
                           context->embedded_lockd_bundle_size > 0u);
  return 1;
}

static int vectis_lua_embedded_lockd_bundle_size(lua_State *lua) {
  vectis_lua_runtime_context *context;

  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  lua_pushinteger(lua, context != NULL
                           ? (lua_Integer)context->embedded_lockd_bundle_size
                           : (lua_Integer)0);
  return 1;
}

static int vectis_lua_embedded_has_assets(lua_State *lua) {
  vectis_lua_runtime_context *context;

  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  lua_pushboolean(lua, context != NULL && context->embedded_fs != NULL);
  return 1;
}

static int vectis_lua_embedded_read(lua_State *lua) {
  vectis_lua_runtime_context *context;
  const char *path;
  vectis_bytes body;
  vectis_error error;
  vectis_status status;
  int found;

  path = luaL_checkstring(lua, 1);
  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  if (context == NULL || context->embedded_fs == NULL) {
    lua_pushnil(lua);
    lua_pushliteral(lua, "no embedded assets");
    return 2;
  }
  vectis_error_clear(&error);
  found = 0;
  body.data = NULL;
  body.size = 0u;
  status = vectis_embedded_fs_read(context->embedded_fs, path, &found, &body,
                                   &error);
  if (status != VECTIS_OK) {
    lua_pushnil(lua);
    lua_pushstring(lua, error.message[0] != '\0'
                            ? error.message
                            : vectis_status_string(status));
    return 2;
  }
  if (!found) {
    lua_pushnil(lua);
    lua_pushliteral(lua, "embedded asset not found");
    return 2;
  }
  lua_pushlstring(lua, (const char *)body.data, body.size);
  return 1;
}

static int vectis_lua_embedded_stat(lua_State *lua) {
  vectis_lua_runtime_context *context;
  const char *path;
  vectis_embedded_fs_entry entry;
  vectis_error error;
  vectis_status status;
  int found;

  path = luaL_checkstring(lua, 1);
  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  if (context == NULL || context->embedded_fs == NULL) {
    lua_pushnil(lua);
    lua_pushliteral(lua, "no embedded assets");
    return 2;
  }
  vectis_error_clear(&error);
  found = 0;
  memset(&entry, 0, sizeof(entry));
  status = vectis_embedded_fs_lookup(context->embedded_fs, path, &found, &entry,
                                     &error);
  if (status != VECTIS_OK) {
    lua_pushnil(lua);
    lua_pushstring(lua, error.message[0] != '\0'
                            ? error.message
                            : vectis_status_string(status));
    return 2;
  }
  if (!found) {
    lua_pushnil(lua);
    lua_pushliteral(lua, "embedded asset not found");
    return 2;
  }
  lua_newtable(lua);
  lua_pushstring(lua, entry.path);
  lua_setfield(lua, -2, "path");
  lua_pushinteger(lua, (lua_Integer)entry.size);
  lua_setfield(lua, -2, "size");
  if (entry.content_type != NULL) {
    lua_pushstring(lua, entry.content_type);
    lua_setfield(lua, -2, "content_type");
  }
  if (entry.sha256 != NULL) {
    lua_pushstring(lua, entry.sha256);
    lua_setfield(lua, -2, "sha256");
  }
  return 1;
}

static int vectis_lua_embedded_chunks_next(lua_State *lua) {
  vectis_lua_embedded_chunks_state *state;
  size_t remaining;
  size_t amount;

  state = (vectis_lua_embedded_chunks_state *)lua_touserdata(
      lua, lua_upvalueindex(1));
  if (state == NULL || state->offset >= state->size) {
    return 0;
  }
  remaining = state->size - state->offset;
  amount = remaining < state->chunk_size ? remaining : state->chunk_size;
  lua_pushlstring(lua, state->data + state->offset, amount);
  state->offset += amount;
  return 1;
}

static int vectis_lua_embedded_chunks(lua_State *lua) {
  vectis_lua_runtime_context *context;
  vectis_lua_embedded_chunks_state *state;
  const char *path;
  lua_Integer requested_chunk_size;
  vectis_bytes body;
  vectis_error error;
  vectis_status status;
  int found;

  path = luaL_checkstring(lua, 1);
  requested_chunk_size = luaL_optinteger(lua, 2, 64 * 1024);
  if (requested_chunk_size <= 0 || (unsigned long long)requested_chunk_size >
                                       (unsigned long long)((size_t)-1)) {
    return luaL_error(lua,
                      "embedded chunks chunk_size must be a valid positive "
                      "size");
  }
  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  if (context == NULL || context->embedded_fs == NULL) {
    lua_pushnil(lua);
    lua_pushliteral(lua, "no embedded assets");
    return 2;
  }
  vectis_error_clear(&error);
  found = 0;
  body.data = NULL;
  body.size = 0u;
  status = vectis_embedded_fs_read(context->embedded_fs, path, &found, &body,
                                   &error);
  if (status != VECTIS_OK) {
    lua_pushnil(lua);
    lua_pushstring(lua, error.message[0] != '\0'
                            ? error.message
                            : vectis_status_string(status));
    return 2;
  }
  if (!found) {
    lua_pushnil(lua);
    lua_pushliteral(lua, "embedded asset not found");
    return 2;
  }
  state = (vectis_lua_embedded_chunks_state *)lua_newuserdatauv(
      lua, sizeof(*state), 0);
  state->data = (const char *)body.data;
  state->size = body.size;
  state->offset = 0u;
  state->chunk_size = (size_t)requested_chunk_size;
  lua_pushcclosure(lua, vectis_lua_embedded_chunks_next, 1);
  return 1;
}

typedef struct vectis_lua_embedded_list_state {
  lua_State *lua;
  int table_index;
  int next_index;
} vectis_lua_embedded_list_state;

static const char *vectis_lua_table_string(lua_State *lua, int index,
                                           const char *field);

static vectis_status
vectis_lua_embedded_list_item(const vectis_embedded_fs_entry *entry,
                              void *userdata, vectis_error *error) {
  vectis_lua_embedded_list_state *state;

  (void)error;
  state = (vectis_lua_embedded_list_state *)userdata;
  lua_pushstring(state->lua, entry->path);
  lua_rawseti(state->lua, state->table_index, state->next_index++);
  return VECTIS_OK;
}

static int vectis_lua_embedded_list(lua_State *lua) {
  vectis_lua_runtime_context *context;
  const char *prefix;
  vectis_lua_embedded_list_state state;
  vectis_error error;
  vectis_status status;

  prefix = luaL_optstring(lua, 1, "/");
  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  lua_newtable(lua);
  if (context == NULL || context->embedded_fs == NULL) {
    return 1;
  }
  vectis_error_clear(&error);
  state.lua = lua;
  state.table_index = lua_gettop(lua);
  state.next_index = 1;
  status =
      vectis_embedded_fs_list(context->embedded_fs, prefix,
                              vectis_lua_embedded_list_item, &state, &error);
  if (status != VECTIS_OK) {
    return luaL_error(lua, "%s",
                      error.message[0] != '\0' ? error.message
                                               : vectis_status_string(status));
  }
  return 1;
}

static vectis_embedded_fs_extract_policy
vectis_lua_embedded_extract_policy(lua_State *lua, const char *policy) {
  if (policy == NULL || strcmp(policy, "fail_exists") == 0 ||
      strcmp(policy, "fail") == 0) {
    return VECTIS_EMBEDDED_FS_EXTRACT_FAIL_EXISTS;
  }
  if (strcmp(policy, "skip_existing") == 0 || strcmp(policy, "skip") == 0) {
    return VECTIS_EMBEDDED_FS_EXTRACT_SKIP_EXISTING;
  }
  if (strcmp(policy, "overwrite") == 0) {
    return VECTIS_EMBEDDED_FS_EXTRACT_OVERWRITE;
  }
  if (strcmp(policy, "verify") == 0) {
    return VECTIS_EMBEDDED_FS_EXTRACT_VERIFY;
  }
  if (strcmp(policy, "repair") == 0) {
    return VECTIS_EMBEDDED_FS_EXTRACT_REPAIR;
  }
  (void)luaL_error(lua, "embedded extract policy must be fail_exists, "
                        "skip_existing, overwrite, verify, or repair");
  return VECTIS_EMBEDDED_FS_EXTRACT_FAIL_EXISTS;
}

static int vectis_lua_embedded_extract(lua_State *lua) {
  vectis_lua_runtime_context *context;
  vectis_embedded_fs_extract_config config;
  const char *output_dir;
  const char *policy;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  output_dir = vectis_lua_table_string(lua, 1, "to");
  if (output_dir == NULL || output_dir[0] == '\0') {
    return luaL_error(lua, "embedded extract requires to");
  }
  policy = vectis_lua_table_string(lua, 1, "policy");
  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  if (context == NULL || context->embedded_fs == NULL) {
    lua_pushnil(lua);
    lua_pushliteral(lua, "no embedded assets");
    return 2;
  }
  vectis_embedded_fs_extract_config_init(&config);
  config.output_dir = output_dir;
  config.policy = vectis_lua_embedded_extract_policy(lua, policy);
  vectis_error_clear(&error);
  status = vectis_embedded_fs_extract(context->embedded_fs, &config, &error);
  if (status != VECTIS_OK) {
    lua_pushnil(lua);
    lua_pushstring(lua, error.message[0] != '\0'
                            ? error.message
                            : vectis_status_string(status));
    return 2;
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_push_error(lua_State *lua, vectis_status status,
                                 const vectis_error *error) {
  const char *message;

  message = error != NULL && error->message[0] != '\0'
                ? error->message
                : vectis_status_string(status);
  lua_pushnil(lua);
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)status);
  lua_setfield(lua, -2, "status");
  lua_pushstring(lua, vectis_status_string(status));
  lua_setfield(lua, -2, "status_string");
  lua_pushstring(lua, message != NULL ? message : "vectis error");
  lua_setfield(lua, -2, "message");
  return 2;
}

static int vectis_lua_push_error_text(lua_State *lua, vectis_status status,
                                      const char *message) {
  lua_pushnil(lua);
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)status);
  lua_setfield(lua, -2, "status");
  lua_pushstring(lua, vectis_status_string(status));
  lua_setfield(lua, -2, "status_string");
  lua_pushstring(lua, message != NULL ? message : vectis_status_string(status));
  lua_setfield(lua, -2, "message");
  return 2;
}

static const char *vectis_lua_table_string(lua_State *lua, int index,
                                           const char *field) {
  const char *value;

  lua_getfield(lua, index, field);
  value = lua_isnil(lua, -1) ? NULL : luaL_checkstring(lua, -1);
  lua_pop(lua, 1);
  return value;
}

static size_t vectis_lua_table_size(lua_State *lua, int index,
                                    const char *field, size_t fallback) {
  size_t value;

  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    value = fallback;
  } else {
    value = (size_t)luaL_checkinteger(lua, -1);
  }
  lua_pop(lua, 1);
  return value;
}

static int vectis_lua_table_bool(lua_State *lua, int index, const char *field,
                                 int fallback) {
  int value;

  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    value = fallback;
  } else {
    value = lua_toboolean(lua, -1);
  }
  lua_pop(lua, 1);
  return value;
}

static long vectis_lua_table_long(lua_State *lua, int index, const char *field,
                                  long fallback) {
  long value;

  lua_getfield(lua, index, field);
  value = lua_isnil(lua, -1) ? fallback : (long)luaL_checkinteger(lua, -1);
  lua_pop(lua, 1);
  return value;
}

static void vectis_lua_lonejson_schema_view_init(lonejson_schema_view *view) {
  memset(view, 0, sizeof(*view));
  view->size = sizeof(*view);
  view->abi_version = LONEJSON_VIEW_ABI_VERSION;
}

static void vectis_lua_lonejson_record_view_init(lonejson_record_view *view) {
  memset(view, 0, sizeof(*view));
  view->size = sizeof(*view);
  view->abi_version = LONEJSON_VIEW_ABI_VERSION;
  view->schema.size = sizeof(view->schema);
  view->schema.abi_version = LONEJSON_VIEW_ABI_VERSION;
}

static int vectis_lua_lonejson_check_schema(lua_State *lua, int index,
                                            lonejson_schema_view *view,
                                            const char *context) {
  lonejson_error error;
  lonejson_status status;

  memset(&error, 0, sizeof(error));
  vectis_lua_lonejson_schema_view_init(view);
  status = lonejson_lua_check_schema(lua, index, view, &error);
  if (status != LONEJSON_STATUS_OK) {
    return luaL_error(lua, "%s: %s", context,
                      error.message[0] != '\0' ? error.message
                                               : "invalid lonejson schema");
  }
  return 0;
}

static int vectis_lua_lonejson_assign_table(lua_State *lua, int schema_index,
                                            int record_index, int value_index) {
  lua_getfield(lua, schema_index, "assign");
  lua_pushvalue(lua, schema_index);
  lua_pushvalue(lua, record_index);
  lua_pushvalue(lua, value_index);
  if (lua_pcall(lua, 3, 1, 0) != 0) {
    return luaL_error(lua, "lonejson schema:assign failed: %s",
                      lua_tostring(lua, -1));
  }
  lua_pop(lua, 1);
  return 0;
}

static int vectis_lua_curl_version(lua_State *lua) {
  lua_pushstring(lua, curl_version());
  return 1;
}

static void vectis_lua_curl_global_init_once(void) {
  (void)curl_global_init(CURL_GLOBAL_DEFAULT);
}

static void vectis_lua_curl_buffer_free(vectis_lua_curl_buffer *buffer) {
  if (buffer != NULL) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0u;
    buffer->capacity = 0u;
    buffer->offset = 0u;
  }
}

static int vectis_lua_curl_buffer_append(vectis_lua_curl_buffer *buffer,
                                         const char *data, size_t size) {
  char *grown;
  size_t capacity;

  if (size == 0u) {
    return 1;
  }
  if (buffer->size + size < buffer->size) {
    return 0;
  }
  if (buffer->limit != 0u &&
      (size > buffer->limit || buffer->size > buffer->limit - size)) {
    buffer->limit_exceeded = 1;
    return 0;
  }
  if (buffer->size + size + 1u > buffer->capacity) {
    capacity = buffer->capacity == 0u ? 4096u : buffer->capacity;
    while (capacity < buffer->size + size + 1u) {
      if (capacity > ((size_t)-1) / 2u) {
        return 0;
      }
      capacity *= 2u;
    }
    grown = (char *)realloc(buffer->data, capacity);
    if (grown == NULL) {
      return 0;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
  }
  memcpy(buffer->data + buffer->size, data, size);
  buffer->size += size;
  buffer->data[buffer->size] = '\0';
  return 1;
}

static size_t vectis_lua_curl_write(char *ptr, size_t size, size_t nmemb,
                                    void *userdata) {
  vectis_lua_curl_buffer *buffer;
  size_t bytes;

  buffer = (vectis_lua_curl_buffer *)userdata;
  bytes = size * nmemb;
  if (size != 0u && bytes / size != nmemb) {
    return 0u;
  }
  return vectis_lua_curl_buffer_append(buffer, ptr, bytes) ? bytes : 0u;
}

static size_t vectis_lua_curl_stream_response_write(char *ptr, size_t size,
                                                    size_t nmemb,
                                                    void *userdata) {
  vectis_lua_curl_stream_response *response;
  size_t bytes;

  response = (vectis_lua_curl_stream_response *)userdata;
  if (response == NULL || response->parser == NULL) {
    return 0u;
  }
  bytes = size * nmemb;
  if (size != 0u && bytes / size != nmemb) {
    return 0u;
  }
  if (bytes > response->limit || response->size > response->limit - bytes) {
    response->limit_exceeded = 1;
    return 0u;
  }
  response->size += bytes;
  return lonejson_curl_write_callback(ptr, size, nmemb, response->parser);
}

static size_t vectis_lua_curl_read(char *ptr, size_t size, size_t nmemb,
                                   void *userdata) {
  vectis_lua_curl_buffer *buffer;
  size_t capacity;
  size_t remaining;
  size_t chunk;

  buffer = (vectis_lua_curl_buffer *)userdata;
  capacity = size * nmemb;
  if (size != 0u && capacity / size != nmemb) {
    return 0u;
  }
  if (capacity == 0u || buffer->offset >= buffer->size) {
    return 0u;
  }
  remaining = buffer->size - buffer->offset;
  chunk = remaining < capacity ? remaining : capacity;
  memcpy(ptr, buffer->data + buffer->offset, chunk);
  buffer->offset += chunk;
  return chunk;
}

static size_t vectis_lua_curl_abort_read(char *ptr, size_t size, size_t nmemb,
                                         void *userdata) {
  (void)ptr;
  (void)size;
  (void)nmemb;
  (void)userdata;
  return CURL_READFUNC_ABORT;
}

static int vectis_lua_curl_append_header(lua_State *lua,
                                         struct curl_slist **headers,
                                         const char *key, const char *value) {
  vectis_lua_curl_buffer line;
  struct curl_slist *grown;
  int ok;

  memset(&line, 0, sizeof(line));
  ok = 1;
  if (key != NULL) {
    ok = vectis_lua_curl_buffer_append(&line, key, strlen(key)) &&
         vectis_lua_curl_buffer_append(&line, ": ", 2u);
  }
  if (ok) {
    ok = vectis_lua_curl_buffer_append(&line, value, strlen(value));
  }
  if (!ok) {
    vectis_lua_curl_buffer_free(&line);
    return luaL_error(lua, "curl header allocation failed");
  }
  grown = curl_slist_append(*headers, line.data);
  vectis_lua_curl_buffer_free(&line);
  if (grown == NULL) {
    return luaL_error(lua, "curl header allocation failed");
  }
  *headers = grown;
  return 0;
}

static int vectis_lua_curl_apply_headers(lua_State *lua, CURL *curl,
                                         int option_index,
                                         struct curl_slist **headers) {
  const char *key;
  const char *value;

  lua_getfield(lua, option_index, "headers");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return 0;
  }
  if (!lua_istable(lua, -1)) {
    return luaL_error(lua, "curl headers must be a table");
  }
  lua_pushnil(lua);
  while (lua_next(lua, -2) != 0) {
    if (!lua_isstring(lua, -1)) {
      return luaL_error(lua, "curl header values must be strings");
    }
    value = lua_tostring(lua, -1);
    key = lua_type(lua, -2) == LUA_TSTRING ? lua_tostring(lua, -2) : NULL;
    vectis_lua_curl_append_header(lua, headers, key, value);
    lua_pop(lua, 1);
  }
  lua_pop(lua, 1);
  if (*headers != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *headers);
  }
  return 0;
}

static void vectis_lua_curl_apply_tls(lua_State *lua, CURL *curl,
                                      int option_index) {
  const char *ca_file;
  const char *ca_path;
  const char *client_cert;
  const char *client_key;
  const char *client_cert_type;
  const char *key_password;

  if (!vectis_lua_table_bool(lua, option_index, "verify_peer", 1)) {
    (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  }
  if (!vectis_lua_table_bool(lua, option_index, "verify_host", 1)) {
    (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  ca_file = vectis_lua_table_string(lua, option_index, "ca_file");
  if (ca_file != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
  }
  ca_path = vectis_lua_table_string(lua, option_index, "ca_path");
  if (ca_path != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_CAPATH, ca_path);
  }
  client_cert = vectis_lua_table_string(lua, option_index, "client_cert");
  if (client_cert != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_SSLCERT, client_cert);
  }
  client_key = vectis_lua_table_string(lua, option_index, "client_key");
  if (client_key != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_SSLKEY, client_key);
  }
  client_cert_type =
      vectis_lua_table_string(lua, option_index, "client_cert_type");
  if (client_cert_type != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, client_cert_type);
  }
  key_password = vectis_lua_table_string(lua, option_index, "key_password");
  if (key_password != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_KEYPASSWD, key_password);
  }
}

static int vectis_lua_curl_apply_mail_rcpt(lua_State *lua, CURL *curl,
                                           int smtp_index,
                                           struct curl_slist **recipients) {
  struct curl_slist *grown;
  const char *recipient;

  lua_getfield(lua, smtp_index, "rcpt");
  if (!lua_istable(lua, -1)) {
    return luaL_error(lua, "curl smtp.rcpt table is required");
  }
  lua_pushnil(lua);
  while (lua_next(lua, -2) != 0) {
    if (!lua_isstring(lua, -1)) {
      return luaL_error(lua, "curl smtp.rcpt values must be strings");
    }
    recipient = lua_tostring(lua, -1);
    grown = curl_slist_append(*recipients, recipient);
    if (grown == NULL) {
      return luaL_error(lua, "curl smtp recipient allocation failed");
    }
    *recipients = grown;
    lua_pop(lua, 1);
  }
  lua_pop(lua, 1);
  if (*recipients == NULL) {
    return luaL_error(lua, "curl smtp.rcpt requires at least one recipient");
  }
  (void)curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, *recipients);
  return 0;
}

static int vectis_lua_curl_apply_smtp(lua_State *lua, CURL *curl,
                                      int option_index,
                                      vectis_lua_curl_buffer *upload,
                                      struct curl_slist **recipients) {
  const char *mail_from;
  const char *body;
  size_t body_size;
  int smtp_index;
  int probe;

  lua_getfield(lua, option_index, "smtp");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return 0;
  }
  if (!lua_istable(lua, -1)) {
    return luaL_error(lua, "curl smtp must be a table");
  }
  smtp_index = lua_gettop(lua);
  lua_getfield(lua, smtp_index, "mail_from");
  mail_from = luaL_checkstring(lua, -1);
  (void)curl_easy_setopt(curl, CURLOPT_MAIL_FROM, mail_from);
  lua_pop(lua, 1);

  vectis_lua_curl_apply_mail_rcpt(lua, curl, smtp_index, recipients);
  if (vectis_lua_table_bool(lua, smtp_index, "use_ssl", 0)) {
    (void)curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
  }
  probe = vectis_lua_table_bool(lua, smtp_index, "probe", 0);
  if (probe) {
    lua_pop(lua, 1);
    (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                           vectis_lua_curl_abort_read);
    (void)curl_easy_setopt(curl, CURLOPT_READDATA, NULL);
    (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)-1);
    return 1;
  }

  lua_getfield(lua, option_index, "body");
  body = lua_tolstring(lua, -1, &body_size);
  if (body == NULL) {
    return luaL_error(lua, "curl body is required for SMTP upload");
  }
  if (!vectis_lua_curl_buffer_append(upload, body, body_size)) {
    return luaL_error(lua, "curl SMTP body allocation failed");
  }
  lua_pop(lua, 1);
  (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION, vectis_lua_curl_read);
  (void)curl_easy_setopt(curl, CURLOPT_READDATA, upload);
  (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                         (curl_off_t)upload->size);
  lua_pop(lua, 1);
  return 1;
}

static int vectis_lua_curl_apply_upload(lua_State *lua, CURL *curl,
                                        int option_index,
                                        vectis_lua_curl_buffer *upload,
                                        int is_smtp, int has_streaming_upload) {
  const char *body;
  size_t body_size;

  if (is_smtp || has_streaming_upload ||
      !vectis_lua_table_bool(lua, option_index, "upload", 0)) {
    return 0;
  }
  lua_getfield(lua, option_index, "body");
  body = lua_tolstring(lua, -1, &body_size);
  if (body == NULL) {
    return luaL_error(lua, "curl body is required for upload");
  }
  if (!vectis_lua_curl_buffer_append(upload, body, body_size)) {
    return luaL_error(lua, "curl upload body allocation failed");
  }
  lua_pop(lua, 1);
  (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION, vectis_lua_curl_read);
  (void)curl_easy_setopt(curl, CURLOPT_READDATA, upload);
  (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                         (curl_off_t)upload->size);
  return 1;
}

static int vectis_lua_curl_apply_method(lua_State *lua, CURL *curl,
                                        int option_index, int is_smtp,
                                        int has_streaming_upload,
                                        lonejson_curl_upload *json_upload) {
  const char *method;
  const char *body;
  size_t body_size;

  if (is_smtp) {
    return 0;
  }
  method = vectis_lua_table_string(lua, option_index, "method");
  lua_getfield(lua, option_index, "body");
  body = lua_tolstring(lua, -1, &body_size);
  if (method == NULL) {
    if (body != NULL || has_streaming_upload) {
      method = "POST";
    } else {
      lua_pop(lua, 1);
      return 0;
    }
  }
  if (strcmp(method, "GET") == 0) {
    (void)curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  } else if (strcmp(method, "POST") == 0) {
    if (has_streaming_upload) {
      curl_off_t upload_size;

      upload_size = lonejson_curl_upload_size(json_upload);
      (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
      (void)curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
      (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                             lonejson_curl_read_callback);
      (void)curl_easy_setopt(curl, CURLOPT_READDATA, json_upload);
      (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                             upload_size > 0 ? upload_size : (curl_off_t)-1);
    } else {
      (void)curl_easy_setopt(curl, CURLOPT_POST, 1L);
      (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS,
                             body != NULL ? body : "");
      (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                             (curl_off_t)(body != NULL ? body_size : 0u));
    }
  } else if (strcmp(method, "PUT") == 0 || strcmp(method, "PATCH") == 0 ||
             strcmp(method, "DELETE") == 0) {
    (void)curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (has_streaming_upload) {
      (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
      (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                             lonejson_curl_read_callback);
      (void)curl_easy_setopt(curl, CURLOPT_READDATA, json_upload);
      (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                             lonejson_curl_upload_size(json_upload));
    } else if (body != NULL) {
      (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
      (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                             (curl_off_t)body_size);
    }
  } else if (strcmp(method, "HEAD") == 0) {
    (void)curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  } else {
    return luaL_error(lua, "unsupported curl method: %s", method);
  }
  lua_pop(lua, 1);
  return 0;
}

static int vectis_lua_curl_proxy_type(lua_State *lua, const char *value,
                                      long *out) {
  if (strcmp(value, "http") == 0) {
    *out = (long)CURLPROXY_HTTP;
  } else if (strcmp(value, "https") == 0) {
    *out = (long)CURLPROXY_HTTPS;
  } else if (strcmp(value, "socks4") == 0) {
    *out = (long)CURLPROXY_SOCKS4;
  } else if (strcmp(value, "socks4a") == 0) {
    *out = (long)CURLPROXY_SOCKS4A;
  } else if (strcmp(value, "socks5") == 0) {
    *out = (long)CURLPROXY_SOCKS5;
  } else if (strcmp(value, "socks5_hostname") == 0) {
    *out = (long)CURLPROXY_SOCKS5_HOSTNAME;
  } else {
    return luaL_error(lua, "unsupported curl proxy_type: %s", value);
  }
  return 0;
}

static int vectis_lua_curl_apply_protocol_options(lua_State *lua, CURL *curl,
                                                  int option_index) {
  const char *value;
  long proxy_type;
  long long_value;

  proxy_type = 0L;
  value = vectis_lua_table_string(lua, option_index, "protocols");
  if (value != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, value);
  }
  value = vectis_lua_table_string(lua, option_index, "proxy");
  if (value != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_PROXY, value);
  }
  value = vectis_lua_table_string(lua, option_index, "proxy_type");
  if (value != NULL) {
    if (vectis_lua_curl_proxy_type(lua, value, &proxy_type) != 0) {
      return 1;
    }
    (void)curl_easy_setopt(curl, CURLOPT_PROXYTYPE, proxy_type);
  }
  value = vectis_lua_table_string(lua, option_index, "proxy_username");
  if (value != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME, value);
  }
  value = vectis_lua_table_string(lua, option_index, "proxy_password");
  if (value != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD, value);
  }
  value = vectis_lua_table_string(lua, option_index, "interface");
  if (value != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_INTERFACE, value);
  }
  value = vectis_lua_table_string(lua, option_index, "user_agent");
  if (value != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, value);
  }
  value = vectis_lua_table_string(lua, option_index, "accept_encoding");
  if (value != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, value);
  }
  value = vectis_lua_table_string(lua, option_index, "ssh_private_key");
  if (value != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_SSH_PRIVATE_KEYFILE, value);
  }
  value = vectis_lua_table_string(lua, option_index, "ssh_public_key");
  if (value != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_SSH_PUBLIC_KEYFILE, value);
  }
  value = vectis_lua_table_string(lua, option_index, "ssh_known_hosts");
  if (value != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_SSH_KNOWNHOSTS, value);
  }
  long_value =
      vectis_lua_table_long(lua, option_index, "connect_timeout_ms", 0L);
  if (long_value > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, long_value);
  }
  long_value = vectis_lua_table_long(lua, option_index, "low_speed_limit", 0L);
  if (long_value > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, long_value);
  }
  long_value = vectis_lua_table_long(lua, option_index, "low_speed_time", 0L);
  if (long_value > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, long_value);
  }
  if (vectis_lua_table_bool(lua, option_index, "tcp_keepalive", 0)) {
    (void)curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  }
  if (vectis_lua_table_bool(lua, option_index, "no_signal", 1)) {
    (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  }
  return 0;
}

static void vectis_lua_curl_push_result(lua_State *lua, CURLcode code,
                                        CURL *curl,
                                        const vectis_lua_curl_buffer *body,
                                        const vectis_lua_curl_buffer *headers,
                                        const char *error_buffer) {
  long response_code;
  char *effective_url;

  response_code = 0L;
  effective_url = NULL;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  (void)curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);

  lua_newtable(lua);
  lua_pushboolean(lua, code == CURLE_OK);
  lua_setfield(lua, -2, "ok");
  lua_pushinteger(lua, (lua_Integer)code);
  lua_setfield(lua, -2, "code");
  lua_pushstring(lua, curl_easy_strerror(code));
  lua_setfield(lua, -2, "code_name");
  lua_pushinteger(lua, (lua_Integer)response_code);
  lua_setfield(lua, -2, "status");
  lua_pushlstring(lua, body->data != NULL ? body->data : "", body->size);
  lua_setfield(lua, -2, "body");
  lua_pushlstring(lua, headers->data != NULL ? headers->data : "",
                  headers->size);
  lua_setfield(lua, -2, "headers");
  if (effective_url != NULL) {
    lua_pushstring(lua, effective_url);
    lua_setfield(lua, -2, "effective_url");
  }
  if (code != CURLE_OK) {
    if (body->limit_exceeded) {
      lua_pushliteral(lua, "curl response body exceeded 8388608 byte limit");
    } else if (headers->limit_exceeded) {
      lua_pushliteral(lua, "curl response headers exceeded 65536 byte limit");
    } else {
      lua_pushstring(lua, error_buffer[0] != '\0' ? error_buffer
                                                  : curl_easy_strerror(code));
    }
    lua_setfield(lua, -2, "error");
  }
}

static int vectis_lua_curl_perform(lua_State *lua) {
  CURL *curl;
  CURLcode code;
  struct curl_slist *headers;
  struct curl_slist *recipients;
  vectis_lua_curl_buffer body;
  vectis_lua_curl_buffer response;
  vectis_lua_curl_buffer response_headers;
  vectis_lua_curl_stream_response stream_response;
  lonejson_curl_parse json_response;
  lonejson_curl_upload json_upload;
  lonejson_schema_view request_schema;
  lonejson_schema_view response_schema;
  lonejson_record_view request_record;
  lonejson_record_view response_record;
  const char *url;
  const char *username;
  const char *password;
  char error_buffer[CURL_ERROR_SIZE];
  lonejson_error json_error;
  lonejson_status json_status;
  long timeout_ms;
  int is_smtp;
  int is_upload;
  int request_schema_index;
  int request_value_index;
  int request_record_index;
  int response_schema_index;
  int response_record_index;
  int has_streaming_upload;
  int has_streaming_response;

  luaL_checktype(lua, 1, LUA_TTABLE);
  url = vectis_lua_table_string(lua, 1, "url");
  if (url == NULL || url[0] == '\0') {
    return luaL_error(lua, "curl url is required");
  }

  memset(&body, 0, sizeof(body));
  memset(&response, 0, sizeof(response));
  memset(&response_headers, 0, sizeof(response_headers));
  response.limit = VECTIS_LUA_CURL_RESPONSE_BODY_LIMIT;
  response_headers.limit = VECTIS_LUA_CURL_RESPONSE_HEADER_LIMIT;
  memset(&stream_response, 0, sizeof(stream_response));
  memset(&json_response, 0, sizeof(json_response));
  memset(&json_upload, 0, sizeof(json_upload));
  vectis_lua_lonejson_schema_view_init(&request_schema);
  vectis_lua_lonejson_schema_view_init(&response_schema);
  vectis_lua_lonejson_record_view_init(&request_record);
  vectis_lua_lonejson_record_view_init(&response_record);
  memset(error_buffer, 0, sizeof(error_buffer));
  memset(&json_error, 0, sizeof(json_error));
  headers = NULL;
  recipients = NULL;
  request_schema_index = 0;
  request_value_index = 0;
  request_record_index = 0;
  response_schema_index = 0;
  response_record_index = 0;
  has_streaming_upload = 0;
  has_streaming_response = 0;
  (void)pthread_once(&vectis_lua_curl_once, vectis_lua_curl_global_init_once);
  curl = curl_easy_init();
  if (curl == NULL) {
    return luaL_error(lua, "curl_easy_init failed");
  }

  (void)curl_easy_setopt(curl, CURLOPT_URL, url);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  lua_getfield(lua, 1, "request_schema");
  if (!lua_isnil(lua, -1)) {
    request_schema_index = lua_gettop(lua);
    (void)vectis_lua_lonejson_check_schema(
        lua, request_schema_index, &request_schema, "curl request_schema");
    lua_getfield(lua, 1, "body_json");
    if (lua_isnil(lua, -1)) {
      return luaL_error(lua, "curl request_schema requires body_json");
    }
    request_value_index = lua_gettop(lua);
    memset(&json_error, 0, sizeof(json_error));
    vectis_lua_lonejson_record_view_init(&request_record);
    json_status = lonejson_lua_new_record(lua, request_schema_index,
                                          &request_record_index,
                                          &request_record, &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
      return luaL_error(lua, "lonejson request record failed: %s",
                        json_error.message[0] != '\0' ? json_error.message
                                                      : "invalid record");
    }
    (void)vectis_lua_lonejson_assign_table(
        lua, request_schema_index, request_record_index, request_value_index);
    memset(&json_error, 0, sizeof(json_error));
    vectis_lua_lonejson_record_view_init(&request_record);
    json_status =
        lonejson_lua_check_record(lua, request_record_index, &request_schema,
                                  &request_record, &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
      return luaL_error(lua, "lonejson request record failed: %s",
                        json_error.message[0] != '\0' ? json_error.message
                                                      : "invalid record");
    }
    json_status =
        lonejson_curl_upload_init(&json_upload, request_schema.runtime,
                                  request_schema.map, request_record.record);
    if (json_status != LONEJSON_STATUS_OK) {
      return luaL_error(lua, "lonejson curl upload init failed");
    }
    has_streaming_upload = 1;
  } else {
    lua_pop(lua, 1);
  }

  lua_getfield(lua, 1, "response_schema");
  if (!lua_isnil(lua, -1)) {
    response_schema_index = lua_gettop(lua);
    (void)vectis_lua_lonejson_check_schema(
        lua, response_schema_index, &response_schema, "curl response_schema");
    memset(&json_error, 0, sizeof(json_error));
    vectis_lua_lonejson_record_view_init(&response_record);
    json_status = lonejson_lua_new_record(lua, response_schema_index,
                                          &response_record_index,
                                          &response_record, &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
      if (has_streaming_upload) {
        lonejson_curl_upload_cleanup(&json_upload);
      }
      return luaL_error(lua, "lonejson response record failed: %s",
                        json_error.message[0] != '\0' ? json_error.message
                                                      : "invalid record");
    }
    json_status =
        lonejson_curl_parse_init(&json_response, response_schema.runtime,
                                 response_schema.map, response_record.record);
    if (json_status != LONEJSON_STATUS_OK) {
      if (has_streaming_upload) {
        lonejson_curl_upload_cleanup(&json_upload);
      }
      return luaL_error(lua, "lonejson curl parse init failed");
    }
    has_streaming_response = 1;
    stream_response.parser = &json_response;
    stream_response.limit = VECTIS_LUA_CURL_RESPONSE_BODY_LIMIT;
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                           vectis_lua_curl_stream_response_write);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream_response);
  } else {
    lua_pop(lua, 1);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, vectis_lua_curl_write);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  }
  (void)curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, vectis_lua_curl_write);
  (void)curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
  (void)curl_easy_setopt(
      curl, CURLOPT_FOLLOWLOCATION,
      vectis_lua_table_bool(lua, 1, "follow_redirects", 0) ? 1L : 0L);
  timeout_ms = vectis_lua_table_long(lua, 1, "timeout_ms", 0L);
  if (timeout_ms > 0L) {
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  }
  if (vectis_lua_table_bool(lua, 1, "http2", 0)) {
    (void)curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                           (long)CURL_HTTP_VERSION_2TLS);
  }
  vectis_lua_curl_apply_tls(lua, curl, 1);
  username = vectis_lua_table_string(lua, 1, "username");
  password = vectis_lua_table_string(lua, 1, "password");
  if (username != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_USERNAME, username);
  }
  if (password != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_PASSWORD, password);
  }
  if (vectis_lua_curl_apply_protocol_options(lua, curl, 1) != 0) {
    curl_easy_cleanup(curl);
    vectis_lua_curl_buffer_free(&body);
    vectis_lua_curl_buffer_free(&response);
    vectis_lua_curl_buffer_free(&response_headers);
    return 1;
  }

  vectis_lua_curl_apply_headers(lua, curl, 1, &headers);
  if (has_streaming_upload) {
    vectis_lua_curl_append_header(lua, &headers, "Transfer-Encoding",
                                  "chunked");
    (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }
  is_smtp = vectis_lua_curl_apply_smtp(lua, curl, 1, &body, &recipients);
  is_upload = is_smtp;
  if (vectis_lua_curl_apply_upload(lua, curl, 1, &body, is_smtp,
                                   has_streaming_upload) != 0) {
    is_upload = 1;
  }
  if (has_streaming_upload) {
    is_upload = 1;
  }
  vectis_lua_curl_apply_method(lua, curl, 1,
                               is_smtp || (is_upload && !has_streaming_upload),
                               has_streaming_upload, &json_upload);

  code = curl_easy_perform(curl);
  if (stream_response.limit_exceeded) {
    response.limit_exceeded = 1;
  }
  if (has_streaming_response && code == CURLE_OK) {
    json_status = lonejson_curl_parse_finish(&json_response);
  } else {
    json_status = LONEJSON_STATUS_OK;
  }
  vectis_lua_curl_push_result(lua, code, curl, &response, &response_headers,
                              error_buffer);
  if (has_streaming_response) {
    if (code == CURLE_OK && json_status == LONEJSON_STATUS_OK) {
      if (lonejson_lua_record_to_table(lua, response_record_index) == 0) {
        lua_pushboolean(lua, 0);
        lua_setfield(lua, -3, "ok");
        lua_pushliteral(lua, "lonejson response record conversion failed");
        lua_setfield(lua, -3, "error");
      } else {
        lua_pushvalue(lua, -1);
        lua_setfield(lua, -3, "json");
        lua_setfield(lua, -2, "response_json");
      }
    } else if (code == CURLE_OK) {
      json_error = json_response.error;
      lua_pushboolean(lua, 0);
      lua_setfield(lua, -2, "ok");
      lua_pushstring(lua, json_error.message);
      lua_setfield(lua, -2, "error");
      lua_pushstring(lua, json_error.message);
      lua_setfield(lua, -2, "json_error");
    }
  }

  curl_slist_free_all(headers);
  curl_slist_free_all(recipients);
  curl_easy_cleanup(curl);
  if (has_streaming_response) {
    lonejson_curl_parse_cleanup(&json_response);
  }
  if (has_streaming_upload) {
    lonejson_curl_upload_cleanup(&json_upload);
  }
  vectis_lua_curl_buffer_free(&body);
  vectis_lua_curl_buffer_free(&response);
  vectis_lua_curl_buffer_free(&response_headers);
  return 1;
}

static int64_t vectis_lua_table_i64(lua_State *lua, int index,
                                    const char *field, int64_t fallback) {
  int64_t value;

  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    value = fallback;
  } else {
    value = (int64_t)luaL_checkinteger(lua, -1);
  }
  lua_pop(lua, 1);
  return value;
}

static unsigned vectis_lua_auth_mode_value(const char *mode) {
  if (mode == NULL || strcmp(mode, "default") == 0) {
    return VECTIS_AUTH_MODE_DEFAULT;
  }
  if (strcmp(mode, "basic") == 0) {
    return VECTIS_AUTH_MODE_BASIC;
  }
  if (strcmp(mode, "bearer") == 0) {
    return VECTIS_AUTH_MODE_BEARER;
  }
  return VECTIS_AUTH_MODE_DEFAULT;
}

static unsigned vectis_lua_auth_modes_at(lua_State *lua, int index,
                                         unsigned fallback) {
  unsigned modes;
  size_t count;
  size_t i;

  if (lua_isnil(lua, index)) {
    return fallback;
  }
  if (lua_istable(lua, index)) {
    modes = 0u;
    count = lua_rawlen(lua, index);
    for (i = 1u; i <= count; ++i) {
      lua_rawgeti(lua, index, (lua_Integer)i);
      if (lua_isnumber(lua, -1)) {
        modes |= (unsigned)lua_tointeger(lua, -1);
      } else {
        modes |= vectis_lua_auth_mode_value(luaL_checkstring(lua, -1));
      }
      lua_pop(lua, 1);
    }
    return modes != 0u ? modes : fallback;
  }
  if (lua_isnumber(lua, index)) {
    return (unsigned)lua_tointeger(lua, index);
  }
  return vectis_lua_auth_mode_value(luaL_checkstring(lua, index));
}

static unsigned vectis_lua_auth_modes_field(lua_State *lua, int index,
                                            const char *field,
                                            unsigned fallback) {
  unsigned modes;

  lua_getfield(lua, index, field);
  modes = vectis_lua_auth_modes_at(lua, -1, fallback);
  lua_pop(lua, 1);
  return modes;
}

static const char *vectis_lua_auth_mode_name(unsigned mode) {
  if ((mode & VECTIS_AUTH_MODE_BASIC) != 0u) {
    return "basic";
  }
  if ((mode & VECTIS_AUTH_MODE_BEARER) != 0u) {
    return "bearer";
  }
  return "default";
}

static int vectis_lua_auth_action_name_valid(const char *action) {
  return action != NULL &&
         (strcmp(action, "allow") == 0 || strcmp(action, "deny") == 0 ||
          strcmp(action, "required") == 0 || strcmp(action, "redirect") == 0);
}

static int vectis_lua_copy_optional_string_field(lua_State *lua, int source,
                                                 int destination,
                                                 const char *field,
                                                 const char **message) {
  size_t len;
  const char *value;

  source = lua_absindex(lua, source);
  destination = lua_absindex(lua, destination);
  lua_getfield(lua, source, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return 1;
  }
  value = lua_tolstring(lua, -1, &len);
  if (value == NULL) {
    if (message != NULL) {
      *message = "auth callback response string field is invalid";
    }
    lua_pop(lua, 1);
    return 0;
  }
  lua_pushlstring(lua, value, len);
  lua_setfield(lua, destination, field);
  lua_pop(lua, 1);
  return 1;
}

static void vectis_lua_auth_store_config(lua_State *lua, int index,
                                         vectis_auth_store_config *config) {
  const char *path;

  vectis_auth_store_config_init(config);
  index = lua_absindex(lua, index);
  path = vectis_lua_table_string(lua, index, "credentials_path");
  if (path == NULL) {
    path = vectis_lua_table_string(lua, index, "path");
  }
  config->credentials_path = path;
  config->max_store_bytes = vectis_lua_table_size(
      lua, index, "max_store_bytes", VECTIS_AUTH_DEFAULT_MAX_STORE_BYTES);
}

static void vectis_lua_auth_push_result(lua_State *lua,
                                        const vectis_auth_result *result) {
  lua_newtable(lua);
  lua_pushboolean(lua, result != NULL && result->authenticated);
  lua_setfield(lua, -2, "authenticated");
  lua_pushstring(lua, result != NULL
                          ? vectis_lua_auth_mode_name(result->auth_mode)
                          : "default");
  lua_setfield(lua, -2, "auth_mode");
  if (result != NULL && result->client_id != NULL) {
    lua_pushstring(lua, result->client_id);
    lua_setfield(lua, -2, "client_id");
  }
  if (result != NULL && result->claim_json != NULL) {
    lua_pushstring(lua, result->claim_json);
    lua_setfield(lua, -2, "claim_json");
  }
}

static void vectis_lua_auth_push_issued_credential(
    lua_State *lua, const vectis_auth_issued_credential *credential) {
  lua_newtable(lua);
  if (credential != NULL && credential->client_id != NULL) {
    lua_pushstring(lua, credential->client_id);
    lua_setfield(lua, -2, "client_id");
  }
  if (credential != NULL && credential->client_secret != NULL) {
    lua_pushstring(lua, credential->client_secret);
    lua_setfield(lua, -2, "client_secret");
  }
  if (credential != NULL && credential->api_key != NULL) {
    lua_pushstring(lua, credential->api_key);
    lua_setfield(lua, -2, "api_key");
  }
  if (credential != NULL && credential->claim_json != NULL) {
    lua_pushstring(lua, credential->claim_json);
    lua_setfield(lua, -2, "claim_json");
  }
}

static int vectis_lua_auth_copy_token_flow_string(lua_State *lua, int index,
                                                  const char *field,
                                                  char **out) {
  const char *value;

  value = vectis_lua_table_string(lua, index, field);
  if (value == NULL) {
    *out = NULL;
    return 1;
  }
  *out = vectis_cli_strdup(value);
  return *out != NULL;
}

static int
vectis_lua_auth_token_flow_config(lua_State *lua, int index,
                                  vectis_auth_oauth2_token_flow *flow) {
  vectis_auth_oauth2_token_flow_init(flow);
  if (!vectis_lua_auth_copy_token_flow_string(lua, index, "access_token",
                                              &flow->access_token) ||
      !vectis_lua_auth_copy_token_flow_string(lua, index, "token_type",
                                              &flow->token_type) ||
      !vectis_lua_auth_copy_token_flow_string(lua, index, "refresh_token",
                                              &flow->refresh_token) ||
      !vectis_lua_auth_copy_token_flow_string(lua, index, "scope",
                                              &flow->scope) ||
      !vectis_lua_auth_copy_token_flow_string(lua, index, "id_token",
                                              &flow->id_token)) {
    vectis_auth_oauth2_token_flow_cleanup(flow);
    return 0;
  }
  flow->expires_at = vectis_lua_table_i64(lua, index, "expires_at", 0);
  flow->has_expires_at = vectis_lua_table_bool(lua, index, "has_expires_at",
                                               flow->expires_at != 0);
  return 1;
}

static void
vectis_lua_auth_push_token_flow(lua_State *lua,
                                const vectis_auth_oauth2_token_flow *flow) {
  lua_newtable(lua);
  if (flow != NULL && flow->access_token != NULL) {
    lua_pushstring(lua, flow->access_token);
    lua_setfield(lua, -2, "access_token");
  }
  if (flow != NULL && flow->token_type != NULL) {
    lua_pushstring(lua, flow->token_type);
    lua_setfield(lua, -2, "token_type");
  }
  if (flow != NULL && flow->refresh_token != NULL) {
    lua_pushstring(lua, flow->refresh_token);
    lua_setfield(lua, -2, "refresh_token");
  }
  if (flow != NULL && flow->scope != NULL) {
    lua_pushstring(lua, flow->scope);
    lua_setfield(lua, -2, "scope");
  }
  if (flow != NULL && flow->id_token != NULL) {
    lua_pushstring(lua, flow->id_token);
    lua_setfield(lua, -2, "id_token");
  }
  if (flow != NULL && flow->has_expires_at) {
    lua_pushinteger(lua, (lua_Integer)flow->expires_at);
    lua_setfield(lua, -2, "expires_at");
  }
  lua_pushboolean(lua, flow != NULL && flow->has_expires_at);
  lua_setfield(lua, -2, "has_expires_at");
}

static void vectis_lua_auth_push_token_response(
    lua_State *lua, const vectis_auth_oauth2_token_response *response) {
  lua_newtable(lua);
  if (response != NULL && response->access_token != NULL) {
    lua_pushstring(lua, response->access_token);
    lua_setfield(lua, -2, "access_token");
  }
  if (response != NULL && response->token_type != NULL) {
    lua_pushstring(lua, response->token_type);
    lua_setfield(lua, -2, "token_type");
  }
  if (response != NULL && response->refresh_token != NULL) {
    lua_pushstring(lua, response->refresh_token);
    lua_setfield(lua, -2, "refresh_token");
  }
  if (response != NULL && response->scope != NULL) {
    lua_pushstring(lua, response->scope);
    lua_setfield(lua, -2, "scope");
  }
  if (response != NULL && response->id_token != NULL) {
    lua_pushstring(lua, response->id_token);
    lua_setfield(lua, -2, "id_token");
  }
  if (response != NULL && response->has_expires_in) {
    lua_pushinteger(lua, (lua_Integer)response->expires_in);
    lua_setfield(lua, -2, "expires_in");
  }
  lua_pushboolean(lua, response != NULL && response->has_expires_in);
  lua_setfield(lua, -2, "has_expires_in");
}

static const char *vectis_lua_auth_oauth2_flow_state_name(
    vectis_auth_oauth2_token_flow_state state) {
  switch (state) {
  case VECTIS_AUTH_OAUTH2_TOKEN_FLOW_REFRESHED:
    return "refreshed";
  case VECTIS_AUTH_OAUTH2_TOKEN_FLOW_NEEDS_INTERACTION:
    return "needs_interaction";
  case VECTIS_AUTH_OAUTH2_TOKEN_FLOW_FAILED:
    return "failed";
  case VECTIS_AUTH_OAUTH2_TOKEN_FLOW_READY:
  default:
    return "ready";
  }
}

static void vectis_lua_auth_push_token_flow_result(
    lua_State *lua, const vectis_auth_oauth2_token_flow_result *result) {
  lua_newtable(lua);
  lua_pushstring(lua, result != NULL ? vectis_lua_auth_oauth2_flow_state_name(
                                           result->state)
                                     : "ready");
  lua_setfield(lua, -2, "state");
  lua_pushinteger(lua, result != NULL ? (lua_Integer)result->attempts
                                      : (lua_Integer)0);
  lua_setfield(lua, -2, "attempts");
  lua_pushboolean(lua, result != NULL && result->refreshed);
  lua_setfield(lua, -2, "refreshed");
}

static void vectis_lua_auth_oauth2_transport_init(
    vectis_lua_auth_oauth2_transport *transport) {
  transport->lua = NULL;
  transport->callback_ref = LUA_NOREF;
}

static void vectis_lua_auth_oauth2_transport_cleanup(
    vectis_lua_auth_oauth2_transport *transport) {
  if (transport != NULL && transport->lua != NULL &&
      transport->callback_ref != LUA_NOREF) {
    luaL_unref(transport->lua, LUA_REGISTRYINDEX, transport->callback_ref);
  }
  if (transport != NULL) {
    vectis_lua_auth_oauth2_transport_init(transport);
  }
}

static int vectis_lua_auth_oauth2_transport_config(
    lua_State *lua, int index, vectis_auth_oauth2_transport_config *config,
    vectis_lua_auth_oauth2_transport *transport) {
  vectis_auth_oauth2_transport_config_init(config);
  vectis_lua_auth_oauth2_transport_init(transport);
  index = lua_absindex(lua, index);
  config->user_agent = vectis_lua_table_string(lua, index, "user_agent");
  lua_getfield(lua, index, "transport");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_getfield(lua, index, "http_callback");
  }
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return 1;
  }
  luaL_checktype(lua, -1, LUA_TFUNCTION);
  transport->lua = lua;
  transport->callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  config->request = NULL;
  config->request_userdata = transport;
  return 1;
}

static vectis_status vectis_lua_auth_oauth2_http_request(
    const vectis_auth_oauth2_http_request *request,
    vectis_auth_oauth2_http_response *response, void *userdata,
    vectis_error *error) {
  vectis_lua_auth_oauth2_transport *transport;
  lua_State *lua;
  const char *content_type;
  const char *body;
  size_t body_size;
  long status_code;
  int base;

  (void)error;
  transport = (vectis_lua_auth_oauth2_transport *)userdata;
  if (transport == NULL || transport->lua == NULL ||
      transport->callback_ref == LUA_NOREF || request == NULL ||
      response == NULL) {
    return VECTIS_ERR_INVALID;
  }
  lua = transport->lua;
  base = lua_gettop(lua);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, transport->callback_ref);
  lua_newtable(lua);
  if (request->method != NULL) {
    lua_pushstring(lua, request->method);
    lua_setfield(lua, -2, "method");
  }
  if (request->url != NULL) {
    lua_pushstring(lua, request->url);
    lua_setfield(lua, -2, "url");
  }
  if (request->content_type != NULL) {
    lua_pushstring(lua, request->content_type);
    lua_setfield(lua, -2, "content_type");
  }
  if (request->authorization != NULL) {
    lua_pushstring(lua, request->authorization);
    lua_setfield(lua, -2, "authorization");
  }
  if (request->user_agent != NULL) {
    lua_pushstring(lua, request->user_agent);
    lua_setfield(lua, -2, "user_agent");
  }
  if (request->body != NULL && request->body_size > 0u) {
    lua_pushlstring(lua, (const char *)request->body, request->body_size);
    lua_setfield(lua, -2, "body");
  }
  lua_pushinteger(lua, (lua_Integer)request->max_response_bytes);
  lua_setfield(lua, -2, "max_response_bytes");
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    lua_settop(lua, base);
    return VECTIS_ERR_STATE;
  }
  if (!lua_istable(lua, -1)) {
    lua_settop(lua, base);
    return VECTIS_ERR_INVALID;
  }
  lua_getfield(lua, -1, "status_code");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_getfield(lua, -1, "status");
  }
  if (!lua_isnil(lua, -1) && !lua_isnumber(lua, -1)) {
    lua_pop(lua, 1);
    lua_settop(lua, base);
    return VECTIS_ERR_INVALID;
  }
  status_code = lua_isnil(lua, -1) ? 200L : (long)lua_tointeger(lua, -1);
  lua_pop(lua, 1);
  lua_getfield(lua, -1, "content_type");
  content_type = lua_isnil(lua, -1) ? NULL : lua_tolstring(lua, -1, NULL);
  if (!lua_isnil(lua, -1) && content_type == NULL) {
    lua_pop(lua, 1);
    lua_settop(lua, base);
    return VECTIS_ERR_INVALID;
  }
  response->content_type = vectis_cli_strdup(content_type);
  lua_pop(lua, 1);
  if (content_type != NULL && response->content_type == NULL) {
    lua_settop(lua, base);
    return VECTIS_ERR_NOMEM;
  }
  lua_getfield(lua, -1, "body");
  if (lua_isnil(lua, -1)) {
    body = "";
    body_size = 0u;
  } else {
    body = lua_tolstring(lua, -1, &body_size);
    if (body == NULL) {
      lua_pop(lua, 1);
      lua_settop(lua, base);
      return VECTIS_ERR_INVALID;
    }
  }
  response->body = vectis_cli_memdup(body, body_size);
  response->body_size = body_size;
  response->status_code = status_code;
  if (body_size > 0u && response->body == NULL) {
    lua_pop(lua, 1);
    lua_settop(lua, base);
    return VECTIS_ERR_NOMEM;
  }
  lua_pop(lua, 1);
  lua_settop(lua, base);
  return VECTIS_OK;
}

static int vectis_lua_auth_oauth2_transport_prepare(
    lua_State *lua, int index, vectis_auth_oauth2_transport_config *config,
    vectis_lua_auth_oauth2_transport *transport) {
  if (!vectis_lua_auth_oauth2_transport_config(lua, index, config, transport)) {
    return 0;
  }
  if (transport->callback_ref != LUA_NOREF) {
    config->request = vectis_lua_auth_oauth2_http_request;
  }
  return 1;
}

static void vectis_lua_auth_push_provider_response(
    lua_State *lua, const vectis_auth_provider_response *response) {
  lua_newtable(lua);
  switch (response != NULL ? response->action : VECTIS_AUTH_DENY) {
  case VECTIS_AUTH_ALLOW:
    lua_pushliteral(lua, "allow");
    break;
  case VECTIS_AUTH_REQUIRED:
    lua_pushliteral(lua, "required");
    break;
  case VECTIS_AUTH_REDIRECT:
    lua_pushliteral(lua, "redirect");
    break;
  case VECTIS_AUTH_DENY:
  default:
    lua_pushliteral(lua, "deny");
    break;
  }
  lua_setfield(lua, -2, "action");
  lua_pushinteger(lua, response != NULL ? (lua_Integer)response->status_code
                                        : (lua_Integer)0);
  lua_setfield(lua, -2, "status_code");
  if (response != NULL && response->location != NULL) {
    lua_pushstring(lua, response->location);
    lua_setfield(lua, -2, "location");
  }
  if (response != NULL && response->www_authenticate[0] != '\0') {
    lua_pushstring(lua, response->www_authenticate);
    lua_setfield(lua, -2, "www_authenticate");
  }
  if (response != NULL && response->content_type != NULL) {
    lua_pushstring(lua, response->content_type);
    lua_setfield(lua, -2, "content_type");
  }
  if (response != NULL && response->body != NULL && response->body_size > 0u) {
    lua_pushlstring(lua, (const char *)response->body, response->body_size);
    lua_setfield(lua, -2, "body");
  }
  if (response != NULL && response->principal[0] != '\0') {
    lua_pushstring(lua, response->principal);
    lua_setfield(lua, -2, "principal");
  }
  if (response != NULL) {
    vectis_lua_auth_push_result(lua, &response->result);
    lua_setfield(lua, -2, "result");
  }
}

static int vectis_lua_auth_store_init(lua_State *lua) {
  vectis_auth_store_config config;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &config);
  status = vectis_auth_store_init(&config, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_auth_issue(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_auth_issue_config issue;
  vectis_auth_issued_credential credential;
  vectis_error error;
  vectis_status status;
  const char *purpose;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  vectis_auth_issue_config_init(&issue);
  issue.subject = vectis_lua_table_string(lua, 1, "subject");
  purpose = vectis_lua_table_string(lua, 1, "purpose");
  if (purpose != NULL) {
    issue.purpose = purpose;
  }
  issue.auth_modes =
      vectis_lua_auth_modes_field(lua, 1, "modes", VECTIS_AUTH_MODE_BEARER);
  issue.max_record_bytes =
      vectis_lua_table_size(lua, 1, "max_record_bytes", 0u);
  vectis_auth_issued_credential_init(&credential);
  status = vectis_auth_issue_credential(&store, &issue, &credential, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_auth_push_issued_credential(lua, &credential);
  vectis_auth_issued_credential_cleanup(&credential);
  return 1;
}

static int vectis_lua_auth_verify(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_auth_result result;
  vectis_error error;
  vectis_status status;
  const char *authorization;
  unsigned modes;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  authorization = vectis_lua_table_string(lua, 1, "authorization");
  modes = vectis_lua_auth_modes_field(lua, 1, "allowed_modes",
                                      VECTIS_AUTH_MODE_DEFAULT);
  vectis_auth_result_init(&result);
  status = vectis_auth_verify_authorization(&store, authorization, modes,
                                            &result, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_auth_push_result(lua, &result);
  vectis_auth_result_cleanup(&result);
  return 1;
}

static int vectis_lua_auth_revoke(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_error error;
  vectis_status status;
  const char *client_id;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  client_id = vectis_lua_table_string(lua, 1, "client_id");
  status = vectis_auth_revoke_client(&store, client_id, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static void vectis_lua_auth_user_config(lua_State *lua, int index,
                                        vectis_auth_user_config *config) {
  const char *issuer;

  vectis_auth_user_config_init(config);
  config->username = vectis_lua_table_string(lua, index, "username");
  config->password = vectis_lua_table_string(lua, index, "password");
  config->enable_totp = vectis_lua_table_bool(lua, index, "totp", 0);
  config->totp_secret = vectis_lua_table_string(lua, index, "totp_secret");
  if (config->totp_secret != NULL) {
    config->enable_totp = 1;
  }
  config->totp_label = vectis_lua_table_string(lua, index, "totp_label");
  issuer = vectis_lua_table_string(lua, index, "totp_issuer");
  if (issuer == NULL) {
    issuer = vectis_lua_table_string(lua, index, "issuer");
  }
  if (issuer != NULL) {
    config->totp_issuer = issuer;
  }
}

static void vectis_lua_auth_login_config(lua_State *lua, int index,
                                         vectis_auth_login_config *config) {
  vectis_auth_login_config_init(config);
  config->username = vectis_lua_table_string(lua, index, "username");
  config->password = vectis_lua_table_string(lua, index, "password");
  config->totp_code = vectis_lua_table_string(lua, index, "totp_code");
  config->unix_seconds =
      (uint64_t)vectis_lua_table_size(lua, index, "time", 0u);
  config->totp_window =
      (unsigned int)vectis_lua_table_size(lua, index, "window", 1u);
}

static int vectis_lua_auth_user_add(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_auth_user_config user;
  vectis_auth_user_enrollment enrollment;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  vectis_lua_auth_user_config(lua, 1, &user);
  vectis_auth_user_enrollment_init(&enrollment);
  status = vectis_auth_user_add_or_update(&store, &user, &enrollment, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_newtable(lua);
  if (enrollment.username != NULL) {
    lua_pushstring(lua, enrollment.username);
    lua_setfield(lua, -2, "username");
  }
  if (enrollment.generated_password != NULL) {
    lua_pushstring(lua, enrollment.generated_password);
    lua_setfield(lua, -2, "password");
  }
  if (enrollment.totp_secret != NULL) {
    lua_pushstring(lua, enrollment.totp_secret);
    lua_setfield(lua, -2, "totp_secret");
  }
  if (enrollment.totp_uri != NULL) {
    lua_pushstring(lua, enrollment.totp_uri);
    lua_setfield(lua, -2, "totp_uri");
  }
  if (enrollment.totp_qr_ansi != NULL) {
    lua_pushstring(lua, enrollment.totp_qr_ansi);
    lua_setfield(lua, -2, "totp_qr");
  }
  vectis_auth_user_enrollment_cleanup(&enrollment);
  return 1;
}

static int vectis_lua_auth_user_login(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_auth_login_config login;
  vectis_auth_result result;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  vectis_lua_auth_login_config(lua, 1, &login);
  vectis_auth_result_init(&result);
  status = vectis_auth_user_login(&store, &login, &result, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_auth_push_result(lua, &result);
  vectis_auth_result_cleanup(&result);
  return 1;
}

static int vectis_lua_auth_webdav_key(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_auth_login_config login;
  vectis_auth_issued_credential credential;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  vectis_lua_auth_login_config(lua, 1, &login);
  vectis_auth_issued_credential_init(&credential);
  status = vectis_auth_issue_webdav_key_for_login(&store, &login, &credential,
                                                  &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_auth_push_issued_credential(lua, &credential);
  vectis_auth_issued_credential_cleanup(&credential);
  return 1;
}

static int vectis_lua_auth_oidc_authorization(lua_State *lua) {
  vectis_auth_oidc_authorization_config config;
  vectis_auth_oidc_authorization authorization;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_auth_oidc_authorization_config_init(&config);
  config.authorization_endpoint =
      vectis_lua_table_string(lua, 1, "authorization_endpoint");
  config.client_id = vectis_lua_table_string(lua, 1, "client_id");
  config.redirect_uri = vectis_lua_table_string(lua, 1, "redirect_uri");
  config.scope = vectis_lua_table_string(lua, 1, "scope");
  config.state = vectis_lua_table_string(lua, 1, "state");
  config.nonce = vectis_lua_table_string(lua, 1, "nonce");
  config.code_verifier = vectis_lua_table_string(lua, 1, "code_verifier");
  config.code_challenge = vectis_lua_table_string(lua, 1, "code_challenge");
  config.audience = vectis_lua_table_string(lua, 1, "audience");
  config.resource = vectis_lua_table_string(lua, 1, "resource");
  config.verifier_bytes = vectis_lua_table_size(lua, 1, "verifier_bytes", 0u);
  config.max_url_bytes = vectis_lua_table_size(lua, 1, "max_url_bytes", 0u);
  vectis_auth_oidc_authorization_init(&authorization);
  status =
      vectis_auth_oidc_authorization_start(&config, &authorization, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_newtable(lua);
  if (authorization.authorization_url != NULL) {
    lua_pushstring(lua, authorization.authorization_url);
    lua_setfield(lua, -2, "authorization_url");
  }
  if (authorization.code_verifier != NULL) {
    lua_pushstring(lua, authorization.code_verifier);
    lua_setfield(lua, -2, "code_verifier");
  }
  if (authorization.code_challenge != NULL) {
    lua_pushstring(lua, authorization.code_challenge);
    lua_setfield(lua, -2, "code_challenge");
  }
  if (authorization.state != NULL) {
    lua_pushstring(lua, authorization.state);
    lua_setfield(lua, -2, "state");
  }
  if (authorization.nonce != NULL) {
    lua_pushstring(lua, authorization.nonce);
    lua_setfield(lua, -2, "nonce");
  }
  vectis_auth_oidc_authorization_cleanup(&authorization);
  return 1;
}

static int vectis_lua_auth_oauth2_client_credentials(lua_State *lua) {
  vectis_auth_oauth2_client_credentials_config config;
  vectis_lua_auth_oauth2_transport transport;
  vectis_auth_oauth2_token_response response;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_auth_oauth2_client_credentials_config_init(&config);
  if (!vectis_lua_auth_oauth2_transport_prepare(lua, 1, &config.transport,
                                                &transport)) {
    return vectis_lua_push_error(lua, VECTIS_ERR_INVALID, &error);
  }
  config.token_endpoint = vectis_lua_table_string(lua, 1, "token_endpoint");
  config.client_id = vectis_lua_table_string(lua, 1, "client_id");
  config.client_secret = vectis_lua_table_string(lua, 1, "client_secret");
  config.scope = vectis_lua_table_string(lua, 1, "scope");
  config.audience = vectis_lua_table_string(lua, 1, "audience");
  config.resource = vectis_lua_table_string(lua, 1, "resource");
  config.max_response_bytes =
      vectis_lua_table_size(lua, 1, "max_response_bytes", 0u);
  config.max_body_bytes = vectis_lua_table_size(lua, 1, "max_body_bytes", 0u);
  vectis_auth_oauth2_token_response_init(&response);
  status =
      vectis_auth_oauth2_client_credentials_request(&config, &response, &error);
  vectis_lua_auth_oauth2_transport_cleanup(&transport);
  if (status != VECTIS_OK) {
    vectis_auth_oauth2_token_response_cleanup(&response);
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_auth_push_token_response(lua, &response);
  vectis_auth_oauth2_token_response_cleanup(&response);
  return 1;
}

static int vectis_lua_auth_oauth2_flow_ensure(lua_State *lua) {
  vectis_auth_oauth2_token_flow flow;
  vectis_auth_oauth2_token_flow_policy policy;
  vectis_auth_oauth2_token_flow_result result;
  vectis_lua_auth_oauth2_transport transport;
  vectis_error error;
  vectis_status status;
  int flow_index;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  lua_getfield(lua, 1, "flow");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    if (!vectis_lua_auth_token_flow_config(lua, 1, &flow)) {
      return vectis_lua_push_error(lua, VECTIS_ERR_NOMEM, &error);
    }
  } else {
    luaL_checktype(lua, -1, LUA_TTABLE);
    flow_index = lua_absindex(lua, -1);
    if (!vectis_lua_auth_token_flow_config(lua, flow_index, &flow)) {
      lua_pop(lua, 1);
      return vectis_lua_push_error(lua, VECTIS_ERR_NOMEM, &error);
    }
    lua_pop(lua, 1);
  }
  vectis_auth_oauth2_token_flow_policy_init(&policy);
  if (!vectis_lua_auth_oauth2_transport_prepare(lua, 1, &policy.transport,
                                                &transport)) {
    vectis_auth_oauth2_token_flow_cleanup(&flow);
    return vectis_lua_push_error(lua, VECTIS_ERR_INVALID, &error);
  }
  policy.token_endpoint = vectis_lua_table_string(lua, 1, "token_endpoint");
  policy.client_id = vectis_lua_table_string(lua, 1, "client_id");
  policy.client_secret = vectis_lua_table_string(lua, 1, "client_secret");
  policy.scope = vectis_lua_table_string(lua, 1, "scope");
  policy.now = vectis_lua_table_i64(lua, 1, "now", 0);
  policy.refresh_skew_seconds =
      vectis_lua_table_i64(lua, 1, "refresh_skew_seconds", 0);
  policy.max_response_bytes =
      vectis_lua_table_size(lua, 1, "max_response_bytes", 0u);
  policy.max_retries =
      (unsigned)vectis_lua_table_size(lua, 1, "max_retries", 0u);
  policy.disable_refresh = vectis_lua_table_bool(lua, 1, "disable_refresh", 0);
  policy.disable_retry = vectis_lua_table_bool(lua, 1, "disable_retry", 0);
  vectis_auth_oauth2_token_flow_result_init(&result);
  status =
      vectis_auth_oauth2_token_flow_ensure(&flow, &policy, &result, &error);
  vectis_lua_auth_oauth2_transport_cleanup(&transport);
  if (status != VECTIS_OK) {
    vectis_auth_oauth2_token_flow_cleanup(&flow);
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_newtable(lua);
  vectis_lua_auth_push_token_flow(lua, &flow);
  lua_setfield(lua, -2, "flow");
  vectis_lua_auth_push_token_flow_result(lua, &result);
  lua_setfield(lua, -2, "result");
  vectis_auth_oauth2_token_flow_cleanup(&flow);
  return 1;
}

static int vectis_lua_auth_oauth2_stored_flow_ensure(lua_State *lua) {
  vectis_auth_oauth2_stored_token_flow_policy policy;
  vectis_auth_oauth2_stored_token_flow flow;
  vectis_auth_oauth2_token_flow_result result;
  vectis_lua_auth_oauth2_transport transport;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_auth_oauth2_stored_token_flow_policy_init(&policy);
  vectis_lua_auth_store_config(lua, 1, &policy.store);
  policy.flow_id = vectis_lua_table_string(lua, 1, "flow_id");
  if (!vectis_lua_auth_oauth2_transport_prepare(
          lua, 1, &policy.flow_policy.transport, &transport)) {
    return vectis_lua_push_error(lua, VECTIS_ERR_INVALID, &error);
  }
  policy.flow_policy.token_endpoint =
      vectis_lua_table_string(lua, 1, "token_endpoint");
  policy.flow_policy.client_id = vectis_lua_table_string(lua, 1, "client_id");
  policy.flow_policy.client_secret =
      vectis_lua_table_string(lua, 1, "client_secret");
  policy.flow_policy.scope = vectis_lua_table_string(lua, 1, "scope");
  policy.flow_policy.now = vectis_lua_table_i64(lua, 1, "now", 0);
  policy.flow_policy.refresh_skew_seconds =
      vectis_lua_table_i64(lua, 1, "refresh_skew_seconds", 0);
  policy.flow_policy.max_response_bytes =
      vectis_lua_table_size(lua, 1, "max_response_bytes", 0u);
  policy.flow_policy.max_retries =
      (unsigned)vectis_lua_table_size(lua, 1, "max_retries", 0u);
  policy.flow_policy.disable_refresh =
      vectis_lua_table_bool(lua, 1, "disable_refresh", 0);
  policy.flow_policy.disable_retry =
      vectis_lua_table_bool(lua, 1, "disable_retry", 0);
  policy.revoke_webdav_keys_on_failure =
      vectis_lua_table_bool(lua, 1, "revoke_webdav_keys_on_failure", 1);
  vectis_auth_oauth2_stored_token_flow_init(&flow);
  vectis_auth_oauth2_token_flow_result_init(&result);
  status = vectis_auth_oauth2_stored_token_flow_ensure(&policy, &flow, &result,
                                                       &error);
  vectis_lua_auth_oauth2_transport_cleanup(&transport);
  if (status != VECTIS_OK) {
    vectis_auth_oauth2_stored_token_flow_cleanup(&flow);
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_newtable(lua);
  lua_pushboolean(lua, flow.found);
  lua_setfield(lua, -2, "found");
  if (flow.flow_id != NULL) {
    lua_pushstring(lua, flow.flow_id);
    lua_setfield(lua, -2, "flow_id");
  }
  if (flow.subject != NULL) {
    lua_pushstring(lua, flow.subject);
    lua_setfield(lua, -2, "subject");
  }
  if (flow.webdav_client_id != NULL) {
    lua_pushstring(lua, flow.webdav_client_id);
    lua_setfield(lua, -2, "webdav_client_id");
  }
  vectis_lua_auth_push_token_flow(lua, &flow.flow);
  lua_setfield(lua, -2, "flow");
  vectis_lua_auth_push_token_flow_result(lua, &result);
  lua_setfield(lua, -2, "result");
  vectis_auth_oauth2_stored_token_flow_cleanup(&flow);
  return 1;
}

static int vectis_lua_auth_oidc_exchange_callback(lua_State *lua) {
  vectis_auth_oidc_token_exchange_config config;
  vectis_auth_oidc_token_exchange exchange;
  vectis_lua_auth_oauth2_transport transport;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_auth_oidc_token_exchange_config_init(&config);
  if (!vectis_lua_auth_oauth2_transport_prepare(lua, 1, &config.transport,
                                                &transport)) {
    return vectis_lua_push_error(lua, VECTIS_ERR_INVALID, &error);
  }
  config.token_endpoint = vectis_lua_table_string(lua, 1, "token_endpoint");
  config.client_id = vectis_lua_table_string(lua, 1, "client_id");
  config.client_secret = vectis_lua_table_string(lua, 1, "client_secret");
  config.redirect_uri = vectis_lua_table_string(lua, 1, "redirect_uri");
  config.code_verifier = vectis_lua_table_string(lua, 1, "code_verifier");
  config.callback_query = vectis_lua_table_string(lua, 1, "callback_query");
  config.expected_state = vectis_lua_table_string(lua, 1, "expected_state");
  config.now = vectis_lua_table_i64(lua, 1, "now", 0);
  config.max_response_bytes =
      vectis_lua_table_size(lua, 1, "max_response_bytes", 0u);
  config.max_query_bytes = vectis_lua_table_size(lua, 1, "max_query_bytes", 0u);
  vectis_auth_oidc_token_exchange_init(&exchange);
  status = vectis_auth_oidc_exchange_callback(&config, &exchange, &error);
  vectis_lua_auth_oauth2_transport_cleanup(&transport);
  if (status != VECTIS_OK) {
    vectis_auth_oidc_token_exchange_cleanup(&exchange);
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_newtable(lua);
  if (exchange.code != NULL) {
    lua_pushstring(lua, exchange.code);
    lua_setfield(lua, -2, "code");
  }
  if (exchange.state != NULL) {
    lua_pushstring(lua, exchange.state);
    lua_setfield(lua, -2, "state");
  }
  vectis_lua_auth_push_token_response(lua, &exchange.token);
  lua_setfield(lua, -2, "token");
  vectis_lua_auth_push_token_flow(lua, &exchange.flow);
  lua_setfield(lua, -2, "flow");
  vectis_auth_oidc_token_exchange_cleanup(&exchange);
  return 1;
}

static int vectis_lua_auth_oauth2_flow_upsert(lua_State *lua) {
  vectis_auth_oauth2_token_flow_store_config config;
  vectis_error error;
  vectis_status status;
  int flow_index;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_auth_oauth2_token_flow_store_config_init(&config);
  vectis_lua_auth_store_config(lua, 1, &config.store);
  config.flow_id = vectis_lua_table_string(lua, 1, "flow_id");
  config.subject = vectis_lua_table_string(lua, 1, "subject");
  config.webdav_client_id = vectis_lua_table_string(lua, 1, "webdav_client_id");
  lua_getfield(lua, 1, "flow");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    if (!vectis_lua_auth_token_flow_config(lua, 1, &config.flow)) {
      return vectis_lua_push_error(lua, VECTIS_ERR_NOMEM, &error);
    }
  } else {
    luaL_checktype(lua, -1, LUA_TTABLE);
    flow_index = lua_absindex(lua, -1);
    if (!vectis_lua_auth_token_flow_config(lua, flow_index, &config.flow)) {
      lua_pop(lua, 1);
      return vectis_lua_push_error(lua, VECTIS_ERR_NOMEM, &error);
    }
    lua_pop(lua, 1);
  }
  status = vectis_auth_oauth2_token_flow_upsert(&config, &error);
  vectis_auth_oauth2_token_flow_cleanup(&config.flow);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_auth_oauth2_flow_load(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_auth_oauth2_stored_token_flow flow;
  vectis_error error;
  vectis_status status;
  const char *flow_id;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  flow_id = vectis_lua_table_string(lua, 1, "flow_id");
  vectis_auth_oauth2_stored_token_flow_init(&flow);
  status = vectis_auth_oauth2_token_flow_load(&store, flow_id, &flow, &error);
  if (status != VECTIS_OK) {
    vectis_auth_oauth2_stored_token_flow_cleanup(&flow);
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_newtable(lua);
  lua_pushboolean(lua, flow.found);
  lua_setfield(lua, -2, "found");
  if (flow.flow_id != NULL) {
    lua_pushstring(lua, flow.flow_id);
    lua_setfield(lua, -2, "flow_id");
  }
  if (flow.subject != NULL) {
    lua_pushstring(lua, flow.subject);
    lua_setfield(lua, -2, "subject");
  }
  if (flow.webdav_client_id != NULL) {
    lua_pushstring(lua, flow.webdav_client_id);
    lua_setfield(lua, -2, "webdav_client_id");
  }
  vectis_lua_auth_push_token_flow(lua, &flow.flow);
  lua_setfield(lua, -2, "flow");
  vectis_auth_oauth2_stored_token_flow_cleanup(&flow);
  return 1;
}

static int vectis_lua_auth_oauth2_webdav_key(lua_State *lua) {
  vectis_auth_oauth2_webdav_key_config config;
  vectis_auth_issued_credential credential;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_auth_oauth2_webdav_key_config_init(&config);
  vectis_lua_auth_store_config(lua, 1, &config.store);
  config.flow_id = vectis_lua_table_string(lua, 1, "flow_id");
  config.subject = vectis_lua_table_string(lua, 1, "subject");
  config.max_record_bytes =
      vectis_lua_table_size(lua, 1, "max_record_bytes", 0u);
  vectis_auth_issued_credential_init(&credential);
  status = vectis_auth_issue_webdav_key_for_oauth2_flow(&config, &credential,
                                                        &error);
  if (status != VECTIS_OK) {
    vectis_auth_issued_credential_cleanup(&credential);
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_auth_push_issued_credential(lua, &credential);
  vectis_auth_issued_credential_cleanup(&credential);
  return 1;
}

static int vectis_lua_auth_native_provider_authenticate(lua_State *lua) {
  vectis_auth_native_provider_config config;
  vectis_auth_provider provider;
  vectis_auth_provider_request request;
  vectis_auth_provider_response response;
  vectis_error error;
  vectis_status status;
  int request_index;

  luaL_checktype(lua, 1, LUA_TTABLE);
  request_index = lua_gettop(lua) >= 2 && !lua_isnil(lua, 2) ? 2 : 0;
  vectis_error_clear(&error);
  vectis_auth_native_provider_config_init(&config);
  vectis_lua_auth_store_config(lua, 1, &config.store);
  config.purpose = vectis_lua_table_string(lua, 1, "purpose");
  config.realm = vectis_lua_table_string(lua, 1, "realm");
  config.allowed_auth_modes = vectis_lua_auth_modes_field(
      lua, 1, "allowed_modes", VECTIS_AUTH_MODE_DEFAULT);
  if (config.realm == NULL) {
    config.realm = "vectis";
  }
  vectis_auth_provider_init(&provider);
  status = vectis_auth_provider_from_native_store(&provider, &config, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_auth_provider_request_init(&request);
  if (request_index != 0) {
    luaL_checktype(lua, request_index, LUA_TTABLE);
    request.authorization =
        vectis_lua_table_string(lua, request_index, "authorization");
    request.purpose = vectis_lua_table_string(lua, request_index, "purpose");
    request.resource = vectis_lua_table_string(lua, request_index, "resource");
    request.allowed_auth_modes = vectis_lua_auth_modes_field(
        lua, request_index, "allowed_modes", VECTIS_AUTH_MODE_DEFAULT);
  }
  vectis_auth_provider_response_init(&response);
  status =
      vectis_auth_provider_authenticate(&provider, &request, &response, &error);
  if (status != VECTIS_OK) {
    vectis_auth_provider_response_cleanup(&response);
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_auth_push_provider_response(lua, &response);
  vectis_auth_provider_response_cleanup(&response);
  return 1;
}

static int vectis_lua_auth_provider_native(lua_State *lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  lua_newtable(lua);
  lua_pushliteral(lua, "native");
  lua_setfield(lua, -2, "kind");
  lua_pushvalue(lua, 1);
  lua_setfield(lua, -2, "config");
  lua_getfield(lua, 1, "credentials_path");
  lua_setfield(lua, -2, "credentials_path");
  lua_getfield(lua, 1, "path");
  lua_setfield(lua, -2, "path");
  lua_getfield(lua, 1, "max_store_bytes");
  lua_setfield(lua, -2, "max_store_bytes");
  lua_getfield(lua, 1, "purpose");
  lua_setfield(lua, -2, "purpose");
  lua_getfield(lua, 1, "realm");
  lua_setfield(lua, -2, "realm");
  lua_getfield(lua, 1, "allowed_modes");
  lua_setfield(lua, -2, "allowed_modes");
  lua_pushcfunction(lua, vectis_lua_auth_native_provider_authenticate);
  lua_setfield(lua, -2, "authenticate");
  return 1;
}

static int vectis_lua_auth_callback_provider_authenticate(lua_State *lua) {
  int base;
  int output_index;
  int response_index;
  lua_Integer status_code;
  const char *action;
  const char *message;
  char error_message[256];

  luaL_checktype(lua, 1, LUA_TTABLE);
  if (lua_gettop(lua) < 2) {
    lua_newtable(lua);
  } else {
    luaL_checktype(lua, 2, LUA_TTABLE);
  }
  base = lua_gettop(lua);
  lua_getfield(lua, 1, "callback");
  luaL_checktype(lua, -1, LUA_TFUNCTION);
  lua_pushvalue(lua, 2);
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    message = lua_tostring(lua, -1);
    (void)snprintf(error_message, sizeof(error_message),
                   "auth callback failed: %s",
                   message != NULL ? message : "unknown Lua error");
    lua_settop(lua, base);
    return vectis_lua_push_error_text(lua, VECTIS_ERR_STATE, error_message);
  }
  if (!lua_istable(lua, -1)) {
    lua_settop(lua, base);
    return vectis_lua_push_error_text(
        lua, VECTIS_ERR_INVALID,
        "auth callback must return a provider response table");
  }
  response_index = lua_absindex(lua, -1);
  lua_getfield(lua, response_index, "action");
  action = lua_isnil(lua, -1) ? "deny" : lua_tostring(lua, -1);
  if (!vectis_lua_auth_action_name_valid(action)) {
    lua_pop(lua, 1);
    lua_settop(lua, base);
    return vectis_lua_push_error_text(
        lua, VECTIS_ERR_INVALID,
        "auth callback response action must be allow, deny, required, or "
        "redirect");
  }
  lua_pop(lua, 1);
  lua_newtable(lua);
  output_index = lua_absindex(lua, -1);
  lua_pushstring(lua, action);
  lua_setfield(lua, output_index, "action");
  lua_getfield(lua, response_index, "status_code");
  if (lua_isnil(lua, -1)) {
    status_code = 0;
  } else if (lua_isnumber(lua, -1)) {
    status_code = lua_tointeger(lua, -1);
  } else {
    lua_pop(lua, 1);
    lua_settop(lua, base);
    return vectis_lua_push_error_text(
        lua, VECTIS_ERR_INVALID,
        "auth callback response status_code must be a number");
  }
  lua_pop(lua, 1);
  lua_pushinteger(lua, status_code);
  lua_setfield(lua, output_index, "status_code");
  message = NULL;
  if (!vectis_lua_copy_optional_string_field(lua, response_index, output_index,
                                             "location", &message) ||
      !vectis_lua_copy_optional_string_field(lua, response_index, output_index,
                                             "www_authenticate", &message) ||
      !vectis_lua_copy_optional_string_field(lua, response_index, output_index,
                                             "content_type", &message) ||
      !vectis_lua_copy_optional_string_field(lua, response_index, output_index,
                                             "body", &message) ||
      !vectis_lua_copy_optional_string_field(lua, response_index, output_index,
                                             "principal", &message)) {
    lua_settop(lua, base);
    return vectis_lua_push_error_text(lua, VECTIS_ERR_INVALID, message);
  }
  lua_remove(lua, response_index);
  return 1;
}

static int vectis_lua_auth_provider_callback(lua_State *lua) {
  luaL_checktype(lua, 1, LUA_TFUNCTION);
  lua_newtable(lua);
  lua_pushliteral(lua, "callback");
  lua_setfield(lua, -2, "kind");
  lua_pushvalue(lua, 1);
  lua_setfield(lua, -2, "callback");
  lua_pushcfunction(lua, vectis_lua_auth_callback_provider_authenticate);
  lua_setfield(lua, -2, "authenticate");
  return 1;
}

static vectis_lua_totp *vectis_lua_check_totp(lua_State *lua, int index) {
  return (vectis_lua_totp *)luaL_checkudata(lua, index, VECTIS_LUA_TOTP);
}

static vectis_lua_qr *vectis_lua_check_qr(lua_State *lua, int index) {
  return (vectis_lua_qr *)luaL_checkudata(lua, index, VECTIS_LUA_QR);
}

static uint64_t vectis_lua_totp_time(lua_State *lua, int index) {
  lua_Integer value;

  if (lua_isnoneornil(lua, index)) {
    return (uint64_t)time(NULL);
  }
  value = luaL_checkinteger(lua, index);
  if (value < 0) {
    luaL_error(lua, "TOTP unix time must not be negative");
  }
  return (uint64_t)value;
}

static int vectis_lua_totp_new(lua_State *lua) {
  vectis_lua_totp *totp;
  vectis_totp_qr_status status;
  const char *secret;

  secret = luaL_checkstring(lua, 1);
  totp = (vectis_lua_totp *)lua_newuserdata(lua, sizeof(*totp));
  status = vectis_totp_init(&totp->value, secret);
  if (status != VECTIS_TOTP_QR_OK) {
    return luaL_error(lua, "TOTP secret is invalid: %s",
                      vectis_totp_qr_status_string(status));
  }
  luaL_getmetatable(lua, VECTIS_LUA_TOTP);
  lua_setmetatable(lua, -2);
  return 1;
}

static int vectis_lua_totp_secret(lua_State *lua) {
  vectis_lua_totp *totp;

  totp = vectis_lua_check_totp(lua, 1);
  lua_pushstring(lua, totp->value.secret);
  return 1;
}

static int vectis_lua_totp_generate(lua_State *lua) {
  vectis_lua_totp *totp;
  vectis_totp_qr_status status;
  char code[VECTIS_TOTP_CODE_LENGTH + 1u];

  totp = vectis_lua_check_totp(lua, 1);
  status =
      vectis_totp_generate(&totp->value, vectis_lua_totp_time(lua, 2), code);
  if (status != VECTIS_TOTP_QR_OK) {
    return luaL_error(lua, "TOTP generation failed: %s",
                      vectis_totp_qr_status_string(status));
  }
  lua_pushstring(lua, code);
  return 1;
}

static int vectis_lua_totp_validate(lua_State *lua) {
  vectis_lua_totp *totp;
  lua_Integer window;
  const char *code;

  totp = vectis_lua_check_totp(lua, 1);
  code = luaL_checkstring(lua, 2);
  window = lua_isnoneornil(lua, 4) ? 1 : luaL_checkinteger(lua, 4);
  if (window < 0 || window > 10) {
    return luaL_error(lua, "TOTP validation window must be between 0 and 10");
  }
  lua_pushboolean(lua, vectis_totp_validate(&totp->value, code,
                                            vectis_lua_totp_time(lua, 3),
                                            (unsigned int)window));
  return 1;
}

static int vectis_lua_qr_ansi_value(lua_State *lua, const vectis_qr *qr) {
  vectis_totp_qr_status status;
  char *rendered;
  size_t rendered_len;

  rendered = NULL;
  rendered_len = 0u;
  status = vectis_qr_render_ansi(qr, &rendered, &rendered_len);
  if (status != VECTIS_TOTP_QR_OK) {
    return luaL_error(lua, "terminal QR rendering failed: %s",
                      vectis_totp_qr_status_string(status));
  }
  lua_pushlstring(lua, rendered, rendered_len);
  vectis_totp_qr_free(rendered);
  return 1;
}

static int vectis_lua_totp_uri(lua_State *lua) {
  vectis_lua_totp *totp;
  vectis_totp_qr_status status;
  const char *label;
  const char *issuer;
  char *uri;

  totp = vectis_lua_check_totp(lua, 1);
  label = luaL_checkstring(lua, 2);
  issuer = luaL_checkstring(lua, 3);
  uri = NULL;
  status = vectis_totp_uri(&totp->value, label, issuer, &uri);
  if (status != VECTIS_TOTP_QR_OK) {
    return luaL_error(lua, "TOTP URI generation failed: %s",
                      vectis_totp_qr_status_string(status));
  }
  lua_pushstring(lua, uri);
  vectis_totp_qr_free(uri);
  return 1;
}

static int vectis_lua_totp_qr(lua_State *lua) {
  vectis_lua_totp *totp;
  vectis_totp_qr_status status;
  const char *label;
  const char *issuer;
  vectis_qr qr;

  totp = vectis_lua_check_totp(lua, 1);
  label = luaL_checkstring(lua, 2);
  issuer = luaL_checkstring(lua, 3);
  status = vectis_totp_enrollment_qr(&totp->value, label, issuer, &qr);
  if (status != VECTIS_TOTP_QR_OK) {
    return luaL_error(lua, "TOTP QR generation failed: %s",
                      vectis_totp_qr_status_string(status));
  }
  return vectis_lua_qr_ansi_value(lua, &qr);
}

static int vectis_lua_qr_new(lua_State *lua) {
  vectis_lua_qr *qr;
  vectis_totp_qr_status status;
  const char *text;
  size_t text_len;

  text = luaL_checkstring(lua, 1);
  text_len = strlen(text);
  qr = (vectis_lua_qr *)lua_newuserdata(lua, sizeof(*qr));
  status = vectis_qr_encode(&qr->value, (const unsigned char *)text, text_len);
  if (status != VECTIS_TOTP_QR_OK) {
    return luaL_error(lua, "QR text is invalid: %s",
                      vectis_totp_qr_status_string(status));
  }
  luaL_getmetatable(lua, VECTIS_LUA_QR);
  lua_setmetatable(lua, -2);
  return 1;
}

static int vectis_lua_qr_ansi(lua_State *lua) {
  vectis_lua_qr *qr;

  qr = vectis_lua_check_qr(lua, 1);
  return vectis_lua_qr_ansi_value(lua, &qr->value);
}

static int vectis_lua_qr_size(lua_State *lua) {
  vectis_lua_qr *qr;

  qr = vectis_lua_check_qr(lua, 1);
  lua_pushinteger(lua, (lua_Integer)vectis_qr_size(&qr->value));
  return 1;
}

static void vectis_lua_register_totp_qr(lua_State *lua) {
  if (luaL_newmetatable(lua, VECTIS_LUA_TOTP)) {
    lua_newtable(lua);
    lua_pushcfunction(lua, vectis_lua_totp_secret);
    lua_setfield(lua, -2, "secret");
    lua_pushcfunction(lua, vectis_lua_totp_generate);
    lua_setfield(lua, -2, "generate");
    lua_pushcfunction(lua, vectis_lua_totp_validate);
    lua_setfield(lua, -2, "validate");
    lua_pushcfunction(lua, vectis_lua_totp_uri);
    lua_setfield(lua, -2, "uri");
    lua_pushcfunction(lua, vectis_lua_totp_qr);
    lua_setfield(lua, -2, "qr");
    lua_setfield(lua, -2, "__index");
  }
  lua_pop(lua, 1);
  if (luaL_newmetatable(lua, VECTIS_LUA_QR)) {
    lua_newtable(lua);
    lua_pushcfunction(lua, vectis_lua_qr_ansi);
    lua_setfield(lua, -2, "ansi");
    lua_pushcfunction(lua, vectis_lua_qr_size);
    lua_setfield(lua, -2, "size");
    lua_setfield(lua, -2, "__index");
  }
  lua_pop(lua, 1);
}

static void vectis_lua_push_auth_table(lua_State *lua) {
  lua_newtable(lua);
  lua_pushinteger(lua, VECTIS_AUTH_MODE_BASIC);
  lua_setfield(lua, -2, "BASIC");
  lua_pushinteger(lua, VECTIS_AUTH_MODE_BEARER);
  lua_setfield(lua, -2, "BEARER");
  lua_pushcfunction(lua, vectis_lua_auth_store_init);
  lua_setfield(lua, -2, "store_init");
  lua_pushcfunction(lua, vectis_lua_auth_issue);
  lua_setfield(lua, -2, "issue");
  lua_pushcfunction(lua, vectis_lua_auth_verify);
  lua_setfield(lua, -2, "verify");
  lua_pushcfunction(lua, vectis_lua_auth_revoke);
  lua_setfield(lua, -2, "revoke");
  lua_pushcfunction(lua, vectis_lua_auth_user_add);
  lua_setfield(lua, -2, "user_add");
  lua_pushcfunction(lua, vectis_lua_auth_user_login);
  lua_setfield(lua, -2, "user_login");
  lua_pushcfunction(lua, vectis_lua_auth_webdav_key);
  lua_setfield(lua, -2, "webdav_key");
  lua_pushcfunction(lua, vectis_lua_auth_oidc_authorization);
  lua_setfield(lua, -2, "oidc_authorization");
  lua_pushcfunction(lua, vectis_lua_auth_oidc_exchange_callback);
  lua_setfield(lua, -2, "oidc_exchange_callback");
  lua_pushcfunction(lua, vectis_lua_auth_oauth2_client_credentials);
  lua_setfield(lua, -2, "oauth2_client_credentials");
  lua_pushcfunction(lua, vectis_lua_auth_oauth2_flow_ensure);
  lua_setfield(lua, -2, "oauth2_flow_ensure");
  lua_pushcfunction(lua, vectis_lua_auth_oauth2_stored_flow_ensure);
  lua_setfield(lua, -2, "oauth2_stored_flow_ensure");
  lua_pushcfunction(lua, vectis_lua_auth_oauth2_flow_upsert);
  lua_setfield(lua, -2, "oauth2_flow_upsert");
  lua_pushcfunction(lua, vectis_lua_auth_oauth2_flow_load);
  lua_setfield(lua, -2, "oauth2_flow_load");
  lua_pushcfunction(lua, vectis_lua_auth_oauth2_webdav_key);
  lua_setfield(lua, -2, "oauth2_webdav_key");
  lua_pushcfunction(lua, vectis_lua_auth_provider_native);
  lua_setfield(lua, -2, "provider_native");
  lua_pushcfunction(lua, vectis_lua_auth_provider_callback);
  lua_setfield(lua, -2, "provider_callback");
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_lua_totp_new);
  lua_setfield(lua, -2, "new");
  lua_setfield(lua, -2, "totp");
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_lua_qr_new);
  lua_setfield(lua, -2, "new");
  lua_setfield(lua, -2, "qr");
}

static int luaopen_vectis(lua_State *lua) {
  vectis_lua_register_totp_qr(lua);
  lua_newtable(lua);
  lua_pushliteral(lua, VECTIS_VERSION);
  lua_setfield(lua, -2, "version");
  lua_pushinteger(lua, VECTIS_OK);
  lua_setfield(lua, -2, "OK");
  lua_pushinteger(lua, VECTIS_ERR_INVALID);
  lua_setfield(lua, -2, "ERR_INVALID");
  lua_pushinteger(lua, VECTIS_ERR_TIMEOUT);
  lua_setfield(lua, -2, "ERR_TIMEOUT");
  lua_pushcfunction(lua, vectis_lua_status_string);
  lua_setfield(lua, -2, "status_string");
  lua_pushcfunction(lua, vectis_lua_has_embedded_lockd_bundle);
  lua_setfield(lua, -2, "has_embedded_lockd_bundle");
  lua_pushcfunction(lua, vectis_lua_embedded_lockd_bundle_size);
  lua_setfield(lua, -2, "embedded_lockd_bundle_size");
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_lua_embedded_has_assets);
  lua_setfield(lua, -2, "has_assets");
  lua_pushcfunction(lua, vectis_lua_embedded_read);
  lua_setfield(lua, -2, "read");
  lua_pushcfunction(lua, vectis_lua_embedded_stat);
  lua_setfield(lua, -2, "stat");
  lua_pushcfunction(lua, vectis_lua_embedded_chunks);
  lua_setfield(lua, -2, "chunks");
  lua_pushcfunction(lua, vectis_lua_embedded_list);
  lua_setfield(lua, -2, "list");
  lua_pushcfunction(lua, vectis_lua_embedded_extract);
  lua_setfield(lua, -2, "extract");
  lua_setfield(lua, -2, "embedded");
  vectis_lua_push_auth_table(lua);
  lua_setfield(lua, -2, "auth");
  return 1;
}

static int vectis_luaopen_vectis(void *lua_state) {
  return luaopen_vectis((lua_State *)lua_state);
}

static int vectis_luaopen_lockdc_core(void *lua_state) {
  return luaopen_lockdc_core((lua_State *)lua_state);
}

static int vectis_luaopen_lonejson_core(void *lua_state) {
  return luaopen_lonejson_core((lua_State *)lua_state);
}

static int luaopen_curl_core(lua_State *lua) {
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_lua_curl_version);
  lua_setfield(lua, -2, "version");
  lua_pushcfunction(lua, vectis_lua_curl_perform);
  lua_setfield(lua, -2, "perform");
  return 1;
}

static int vectis_luaopen_curl_core(void *lua_state) {
  return luaopen_curl_core((lua_State *)lua_state);
}

static int vectis_luaopen_cai(void *lua_state) {
  return luaopen_cai((lua_State *)lua_state);
}

static int vectis_luaopen_libmdf_core(void *lua_state) {
  return luaopen_libmdf_core((lua_State *)lua_state);
}

static int vectis_luaopen_pslog_core(void *lua_state) {
  return luaopen_pslog_core((lua_State *)lua_state);
}

static int vectis_luaopen_softline(void *lua_state) {
  return luaopen_softline((lua_State *)lua_state);
}

static void vectis_lua_trace_hook(lua_State *lua, lua_Debug *debug) {
  const char *source;

  if (lua == NULL || debug == NULL) {
    return;
  }
  if (lua_getinfo(lua, "Sl", debug) == 0 || debug->currentline <= 0) {
    return;
  }
  source = debug->short_src[0] != '\0' ? debug->short_src : "?";
  fprintf(stderr, "+ %s:%d\n", source, debug->currentline);
}

static int vectis_luaopen_trace(void *lua_state) {
  lua_State *lua;

  lua = (lua_State *)lua_state;
  if (lua == NULL) {
    return 0;
  }
  lua_sethook(lua, vectis_lua_trace_hook, LUA_MASKLINE, 0);
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_report_status(cpkt_lua_runtime *runtime,
                                    cpkt_lua_runtime_status status) {
  const char *message;

  if (status == CPKT_LUA_RUNTIME_OK) {
    return 0;
  }
  message = runtime != NULL ? cpkt_lua_runtime_error(runtime) : NULL;
  if (message == NULL || message[0] == '\0') {
    message = cpkt_lua_runtime_status_string(status);
  }
  fprintf(stderr, "vectis: %s\n", message);
  if (status == CPKT_LUA_RUNTIME_ERR_ALLOC) {
    return 70;
  }
  return 1;
}

static cpkt_lua_runtime_status
vectis_lua_register_modules(cpkt_lua_runtime *runtime) {
  cpkt_lua_runtime_status status;

  status = cpkt_lua_runtime_register_c_module(runtime, "vectis",
                                              vectis_luaopen_vectis);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_c_module(runtime, "lockdc.core",
                                              vectis_luaopen_lockdc_core);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "lockdc", vectis_lockdc_lua_init, sizeof(vectis_lockdc_lua_init),
      "lockdc.init");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_c_module(runtime, "lonejson.core",
                                              vectis_luaopen_lonejson_core);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "lonejson", (const unsigned char *)vectis_lonejson_lua_init,
      sizeof(vectis_lonejson_lua_init) - 1u, "lonejson.init");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_c_module(runtime, "curl.core",
                                              vectis_luaopen_curl_core);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "curl", vectis_curl_lua_init, sizeof(vectis_curl_lua_init),
      "curl.init");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status =
      cpkt_lua_runtime_register_c_module(runtime, "cai", vectis_luaopen_cai);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_c_module(runtime, "pslog.core",
                                              vectis_luaopen_pslog_core);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "pslog", vectis_pslog_lua_init, sizeof(vectis_pslog_lua_init),
      "pslog.init");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_c_module(runtime, "libmdf.core",
                                              vectis_luaopen_libmdf_core);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "libmdf", vectis_libmdf_lua_init, sizeof(vectis_libmdf_lua_init),
      "libmdf.init");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  return cpkt_lua_runtime_register_c_module(runtime, "softline",
                                            vectis_luaopen_softline);
}

static int vectis_lua_prepare_runtime(cpkt_lua_runtime **out,
                                      vectis_lua_runtime_context *context,
                                      int trace_enabled) {
  cpkt_lua_runtime *runtime;
  cpkt_lua_runtime_status status;
  int rc;

  runtime = NULL;
  status = cpkt_lua_runtime_new(&runtime);
  if (status == CPKT_LUA_RUNTIME_OK) {
    cpkt_lua_runtime_set_context(runtime, context);
    status = cpkt_lua_runtime_openlibs(runtime);
  }
  if (status == CPKT_LUA_RUNTIME_OK) {
    status = vectis_lua_register_modules(runtime);
  }
  if (status == CPKT_LUA_RUNTIME_OK && trace_enabled) {
    status = cpkt_lua_runtime_register_c_module(runtime, "__vectis_trace",
                                                vectis_luaopen_trace);
  }
  if (status == CPKT_LUA_RUNTIME_OK && trace_enabled) {
    status = cpkt_lua_runtime_require(runtime, "__vectis_trace");
  }
  if (status != CPKT_LUA_RUNTIME_OK) {
    rc = vectis_lua_report_status(runtime, status);
    cpkt_lua_runtime_free(runtime);
    return rc;
  }
  *out = runtime;
  return 0;
}

static int vectis_lua_run_buffer(
    const char *script_name, const unsigned char *script, size_t script_size,
    const unsigned char *lockd_bundle, size_t lockd_bundle_size,
    const unsigned char *asset_payload, size_t asset_payload_size,
    const unsigned char *manifest, size_t manifest_size, int argc, char **argv,
    int trace_enabled) {
  cpkt_lua_runtime *runtime;
  vectis_lua_runtime_context context;
  vectis_embedded_fs_config fs_config;
  vectis_error error;
  const unsigned char *load_script;
  size_t load_size;
  cpkt_lua_runtime_status status;
  int rc;

  memset(&context, 0, sizeof(context));
  context.embedded_lockd_bundle = lockd_bundle;
  context.embedded_lockd_bundle_size = lockd_bundle_size;
  context.embedded_asset_payload = asset_payload;
  context.embedded_asset_payload_size = asset_payload_size;
  context.embedded_manifest = manifest;
  context.embedded_manifest_size = manifest_size;
  if (asset_payload != NULL && asset_payload_size > 0u && manifest != NULL &&
      manifest_size > 0u) {
    vectis_embedded_fs_config_init(&fs_config);
    fs_config.payload = asset_payload;
    fs_config.payload_size = asset_payload_size;
    fs_config.manifest_json = manifest;
    fs_config.manifest_json_size = manifest_size;
    vectis_error_clear(&error);
    if (vectis_embedded_fs_from_pack(&fs_config, &context.embedded_fs,
                                     &error) != VECTIS_OK) {
      fprintf(stderr, "vectis: %s\n",
              error.message[0] != '\0' ? error.message
                                       : "embedded asset manifest is invalid");
      return 1;
    }
  }
  rc = vectis_lua_prepare_runtime(&runtime, &context, trace_enabled);
  if (rc != 0) {
    vectis_embedded_fs_close(context.embedded_fs);
    return rc;
  }

  load_script = script;
  load_size = script_size;
  if (load_size >= 2u && load_script[0] == '#' && load_script[1] == '!') {
    while (load_size > 0u && *load_script != '\n') {
      load_script++;
      load_size--;
    }
    if (load_size > 0u) {
      load_script++;
      load_size--;
    }
  }
  status = cpkt_lua_runtime_run_buffer(
      runtime, load_script, load_size, script_name, argc > 0 ? argc - 1 : 0,
      argc > 0 ? (const char *const *)(argv + 1) : NULL, 0);
  rc = vectis_lua_report_status(runtime, status);
  cpkt_lua_runtime_free(runtime);
  vectis_embedded_fs_close(context.embedded_fs);
  return rc;
}

static int vectis_lua_run_script(int argc, char **argv, int script_index,
                                 int trace_enabled) {
  cpkt_lua_runtime *runtime;
  vectis_lua_runtime_context context;
  cpkt_lua_runtime_status status;
  int rc;

  memset(&context, 0, sizeof(context));
  rc = vectis_lua_prepare_runtime(&runtime, &context, trace_enabled);
  if (rc != 0) {
    return rc;
  }

  status = cpkt_lua_runtime_run_file(
      runtime, argv[script_index], argc - script_index - 1,
      (const char *const *)(argv + script_index + 1), 0);
  rc = vectis_lua_report_status(runtime, status);
  cpkt_lua_runtime_free(runtime);
  return rc;
}

static int vectis_lua_run_embedded(int argc, char **argv) {
  unsigned char *self;
  unsigned char *script;
  unsigned char *bundle;
  unsigned char *asset_payload;
  unsigned char *manifest;
  unsigned char footer[VECTIS_PACK_FOOTER_SIZE];
  unsigned char actual_sha[SHA256_DIGEST_LENGTH];
  size_t self_size;
  size_t footer_offset;
  size_t script_offset;
  size_t script_size;
  size_t bundle_offset;
  size_t bundle_size;
  size_t asset_offset;
  size_t asset_size;
  size_t manifest_offset;
  size_t manifest_size;
  char self_path[4096];
  int rc;

  if (vectis_self_path(argv[0], self_path, sizeof(self_path)) != 0) {
    return -1;
  }
  self = NULL;
  if (vectis_read_all(self_path, &self, &self_size) != 0) {
    return -1;
  }
  if (self_size < VECTIS_PACK_FOOTER_SIZE) {
    free(self);
    return -1;
  }
  memcpy(footer, self + self_size - VECTIS_PACK_FOOTER_SIZE,
         VECTIS_PACK_FOOTER_SIZE);
  if (!vectis_pack_footer_valid(footer)) {
    free(self);
    return -1;
  }
  footer_offset = self_size - VECTIS_PACK_FOOTER_SIZE;
  script_offset = (size_t)vectis_pack_read_u64(footer + 16u);
  script_size = (size_t)vectis_pack_read_u64(footer + 24u);
  bundle_offset = (size_t)vectis_pack_read_u64(footer + 32u);
  bundle_size = (size_t)vectis_pack_read_u64(footer + 40u);
  asset_offset = (size_t)vectis_pack_read_u64(footer + 48u);
  asset_size = (size_t)vectis_pack_read_u64(footer + 56u);
  manifest_offset = (size_t)vectis_pack_read_u64(footer + 64u);
  manifest_size = (size_t)vectis_pack_read_u64(footer + 72u);
  if (script_size == 0u || script_offset > footer_offset ||
      script_size > footer_offset - script_offset ||
      bundle_offset > footer_offset ||
      bundle_size > footer_offset - bundle_offset ||
      asset_offset > footer_offset ||
      asset_size > footer_offset - asset_offset ||
      manifest_offset > footer_offset ||
      manifest_size > footer_offset - manifest_offset ||
      bundle_offset != script_offset + script_size ||
      asset_offset != bundle_offset + bundle_size ||
      manifest_offset != asset_offset + asset_size ||
      manifest_offset + manifest_size != footer_offset) {
    free(self);
    fputs("vectis: embedded payload is invalid\n", stderr);
    return 1;
  }
  script = self + script_offset;
  bundle = self + bundle_offset;
  asset_payload = self + asset_offset;
  manifest = self + manifest_offset;
  SHA256(script, script_size, actual_sha);
  if (memcmp(actual_sha, footer + 80u, SHA256_DIGEST_LENGTH) != 0) {
    free(self);
    fputs("vectis: embedded Lua script hash mismatch\n", stderr);
    return 1;
  }
  if (bundle_size > 0u) {
    SHA256(bundle, bundle_size, actual_sha);
    if (memcmp(actual_sha, footer + 112u, SHA256_DIGEST_LENGTH) != 0) {
      free(self);
      fputs("vectis: embedded lockd bundle hash mismatch\n", stderr);
      return 1;
    }
  }
  if (asset_size > 0u) {
    SHA256(asset_payload, asset_size, actual_sha);
    if (memcmp(actual_sha, footer + 144u, SHA256_DIGEST_LENGTH) != 0) {
      free(self);
      fputs("vectis: embedded asset payload hash mismatch\n", stderr);
      return 1;
    }
  }
  if (manifest_size > 0u) {
    SHA256(manifest, manifest_size, actual_sha);
    if (memcmp(actual_sha, footer + 176u, SHA256_DIGEST_LENGTH) != 0) {
      free(self);
      fputs("vectis: embedded asset manifest hash mismatch\n", stderr);
      return 1;
    }
  }
  rc = vectis_lua_run_buffer(
      argv[0], script, script_size, bundle_size > 0u ? bundle : NULL,
      bundle_size, asset_size > 0u ? asset_payload : NULL, asset_size,
      manifest_size > 0u ? manifest : NULL, manifest_size, argc, argv, 0);
  free(self);
  return rc;
}

int vectis_cli_main(int argc, char **argv) {
  int rc;

  rc = vectis_lua_run_embedded(argc, argv);
  if (rc >= 0) {
    return rc;
  }

  if (argc > 1 && strcmp(argv[1], "--help") == 0) {
    vectis_cli_usage(stdout);
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "--version") == 0) {
    puts("vectis " VECTIS_VERSION);
    return 0;
  }
  if (argc > 1 &&
      (strcmp(argv[1], "-a") == 0 || strcmp(argv[1], "--action") == 0)) {
    return vectis_action_command(argc, argv, 2);
  }
  if (argc > 1 && strcmp(argv[1], "-x") == 0) {
    if (argc > 2) {
      return vectis_lua_run_script(argc, argv, 2, 1);
    }
    vectis_cli_usage(stderr);
    return 64;
  }

  if (argc > 1) {
    return vectis_lua_run_script(argc, argv, 1, 0);
  }
  vectis_cli_usage(stderr);
  return 64;
}
