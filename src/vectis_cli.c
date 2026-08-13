#include "vectis_cli.h"

#include <arpa/inet.h>
#include <cpkt/lua_runtime.h>
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <lauxlib.h>
#include <limits.h>
#ifndef LONEJSON_WITH_CURL
#define LONEJSON_WITH_CURL 1
#endif
#include <lc/lc.h>
#include <lonejson.h>
#include <lonejson_lua.h>
#include <lua.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <vectis/auth.h>
#include <vectis/embedded_fs.h>
#include <vectis/totp_qr.h>
#include <vectis/vectis.h>
#include <vectis/vectis_version.h>
#include <vectis/webdav.h>

#include "vectis_curl_lua_init.h"
#include "vectis_dsv_lua_init.h"
#include "vectis_http_lua_init.h"
#include "vectis_libmdf_lua_init.h"
#include "vectis_liblql_lua_init.h"
#include "vectis_lockd_lua_init.h"
#include "vectis_lockdc_lua_init.h"
#include "vectis_pslog_lua_init.h"
#include "vectis_webdav_lua_init.h"
#include "vectis_xml_lua_init.h"

#define VECTIS_PACK_FOOTER_SIZE 256u
#define VECTIS_PACK_MAGIC "VECTIS_PACK"
#define VECTIS_PACK_MAGIC_SIZE 11u
#define VECTIS_LUA_CURL_RESPONSE_BODY_LIMIT (8u * 1024u * 1024u)
#define VECTIS_LUA_CURL_RESPONSE_HEADER_LIMIT (64u * 1024u)
#define VECTIS_PACK_MAX_DIR_STACK 1024u
#define VECTIS_LUA_SERVER "vectis.server"

typedef struct vectis_lua_runtime_context {
  const unsigned char *embedded_lockd_bundle;
  size_t embedded_lockd_bundle_size;
  const unsigned char *embedded_asset_payload;
  size_t embedded_asset_payload_size;
  const unsigned char *embedded_manifest;
  size_t embedded_manifest_size;
  vectis_embedded_fs *embedded_fs;
} vectis_lua_runtime_context;

typedef struct vectis_lua_memory_source {
  const unsigned char *data;
  size_t size;
  size_t offset;
  int closed;
} vectis_lua_memory_source;

typedef struct vectis_lua_dsv_rows_context {
  lua_State *lua;
  int schema_index;
  int record_index;
  int output_index;
  int callback_ref;
  int next_index;
} vectis_lua_dsv_rows_context;

static void vectis_cli_error_set(vectis_error *error, vectis_status status,
                                 const char *message);

typedef struct vectis_lua_server_native_auth {
  lua_State *lua;
  char *credentials_path;
  char *purpose;
  char *realm;
  char *callback_location;
  char *callback_content_type;
  char *callback_body;
  size_t callback_body_size;
  int callback_ref;
  unsigned allowed_auth_modes;
  vectis_auth_native_provider_config native_config;
  vectis_auth_provider provider;
  vectis_webdav_auth_provider_config webdav_config;
  struct vectis_lua_server_native_auth *next;
} vectis_lua_server_native_auth;

typedef struct vectis_lua_server_auth_json_route {
  char *body;
  char *content_type;
  char *purpose;
  int status_code;
  vectis_lua_server_native_auth *auth;
  struct vectis_lua_server_auth_json_route *next;
} vectis_lua_server_auth_json_route;

typedef struct vectis_lua_server_json_route {
  char *body;
  char *content_type;
  char *cache_control;
  int status_code;
  struct vectis_lua_server_json_route *next;
} vectis_lua_server_json_route;

typedef struct vectis_lua_consumer_registration {
  vectis_consumer_service *service;
  vectis_consumer_service_receiver_config config;
  vectis_webdav_marker_receiver_config webdav_marker;
  char *name;
  char *queue;
  char *owner;
  char *cache_dir;
  char *site_id;
  char *processing_path;
  char *done_path;
  char *processing_body;
  char *done_body;
  long processing_delay_seconds;
  int started;
  struct vectis_lua_consumer_registration *next;
} vectis_lua_consumer_registration;

typedef struct vectis_lua_server {
  vectis_app *app;
  vectis_lua_server_json_route *json_routes;
  vectis_lua_server_native_auth *native_auths;
  vectis_lua_server_auth_json_route *auth_json_routes;
  vectis_lua_consumer_registration *consumer_services;
} vectis_lua_server;

typedef enum vectis_pack_asset_kind {
  VECTIS_PACK_ASSET_FILE = 1,
  VECTIS_PACK_ASSET_DIRECTORY = 2
} vectis_pack_asset_kind;

typedef struct vectis_pack_asset {
  vectis_pack_asset_kind kind;
  char *source_path;
  char *logical_path;
  char *content_type;
  unsigned char *data;
  size_t size;
  size_t offset;
  unsigned mode;
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
  int follow_symlinks;
} vectis_pack_asset_manifest_state;

typedef struct vectis_pack_dir_visit {
  dev_t dev;
  ino_t ino;
} vectis_pack_dir_visit;

typedef struct vectis_pack_dir_stack {
  vectis_pack_dir_visit items[VECTIS_PACK_MAX_DIR_STACK];
  size_t count;
} vectis_pack_dir_stack;

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

typedef struct vectis_lua_curl_file_upload {
  FILE *file;
  curl_off_t size;
} vectis_lua_curl_file_upload;

typedef struct vectis_lua_curl_retry_config {
  unsigned max_attempts;
  long initial_delay_ms;
  long max_delay_ms;
  vectis_http_retry_conditions conditions;
} vectis_lua_curl_retry_config;

typedef struct vectis_lua_embedded_chunks_state {
  lc_source *source;
  unsigned char *buffer;
  size_t chunk_size;
} vectis_lua_embedded_chunks_state;

typedef struct vectis_lua_auth_oauth2_transport {
  lua_State *lua;
  int callback_ref;
} vectis_lua_auth_oauth2_transport;

#define VECTIS_LUA_TOTP "vectis.totp"
#define VECTIS_LUA_QR "vectis.qr"
#define VECTIS_LUA_EMBEDDED_CHUNKS "vectis.embedded.chunks"

extern int luaopen_lonejson_core(lua_State *lua);
extern int luaopen_lockdc_core(lua_State *lua);
extern int luaopen_cai(lua_State *lua);
extern int luaopen_lql_core(lua_State *lua);
extern int luaopen_libmdf_core(lua_State *lua);
extern int luaopen_pslog_core(lua_State *lua);
extern int luaopen_softline(lua_State *lua);
extern int luaopen_opcua(lua_State *lua);

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
        "[--content-type-map types.json] [--extract-mode mode] "
        "[--follow-symlinks] [--codesign identity | --ad-hoc-codesign] "
        "[--hardened-runtime] [--timestamp] [--entitlements plist]\n"
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

static int vectis_pack_stat_source(const char *source_path, int follow_symlinks,
                                   struct stat *out) {
  struct stat link_st;

  if (source_path == NULL || out == NULL) {
    return -1;
  }
  if (lstat(source_path, &link_st) != 0) {
    fprintf(stderr, "vectis: failed to stat embedded asset: %s\n", source_path);
    return -1;
  }
  if (S_ISLNK(link_st.st_mode)) {
    if (!follow_symlinks) {
      fprintf(stderr,
              "vectis: embedded asset symlink requires --follow-symlinks: %s\n",
              source_path);
      return -1;
    }
    if (stat(source_path, out) != 0) {
      fprintf(stderr, "vectis: failed to follow embedded asset symlink: %s\n",
              source_path);
      return -1;
    }
    return 0;
  }
  *out = link_st;
  return 0;
}

static int vectis_pack_dir_stack_push(vectis_pack_dir_stack *stack,
                                      const struct stat *st,
                                      const char *source_path) {
  size_t i;

  for (i = 0u; i < stack->count; ++i) {
    if (stack->items[i].dev == st->st_dev &&
        stack->items[i].ino == st->st_ino) {
      fprintf(stderr,
              "vectis: embedded asset symlink cycle detected at directory: "
              "%s\n",
              source_path != NULL ? source_path : "(null)");
      return -1;
    }
  }
  if (stack->count >= VECTIS_PACK_MAX_DIR_STACK) {
    fprintf(stderr,
            "vectis: embedded asset directory nesting is too deep at: %s\n",
            source_path != NULL ? source_path : "(null)");
    return -1;
  }
  stack->items[stack->count].dev = st->st_dev;
  stack->items[stack->count].ino = st->st_ino;
  stack->count++;
  return 0;
}

static void vectis_pack_dir_stack_pop(vectis_pack_dir_stack *stack) {
  if (stack != NULL && stack->count > 0u) {
    stack->count--;
  }
}

static int vectis_pack_asset_add(vectis_pack_asset_list *list,
                                 const char *source_path,
                                 const char *logical_path,
                                 const char *content_type_override,
                                 const vectis_pack_content_type_map *map,
                                 int follow_symlinks) {
  vectis_pack_asset *asset;
  unsigned char *data;
  const char *content_type;
  struct stat st;
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
  if (vectis_pack_stat_source(source_path, follow_symlinks, &st) != 0) {
    return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    fprintf(stderr, "vectis: unsupported embedded asset type: %s\n",
            source_path != NULL ? source_path : "(null)");
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
  asset->kind = VECTIS_PACK_ASSET_FILE;
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
  asset->mode =
      (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0 ? 0555u : 0444u;
  SHA256(data, size, asset->sha);
  list->count++;
  return 0;
}

static int vectis_pack_asset_add_directory(vectis_pack_asset_list *list,
                                           const char *source_path,
                                           const char *logical_path) {
  vectis_pack_asset *asset;

  if (!vectis_pack_logical_path_valid(logical_path)) {
    fprintf(stderr, "vectis: invalid embedded asset directory path: %s\n",
            logical_path != NULL ? logical_path : "(null)");
    return -1;
  }
  if (vectis_pack_asset_list_reserve(list, list->count + 1u) != 0) {
    return -1;
  }
  asset = list->items + list->count;
  memset(asset, 0, sizeof(*asset));
  asset->kind = VECTIS_PACK_ASSET_DIRECTORY;
  asset->source_path = vectis_cli_strdup(source_path);
  asset->logical_path = vectis_cli_strdup(logical_path);
  if (asset->source_path == NULL || asset->logical_path == NULL) {
    free(asset->source_path);
    free(asset->logical_path);
    memset(asset, 0, sizeof(*asset));
    return -1;
  }
  asset->mode = 0555u;
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
                            asset->content_type, state->content_types,
                            state->follow_symlinks) != 0) {
    return LONEJSON_STATUS_INVALID_JSON;
  }
  return LONEJSON_STATUS_OK;
}

static int vectis_pack_read_asset_manifest(
    vectis_pack_asset_list *assets, const char *path,
    const vectis_pack_content_type_map *content_types, int follow_symlinks) {
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
  state.follow_symlinks = follow_symlinks;
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
                                   const vectis_pack_content_type_map *map,
                                   int follow_symlinks,
                                   vectis_pack_dir_stack *dir_stack) {
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  char *source_child;
  char *logical_child;
  int rc;

  if (vectis_pack_stat_source(source_dir, follow_symlinks, &st) != 0) {
    return -1;
  }
  if (!S_ISDIR(st.st_mode)) {
    fprintf(stderr, "vectis: embedded asset directory is not a directory: %s\n",
            source_dir != NULL ? source_dir : "(null)");
    return -1;
  }
  if (vectis_pack_dir_stack_push(dir_stack, &st, source_dir) != 0) {
    return -1;
  }
  if (strcmp(logical_root, "/") != 0 &&
      vectis_pack_asset_add_directory(list, source_dir, logical_root) != 0) {
    vectis_pack_dir_stack_pop(dir_stack);
    return -1;
  }
  dir = opendir(source_dir);
  if (dir == NULL) {
    fprintf(stderr, "vectis: failed to open embedded asset directory: %s\n",
            source_dir);
    vectis_pack_dir_stack_pop(dir_stack);
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
    if (vectis_pack_stat_source(source_child, follow_symlinks, &st) != 0) {
      free(source_child);
      free(logical_child);
      rc = -1;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      rc = vectis_pack_collect_dir(list, source_child, logical_child, map,
                                   follow_symlinks, dir_stack);
    } else if (S_ISREG(st.st_mode)) {
      rc = vectis_pack_asset_add(list, source_child, logical_child, NULL, map,
                                 follow_symlinks);
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
  vectis_pack_dir_stack_pop(dir_stack);
  return rc;
}

static int vectis_pack_hash_asset_tree(
    vectis_pack_asset_list *assets, unsigned char out[SHA256_DIGEST_LENGTH]) {
  EVP_MD_CTX *ctx;
  unsigned int digest_size;
  size_t i;
  char size_buf[32];
  char mode_buf[16];
  const char *kind;
  char nul;

  ctx = EVP_MD_CTX_new();
  if (ctx == NULL) {
    return -1;
  }
  if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
    EVP_MD_CTX_free(ctx);
    return -1;
  }
  nul = '\0';
  for (i = 0u; i < assets->count; ++i) {
    (void)snprintf(size_buf, sizeof(size_buf), "%lu",
                   (unsigned long)assets->items[i].size);
    (void)snprintf(mode_buf, sizeof(mode_buf), "%u", assets->items[i].mode);
    kind = assets->items[i].kind == VECTIS_PACK_ASSET_DIRECTORY ? "directory"
                                                                : "file";
    if (EVP_DigestUpdate(ctx, assets->items[i].logical_path,
                         strlen(assets->items[i].logical_path)) != 1 ||
        EVP_DigestUpdate(ctx, &nul, 1u) != 1 ||
        EVP_DigestUpdate(ctx, kind, strlen(kind)) != 1 ||
        EVP_DigestUpdate(ctx, &nul, 1u) != 1 ||
        (assets->items[i].kind == VECTIS_PACK_ASSET_FILE &&
         EVP_DigestUpdate(ctx, assets->items[i].sha, SHA256_DIGEST_LENGTH) !=
             1) ||
        EVP_DigestUpdate(ctx, &nul, 1u) != 1 ||
        EVP_DigestUpdate(ctx, size_buf, strlen(size_buf)) != 1 ||
        EVP_DigestUpdate(ctx, &nul, 1u) != 1 ||
        EVP_DigestUpdate(ctx, mode_buf, strlen(mode_buf)) != 1 ||
        EVP_DigestUpdate(ctx, &nul, 1u) != 1 ||
        (assets->items[i].content_type != NULL &&
         EVP_DigestUpdate(ctx, assets->items[i].content_type,
                          strlen(assets->items[i].content_type)) != 1) ||
        EVP_DigestUpdate(ctx, &nul, 1u) != 1) {
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

static int vectis_pack_build_manifest(vectis_pack_asset_list *assets,
                                      const char *extract_mode,
                                      unsigned char **out, size_t *out_size) {
  lonejson *runtime;
  lonejson_writer writer;
  lonejson_owned_buffer buffer;
  lonejson_error error;
  lonejson_status status;
  unsigned char *copy;
  size_t i;
  char sha_hex[SHA256_DIGEST_LENGTH * 2u + 1u];
  char etag[SHA256_DIGEST_LENGTH * 2u + 3u];
  const char *kind;
  unsigned char tree_sha[SHA256_DIGEST_LENGTH];
  char tree_sha_hex[SHA256_DIGEST_LENGTH * 2u + 1u];

  *out = NULL;
  *out_size = 0u;
  if (assets->count == 0u) {
    return 0;
  }
  if (vectis_pack_hash_asset_tree(assets, tree_sha) != 0) {
    return -1;
  }
  vectis_sha256_hex(tree_sha, tree_sha_hex);
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
    status = lonejson_writer_key(&writer, "tree_sha256", 11u, &error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_string(&writer, tree_sha_hex,
                                    strlen(tree_sha_hex), &error);
  }
  if (status == LONEJSON_STATUS_OK && extract_mode != NULL) {
    status = lonejson_writer_key(&writer, "extract_mode", 12u, &error);
  }
  if (status == LONEJSON_STATUS_OK && extract_mode != NULL) {
    status = lonejson_writer_string(&writer, extract_mode, strlen(extract_mode),
                                    &error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_key(&writer, "assets", 6u, &error);
  }
  if (status == LONEJSON_STATUS_OK) {
    status = lonejson_writer_begin_array(&writer, &error);
  }
  for (i = 0u; i < assets->count; ++i) {
    kind = assets->items[i].kind == VECTIS_PACK_ASSET_DIRECTORY ? "directory"
                                                                : "file";
    if (assets->items[i].kind == VECTIS_PACK_ASSET_FILE) {
      vectis_sha256_hex(assets->items[i].sha, sha_hex);
      (void)snprintf(etag, sizeof(etag), "\"%s\"", sha_hex);
    }
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
      status = lonejson_writer_key(&writer, "kind", 4u, &error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_string(&writer, kind, strlen(kind), &error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_key(&writer, "mode", 4u, &error);
    }
    if (status == LONEJSON_STATUS_OK) {
      status = lonejson_writer_u64(&writer,
                                   (lonejson_uint64)assets->items[i].mode,
                                   &error);
    }
    if (status == LONEJSON_STATUS_OK &&
        assets->items[i].kind == VECTIS_PACK_ASSET_FILE) {
      status = lonejson_writer_key(&writer, "offset", 6u, &error);
    }
    if (status == LONEJSON_STATUS_OK &&
        assets->items[i].kind == VECTIS_PACK_ASSET_FILE) {
      status = lonejson_writer_u64(
          &writer, (lonejson_uint64)assets->items[i].offset, &error);
    }
    if (status == LONEJSON_STATUS_OK &&
        assets->items[i].kind == VECTIS_PACK_ASSET_FILE) {
      status = lonejson_writer_key(&writer, "size", 4u, &error);
    }
    if (status == LONEJSON_STATUS_OK &&
        assets->items[i].kind == VECTIS_PACK_ASSET_FILE) {
      status = lonejson_writer_u64(
          &writer, (lonejson_uint64)assets->items[i].size, &error);
    }
    if (status == LONEJSON_STATUS_OK &&
        assets->items[i].kind == VECTIS_PACK_ASSET_FILE) {
      status = lonejson_writer_key(&writer, "sha256", 6u, &error);
    }
    if (status == LONEJSON_STATUS_OK &&
        assets->items[i].kind == VECTIS_PACK_ASSET_FILE) {
      status =
          lonejson_writer_string(&writer, sha_hex, strlen(sha_hex), &error);
    }
    if (status == LONEJSON_STATUS_OK &&
        assets->items[i].kind == VECTIS_PACK_ASSET_FILE) {
      status = lonejson_writer_key(&writer, "etag", 4u, &error);
    }
    if (status == LONEJSON_STATUS_OK &&
        assets->items[i].kind == VECTIS_PACK_ASSET_FILE) {
      status = lonejson_writer_string(&writer, etag, strlen(etag), &error);
    }
    if (status == LONEJSON_STATUS_OK &&
        assets->items[i].kind == VECTIS_PACK_ASSET_FILE &&
        assets->items[i].content_type != NULL) {
      status = lonejson_writer_key(&writer, "content_type", 12u, &error);
    }
    if (status == LONEJSON_STATUS_OK &&
        assets->items[i].kind == VECTIS_PACK_ASSET_FILE &&
        assets->items[i].content_type != NULL) {
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
    if (assets->items[i].kind != VECTIS_PACK_ASSET_FILE) {
      continue;
    }
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

static void vectis_cli_error_set(vectis_error *error, vectis_status status,
                                 const char *message) {
  if (error == NULL) {
    return;
  }
  vectis_error_clear(error);
  error->code = status;
  if (message != NULL) {
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
  }
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
  const char *extract_mode;
  const char *codesign_identity;
  const char *entitlements_path;
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
  vectis_pack_dir_stack dir_stack;
  vectis_embedded_fs_extract_policy extract_policy;
  char self_path[4096];
  FILE *out;
  int i;
  int follow_symlinks;
  int ad_hoc_codesign;
  int hardened_runtime;
  int timestamp;

  script_path = NULL;
  output_path = NULL;
  bundle_path = NULL;
  extract_mode = NULL;
  codesign_identity = NULL;
  entitlements_path = NULL;
  follow_symlinks = 0;
  ad_hoc_codesign = 0;
  hardened_runtime = 0;
  timestamp = 0;
  memset(&assets, 0, sizeof(assets));
  memset(&content_types, 0, sizeof(content_types));
  memset(&dir_stack, 0, sizeof(dir_stack));
  for (i = index; i < argc; ++i) {
    if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
      script_path = argv[++i];
    } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      output_path = argv[++i];
    } else if (strcmp(argv[i], "--lockd-bundle") == 0 && i + 1 < argc) {
      bundle_path = argv[++i];
    } else if (strcmp(argv[i], "--extract-mode") == 0 && i + 1 < argc) {
      if (!vectis_embedded_fs_extract_policy_parse(argv[++i],
                                                   &extract_policy)) {
        fputs("vectis: --extract-mode must be fail-exists, skip-existing, "
              "overwrite, verify, or repair\n",
              stderr);
        vectis_pack_command_cleanup(&assets, &content_types);
        return 64;
      }
      extract_mode = vectis_embedded_fs_extract_policy_string(extract_policy);
    } else if (strcmp(argv[i], "--content-type-map") == 0 && i + 1 < argc) {
      if (vectis_pack_read_content_type_map(&content_types, argv[++i]) != 0) {
        vectis_pack_command_cleanup(&assets, &content_types);
        return 1;
      }
    } else if (strcmp(argv[i], "--follow-symlinks") == 0) {
      follow_symlinks = 1;
    } else if (strcmp(argv[i], "--codesign") == 0 && i + 1 < argc) {
      codesign_identity = argv[++i];
    } else if (strcmp(argv[i], "--ad-hoc-codesign") == 0) {
      ad_hoc_codesign = 1;
    } else if (strcmp(argv[i], "--hardened-runtime") == 0) {
      hardened_runtime = 1;
    } else if (strcmp(argv[i], "--timestamp") == 0) {
      timestamp = 1;
    } else if (strcmp(argv[i], "--entitlements") == 0 && i + 1 < argc) {
      entitlements_path = argv[++i];
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
         strcmp(argv[i], "--extract-mode") == 0 ||
         strcmp(argv[i], "--content-type-map") == 0 ||
         strcmp(argv[i], "--codesign") == 0 ||
         strcmp(argv[i], "--entitlements") == 0) &&
        i + 1 < argc) {
      i++;
    } else if (strcmp(argv[i], "--follow-symlinks") == 0 ||
               strcmp(argv[i], "--ad-hoc-codesign") == 0 ||
               strcmp(argv[i], "--hardened-runtime") == 0 ||
               strcmp(argv[i], "--timestamp") == 0) {
      continue;
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
                                &content_types, follow_symlinks) != 0) {
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
                                  &content_types, follow_symlinks,
                                  &dir_stack) != 0) {
        free(asset_source);
        free(asset_logical);
        vectis_pack_command_cleanup(&assets, &content_types);
        return 1;
      }
      free(asset_source);
      free(asset_logical);
    } else if (strcmp(argv[i], "--asset-manifest") == 0 && i + 1 < argc) {
      if (vectis_pack_read_asset_manifest(&assets, argv[++i], &content_types,
                                          follow_symlinks) != 0) {
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
  if (codesign_identity != NULL && ad_hoc_codesign) {
    fputs("vectis: --codesign and --ad-hoc-codesign are mutually exclusive\n",
          stderr);
    vectis_pack_command_cleanup(&assets, &content_types);
    return 64;
  }
  if (codesign_identity != NULL || ad_hoc_codesign || hardened_runtime ||
      timestamp || entitlements_path != NULL) {
#ifdef __APPLE__
    fputs("vectis: Darwin pack signing requires Mach-O pack support, which is "
          "not implemented\n",
          stderr);
#else
    fputs("vectis: Darwin pack signing options require a Darwin/Mach-O pack "
          "output\n",
          stderr);
#endif
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
  if (vectis_pack_build_manifest(&assets, extract_mode, &manifest,
                                 &manifest_size) != 0) {
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
    if (assets.items[i].kind != VECTIS_PACK_ASSET_FILE) {
      continue;
    }
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

static vectis_lua_memory_source *vectis_lua_check_memory_source(lua_State *lua,
                                                                int index) {
  vectis_lua_memory_source *source;

  source = (vectis_lua_memory_source *)lua_touserdata(lua, index);
  if (source == NULL) {
    (void)luaL_error(lua, "embedded memory source state is required");
  }
  return source;
}

static int vectis_lua_memory_source_read(lua_State *lua) {
  vectis_lua_memory_source *source;
  lua_Integer requested;
  size_t count;
  size_t remaining;
  size_t chunk;

  source = vectis_lua_check_memory_source(lua, lua_upvalueindex(1));
  requested = luaL_checkinteger(lua, 1);
  if (requested < 0) {
    return luaL_error(lua, "source read size must not be negative");
  }
  if (source->closed || source->offset >= source->size) {
    lua_pushnil(lua);
    lua_pushnil(lua);
    return 2;
  }
  count = (size_t)requested;
  remaining = source->size - source->offset;
  chunk = remaining < count ? remaining : count;
  lua_pushlstring(lua, (const char *)source->data + source->offset, chunk);
  source->offset += chunk;
  lua_pushnil(lua);
  return 2;
}

static int vectis_lua_memory_source_reset(lua_State *lua) {
  vectis_lua_memory_source *source;

  source = vectis_lua_check_memory_source(lua, lua_upvalueindex(1));
  source->offset = 0u;
  source->closed = 0;
  lua_pushboolean(lua, 1);
  lua_pushnil(lua);
  return 2;
}

static int vectis_lua_memory_source_close(lua_State *lua) {
  vectis_lua_memory_source *source;

  source = vectis_lua_check_memory_source(lua, lua_upvalueindex(1));
  source->closed = 1;
  source->offset = source->size;
  return 0;
}

static int vectis_lua_embedded_lockd_bundle_source(lua_State *lua) {
  vectis_lua_runtime_context *context;
  vectis_lua_memory_source *source;
  int table_index;

  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  if (context == NULL || context->embedded_lockd_bundle == NULL ||
      context->embedded_lockd_bundle_size == 0u) {
    lua_pushnil(lua);
    lua_pushliteral(lua, "no embedded lockd bundle");
    return 2;
  }

  lua_newtable(lua);
  table_index = lua_gettop(lua);
  source = (vectis_lua_memory_source *)lua_newuserdata(lua, sizeof(*source));
  source->data = context->embedded_lockd_bundle;
  source->size = context->embedded_lockd_bundle_size;
  source->offset = 0u;
  source->closed = 0;
  lua_pushvalue(lua, -1);
  lua_pushcclosure(lua, vectis_lua_memory_source_read, 1);
  lua_setfield(lua, table_index, "read");
  lua_pushvalue(lua, -1);
  lua_pushcclosure(lua, vectis_lua_memory_source_reset, 1);
  lua_setfield(lua, table_index, "reset");
  lua_pushcclosure(lua, vectis_lua_memory_source_close, 1);
  lua_setfield(lua, table_index, "close");
  return 1;
}

static int vectis_lua_embedded_has_assets(lua_State *lua) {
  vectis_lua_runtime_context *context;

  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  lua_pushboolean(lua, context != NULL && context->embedded_fs != NULL);
  return 1;
}

static int vectis_lua_embedded_default_extract_policy(lua_State *lua) {
  vectis_lua_runtime_context *context;
  vectis_embedded_fs_extract_policy policy;

  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  policy = context != NULL
               ? vectis_embedded_fs_default_extract_policy(context->embedded_fs)
               : VECTIS_EMBEDDED_FS_EXTRACT_FAIL_EXISTS;
  lua_pushstring(lua, vectis_embedded_fs_extract_policy_string(policy));
  return 1;
}

static int vectis_lua_embedded_tree_sha256(lua_State *lua) {
  vectis_lua_runtime_context *context;
  const char *tree_sha256;

  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  if (context == NULL || context->embedded_fs == NULL) {
    lua_pushnil(lua);
    lua_pushliteral(lua, "no embedded assets");
    return 2;
  }
  tree_sha256 = vectis_embedded_fs_tree_sha256(context->embedded_fs);
  if (tree_sha256 == NULL) {
    lua_pushnil(lua);
    lua_pushliteral(lua, "embedded asset tree hash is unavailable");
    return 2;
  }
  lua_pushstring(lua, tree_sha256);
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
  lua_pushstring(lua, entry.kind == VECTIS_EMBEDDED_FS_ENTRY_DIRECTORY
                          ? "directory"
                          : "file");
  lua_setfield(lua, -2, "kind");
  lua_pushinteger(lua, (lua_Integer)entry.mode);
  lua_setfield(lua, -2, "mode");
  if (entry.content_type != NULL) {
    lua_pushstring(lua, entry.content_type);
    lua_setfield(lua, -2, "content_type");
  }
  if (entry.sha256 != NULL) {
    lua_pushstring(lua, entry.sha256);
    lua_setfield(lua, -2, "sha256");
  }
  if (entry.etag != NULL) {
    lua_pushstring(lua, entry.etag);
    lua_setfield(lua, -2, "etag");
  }
  return 1;
}

static int vectis_lua_embedded_chunks_next(lua_State *lua) {
  vectis_lua_embedded_chunks_state *state;
  lc_error error;
  size_t nread;

  state = (vectis_lua_embedded_chunks_state *)lua_touserdata(
      lua, lua_upvalueindex(1));
  if (state == NULL || state->source == NULL || state->buffer == NULL) {
    return 0;
  }
  lc_error_init(&error);
  nread = state->source->read(state->source, state->buffer, state->chunk_size,
                              &error);
  if (nread == 0u) {
    if (error.code != LC_OK) {
      const char *message;

      message = error.message != NULL ? error.message
                                      : "embedded chunks source read failed";
      lua_pushstring(lua, message);
      lc_error_cleanup(&error);
      if (state->source != NULL) {
        lc_source_close(state->source);
        state->source = NULL;
      }
      free(state->buffer);
      state->buffer = NULL;
      return lua_error(lua);
    }
    lc_error_cleanup(&error);
    lc_source_close(state->source);
    state->source = NULL;
    free(state->buffer);
    state->buffer = NULL;
    return 0;
  }
  lc_error_cleanup(&error);
  lua_pushlstring(lua, (const char *)state->buffer, nread);
  return 1;
}

static int vectis_lua_embedded_chunks_gc(lua_State *lua) {
  vectis_lua_embedded_chunks_state *state;

  state = (vectis_lua_embedded_chunks_state *)lua_touserdata(lua, 1);
  if (state != NULL) {
    if (state->source != NULL) {
      lc_source_close(state->source);
      state->source = NULL;
    }
    free(state->buffer);
    state->buffer = NULL;
  }
  return 0;
}

static int vectis_lua_embedded_chunks(lua_State *lua) {
  vectis_lua_runtime_context *context;
  vectis_lua_embedded_chunks_state *state;
  const char *path;
  lua_Integer requested_chunk_size;
  vectis_error error;
  vectis_status status;
  lc_source *source;
  unsigned char *buffer;
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
  source = NULL;
  status = vectis_embedded_fs_open_source(context->embedded_fs, path, &found,
                                          &source, &error);
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
  buffer = (unsigned char *)malloc((size_t)requested_chunk_size);
  if (buffer == NULL) {
    lc_source_close(source);
    return luaL_error(lua, "failed to allocate embedded chunks buffer");
  }
  state = (vectis_lua_embedded_chunks_state *)lua_newuserdatauv(
      lua, sizeof(*state), 0);
  memset(state, 0, sizeof(*state));
  state->source = source;
  state->buffer = buffer;
  state->chunk_size = (size_t)requested_chunk_size;
  luaL_getmetatable(lua, VECTIS_LUA_EMBEDDED_CHUNKS);
  lua_setmetatable(lua, -2);
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
  vectis_embedded_fs_extract_policy parsed;

  if (policy != NULL &&
      vectis_embedded_fs_extract_policy_parse(policy, &parsed)) {
    return parsed;
  }
  (void)luaL_error(lua, "embedded extract policy must be fail_exists, "
                        "fail-exists, skip_existing, skip-existing, "
                        "overwrite, verify, or repair");
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
  config.policy =
      vectis_embedded_fs_default_extract_policy(context->embedded_fs);
  if (policy != NULL) {
    config.policy = vectis_lua_embedded_extract_policy(lua, policy);
  }
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

static int vectis_lua_auth_factor_bit(const char *name, unsigned int *out) {
  if (out == NULL) {
    return 0;
  }
  if (name == NULL || name[0] == '\0') {
    return 0;
  }
  if (strcmp(name, "password") == 0) {
    *out = VECTIS_AUTH_ROUTE_FACTOR_PASSWORD;
    return 1;
  }
  if (strcmp(name, "totp") == 0 || strcmp(name, "totp_code") == 0 ||
      strcmp(name, "totp-code") == 0) {
    *out = VECTIS_AUTH_ROUTE_FACTOR_TOTP;
    return 1;
  }
  if (strcmp(name, "email_token") == 0 || strcmp(name, "email-token") == 0 ||
      strcmp(name, "email") == 0) {
    *out = VECTIS_AUTH_ROUTE_FACTOR_EMAIL_TOKEN;
    return 1;
  }
  return 0;
}

static int vectis_lua_ascii_equal_ci(const char *left, const char *right) {
  unsigned char lc;
  unsigned char rc;

  if (left == NULL || right == NULL) {
    return 0;
  }
  while (*left != '\0' && *right != '\0') {
    lc = (unsigned char)*left;
    rc = (unsigned char)*right;
    if (lc >= (unsigned char)'A' && lc <= (unsigned char)'Z') {
      lc = (unsigned char)(lc - (unsigned char)'A' + (unsigned char)'a');
    }
    if (rc >= (unsigned char)'A' && rc <= (unsigned char)'Z') {
      rc = (unsigned char)(rc - (unsigned char)'A' + (unsigned char)'a');
    }
    if (lc != rc) {
      return 0;
    }
    left++;
    right++;
  }
  return *left == '\0' && *right == '\0';
}

static int vectis_lua_http_method_bit(const char *name,
                                      vectis_http_methods *out) {
  if (out == NULL || name == NULL || name[0] == '\0') {
    return 0;
  }
  if (vectis_lua_ascii_equal_ci(name, "GET")) {
    *out = VECTIS_HTTP_METHODS_GET;
    return 1;
  }
  if (vectis_lua_ascii_equal_ci(name, "POST")) {
    *out = VECTIS_HTTP_METHODS_POST;
    return 1;
  }
  if (vectis_lua_ascii_equal_ci(name, "PUT")) {
    *out = VECTIS_HTTP_METHODS_PUT;
    return 1;
  }
  if (vectis_lua_ascii_equal_ci(name, "PATCH")) {
    *out = VECTIS_HTTP_METHODS_PATCH;
    return 1;
  }
  if (vectis_lua_ascii_equal_ci(name, "DELETE")) {
    *out = VECTIS_HTTP_METHODS_DELETE;
    return 1;
  }
  if (vectis_lua_ascii_equal_ci(name, "HEAD")) {
    *out = VECTIS_HTTP_METHODS_HEAD;
    return 1;
  }
  if (vectis_lua_ascii_equal_ci(name, "OPTIONS")) {
    *out = VECTIS_HTTP_METHODS_OPTIONS;
    return 1;
  }
  if (vectis_lua_ascii_equal_ci(name, "PROPFIND")) {
    *out = VECTIS_HTTP_METHODS_PROPFIND;
    return 1;
  }
  if (vectis_lua_ascii_equal_ci(name, "MKCOL")) {
    *out = VECTIS_HTTP_METHODS_MKCOL;
    return 1;
  }
  if (vectis_lua_ascii_equal_ci(name, "COPY")) {
    *out = VECTIS_HTTP_METHODS_COPY;
    return 1;
  }
  if (vectis_lua_ascii_equal_ci(name, "MOVE")) {
    *out = VECTIS_HTTP_METHODS_MOVE;
    return 1;
  }
  return 0;
}

static vectis_http_methods
vectis_lua_route_methods(lua_State *lua, int index,
                         vectis_http_methods fallback, const char *label) {
  vectis_http_methods methods;
  vectis_http_methods bit;
  const char *name;
  size_t count;
  size_t i;
  int type;

  index = lua_absindex(lua, index);
  lua_getfield(lua, index, "methods");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    name = vectis_lua_table_string(lua, index, "method");
    if (name == NULL) {
      return fallback;
    }
    if (!vectis_lua_http_method_bit(name, &bit)) {
      luaL_error(lua, "%s method is unsupported", label);
      return fallback;
    }
    return bit;
  }

  type = lua_type(lua, -1);
  if (type == LUA_TSTRING) {
    name = lua_tostring(lua, -1);
    if (!vectis_lua_http_method_bit(name, &bit)) {
      lua_pop(lua, 1);
      luaL_error(lua, "%s methods contains unsupported method", label);
      return fallback;
    }
    lua_pop(lua, 1);
    return bit;
  }
  if (type != LUA_TTABLE) {
    lua_pop(lua, 1);
    luaL_error(lua, "%s methods must be a string or table", label);
    return fallback;
  }

  count = lua_rawlen(lua, -1);
  if (count == 0u) {
    lua_pop(lua, 1);
    luaL_error(lua, "%s methods must not be empty", label);
    return fallback;
  }
  methods = VECTIS_HTTP_METHODS_NONE;
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, -1, (lua_Integer)i + 1);
    name = luaL_checkstring(lua, -1);
    if (!vectis_lua_http_method_bit(name, &bit)) {
      lua_pop(lua, 2);
      luaL_error(lua, "%s methods contains unsupported method", label);
      return fallback;
    }
    methods |= bit;
    lua_pop(lua, 1);
  }
  lua_pop(lua, 1);
  return methods;
}

static int vectis_lua_auth_required_factors(lua_State *lua, int index,
                                            const char *field,
                                            unsigned int *out) {
  unsigned int bits;
  unsigned int bit;
  const char *name;
  size_t count;
  size_t i;
  int type;

  if (out == NULL) {
    return 0;
  }
  index = lua_absindex(lua, index);
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return 0;
  }
  type = lua_type(lua, -1);
  if (type == LUA_TSTRING) {
    name = lua_tostring(lua, -1);
    if (!vectis_lua_auth_factor_bit(name, &bit)) {
      lua_pop(lua, 1);
      luaL_error(lua,
                 "auth route required_factors contains unsupported factor");
      return 0;
    }
    *out = bit;
    lua_pop(lua, 1);
    return 1;
  }
  if (type != LUA_TTABLE) {
    lua_pop(lua, 1);
    luaL_error(lua, "auth route required_factors must be a string or table");
    return 0;
  }
  bits = 0u;
  count = lua_rawlen(lua, -1);
  if (count == 0u) {
    lua_pop(lua, 1);
    luaL_error(lua, "auth route required_factors must not be empty");
    return 0;
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, -1, (lua_Integer)i + 1);
    name = luaL_checkstring(lua, -1);
    if (!vectis_lua_auth_factor_bit(name, &bit)) {
      lua_pop(lua, 2);
      luaL_error(lua,
                 "auth route required_factors contains unsupported factor");
      return 0;
    }
    bits |= bit;
    lua_pop(lua, 1);
  }
  lua_pop(lua, 1);
  *out = bits;
  return 1;
}

static long vectis_lua_table_long(lua_State *lua, int index, const char *field,
                                  long fallback) {
  long value;

  lua_getfield(lua, index, field);
  value = lua_isnil(lua, -1) ? fallback : (long)luaL_checkinteger(lua, -1);
  lua_pop(lua, 1);
  return value;
}

static const char *vectis_lua_table_lstring(lua_State *lua, int index,
                                            const char *field,
                                            size_t *out_size) {
  const char *value;
  size_t size;

  if (out_size != NULL) {
    *out_size = 0u;
  }
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return NULL;
  }
  value = luaL_checklstring(lua, -1, &size);
  if (out_size != NULL) {
    *out_size = size;
  }
  lua_pop(lua, 1);
  return value;
}

static int vectis_lua_ssh_exec(lua_State *lua) {
  vectis_ssh_config config;
  vectis_ssh *ssh;
  vectis_ssh_exec_result result;
  vectis_error error;
  vectis_status status;
  const char *command;
  const char *private_key;
  long port;
  size_t private_key_size;

  luaL_checktype(lua, 1, LUA_TTABLE);
  command = vectis_lua_table_string(lua, 1, "command");
  if (command == NULL || command[0] == '\0') {
    return vectis_lua_push_error_text(lua, VECTIS_ERR_INVALID,
                                      "SSH command is required");
  }

  vectis_ssh_config_init(&config);
  config.host = vectis_lua_table_string(lua, 1, "host");
  config.username = vectis_lua_table_string(lua, 1, "username");
  config.password = vectis_lua_table_string(lua, 1, "password");
  config.private_key_path = vectis_lua_table_string(lua, 1, "private_key_path");
  if (config.private_key_path == NULL) {
    config.private_key_path = vectis_lua_table_string(lua, 1, "key_path");
  }
  private_key =
      vectis_lua_table_lstring(lua, 1, "private_key", &private_key_size);
  if (private_key != NULL) {
    config.private_key =
        vectis_source_from_memory(private_key, private_key_size);
    config.private_key_path = NULL;
  }
  config.known_hosts_path = vectis_lua_table_string(lua, 1, "known_hosts_path");
  if (config.known_hosts_path == NULL) {
    config.known_hosts_path = vectis_lua_table_string(lua, 1, "known_hosts");
  }
  config.timeout_ms =
      vectis_lua_table_long(lua, 1, "timeout_ms", config.timeout_ms);
  port = vectis_lua_table_long(lua, 1, "port", (long)config.port);
  if (port <= 0L || port > 65535L) {
    return vectis_lua_push_error_text(lua, VECTIS_ERR_INVALID,
                                      "SSH port must be between 1 and 65535");
  }
  config.port = (unsigned short)port;

  ssh = NULL;
  memset(&result, 0, sizeof(result));
  vectis_error_clear(&error);
  status = vectis_ssh_new(&config, &ssh, &error);
  if (status == VECTIS_OK) {
    status = ssh->exec(ssh, command, &result, &error);
  }
  if (status != VECTIS_OK) {
    if (ssh != NULL) {
      ssh->close(ssh);
    }
    return vectis_lua_push_error(lua, status, &error);
  }

  lua_newtable(lua);
  lua_pushboolean(lua, 1);
  lua_setfield(lua, -2, "ok");
  lua_pushinteger(lua, (lua_Integer)result.exit_status);
  lua_setfield(lua, -2, "exit_status");
  lua_pushlstring(lua, result.stdout_data != NULL ? result.stdout_data : "",
                  result.stdout_size);
  lua_setfield(lua, -2, "stdout");
  lua_pushinteger(lua, (lua_Integer)result.stdout_size);
  lua_setfield(lua, -2, "stdout_size");
  lua_pushlstring(lua, result.stderr_data != NULL ? result.stderr_data : "",
                  result.stderr_size);
  lua_setfield(lua, -2, "stderr");
  lua_pushinteger(lua, (lua_Integer)result.stderr_size);
  lua_setfield(lua, -2, "stderr_size");

  vectis_ssh_exec_result_cleanup(&result);
  if (ssh != NULL) {
    ssh->close(ssh);
  }
  return 1;
}

static const char **vectis_lua_string_array_field(lua_State *lua, int index,
                                                  const char *field,
                                                  size_t *out_count) {
  const char **values;
  size_t count;
  size_t i;

  if (out_count != NULL) {
    *out_count = 0u;
  }
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return NULL;
  }
  luaL_checktype(lua, -1, LUA_TTABLE);
  count = lua_rawlen(lua, -1);
  if (count == 0u) {
    lua_pop(lua, 1);
    return NULL;
  }
  values = (const char **)calloc(count, sizeof(*values));
  if (values == NULL) {
    lua_pop(lua, 1);
    luaL_error(lua, "failed to allocate Lua string array");
    return NULL;
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, -1, (lua_Integer)i + 1);
    values[i] = luaL_checkstring(lua, -1);
    lua_pop(lua, 1);
  }
  lua_pop(lua, 1);
  if (out_count != NULL) {
    *out_count = count;
  }
  return values;
}

static unsigned vectis_lua_auth_modes_field(lua_State *lua, int index,
                                            const char *field,
                                            unsigned fallback);

static int vectis_lua_tls_mode(const char *mode, vectis_tls_mode *out) {
  if (out == NULL) {
    return 0;
  }
  if (mode == NULL || mode[0] == '\0' || strcmp(mode, "disabled") == 0 ||
      strcmp(mode, "off") == 0) {
    *out = VECTIS_TLS_MODE_DISABLED;
    return 1;
  }
  if (strcmp(mode, "manual") == 0) {
    *out = VECTIS_TLS_MODE_MANUAL;
    return 1;
  }
  if (strcmp(mode, "acme") == 0) {
    *out = VECTIS_TLS_MODE_ACME;
    return 1;
  }
  return 0;
}

static void vectis_lua_parse_lockd_config(lua_State *lua, int index,
                                          vectis_lockd_config *lockd,
                                          const char ***endpoints_alloc_out) {
  vectis_lua_runtime_context *context;
  const char **endpoints;
  const char *bundle_mode;

  if (endpoints_alloc_out != NULL) {
    *endpoints_alloc_out = NULL;
  }
  index = lua_absindex(lua, index);
  lua_getfield(lua, index, "lockd");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return;
  }
  if (!lua_istable(lua, -1)) {
    luaL_error(lua, "server lockd must be a table");
    return;
  }
  index = lua_absindex(lua, -1);
  bundle_mode = vectis_lua_table_string(lua, index, "client_bundle");
  if (bundle_mode == NULL) {
    bundle_mode = vectis_lua_table_string(lua, index, "bundle");
  }
  if (bundle_mode != NULL) {
    if (strcmp(bundle_mode, "embedded") != 0) {
      luaL_error(lua, "server lockd.client_bundle must be embedded");
      return;
    }
    context =
        (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(lua);
    if (context == NULL || context->embedded_lockd_bundle == NULL ||
        context->embedded_lockd_bundle_size == 0u) {
      luaL_error(lua, "server lockd.client_bundle requested embedded bundle, "
                      "but no embedded lockd bundle is available");
      return;
    }
    lockd->client_bundle = vectis_source_from_memory(
        context->embedded_lockd_bundle, context->embedded_lockd_bundle_size);
  }
  endpoints = vectis_lua_string_array_field(lua, index, "endpoints",
                                            &lockd->endpoint_count);
  lockd->endpoints = endpoints;
  if (endpoints_alloc_out != NULL) {
    *endpoints_alloc_out = endpoints;
  }
  lockd->unix_socket_path =
      vectis_lua_table_string(lua, index, "unix_socket_path");
  lockd->client_bundle_path =
      vectis_lua_table_string(lua, index, "client_bundle_path");
  if (lockd->client_bundle_path == NULL) {
    lockd->client_bundle_path =
        vectis_lua_table_string(lua, index, "bundle_path");
  }
  lockd->default_namespace =
      vectis_lua_table_string(lua, index, "default_namespace");
  if (lockd->default_namespace == NULL) {
    lockd->default_namespace = vectis_lua_table_string(lua, index, "namespace");
  }
  lockd->timeout_ms =
      vectis_lua_table_long(lua, index, "timeout_ms", lockd->timeout_ms);
  lua_pop(lua, 1);
}

static vectis_lua_server *vectis_lua_check_server(lua_State *lua, int index) {
  return (vectis_lua_server *)luaL_checkudata(lua, index, VECTIS_LUA_SERVER);
}

static vectis_app *vectis_lua_server_app(lua_State *lua, int index) {
  vectis_lua_server *server;

  server = vectis_lua_check_server(lua, index);
  if (server->app == NULL) {
    (void)luaL_error(lua, "vectis server is closed");
  }
  return server->app;
}

static void vectis_lua_server_native_auth_clear_callback(
    vectis_lua_server_native_auth *auth) {
  if (auth == NULL) {
    return;
  }
  free(auth->callback_location);
  free(auth->callback_content_type);
  free(auth->callback_body);
  auth->callback_location = NULL;
  auth->callback_content_type = NULL;
  auth->callback_body = NULL;
  auth->callback_body_size = 0u;
}

static void
vectis_lua_server_native_auth_free(vectis_lua_server_native_auth *auth) {
  if (auth == NULL) {
    return;
  }
  if (auth->lua != NULL && auth->callback_ref != LUA_NOREF) {
    luaL_unref(auth->lua, LUA_REGISTRYINDEX, auth->callback_ref);
    auth->callback_ref = LUA_NOREF;
  }
  vectis_lua_server_native_auth_clear_callback(auth);
  free(auth->credentials_path);
  free(auth->purpose);
  free(auth->realm);
  free(auth);
}

static void vectis_lua_server_native_auth_free_all(vectis_lua_server *server) {
  vectis_lua_server_native_auth *auth;
  vectis_lua_server_native_auth *next;

  if (server == NULL) {
    return;
  }
  auth = server->native_auths;
  server->native_auths = NULL;
  while (auth != NULL) {
    next = auth->next;
    vectis_lua_server_native_auth_free(auth);
    auth = next;
  }
}

static void vectis_lua_server_auth_json_route_free(
    vectis_lua_server_auth_json_route *route) {
  if (route == NULL) {
    return;
  }
  free(route->body);
  free(route->content_type);
  free(route->purpose);
  free(route);
}

static void
vectis_lua_server_json_route_free(vectis_lua_server_json_route *route) {
  if (route == NULL) {
    return;
  }
  free(route->body);
  free(route->content_type);
  free(route->cache_control);
  free(route);
}

static void vectis_lua_server_json_route_free_all(vectis_lua_server *server) {
  vectis_lua_server_json_route *route;
  vectis_lua_server_json_route *next;

  if (server == NULL) {
    return;
  }
  route = server->json_routes;
  server->json_routes = NULL;
  while (route != NULL) {
    next = route->next;
    vectis_lua_server_json_route_free(route);
    route = next;
  }
}

static void
vectis_lua_server_auth_json_route_free_all(vectis_lua_server *server) {
  vectis_lua_server_auth_json_route *route;
  vectis_lua_server_auth_json_route *next;

  if (server == NULL) {
    return;
  }
  route = server->auth_json_routes;
  server->auth_json_routes = NULL;
  while (route != NULL) {
    next = route->next;
    vectis_lua_server_auth_json_route_free(route);
    route = next;
  }
}

static void vectis_lua_server_consumer_service_free(
    vectis_lua_consumer_registration *service) {
  vectis_error error;

  if (service == NULL) {
    return;
  }
  if (service->service != NULL) {
    if (service->started) {
      vectis_error_clear(&error);
      (void)service->service->stop(service->service, &error);
      vectis_error_clear(&error);
      (void)service->service->wait(service->service, &error);
      service->started = 0;
    }
    service->service->close(service->service);
    service->service = NULL;
  }
  free(service->name);
  free(service->queue);
  free(service->owner);
  free(service->cache_dir);
  free(service->site_id);
  free(service->processing_path);
  free(service->done_path);
  free(service->processing_body);
  free(service->done_body);
  free(service);
}

static void
vectis_lua_server_consumer_service_free_all(vectis_lua_server *server) {
  vectis_lua_consumer_registration *service;
  vectis_lua_consumer_registration *next;

  if (server == NULL) {
    return;
  }
  service = server->consumer_services;
  server->consumer_services = NULL;
  while (service != NULL) {
    next = service->next;
    vectis_lua_server_consumer_service_free(service);
    service = next;
  }
}

static int vectis_lua_copy_string_field(lua_State *lua, int index,
                                        const char *field, const char *fallback,
                                        char **out) {
  const char *value;

  value = vectis_lua_table_string(lua, index, field);
  if (value == NULL) {
    value = fallback;
  }
  *out = vectis_cli_strdup(value);
  return value == NULL || *out != NULL;
}

static int vectis_lua_server_auth_copy_response_field(lua_State *lua, int index,
                                                      const char *field,
                                                      char **out,
                                                      size_t *out_size,
                                                      vectis_error *error) {
  size_t len;
  const char *value;

  *out = NULL;
  if (out_size != NULL) {
    *out_size = 0u;
  }
  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return 1;
  }
  value = lua_tolstring(lua, -1, &len);
  if (value == NULL) {
    lua_pop(lua, 1);
    vectis_cli_error_set(error, VECTIS_ERR_INVALID,
                         "auth callback response string field is invalid");
    return 0;
  }
  *out = (char *)malloc(len + 1u);
  if (*out == NULL) {
    lua_pop(lua, 1);
    vectis_cli_error_set(error, VECTIS_ERR_NOMEM,
                         "failed to copy auth callback response field");
    return 0;
  }
  memcpy(*out, value, len);
  (*out)[len] = '\0';
  if (out_size != NULL) {
    *out_size = len;
  }
  lua_pop(lua, 1);
  return 1;
}

static vectis_auth_action
vectis_lua_server_auth_action_from_name(const char *action) {
  if (action != NULL && strcmp(action, "allow") == 0) {
    return VECTIS_AUTH_ALLOW;
  }
  if (action != NULL && strcmp(action, "required") == 0) {
    return VECTIS_AUTH_REQUIRED;
  }
  if (action != NULL && strcmp(action, "redirect") == 0) {
    return VECTIS_AUTH_REDIRECT;
  }
  return VECTIS_AUTH_DENY;
}

static vectis_status vectis_lua_server_callback_authenticate(
    const vectis_auth_provider_request *request,
    vectis_auth_provider_response *response, void *userdata,
    vectis_error *error) {
  vectis_lua_server_native_auth *auth;
  lua_State *lua;
  int base;
  int response_index;
  const char *authorization;
  const char *action;
  const char *principal;
  lua_Integer status_code;

  auth = (vectis_lua_server_native_auth *)userdata;
  if (auth == NULL || auth->lua == NULL || auth->callback_ref == LUA_NOREF) {
    vectis_cli_error_set(error, VECTIS_ERR_INVALID,
                         "Lua auth callback provider is not configured");
    return VECTIS_ERR_INVALID;
  }
  lua = auth->lua;
  authorization = request != NULL ? request->authorization : NULL;
  if (authorization == NULL && request != NULL && request->request != NULL) {
    authorization = vectis_request_header(request->request, "authorization");
  }
  base = lua_gettop(lua);
  vectis_lua_server_native_auth_clear_callback(auth);
  vectis_auth_provider_response_init(response);

  lua_rawgeti(lua, LUA_REGISTRYINDEX, auth->callback_ref);
  if (!lua_isfunction(lua, -1)) {
    lua_settop(lua, base);
    vectis_cli_error_set(error, VECTIS_ERR_INVALID,
                         "Lua auth callback reference is invalid");
    return VECTIS_ERR_INVALID;
  }
  lua_newtable(lua);
  if (authorization != NULL) {
    lua_pushstring(lua, authorization);
    lua_setfield(lua, -2, "authorization");
  }
  if (request != NULL && request->purpose != NULL) {
    lua_pushstring(lua, request->purpose);
    lua_setfield(lua, -2, "purpose");
  }
  if (request != NULL && request->resource != NULL) {
    lua_pushstring(lua, request->resource);
    lua_setfield(lua, -2, "resource");
  }
  lua_pushinteger(
      lua, request != NULL ? (lua_Integer)request->allowed_auth_modes : 0);
  lua_setfield(lua, -2, "allowed_modes");
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    action = lua_tostring(lua, -1);
    vectis_cli_error_set(error, VECTIS_ERR_STATE,
                         action != NULL ? action : "Lua auth callback failed");
    lua_settop(lua, base);
    return VECTIS_ERR_STATE;
  }
  if (!lua_istable(lua, -1)) {
    lua_settop(lua, base);
    vectis_cli_error_set(error, VECTIS_ERR_INVALID,
                         "Lua auth callback must return a response table");
    return VECTIS_ERR_INVALID;
  }
  response_index = lua_absindex(lua, -1);
  lua_getfield(lua, response_index, "action");
  action = lua_isnil(lua, -1) ? "deny" : lua_tostring(lua, -1);
  if (action == NULL ||
      !(strcmp(action, "allow") == 0 || strcmp(action, "deny") == 0 ||
        strcmp(action, "required") == 0 || strcmp(action, "redirect") == 0)) {
    lua_settop(lua, base);
    vectis_cli_error_set(
        error, VECTIS_ERR_INVALID,
        "Lua auth callback action must be allow, deny, required, or redirect");
    return VECTIS_ERR_INVALID;
  }
  response->action = vectis_lua_server_auth_action_from_name(action);
  lua_pop(lua, 1);

  lua_getfield(lua, response_index, "status_code");
  if (lua_isnil(lua, -1)) {
    status_code = 0;
  } else if (lua_isnumber(lua, -1)) {
    status_code = lua_tointeger(lua, -1);
  } else {
    lua_settop(lua, base);
    vectis_cli_error_set(error, VECTIS_ERR_INVALID,
                         "Lua auth callback status_code must be a number");
    return VECTIS_ERR_INVALID;
  }
  lua_pop(lua, 1);
  response->status_code = (int)status_code;

  if (!vectis_lua_server_auth_copy_response_field(
          lua, response_index, "location", &auth->callback_location, NULL,
          error) ||
      !vectis_lua_server_auth_copy_response_field(
          lua, response_index, "content_type", &auth->callback_content_type,
          NULL, error) ||
      !vectis_lua_server_auth_copy_response_field(
          lua, response_index, "body", &auth->callback_body,
          &auth->callback_body_size, error)) {
    lua_settop(lua, base);
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  response->location = auth->callback_location;
  response->content_type = auth->callback_content_type;
  response->body = auth->callback_body;
  response->body_size = auth->callback_body_size;

  lua_getfield(lua, response_index, "www_authenticate");
  if (!lua_isnil(lua, -1)) {
    action = lua_tostring(lua, -1);
    if (action == NULL) {
      lua_settop(lua, base);
      vectis_cli_error_set(
          error, VECTIS_ERR_INVALID,
          "Lua auth callback www_authenticate must be a string");
      return VECTIS_ERR_INVALID;
    }
    snprintf(response->www_authenticate, sizeof(response->www_authenticate),
             "%s", action);
  }
  lua_pop(lua, 1);

  lua_getfield(lua, response_index, "principal");
  if (!lua_isnil(lua, -1)) {
    principal = lua_tostring(lua, -1);
    if (principal == NULL) {
      lua_settop(lua, base);
      vectis_cli_error_set(error, VECTIS_ERR_INVALID,
                           "Lua auth callback principal must be a string");
      return VECTIS_ERR_INVALID;
    }
    snprintf(response->principal, sizeof(response->principal), "%s", principal);
  }
  lua_pop(lua, 1);
  lua_settop(lua, base);
  return VECTIS_OK;
}

static vectis_lua_server_native_auth *
vectis_lua_server_native_auth_new(lua_State *lua, int index,
                                  const char *context, vectis_error *error) {
  vectis_lua_server_native_auth *auth;
  const char *credentials_path;
  const char *kind;
  const char *purpose;
  const char *realm;
  unsigned modes;
  int provider_index;

  index = lua_absindex(lua, index);
  (void)context;
  provider_index = index;
  lua_getfield(lua, index, "provider");
  if (lua_istable(lua, -1)) {
    provider_index = lua_absindex(lua, -1);
  } else {
    lua_pop(lua, 1);
  }
  kind = vectis_lua_table_string(lua, index, "kind");
  if (kind == NULL && provider_index != index) {
    kind = vectis_lua_table_string(lua, provider_index, "kind");
  }
  purpose = vectis_lua_table_string(lua, index, "purpose");
  if (purpose == NULL && provider_index != index) {
    purpose = vectis_lua_table_string(lua, provider_index, "purpose");
  }
  realm = vectis_lua_table_string(lua, index, "realm");
  if (realm == NULL && provider_index != index) {
    realm = vectis_lua_table_string(lua, provider_index, "realm");
  }
  modes = vectis_lua_auth_modes_field(lua, index, "allowed_modes", 0u);
  if (modes == 0u && provider_index != index) {
    modes =
        vectis_lua_auth_modes_field(lua, provider_index, "allowed_modes", 0u);
  }
  if (modes == 0u) {
    modes = vectis_lua_auth_modes_field(lua, index, "modes", 0u);
  }
  if (modes == 0u && provider_index != index) {
    modes = vectis_lua_auth_modes_field(lua, provider_index, "modes", 0u);
  }
  if (modes == 0u) {
    modes = VECTIS_AUTH_MODE_BASIC;
  }
  if (kind != NULL && strcmp(kind, "callback") == 0) {
    auth = (vectis_lua_server_native_auth *)calloc(1u, sizeof(*auth));
    if (auth == NULL) {
      if (provider_index != index) {
        lua_pop(lua, 1);
      }
      vectis_cli_error_set(error, VECTIS_ERR_NOMEM,
                           "failed to allocate callback auth adapter");
      return NULL;
    }
    auth->lua = lua;
    auth->callback_ref = LUA_NOREF;
    auth->purpose = vectis_cli_strdup(purpose != NULL ? purpose : "webdav");
    auth->realm = vectis_cli_strdup(realm != NULL ? realm : "vectis");
    if (auth->purpose == NULL || auth->realm == NULL) {
      if (provider_index != index) {
        lua_pop(lua, 1);
      }
      vectis_lua_server_native_auth_free(auth);
      vectis_cli_error_set(error, VECTIS_ERR_NOMEM,
                           "failed to copy callback auth adapter config");
      return NULL;
    }
    lua_getfield(lua, provider_index, "callback");
    if (!lua_isfunction(lua, -1)) {
      lua_pop(lua, 1);
      if (provider_index != index) {
        lua_pop(lua, 1);
      }
      vectis_lua_server_native_auth_free(auth);
      vectis_cli_error_set(error, VECTIS_ERR_INVALID,
                           "callback auth provider callback is required");
      return NULL;
    }
    auth->callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    auth->allowed_auth_modes = modes;
    vectis_webdav_auth_provider_config_init(&auth->webdav_config);
    auth->webdav_config.provider = &auth->provider;
    auth->webdav_config.purpose = auth->purpose;
    auth->webdav_config.allowed_auth_modes = modes;
    auth->provider.authenticate = vectis_lua_server_callback_authenticate;
    auth->provider.userdata = auth;
    if (provider_index != index) {
      lua_pop(lua, 1);
    }
    return auth;
  }
  if (kind != NULL && strcmp(kind, "native") != 0) {
    if (provider_index != index) {
      lua_pop(lua, 1);
    }
    vectis_cli_error_set(error, VECTIS_ERR_INVALID,
                         "server auth kind must be native or callback");
    return NULL;
  }
  credentials_path =
      vectis_lua_table_string(lua, provider_index, "credentials_path");
  if (credentials_path == NULL) {
    credentials_path = vectis_lua_table_string(lua, provider_index, "path");
  }
  if (credentials_path == NULL || credentials_path[0] == '\0') {
    if (provider_index != index) {
      lua_pop(lua, 1);
    }
    vectis_cli_error_set(error, VECTIS_ERR_INVALID,
                         "native auth credentials_path is required");
    return NULL;
  }

  auth = (vectis_lua_server_native_auth *)calloc(1u, sizeof(*auth));
  if (auth == NULL) {
    if (provider_index != index) {
      lua_pop(lua, 1);
    }
    vectis_cli_error_set(error, VECTIS_ERR_NOMEM,
                         "failed to allocate native auth adapter");
    return NULL;
  }
  auth->callback_ref = LUA_NOREF;
  auth->credentials_path = vectis_cli_strdup(credentials_path);
  auth->purpose = vectis_cli_strdup(purpose != NULL ? purpose : "webdav");
  auth->realm = vectis_cli_strdup(realm != NULL ? realm : "vectis");
  if (auth->credentials_path == NULL || auth->purpose == NULL ||
      auth->realm == NULL) {
    if (provider_index != index) {
      lua_pop(lua, 1);
    }
    vectis_lua_server_native_auth_free(auth);
    vectis_cli_error_set(error, VECTIS_ERR_NOMEM,
                         "failed to copy native auth adapter config");
    return NULL;
  }

  vectis_auth_native_provider_config_init(&auth->native_config);
  auth->native_config.store.credentials_path = auth->credentials_path;
  auth->native_config.store.max_store_bytes = vectis_lua_table_size(
      lua, index, "max_store_bytes", VECTIS_AUTH_DEFAULT_MAX_STORE_BYTES);
  auth->native_config.purpose = auth->purpose;
  auth->native_config.realm = auth->realm;
  auth->native_config.allowed_auth_modes = modes;
  auth->allowed_auth_modes = modes;
  vectis_webdav_auth_provider_config_init(&auth->webdav_config);
  auth->webdav_config.provider = &auth->provider;
  auth->webdav_config.purpose = auth->purpose;
  auth->webdav_config.allowed_auth_modes = modes;
  if (vectis_auth_provider_from_native_store(
          &auth->provider, &auth->native_config, error) != VECTIS_OK) {
    if (provider_index != index) {
      lua_pop(lua, 1);
    }
    vectis_lua_server_native_auth_free(auth);
    return NULL;
  }
  if (provider_index != index) {
    lua_pop(lua, 1);
  }
  return auth;
}

static void
vectis_lua_server_native_auth_retain(vectis_lua_server *server,
                                     vectis_lua_server_native_auth *auth) {
  if (server == NULL || auth == NULL) {
    return;
  }
  auth->next = server->native_auths;
  server->native_auths = auth;
}

static int vectis_lua_server_close(lua_State *lua) {
  vectis_lua_server *server;

  server = vectis_lua_check_server(lua, 1);
  vectis_lua_server_consumer_service_free_all(server);
  if (server->app != NULL) {
    server->app->close(server->app);
    server->app = NULL;
  }
  vectis_lua_server_json_route_free_all(server);
  vectis_lua_server_auth_json_route_free_all(server);
  vectis_lua_server_native_auth_free_all(server);
  return 0;
}

static int vectis_lua_server_start(lua_State *lua) {
  vectis_app *app;
  vectis_error error;
  vectis_status status;

  app = vectis_lua_server_app(lua, 1);
  vectis_error_clear(&error);
  status = app->start(app, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_server_stop(lua_State *lua) {
  vectis_app *app;
  vectis_error error;
  vectis_status status;

  app = vectis_lua_server_app(lua, 1);
  vectis_error_clear(&error);
  status = app->stop(app, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_server_consumer_service(lua_State *lua) {
  vectis_lua_server *server;
  vectis_app *app;
  vectis_lua_consumer_registration *service;
  vectis_error error;
  vectis_status status;
  const char *queue;
  const char *owner;
  const char *name;
  const char *kind;
  int handler_index;
  int start_service;

  server = vectis_lua_check_server(lua, 1);
  app = vectis_lua_server_app(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);

  lua_getfield(lua, 2, "on_message");
  if (!lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return vectis_lua_push_error_text(
        lua, VECTIS_ERR_INVALID,
        "server:consumer_service direct Lua callbacks are not supported; "
        "register a C-owned handler table instead");
  }
  lua_pop(lua, 1);

  queue = vectis_lua_table_string(lua, 2, "queue");
  if (queue == NULL || queue[0] == '\0') {
    return vectis_lua_push_error_text(lua, VECTIS_ERR_INVALID,
                                      "consumer service queue is required");
  }
  owner = vectis_lua_table_string(lua, 2, "owner");
  if (owner == NULL || owner[0] == '\0') {
    owner = "vectis-lua-consumer";
  }
  name = vectis_lua_table_string(lua, 2, "name");
  if (name == NULL || name[0] == '\0') {
    name = owner;
  }

  lua_getfield(lua, 2, "handler");
  if (!lua_istable(lua, -1)) {
    lua_pop(lua, 1);
    return vectis_lua_push_error_text(lua, VECTIS_ERR_INVALID,
                                      "consumer service handler is required");
  }
  handler_index = lua_absindex(lua, -1);
  kind = vectis_lua_table_string(lua, handler_index, "kind");
  if (kind == NULL || kind[0] == '\0') {
    lua_pop(lua, 1);
    return vectis_lua_push_error_text(
        lua, VECTIS_ERR_INVALID, "consumer service handler.kind is required");
  }

  service = (vectis_lua_consumer_registration *)calloc(1u, sizeof(*service));
  if (service == NULL) {
    lua_pop(lua, 1);
    return vectis_lua_push_error_text(lua, VECTIS_ERR_NOMEM,
                                      "failed to allocate consumer service");
  }
  if (!vectis_lua_copy_string_field(lua, 2, "name", name, &service->name) ||
      !vectis_lua_copy_string_field(lua, 2, "queue", queue, &service->queue) ||
      !vectis_lua_copy_string_field(lua, 2, "owner", owner, &service->owner) ||
      !vectis_lua_copy_string_field(lua, handler_index, "cache_dir", NULL,
                                    &service->cache_dir) ||
      !vectis_lua_copy_string_field(lua, handler_index, "site_id", "consumer",
                                    &service->site_id) ||
      !vectis_lua_copy_string_field(lua, handler_index, "processing_path",
                                    "/consumer-processing.txt",
                                    &service->processing_path) ||
      !vectis_lua_copy_string_field(lua, handler_index, "done_path",
                                    "/consumer-done.txt",
                                    &service->done_path) ||
      !vectis_lua_copy_string_field(lua, handler_index, "processing_body",
                                    "processing\n",
                                    &service->processing_body) ||
      !vectis_lua_copy_string_field(lua, handler_index, "done_body",
                                    "handled\n", &service->done_body)) {
    lua_pop(lua, 1);
    vectis_lua_server_consumer_service_free(service);
    return vectis_lua_push_error_text(lua, VECTIS_ERR_NOMEM,
                                      "failed to copy consumer service config");
  }
  lua_pop(lua, 1);

  if (strcmp(kind, "webdav_marker") == 0 &&
      (service->cache_dir == NULL || service->cache_dir[0] == '\0')) {
    vectis_lua_server_consumer_service_free(service);
    return vectis_lua_push_error_text(
        lua, VECTIS_ERR_INVALID,
        "webdav_marker consumer handler cache_dir is required");
  }

  vectis_consumer_service_receiver_config_init(&service->config);
  service->config.name = service->name;
  service->config.queue = service->queue;
  service->config.owner = service->owner;
  service->config.receiver_kind = kind;
  service->config.visibility_timeout_seconds =
      (long)vectis_lua_table_size(lua, 2, "visibility_timeout_seconds", 30u);
  service->config.wait_seconds =
      (long)vectis_lua_table_size(lua, 2, "wait_seconds", 1u);
  service->config.worker_count = vectis_lua_table_size(
      lua, 2, "worker_count", service->config.worker_count);
  if (strcmp(kind, "webdav_marker") == 0) {
    vectis_webdav_marker_receiver_config_init(&service->webdav_marker);
    service->webdav_marker.cache_dir = service->cache_dir;
    service->webdav_marker.site_id = service->site_id;
    service->webdav_marker.processing_path = service->processing_path;
    service->webdav_marker.done_path = service->done_path;
    service->webdav_marker.processing_body = service->processing_body;
    service->webdav_marker.done_body = service->done_body;
    service->webdav_marker.max_file_bytes = vectis_lua_table_size(
        lua, 2, "max_file_bytes", service->webdav_marker.max_file_bytes);
    service->webdav_marker.max_total_bytes = vectis_lua_table_size(
        lua, 2, "max_total_bytes", service->webdav_marker.max_total_bytes);
    service->webdav_marker.max_resources = vectis_lua_table_size(
        lua, 2, "max_resources", service->webdav_marker.max_resources);
    service->config.receiver_config = &service->webdav_marker;
  }
  service->processing_delay_seconds =
      (long)vectis_lua_table_size(lua, 2, "processing_delay_seconds", 0u);
  service->webdav_marker.processing_delay_seconds =
      service->processing_delay_seconds;

  vectis_error_clear(&error);
  status = app->consumer_service_receiver(app, &service->config,
                                          &service->service, &error);
  if (status != VECTIS_OK) {
    vectis_lua_server_consumer_service_free(service);
    return vectis_lua_push_error(lua, status, &error);
  }
  start_service = vectis_lua_table_bool(lua, 2, "start", 1);
  if (start_service) {
    status = service->service->start(service->service, &error);
    if (status != VECTIS_OK) {
      vectis_lua_server_consumer_service_free(service);
      return vectis_lua_push_error(lua, status, &error);
    }
    service->started = 1;
  }

  service->next = server->consumer_services;
  server->consumer_services = service;
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_server_static_embedded(lua_State *lua) {
  vectis_lua_runtime_context *context;
  vectis_app *app;
  vectis_static_embedded_config config;
  vectis_error error;
  vectis_status status;
  const char *path_prefix;

  app = vectis_lua_server_app(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  if (context == NULL || context->embedded_fs == NULL) {
    return vectis_lua_push_error_text(
        lua, VECTIS_ERR_INVALID,
        "embedded static mount requires packed assets");
  }
  path_prefix = vectis_lua_table_string(lua, 2, "path_prefix");
  if (path_prefix == NULL) {
    path_prefix = vectis_lua_table_string(lua, 2, "prefix");
  }
  vectis_static_embedded_config_init(&config);
  config.path_prefix = path_prefix != NULL ? path_prefix : "/";
  config.fs = context->embedded_fs;
  config.content_type = vectis_lua_table_string(lua, 2, "content_type");
  config.cache_control = vectis_lua_table_string(lua, 2, "cache_control");
  config.not_found_body = vectis_lua_table_string(lua, 2, "not_found_body");
  config.not_found_content_type =
      vectis_lua_table_string(lua, 2, "not_found_content_type");
  vectis_error_clear(&error);
  status = app->static_embedded(app, &config, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_server_static_directory(lua_State *lua) {
  vectis_app *app;
  vectis_static_directory_config config;
  vectis_error error;
  vectis_status status;
  const char *path_prefix;
  const char *root_dir;
  vectis_http_methods methods;

  app = vectis_lua_server_app(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  path_prefix = vectis_lua_table_string(lua, 2, "path_prefix");
  if (path_prefix == NULL) {
    path_prefix = vectis_lua_table_string(lua, 2, "prefix");
  }
  root_dir = vectis_lua_table_string(lua, 2, "root_dir");
  if (root_dir == NULL) {
    root_dir = vectis_lua_table_string(lua, 2, "root");
  }
  if (root_dir == NULL) {
    root_dir = vectis_lua_table_string(lua, 2, "dir");
  }
  if (root_dir == NULL || root_dir[0] == '\0') {
    return vectis_lua_push_error_text(lua, VECTIS_ERR_INVALID,
                                      "static directory root_dir is required");
  }

  vectis_static_directory_config_init(&config);
  config.path_prefix = path_prefix != NULL ? path_prefix : "/";
  config.root_dir = root_dir;
  config.content_type = vectis_lua_table_string(lua, 2, "content_type");
  config.index_file = vectis_lua_table_string(lua, 2, "index_file");
  methods = vectis_lua_route_methods(
      lua, 2, VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD,
      "static directory");
  if (methods == VECTIS_HTTP_METHODS_NONE ||
      (methods & ~(VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD)) != 0u) {
    return luaL_error(lua, "static directory methods must be GET and/or HEAD");
  }
  config.methods = methods;

  vectis_error_clear(&error);
  status = app->static_directory(app, &config, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_server_webdav(lua_State *lua) {
  vectis_lua_server *server;
  vectis_app *app;
  vectis_webdav_mount_config config;
  vectis_lua_server_native_auth *auth;
  vectis_error error;
  vectis_status status;
  const char *path_prefix;
  int auth_required;

  server = vectis_lua_check_server(lua, 1);
  app = vectis_lua_server_app(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);

  path_prefix = vectis_lua_table_string(lua, 2, "path_prefix");
  if (path_prefix == NULL) {
    path_prefix = vectis_lua_table_string(lua, 2, "prefix");
  }
  auth_required = vectis_lua_table_bool(lua, 2, "auth_required", 1);

  vectis_webdav_mount_config_init(&config);
  config.path_prefix = path_prefix != NULL ? path_prefix : "/";
  config.storage.cache_dir = vectis_lua_table_string(lua, 2, "cache_dir");
  config.storage.site_id = vectis_lua_table_string(lua, 2, "site_id");
  config.storage.max_file_bytes = vectis_lua_table_size(
      lua, 2, "max_file_bytes", config.storage.max_file_bytes);
  config.storage.max_total_bytes = vectis_lua_table_size(
      lua, 2, "max_total_bytes", config.storage.max_total_bytes);
  config.storage.max_resources = vectis_lua_table_size(
      lua, 2, "max_resources", config.storage.max_resources);
  config.auth_required = auth_required;
  config.conceal_unauthorized =
      vectis_lua_table_bool(lua, 2, "conceal_unauthorized", 1);

  auth = NULL;
  vectis_error_clear(&error);
  if (auth_required) {
    lua_getfield(lua, 2, "auth");
    if (!lua_istable(lua, -1)) {
      lua_pop(lua, 1);
      return vectis_lua_push_error_text(
          lua, VECTIS_ERR_INVALID,
          "webdav mount requires auth when auth_required is true");
    }
    auth =
        vectis_lua_server_native_auth_new(lua, -1, "webdav mount", &error);
    lua_pop(lua, 1);
    if (auth == NULL) {
      return vectis_lua_push_error(
          lua, error.code != VECTIS_OK ? error.code : VECTIS_ERR_NOMEM, &error);
    }
    config.auth = vectis_webdav_auth_provider;
    config.auth_userdata = &auth->webdav_config;
  }

  status = app->webdav(app, &config, &error);
  if (status != VECTIS_OK) {
    vectis_lua_server_native_auth_free(auth);
    return vectis_lua_push_error(lua, status, &error);
  }
  if (auth != NULL) {
    vectis_lua_server_native_auth_retain(server, auth);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_server_webdav_embedded_site(lua_State *lua) {
  vectis_lua_runtime_context *context;
  vectis_lua_server *server;
  vectis_app *app;
  vectis_webdav_embedded_site_config config;
  vectis_lua_server_native_auth *auth;
  vectis_error error;
  vectis_status status;
  const char *path_prefix;
  const char *extract_policy;
  int auth_required;

  server = vectis_lua_check_server(lua, 1);
  app = vectis_lua_server_app(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  if (context == NULL || context->embedded_fs == NULL) {
    return vectis_lua_push_error_text(
        lua, VECTIS_ERR_INVALID, "WebDAV embedded site requires packed assets");
  }

  path_prefix = vectis_lua_table_string(lua, 2, "path_prefix");
  if (path_prefix == NULL) {
    path_prefix = vectis_lua_table_string(lua, 2, "prefix");
  }
  extract_policy = vectis_lua_table_string(lua, 2, "extract_policy");
  if (extract_policy == NULL) {
    extract_policy = vectis_lua_table_string(lua, 2, "extract_mode");
  }
  auth_required = vectis_lua_table_bool(lua, 2, "auth_required", 1);

  vectis_webdav_embedded_site_config_init(&config);
  config.path_prefix = path_prefix != NULL ? path_prefix : "/";
  config.storage.cache_dir = vectis_lua_table_string(lua, 2, "cache_dir");
  config.storage.site_id = vectis_lua_table_string(lua, 2, "site_id");
  config.storage.max_file_bytes = vectis_lua_table_size(
      lua, 2, "max_file_bytes", config.storage.max_file_bytes);
  config.storage.max_total_bytes = vectis_lua_table_size(
      lua, 2, "max_total_bytes", config.storage.max_total_bytes);
  config.storage.max_resources = vectis_lua_table_size(
      lua, 2, "max_resources", config.storage.max_resources);
  config.fs = context->embedded_fs;
  if (extract_policy != NULL) {
    config.extract_policy =
        vectis_lua_embedded_extract_policy(lua, extract_policy);
  }
  config.auth_required = auth_required;
  config.conceal_unauthorized =
      vectis_lua_table_bool(lua, 2, "conceal_unauthorized", 1);

  auth = NULL;
  vectis_error_clear(&error);
  if (auth_required) {
    lua_getfield(lua, 2, "auth");
    if (!lua_istable(lua, -1)) {
      lua_pop(lua, 1);
      return vectis_lua_push_error_text(
          lua, VECTIS_ERR_INVALID,
          "webdav embedded site requires auth when auth_required is true");
    }
    auth = vectis_lua_server_native_auth_new(lua, -1, "webdav embedded site",
                                             &error);
    lua_pop(lua, 1);
    if (auth == NULL) {
      return vectis_lua_push_error(
          lua, error.code != VECTIS_OK ? error.code : VECTIS_ERR_NOMEM, &error);
    }
    config.auth = vectis_webdav_auth_provider;
    config.auth_userdata = &auth->webdav_config;
  }

  status = app->webdav_embedded_site(app, &config, &error);
  if (status != VECTIS_OK) {
    vectis_lua_server_native_auth_free(auth);
    return vectis_lua_push_error(lua, status, &error);
  }
  if (auth != NULL) {
    vectis_lua_server_native_auth_retain(server, auth);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static vectis_status
vectis_lua_auth_json_response(vectis_response *response, int status_code,
                              const char *content_type, const void *body,
                              size_t body_size, const char *fallback_body,
                              vectis_error *error) {
  vectis_bytes bytes;

  if (body != NULL || body_size > 0u) {
    bytes.data = body;
    bytes.size = body_size;
    return vectis_response_bytes(response, status_code, content_type, bytes,
                                 error);
  }
  return vectis_response_text(response, status_code, content_type,
                              fallback_body, error);
}

static vectis_status vectis_lua_server_json_dispatch(vectis_app *app,
                                                     vectis_request *request,
                                                     vectis_response *response,
                                                     void *userdata,
                                                     vectis_error *error) {
  vectis_lua_server_json_route *route;
  vectis_status status;

  (void)app;
  (void)request;
  route = (vectis_lua_server_json_route *)userdata;
  if (route == NULL) {
    vectis_cli_error_set(error, VECTIS_ERR_INVALID,
                         "JSON route configuration is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->cache_control != NULL && route->cache_control[0] != '\0') {
    status = vectis_response_header(response, "cache-control",
                                    route->cache_control, error);
    if (status != VECTIS_OK) {
      return status;
    }
  }
  return vectis_response_text(response, route->status_code, route->content_type,
                              route->body, error);
}

static vectis_status
vectis_lua_server_auth_json_dispatch(vectis_app *app, vectis_request *request,
                                     vectis_response *response, void *userdata,
                                     vectis_error *error) {
  vectis_lua_server_auth_json_route *route;
  vectis_auth_provider_request auth_request;
  vectis_auth_provider_response auth_response;
  vectis_status status;
  int status_code;

  (void)app;
  route = (vectis_lua_server_auth_json_route *)userdata;
  if (route == NULL || route->auth == NULL) {
    vectis_cli_error_set(error, VECTIS_ERR_INVALID,
                         "auth JSON route configuration is required");
    return VECTIS_ERR_INVALID;
  }

  vectis_auth_provider_request_init(&auth_request);
  auth_request.request = request;
  auth_request.purpose = route->purpose;
  auth_request.resource = vectis_request_path(request);
  auth_request.allowed_auth_modes = route->auth->allowed_auth_modes;
  vectis_auth_provider_response_init(&auth_response);
  status = vectis_auth_provider_authenticate(
      &route->auth->provider, &auth_request, &auth_response, error);
  if (status != VECTIS_OK) {
    vectis_auth_provider_response_cleanup(&auth_response);
    return status;
  }

  switch (auth_response.action) {
  case VECTIS_AUTH_ALLOW:
    status =
        vectis_response_header(response, "cache-control", "no-store", error);
    if (status == VECTIS_OK) {
      status = vectis_response_text(response, route->status_code,
                                    route->content_type, route->body, error);
    }
    break;
  case VECTIS_AUTH_REQUIRED:
    if (auth_response.www_authenticate[0] != '\0') {
      status = vectis_response_header(response, "www-authenticate",
                                      auth_response.www_authenticate, error);
      if (status != VECTIS_OK) {
        break;
      }
    }
    status_code =
        auth_response.status_code > 0 ? auth_response.status_code : 401;
    status = vectis_lua_auth_json_response(
        response, status_code,
        auth_response.content_type != NULL ? auth_response.content_type
                                           : "text/plain; charset=utf-8",
        auth_response.body, auth_response.body_size,
        "authentication required\n", error);
    break;
  case VECTIS_AUTH_REDIRECT:
    if (auth_response.location != NULL && auth_response.location[0] != '\0') {
      status = vectis_response_header(response, "location",
                                      auth_response.location, error);
      if (status != VECTIS_OK) {
        break;
      }
    }
    status_code =
        auth_response.status_code > 0 ? auth_response.status_code : 302;
    status = vectis_lua_auth_json_response(
        response, status_code,
        auth_response.content_type != NULL ? auth_response.content_type
                                           : "text/plain; charset=utf-8",
        auth_response.body, auth_response.body_size,
        "authentication required\n", error);
    break;
  case VECTIS_AUTH_DENY:
  default:
    status_code =
        auth_response.status_code > 0 ? auth_response.status_code : 403;
    status = vectis_lua_auth_json_response(
        response, status_code,
        auth_response.content_type != NULL ? auth_response.content_type
                                           : "text/plain; charset=utf-8",
        auth_response.body, auth_response.body_size, "forbidden\n", error);
    break;
  }

  vectis_auth_provider_response_cleanup(&auth_response);
  return status;
}

static int vectis_lua_server_json(lua_State *lua) {
  vectis_lua_server *server;
  vectis_app *app;
  vectis_lua_server_json_route *route_data;
  vectis_route_config route;
  vectis_error error;
  vectis_status status;
  const char *path;
  const char *body;
  const char *content_type;
  const char *cache_control;
  vectis_http_methods methods;
  size_t status_code;

  server = vectis_lua_check_server(lua, 1);
  app = vectis_lua_server_app(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  path = vectis_lua_table_string(lua, 2, "path");
  if (path == NULL || path[0] == '\0') {
    return vectis_lua_push_error_text(lua, VECTIS_ERR_INVALID,
                                      "JSON route path is required");
  }
  body = vectis_lua_table_string(lua, 2, "body");
  if (body == NULL) {
    body = "{\"ok\":true}\n";
  }
  content_type = vectis_lua_table_string(lua, 2, "content_type");
  if (content_type == NULL) {
    content_type = "application/json";
  }
  cache_control = vectis_lua_table_string(lua, 2, "cache_control");
  methods =
      vectis_lua_route_methods(lua, 2, VECTIS_HTTP_METHODS_GET, "JSON route");
  status_code = vectis_lua_table_size(lua, 2, "status_code", 0u);
  if (status_code == 0u) {
    status_code = vectis_lua_table_size(lua, 2, "status", 200u);
  }
  if (status_code < 100u || status_code > 599u) {
    return luaL_error(lua, "JSON route status must be between 100 and 599");
  }

  route_data = (vectis_lua_server_json_route *)calloc(1u, sizeof(*route_data));
  if (route_data == NULL) {
    return vectis_lua_push_error_text(lua, VECTIS_ERR_NOMEM,
                                      "failed to allocate JSON route");
  }
  route_data->body = vectis_cli_strdup(body);
  route_data->content_type = vectis_cli_strdup(content_type);
  if (cache_control != NULL) {
    route_data->cache_control = vectis_cli_strdup(cache_control);
  }
  route_data->status_code = (int)status_code;
  if (route_data->body == NULL || route_data->content_type == NULL ||
      (cache_control != NULL && route_data->cache_control == NULL)) {
    vectis_lua_server_json_route_free(route_data);
    return vectis_lua_push_error_text(lua, VECTIS_ERR_NOMEM,
                                      "failed to copy JSON route config");
  }

  route = vectis_route_methods(methods, path, vectis_lua_server_json_dispatch,
                               route_data);
  vectis_error_clear(&error);
  status = app->route(app, &route, &error);
  if (status != VECTIS_OK) {
    vectis_lua_server_json_route_free(route_data);
    return vectis_lua_push_error(lua, status, &error);
  }

  route_data->next = server->json_routes;
  server->json_routes = route_data;
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_server_auth_json(lua_State *lua) {
  vectis_lua_server *server;
  vectis_app *app;
  vectis_lua_server_native_auth *auth;
  vectis_lua_server_auth_json_route *route_data;
  vectis_route_config route;
  vectis_error error;
  vectis_status status;
  const char *path;
  const char *body;
  const char *content_type;
  vectis_http_methods methods;
  size_t status_code;

  server = vectis_lua_check_server(lua, 1);
  app = vectis_lua_server_app(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  path = vectis_lua_table_string(lua, 2, "path");
  if (path == NULL || path[0] == '\0') {
    return vectis_lua_push_error_text(lua, VECTIS_ERR_INVALID,
                                      "auth JSON route path is required");
  }
  body = vectis_lua_table_string(lua, 2, "body");
  if (body == NULL) {
    body = "{\"ok\":true}\n";
  }
  content_type = vectis_lua_table_string(lua, 2, "content_type");
  if (content_type == NULL) {
    content_type = "application/json";
  }
  methods = vectis_lua_route_methods(lua, 2, VECTIS_HTTP_METHODS_GET,
                                     "auth JSON route");
  status_code = vectis_lua_table_size(lua, 2, "status_code", 0u);
  if (status_code == 0u) {
    status_code = vectis_lua_table_size(lua, 2, "status", 200u);
  }
  if (status_code < 100u || status_code > 599u) {
    return luaL_error(lua,
                      "auth JSON route status must be between 100 and 599");
  }

  lua_getfield(lua, 2, "auth");
  if (!lua_istable(lua, -1)) {
    lua_pop(lua, 1);
    return vectis_lua_push_error_text(lua, VECTIS_ERR_INVALID,
                                      "auth JSON route auth is required");
  }
  vectis_error_clear(&error);
  auth = vectis_lua_server_native_auth_new(lua, -1, "auth JSON route", &error);
  lua_pop(lua, 1);
  if (auth == NULL) {
    return vectis_lua_push_error(
        lua, error.code != VECTIS_OK ? error.code : VECTIS_ERR_NOMEM, &error);
  }

  route_data =
      (vectis_lua_server_auth_json_route *)calloc(1u, sizeof(*route_data));
  if (route_data == NULL) {
    vectis_lua_server_native_auth_free(auth);
    return vectis_lua_push_error_text(lua, VECTIS_ERR_NOMEM,
                                      "failed to allocate auth JSON route");
  }
  route_data->body = vectis_cli_strdup(body);
  route_data->content_type = vectis_cli_strdup(content_type);
  route_data->purpose = vectis_cli_strdup(auth->purpose);
  route_data->status_code = (int)status_code;
  route_data->auth = auth;
  if (route_data->body == NULL || route_data->content_type == NULL ||
      route_data->purpose == NULL) {
    vectis_lua_server_auth_json_route_free(route_data);
    vectis_lua_server_native_auth_free(auth);
    return vectis_lua_push_error_text(lua, VECTIS_ERR_NOMEM,
                                      "failed to copy auth JSON route config");
  }

  route = vectis_route_methods(
      methods, path, vectis_lua_server_auth_json_dispatch, route_data);
  vectis_error_clear(&error);
  status = app->route(app, &route, &error);
  if (status != VECTIS_OK) {
    vectis_lua_server_auth_json_route_free(route_data);
    vectis_lua_server_native_auth_free(auth);
    return vectis_lua_push_error(lua, status, &error);
  }

  vectis_lua_server_native_auth_retain(server, auth);
  route_data->next = server->auth_json_routes;
  server->auth_json_routes = route_data;
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_server_auth_routes(lua_State *lua) {
  vectis_app *app;
  vectis_auth_routes_config config;
  vectis_error error;
  vectis_status status;
  const char *path_prefix;
  const char *credentials_path;
  const char *state_path;
  const char *realm;
  const char *login_title;
  const char *login_template_html;
  const char *login_template_path;
  const char *login_template_embedded_path;
  const char **smtp_allowed_recipients;
  vectis_lua_runtime_context *context;
  size_t smtp_allowed_recipient_count;
  int email_token_index;
  int smtp_index;

  app = vectis_lua_server_app(lua, 1);
  luaL_checktype(lua, 2, LUA_TTABLE);
  smtp_allowed_recipients = NULL;
  smtp_allowed_recipient_count = 0u;
  path_prefix = vectis_lua_table_string(lua, 2, "path_prefix");
  if (path_prefix == NULL) {
    path_prefix = vectis_lua_table_string(lua, 2, "prefix");
  }
  credentials_path = vectis_lua_table_string(lua, 2, "credentials_path");
  if (credentials_path == NULL) {
    credentials_path = vectis_lua_table_string(lua, 2, "path");
  }
  state_path = vectis_lua_table_string(lua, 2, "state_path");
  if (state_path == NULL) {
    state_path = vectis_lua_table_string(lua, 2, "auth_state_path");
  }

  vectis_auth_routes_config_init(&config);
  if (path_prefix != NULL) {
    config.path_prefix = path_prefix;
  }
  config.store.credentials_path = credentials_path;
  config.store.state_path = state_path;
  config.store.max_store_bytes = vectis_lua_table_size(
      lua, 2, "max_store_bytes", config.store.max_store_bytes);
  realm = vectis_lua_table_string(lua, 2, "realm");
  if (realm != NULL) {
    config.realm = realm;
  }
  login_title = vectis_lua_table_string(lua, 2, "login_title");
  if (login_title != NULL) {
    config.login_title = login_title;
  }
  login_template_html = vectis_lua_table_string(lua, 2, "login_template_html");
  if (login_template_html != NULL) {
    config.login_template_html = login_template_html;
  }
  login_template_path = vectis_lua_table_string(lua, 2, "login_template_path");
  if (login_template_path == NULL) {
    login_template_path = vectis_lua_table_string(lua, 2, "template_path");
  }
  if (login_template_path != NULL) {
    config.login_template_path = login_template_path;
  }
  login_template_embedded_path =
      vectis_lua_table_string(lua, 2, "login_template_embedded_path");
  if (login_template_embedded_path == NULL) {
    login_template_embedded_path =
        vectis_lua_table_string(lua, 2, "template_embedded_path");
  }
  if (login_template_embedded_path != NULL) {
    context =
        (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(lua);
    if (context == NULL || context->embedded_fs == NULL) {
      return vectis_lua_push_error_text(
          lua, VECTIS_ERR_INVALID,
          "auth route embedded login template requires packed assets");
    }
    config.login_template_embedded_path = login_template_embedded_path;
    config.login_template_fs = context->embedded_fs;
  }
  config.max_body_bytes =
      vectis_lua_table_size(lua, 2, "max_body_bytes", config.max_body_bytes);
  config.unix_seconds = (uint64_t)vectis_lua_table_size(lua, 2, "time", 0u);
  config.totp_window =
      (unsigned int)vectis_lua_table_size(lua, 2, "window", 0u);
  config.require_email_token =
      vectis_lua_table_bool(lua, 2, "require_email_token", 0);
  (void)vectis_lua_auth_required_factors(lua, 2, "required_factors",
                                         &config.required_factors);
  config.email_token_ttl_seconds = (uint64_t)vectis_lua_table_size(
      lua, 2, "email_token_ttl_seconds", config.email_token_ttl_seconds);
  config.email_token_max_attempts = (unsigned int)vectis_lua_table_size(
      lua, 2, "email_token_max_attempts", config.email_token_max_attempts);
  lua_getfield(lua, 2, "email_token");
  if (!lua_isnil(lua, -1)) {
    luaL_checktype(lua, -1, LUA_TTABLE);
    email_token_index = lua_gettop(lua);
    config.email_token_ttl_seconds = (uint64_t)vectis_lua_table_size(
        lua, email_token_index, "ttl_seconds", config.email_token_ttl_seconds);
    config.email_token_max_attempts = (unsigned int)vectis_lua_table_size(
        lua, email_token_index, "max_attempts",
        config.email_token_max_attempts);
  }
  lua_pop(lua, 1);
  lua_getfield(lua, 2, "email_smtp");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_getfield(lua, 2, "smtp");
  }
  if (!lua_isnil(lua, -1)) {
    luaL_checktype(lua, -1, LUA_TTABLE);
    smtp_index = lua_gettop(lua);
    config.email_smtp.url = vectis_lua_table_string(lua, smtp_index, "url");
    config.email_smtp.mail_from =
        vectis_lua_table_string(lua, smtp_index, "mail_from");
    config.email_smtp.username =
        vectis_lua_table_string(lua, smtp_index, "username");
    config.email_smtp.password =
        vectis_lua_table_string(lua, smtp_index, "password");
    config.email_smtp.subject =
        vectis_lua_table_string(lua, smtp_index, "subject");
    config.email_smtp.ca_bundle_path =
        vectis_lua_table_string(lua, smtp_index, "ca_bundle_path");
    config.email_smtp.timeout_ms = vectis_lua_table_long(
        lua, smtp_index, "timeout_ms", config.email_smtp.timeout_ms);
    config.email_smtp.connect_timeout_ms =
        vectis_lua_table_long(lua, smtp_index, "connect_timeout_ms",
                              config.email_smtp.connect_timeout_ms);
    config.email_smtp.use_ssl =
        vectis_lua_table_bool(lua, smtp_index, "use_ssl", 0);
    config.email_smtp.tls_verify_peer_disabled =
        !vectis_lua_table_bool(lua, smtp_index, "verify_peer", 1);
    config.email_smtp.tls_verify_host_disabled =
        !vectis_lua_table_bool(lua, smtp_index, "verify_host", 1);
    config.email_smtp.allowed_recipient_domain =
        vectis_lua_table_string(lua, smtp_index, "allowed_recipient_domain");
    if (config.email_smtp.allowed_recipient_domain == NULL) {
      config.email_smtp.allowed_recipient_domain =
          vectis_lua_table_string(lua, smtp_index, "allowed_domain");
    }
    smtp_allowed_recipients = vectis_lua_string_array_field(
        lua, smtp_index, "allowed_recipients", &smtp_allowed_recipient_count);
    config.email_smtp.allowed_recipients = smtp_allowed_recipients;
    config.email_smtp.allowed_recipient_count = smtp_allowed_recipient_count;
  }
  lua_pop(lua, 1);

  vectis_error_clear(&error);
  status = app->auth_routes(app, &config, &error);
  free((void *)smtp_allowed_recipients);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_server_new(lua_State *lua) {
  vectis_lua_server *server;
  vectis_app_config config;
  vectis_error error;
  const char *app_name;
  const char *bind;
  const char *mode;
  const char **lockd_endpoints;
  const char **tls_domains;
  size_t port;
  int tls_index;

  luaL_checktype(lua, 1, LUA_TTABLE);
  lockd_endpoints = NULL;
  tls_domains = NULL;
  port = vectis_lua_table_size(lua, 1, "port", 8080u);
  if (port == 0u || port > 65535u) {
    return luaL_error(lua, "server port must be between 1 and 65535");
  }
  app_name = vectis_lua_table_string(lua, 1, "app_name");
  bind = vectis_lua_table_string(lua, 1, "bind");
  vectis_app_config_init(&config);
  config.app_name = app_name != NULL ? app_name : "vectis-lua";
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = bind != NULL ? bind : "127.0.0.1";
  config.tls.port = (unsigned short)port;
  lua_getfield(lua, 1, "tls");
  tls_index = lua_absindex(lua, -1);
  if (!lua_isnil(lua, tls_index)) {
    if (!lua_istable(lua, tls_index)) {
      return luaL_error(lua, "server tls must be a table");
    }
    mode = vectis_lua_table_string(lua, tls_index, "mode");
    if (!vectis_lua_tls_mode(mode, &config.tls.mode)) {
      return luaL_error(lua,
                        "server tls.mode must be disabled, manual, or acme");
    }
    bind = vectis_lua_table_string(lua, tls_index, "bind");
    if (bind != NULL) {
      config.tls.bind = bind;
    }
    port = vectis_lua_table_size(lua, tls_index, "port", config.tls.port);
    if (port == 0u || port > 65535u) {
      return luaL_error(lua, "server tls.port must be between 1 and 65535");
    }
    config.tls.port = (unsigned short)port;
    tls_domains = vectis_lua_string_array_field(lua, tls_index, "domains",
                                                &config.tls.domain_count);
    config.tls.domains = tls_domains;
    config.tls.domain = vectis_lua_table_string(lua, tls_index, "domain");
    config.tls.cert_key_bundle_path =
        vectis_lua_table_string(lua, tls_index, "cert_key_bundle_path");
    if (config.tls.cert_key_bundle_path == NULL) {
      config.tls.cert_key_bundle_path =
          vectis_lua_table_string(lua, tls_index, "bundle_path");
    }
    config.tls.certificate_path =
        vectis_lua_table_string(lua, tls_index, "certificate_path");
    if (config.tls.certificate_path == NULL) {
      config.tls.certificate_path =
          vectis_lua_table_string(lua, tls_index, "cert_path");
    }
    config.tls.private_key_path =
        vectis_lua_table_string(lua, tls_index, "private_key_path");
    if (config.tls.private_key_path == NULL) {
      config.tls.private_key_path =
          vectis_lua_table_string(lua, tls_index, "key_path");
    }
    config.tls.ca_bundle_path =
        vectis_lua_table_string(lua, tls_index, "ca_bundle_path");
    if (config.tls.ca_bundle_path == NULL) {
      config.tls.ca_bundle_path =
          vectis_lua_table_string(lua, tls_index, "ca_path");
    }
    config.tls.client_ca_bundle_path =
        vectis_lua_table_string(lua, tls_index, "client_ca_bundle_path");
    if (config.tls.client_ca_bundle_path == NULL) {
      config.tls.client_ca_bundle_path =
          vectis_lua_table_string(lua, tls_index, "client_ca_path");
    }
    config.tls.require_client_certificate =
        vectis_lua_table_bool(lua, tls_index, "require_client_certificate", 0);
    config.tls.acme_email =
        vectis_lua_table_string(lua, tls_index, "acme_email");
    if (config.tls.acme_email == NULL) {
      config.tls.acme_email = vectis_lua_table_string(lua, tls_index, "email");
    }
    config.tls.acme_directory_url =
        vectis_lua_table_string(lua, tls_index, "acme_directory_url");
    if (config.tls.acme_directory_url == NULL) {
      config.tls.acme_directory_url =
          vectis_lua_table_string(lua, tls_index, "provider");
    }
    config.tls.acme_state_dir =
        vectis_lua_table_string(lua, tls_index, "acme_state_dir");
    if (config.tls.acme_state_dir == NULL) {
      config.tls.acme_state_dir =
          vectis_lua_table_string(lua, tls_index, "cache_dir");
    }
  }
  lua_pop(lua, 1);
  vectis_lua_parse_lockd_config(lua, 1, &config.lockd, &lockd_endpoints);
  server = (vectis_lua_server *)lua_newuserdata(lua, sizeof(*server));
  server->app = NULL;
  server->json_routes = NULL;
  server->native_auths = NULL;
  server->auth_json_routes = NULL;
  server->consumer_services = NULL;
  vectis_error_clear(&error);
  server->app = vectis_app_new(&config, &error);
  free(tls_domains);
  free(lockd_endpoints);
  if (server->app == NULL) {
    return vectis_lua_push_error(
        lua, error.code != VECTIS_OK ? error.code : VECTIS_ERR_NOMEM, &error);
  }
  luaL_getmetatable(lua, VECTIS_LUA_SERVER);
  lua_setmetatable(lua, -2);
  return 1;
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

static int vectis_lua_table_is_nil(lua_State *lua, int index,
                                   const char *field) {
  int is_nil;

  lua_getfield(lua, index, field);
  is_nil = lua_isnil(lua, -1);
  lua_pop(lua, 1);
  return is_nil;
}

static void vectis_lua_xml_parse_config(lua_State *lua, int index,
                                        vectis_xml_config *config) {
  const char *root_element;
  const char *text_key;
  const char *attribute_prefix;

  *config = vectis_xml_default();
  root_element = vectis_lua_table_string(lua, index, "root_element");
  text_key = vectis_lua_table_string(lua, index, "text_key");
  attribute_prefix = vectis_lua_table_string(lua, index, "attribute_prefix");
  if (root_element != NULL) {
    config->root_element = root_element;
  }
  if (text_key != NULL) {
    config->text_key = text_key;
  }
  if (attribute_prefix != NULL) {
    config->attribute_prefix = attribute_prefix;
  }
  config->trim_text = vectis_lua_table_bool(lua, index, "trim_text",
                                            config->trim_text);
  config->max_depth =
      vectis_lua_table_size(lua, index, "max_depth", config->max_depth);
  config->max_text_bytes = vectis_lua_table_size(
      lua, index, "max_text_bytes", config->max_text_bytes);
  if (!vectis_lua_table_is_nil(lua, index, "skip_unknown")) {
    config->skip_unknown_disabled =
        vectis_lua_table_bool(lua, index, "skip_unknown", 1) ? 0 : 1;
  }
  if (!vectis_lua_table_is_nil(lua, index, "strict_unknown")) {
    config->skip_unknown_disabled =
        vectis_lua_table_bool(lua, index, "strict_unknown", 0) ? 1 : 0;
  }
}

static int vectis_lua_xml_source_from_options(lua_State *lua, int index,
                                              vectis_source *source) {
  const char *path;
  const char *data;
  size_t data_size;
  int source_count;

  source_count = 0;
  data = NULL;
  data_size = 0u;
  lua_getfield(lua, index, "xml");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_getfield(lua, index, "data");
  }
  if (!lua_isnil(lua, -1)) {
    data = luaL_checklstring(lua, -1, &data_size);
    *source = vectis_source_from_memory(data, data_size);
    ++source_count;
  } else {
    lua_pop(lua, 1);
  }
  path = vectis_lua_table_string(lua, index, "path");
  if (path != NULL) {
    if (path[0] == '\0') {
      return luaL_error(lua, "vectis.xml path must not be empty");
    }
    *source = vectis_source_from_path(path);
    ++source_count;
  }
  if (source_count != 1) {
    return luaL_error(lua, "vectis.xml requires exactly one of xml, data, or "
                           "path");
  }
  return 0;
}

static int vectis_lua_xml_parse_common(lua_State *lua, int return_record) {
  lonejson_schema_view schema;
  lonejson_record_view record;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_xml_config config;
  vectis_source source;
  vectis_error error;
  vectis_status status;
  int original_top;
  int schema_index;
  int record_index;
  int table_index;

  luaL_checktype(lua, 1, LUA_TTABLE);
  original_top = lua_gettop(lua);
  lua_getfield(lua, 1, "schema");
  if (lua_isnil(lua, -1)) {
    return luaL_error(lua, "vectis.xml requires schema");
  }
  schema_index = lua_gettop(lua);
  (void)vectis_lua_lonejson_check_schema(lua, schema_index, &schema,
                                         "vectis.xml schema");
  vectis_lua_xml_parse_config(lua, 1, &config);
  if (vectis_lua_xml_source_from_options(lua, 1, &source) != 0) {
    return 1;
  }

  memset(&json_error, 0, sizeof(json_error));
  vectis_lua_lonejson_record_view_init(&record);
  json_status =
      lonejson_lua_new_record(lua, schema_index, &record_index, &record,
                              &json_error);
  if (json_status != LONEJSON_STATUS_OK) {
    lua_settop(lua, original_top);
    return vectis_lua_push_error_text(
        lua, VECTIS_ERR_INVALID,
        json_error.message[0] != '\0' ? json_error.message
                                      : "lonejson record allocation failed");
  }

  vectis_error_clear(&error);
  status = vectis_xml_parse_lonejson_source(&source, schema.map, &config,
                                            record.record, &error);
  if (status != VECTIS_OK) {
    lua_settop(lua, original_top);
    return vectis_lua_push_error(lua, status, &error);
  }
  if (return_record) {
    return 1;
  }
  table_index = lonejson_lua_record_to_table(lua, record_index);
  if (table_index == 0) {
    lua_settop(lua, original_top);
    return vectis_lua_push_error_text(lua, VECTIS_ERR_INVALID,
                                      "lonejson record conversion failed");
  }
  return 1;
}

static int vectis_lua_xml_parse(lua_State *lua) {
  return vectis_lua_xml_parse_common(lua, 0);
}

static int vectis_lua_xml_parse_record(lua_State *lua) {
  return vectis_lua_xml_parse_common(lua, 1);
}

static int vectis_lua_dsv_char_option(lua_State *lua, int index,
                                      const char *field, int fallback) {
  const char *value;
  size_t size;
  int result;

  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return fallback;
  }
  if (lua_isinteger(lua, -1)) {
    result = (int)lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    return result;
  }
  value = luaL_checklstring(lua, -1, &size);
  if (size == 0u) {
    return luaL_error(lua, "vectis.dsv %s must not be empty", field);
  }
  result = (unsigned char)value[0];
  lua_pop(lua, 1);
  return result;
}

static int vectis_lua_dsv_columns(lua_State *lua, int index,
                                  vectis_dsv_config *config,
                                  const char ***owned_columns) {
  const char **columns;
  size_t count;
  size_t i;

  *owned_columns = NULL;
  lua_getfield(lua, index, "columns");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return 0;
  }
  luaL_checktype(lua, -1, LUA_TTABLE);
  count = (size_t)lua_rawlen(lua, -1);
  if (count == 0u) {
    lua_pop(lua, 1);
    return luaL_error(lua, "vectis.dsv columns must not be empty");
  }
  columns = (const char **)calloc(count, sizeof(columns[0]));
  if (columns == NULL) {
    lua_pop(lua, 1);
    return luaL_error(lua, "failed to allocate DSV columns");
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, -1, (lua_Integer)i + 1);
    columns[i] = luaL_checkstring(lua, -1);
    lua_pop(lua, 1);
  }
  lua_pop(lua, 1);
  config->columns = columns;
  config->column_count = count;
  *owned_columns = columns;
  return 0;
}

static int vectis_lua_dsv_parse_config(lua_State *lua, int index,
                                       vectis_dsv_config *config,
                                       const char ***owned_columns) {
  const char *format;
  const char *comment_prefix;

  format = vectis_lua_table_string(lua, index, "format");
  if (format != NULL && strcmp(format, "tsv") == 0) {
    *config = vectis_dsv_tsv();
  } else {
    *config = vectis_dsv_csv();
  }
  if (!vectis_lua_table_is_nil(lua, index, "header")) {
    config->header_disabled =
        vectis_lua_table_bool(lua, index, "header", 1) ? 0 : 1;
  }
  if (!vectis_lua_table_is_nil(lua, index, "headerless")) {
    config->header_disabled =
        vectis_lua_table_bool(lua, index, "headerless", 0) ? 1 : 0;
  }
  if (!vectis_lua_table_is_nil(lua, index, "rows_only")) {
    config->header_disabled =
        vectis_lua_table_bool(lua, index, "rows_only", 0) ? 1 : 0;
  }
  config->delimiter =
      vectis_lua_dsv_char_option(lua, index, "delimiter", config->delimiter);
  config->quote = vectis_lua_dsv_char_option(lua, index, "quote",
                                             config->quote);
  config->escape = vectis_lua_dsv_char_option(lua, index, "escape",
                                              config->escape);
  config->max_field_bytes =
      vectis_lua_table_size(lua, index, "max_field_bytes",
                            config->max_field_bytes);
  comment_prefix = vectis_lua_table_string(lua, index, "comment_prefix");
  if (comment_prefix != NULL) {
    config->comment_prefix = comment_prefix;
  }
  if (!vectis_lua_table_is_nil(lua, index, "strict_row_width")) {
    config->strict_row_width_disabled =
        vectis_lua_table_bool(lua, index, "strict_row_width", 1) ? 0 : 1;
  }
  if (!vectis_lua_table_is_nil(lua, index, "trim_cr")) {
    config->trim_cr_disabled =
        vectis_lua_table_bool(lua, index, "trim_cr", 1) ? 0 : 1;
  }
  if (!vectis_lua_table_is_nil(lua, index, "indented_comments")) {
    config->indented_comments_disabled =
        vectis_lua_table_bool(lua, index, "indented_comments", 1) ? 0 : 1;
  }
  return vectis_lua_dsv_columns(lua, index, config, owned_columns);
}

static int vectis_lua_dsv_source_from_options(lua_State *lua, int index,
                                              vectis_source *source) {
  const char *path;
  const char *data;
  size_t data_size;
  int source_count;

  source_count = 0;
  data = NULL;
  data_size = 0u;
  lua_getfield(lua, index, "data");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_getfield(lua, index, "dsv");
  }
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_getfield(lua, index, "csv");
  }
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_getfield(lua, index, "tsv");
  }
  if (!lua_isnil(lua, -1)) {
    data = luaL_checklstring(lua, -1, &data_size);
    *source = vectis_source_from_memory(data, data_size);
    ++source_count;
  } else {
    lua_pop(lua, 1);
  }
  path = vectis_lua_table_string(lua, index, "path");
  if (path != NULL) {
    if (path[0] == '\0') {
      return luaL_error(lua, "vectis.dsv path must not be empty");
    }
    *source = vectis_source_from_path(path);
    ++source_count;
  }
  if (source_count != 1) {
    return luaL_error(lua, "vectis.dsv requires exactly one of data, dsv, csv, "
                           "tsv, or path");
  }
  return 0;
}

static void vectis_lua_dsv_push_spill_result(
    lua_State *lua, const vectis_body_spill_result *result) {
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)result->size);
  lua_setfield(lua, -2, "size");
  lua_pushboolean(lua, result->spooled_to_disk ? 1 : 0);
  lua_setfield(lua, -2, "spooled_to_disk");
  if (result->spooled_to_disk) {
    lua_pushstring(lua, result->path);
    lua_setfield(lua, -2, "path");
  } else {
    lua_pushlstring(lua, (const char *)result->memory.data,
                    result->memory.size);
    lua_setfield(lua, -2, "data");
    lua_pushvalue(lua, -1);
    lua_setfield(lua, -2, "memory");
  }
}

static int vectis_lua_dsv_schema(lua_State *lua, int options_index,
                                 lonejson_schema_view *schema,
                                 int *schema_index) {
  lua_getfield(lua, options_index, "schema");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    *schema_index = 0;
    return 0;
  }
  *schema_index = lua_gettop(lua);
  (void)vectis_lua_lonejson_check_schema(lua, *schema_index, schema,
                                         "vectis.dsv schema");
  return 0;
}

static int vectis_lua_dsv_parse_json(lua_State *lua) {
  vectis_dsv_config config;
  const char **owned_columns;
  vectis_source source;
  vectis_mutable_bytes out;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  if (vectis_lua_dsv_parse_config(lua, 1, &config, &owned_columns) != 0) {
    return 1;
  }
  if (vectis_lua_dsv_source_from_options(lua, 1, &source) != 0) {
    free((void *)owned_columns);
    return 1;
  }
  memset(&out, 0, sizeof(out));
  vectis_error_clear(&error);
  status = vectis_dsv_source_to_json_array(&source, &config, &out, &error);
  free((void *)owned_columns);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushlstring(lua, (const char *)out.data, out.size);
  vectis_mutable_bytes_cleanup(&out);
  return 1;
}

static int vectis_lua_dsv_parse_spill(lua_State *lua) {
  vectis_dsv_config config;
  vectis_body_spill_config spill;
  vectis_body_spill_result out;
  const char **owned_columns;
  const char *directory;
  const char *prefix;
  vectis_source source;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  if (vectis_lua_dsv_parse_config(lua, 1, &config, &owned_columns) != 0) {
    return 1;
  }
  if (vectis_lua_dsv_source_from_options(lua, 1, &source) != 0) {
    free((void *)owned_columns);
    return 1;
  }
  vectis_body_spill_config_init(&spill);
  spill.memory_limit_bytes = vectis_lua_table_size(
      lua, 1, "memory_limit_bytes", spill.memory_limit_bytes);
  directory = vectis_lua_table_string(lua, 1, "directory");
  prefix = vectis_lua_table_string(lua, 1, "prefix");
  if (directory != NULL) {
    spill.directory = directory;
  }
  if (prefix != NULL) {
    spill.prefix = prefix;
  }
  memset(&out, 0, sizeof(out));
  vectis_error_clear(&error);
  status = vectis_dsv_source_to_json_array_spill(&source, &config, &spill,
                                                 &out, &error);
  free((void *)owned_columns);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_dsv_push_spill_result(lua, &out);
  vectis_body_spill_result_cleanup(&out);
  return 1;
}

static vectis_status vectis_lua_dsv_new_row_storage(void *userdata,
                                                    size_t row_number,
                                                    void **row_storage,
                                                    vectis_error *error) {
  vectis_lua_dsv_rows_context *context;
  lonejson_record_view record;
  lonejson_error json_error;
  lonejson_status json_status;

  (void)row_number;
  context = (vectis_lua_dsv_rows_context *)userdata;
  vectis_lua_lonejson_record_view_init(&record);
  memset(&json_error, 0, sizeof(json_error));
  json_status = lonejson_lua_new_record(context->lua, context->schema_index,
                                        &context->record_index, &record,
                                        &json_error);
  if (json_status != LONEJSON_STATUS_OK) {
    vectis_cli_error_set(
        error, VECTIS_ERR_INVALID,
        json_error.message[0] != '\0' ? json_error.message
                                      : "lonejson DSV row allocation failed");
    return VECTIS_ERR_INVALID;
  }
  *row_storage = record.record;
  return VECTIS_OK;
}

static vectis_status vectis_lua_dsv_push_row(void *userdata,
                                             size_t row_number, void *row,
                                             vectis_error *error) {
  vectis_lua_dsv_rows_context *context;
  int table_index;

  (void)row;
  context = (vectis_lua_dsv_rows_context *)userdata;
  table_index = lonejson_lua_record_to_table(context->lua, context->record_index);
  if (table_index == 0) {
    vectis_cli_error_set(error, VECTIS_ERR_INVALID,
                         "failed to convert DSV row record to Lua table");
    return VECTIS_ERR_INVALID;
  }
  if (context->callback_ref != LUA_NOREF) {
    lua_rawgeti(context->lua, LUA_REGISTRYINDEX, context->callback_ref);
    lua_pushinteger(context->lua, (lua_Integer)row_number);
    lua_pushvalue(context->lua, table_index);
    if (lua_pcall(context->lua, 2, 1, 0) != LUA_OK) {
      char message[sizeof(error->message)];

      (void)snprintf(message, sizeof(message), "DSV row callback failed: %s",
                     lua_tostring(context->lua, -1));
      vectis_cli_error_set(error, VECTIS_ERR_STATE, message);
      lua_pop(context->lua, 2);
      return VECTIS_ERR_STATE;
    }
    if (lua_isboolean(context->lua, -1) && !lua_toboolean(context->lua, -1)) {
      lua_pop(context->lua, 2);
      vectis_cli_error_set(error, VECTIS_ERR_STATE, "DSV row callback stopped");
      return VECTIS_ERR_STATE;
    }
    lua_pop(context->lua, 2);
    lua_settop(context->lua, context->record_index - 1);
    return VECTIS_OK;
  }
  lua_rawseti(context->lua, context->output_index, context->next_index++);
  lua_settop(context->lua, context->record_index - 1);
  return VECTIS_OK;
}

static int vectis_lua_dsv_parse_typed_common(lua_State *lua, int callback) {
  lonejson_schema_view schema;
  vectis_dsv_config config;
  const char **owned_columns;
  vectis_source source;
  vectis_error error;
  vectis_status status;
  vectis_lua_dsv_rows_context context;
  int schema_index;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_lua_lonejson_schema_view_init(&schema);
  if (vectis_lua_dsv_schema(lua, 1, &schema, &schema_index) != 0 ||
      schema_index == 0) {
    return luaL_error(lua, "vectis.dsv typed parsing requires schema");
  }
  memset(&context, 0, sizeof(context));
  context.lua = lua;
  context.schema_index = schema_index;
  context.callback_ref = LUA_NOREF;
  context.next_index = 1;
  if (callback) {
    lua_getfield(lua, 1, "on_row");
    if (lua_isnil(lua, -1)) {
      lua_pop(lua, 1);
      lua_getfield(lua, 1, "callback");
    }
    if (!lua_isfunction(lua, -1)) {
      return luaL_error(lua, "vectis.dsv.each requires on_row callback");
    }
    context.callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  } else {
    lua_newtable(lua);
    context.output_index = lua_absindex(lua, -1);
  }
  if (vectis_lua_dsv_parse_config(lua, 1, &config, &owned_columns) != 0) {
    if (context.callback_ref != LUA_NOREF) {
      luaL_unref(lua, LUA_REGISTRYINDEX, context.callback_ref);
    }
    return 1;
  }
  if (vectis_lua_dsv_source_from_options(lua, 1, &source) != 0) {
    free((void *)owned_columns);
    if (context.callback_ref != LUA_NOREF) {
      luaL_unref(lua, LUA_REGISTRYINDEX, context.callback_ref);
    }
    return 1;
  }
  vectis_error_clear(&error);
  status = vectis_dsv_parse_lonejson_view_source(
      &source, &schema, &config, vectis_lua_dsv_new_row_storage,
      vectis_lua_dsv_push_row, &context, &error);
  free((void *)owned_columns);
  if (context.callback_ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, context.callback_ref);
  }
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  if (callback) {
    lua_pushboolean(lua, 1);
    return 1;
  }
  lua_pushvalue(lua, context.output_index);
  return 1;
}

static int vectis_lua_dsv_parse_typed(lua_State *lua) {
  return vectis_lua_dsv_parse_typed_common(lua, 0);
}

static int vectis_lua_dsv_each(lua_State *lua) {
  return vectis_lua_dsv_parse_typed_common(lua, 1);
}

static int vectis_lua_dsv_to_string(lua_State *lua) {
  lonejson_schema_view schema;
  lonejson_record_view record;
  lonejson_error json_error;
  lonejson_status json_status;
  vectis_dsv_config config;
  vectis_dsv_config row_config;
  const char **owned_columns;
  vectis_error error;
  lc_error lc_error_value;
  lc_sink *sink;
  const void *bytes;
  size_t size;
  size_t count;
  size_t i;
  int schema_index;
  int rows_index;
  int row_index;
  int record_index;
  int generated_record;
  int rc;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_lua_lonejson_schema_view_init(&schema);
  if (vectis_lua_dsv_schema(lua, 1, &schema, &schema_index) != 0 ||
      schema_index == 0) {
    return luaL_error(lua, "vectis.dsv.to_string requires schema");
  }
  lua_getfield(lua, 1, "rows");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    lua_getfield(lua, 1, "records");
  }
  luaL_checktype(lua, -1, LUA_TTABLE);
  rows_index = lua_gettop(lua);
  if (vectis_lua_dsv_parse_config(lua, 1, &config, &owned_columns) != 0) {
    return 1;
  }
  lc_error_init(&lc_error_value);
  sink = NULL;
  rc = lc_sink_to_memory(&sink, &lc_error_value);
  if (rc != LC_OK) {
    free((void *)owned_columns);
    lc_error_cleanup(&lc_error_value);
    return vectis_lua_push_error_text(lua, VECTIS_ERR_NOMEM,
                                      "failed to create DSV memory sink");
  }
  lc_error_cleanup(&lc_error_value);
  row_config = config;
  count = (size_t)lua_rawlen(lua, rows_index);
  if (count == 0u) {
    vectis_error_clear(&error);
    status = vectis_dsv_write_lonejson_rows(sink, schema.map, &row_config,
                                            NULL, 0u, 0u, &error);
    if (status != VECTIS_OK) {
      lc_sink_close(sink);
      free((void *)owned_columns);
      return vectis_lua_push_error(lua, status, &error);
    }
  }
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(lua, rows_index, (lua_Integer)i + 1);
    row_index = lua_gettop(lua);
    record_index = row_index;
    generated_record = 0;
    vectis_lua_lonejson_record_view_init(&record);
    memset(&json_error, 0, sizeof(json_error));
    json_status =
        lonejson_lua_check_record(lua, row_index, &schema, &record,
                                  &json_error);
    if (json_status != LONEJSON_STATUS_OK) {
      json_status = lonejson_lua_new_record(lua, schema_index, &record_index,
                                            &record, &json_error);
      if (json_status != LONEJSON_STATUS_OK) {
        lc_sink_close(sink);
        free((void *)owned_columns);
        return vectis_lua_push_error_text(
            lua, VECTIS_ERR_INVALID,
            json_error.message[0] != '\0'
                ? json_error.message
                : "lonejson DSV row record allocation failed");
      }
      generated_record = 1;
      (void)vectis_lua_lonejson_assign_table(lua, schema_index, record_index,
                                             row_index);
      vectis_lua_lonejson_record_view_init(&record);
      memset(&json_error, 0, sizeof(json_error));
      json_status =
          lonejson_lua_check_record(lua, record_index, &schema, &record,
                                    &json_error);
      if (json_status != LONEJSON_STATUS_OK) {
        lc_sink_close(sink);
        free((void *)owned_columns);
        return vectis_lua_push_error_text(
            lua, VECTIS_ERR_INVALID,
            json_error.message[0] != '\0' ? json_error.message
                                          : "lonejson DSV row is invalid");
      }
    }
    vectis_error_clear(&error);
    status = vectis_dsv_write_lonejson_rows(sink, schema.map, &row_config,
                                            record.record, 1u, 0u, &error);
    if (generated_record) {
      (void)lonejson_lua_clear_record(lua, record_index, NULL);
    }
    lua_settop(lua, rows_index);
    if (status != VECTIS_OK) {
      lc_sink_close(sink);
      free((void *)owned_columns);
      return vectis_lua_push_error(lua, status, &error);
    }
    row_config.header_disabled = 1;
  }
  free((void *)owned_columns);
  lc_error_init(&lc_error_value);
  rc = lc_sink_memory_bytes(sink, &bytes, &size, &lc_error_value);
  if (rc != LC_OK) {
    lc_sink_close(sink);
    lc_error_cleanup(&lc_error_value);
    return vectis_lua_push_error_text(lua, VECTIS_ERR_STATE,
                                      "failed to read DSV memory sink");
  }
  lua_pushlstring(lua, (const char *)bytes, size);
  lc_sink_close(sink);
  lc_error_cleanup(&lc_error_value);
  return 1;
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

static size_t vectis_lua_curl_read_file(char *ptr, size_t size, size_t nmemb,
                                        void *userdata) {
  vectis_lua_curl_file_upload *upload;
  size_t capacity;
  size_t bytes;

  upload = (vectis_lua_curl_file_upload *)userdata;
  if (upload == NULL || upload->file == NULL) {
    return CURL_READFUNC_ABORT;
  }
  capacity = size * nmemb;
  if (size != 0u && capacity / size != nmemb) {
    return CURL_READFUNC_ABORT;
  }
  if (capacity == 0u) {
    return 0u;
  }
  bytes = fread(ptr, 1u, capacity, upload->file);
  if (bytes == 0u && ferror(upload->file)) {
    return CURL_READFUNC_ABORT;
  }
  return bytes;
}

static size_t vectis_lua_curl_write_file(char *ptr, size_t size, size_t nmemb,
                                         void *userdata) {
  FILE *file;
  size_t bytes;

  file = (FILE *)userdata;
  bytes = size * nmemb;
  if (size != 0u && bytes / size != nmemb) {
    return 0u;
  }
  if (bytes == 0u) {
    return 0u;
  }
  if (file == NULL) {
    return 0u;
  }
  return fwrite(ptr, 1u, bytes, file) == bytes ? bytes : 0u;
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
                                        int is_smtp, int has_streaming_upload,
                                        vectis_lua_curl_file_upload *file) {
  const char *body;
  size_t body_size;

  if (is_smtp || has_streaming_upload) {
    return 0;
  }
  if (file != NULL && file->file != NULL) {
    (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                           vectis_lua_curl_read_file);
    (void)curl_easy_setopt(curl, CURLOPT_READDATA, file);
    if (file->size >= (curl_off_t)0) {
      (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, file->size);
    }
    return 1;
  }
  if (!vectis_lua_table_bool(lua, option_index, "upload", 0)) {
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
             strcmp(method, "DELETE") == 0 || strcmp(method, "OPTIONS") == 0 ||
             strcmp(method, "PROPFIND") == 0 || strcmp(method, "MKCOL") == 0 ||
             strcmp(method, "COPY") == 0 || strcmp(method, "MOVE") == 0) {
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

static void
vectis_lua_curl_retry_config_init(vectis_lua_curl_retry_config *config) {
  config->max_attempts = 1u;
  config->initial_delay_ms = 250L;
  config->max_delay_ms = 2000L;
  config->conditions = VECTIS_HTTP_RETRY_DEFAULT;
}

static void vectis_lua_curl_sleep_ms(long delay_ms) {
  struct timespec ts;

  if (delay_ms <= 0L) {
    return;
  }
  ts.tv_sec = delay_ms / 1000L;
  ts.tv_nsec = (delay_ms % 1000L) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

static int
vectis_lua_curl_retry_condition_name(lua_State *lua, const char *value,
                                     vectis_http_retry_conditions *out) {
  if (strcmp(value, "none") == 0) {
    *out = VECTIS_HTTP_RETRY_NONE;
  } else if (strcmp(value, "default") == 0) {
    *out = VECTIS_HTTP_RETRY_DEFAULT;
  } else if (strcmp(value, "transport") == 0) {
    *out = VECTIS_HTTP_RETRY_TRANSPORT;
  } else if (strcmp(value, "429") == 0 || strcmp(value, "status_429") == 0) {
    *out = VECTIS_HTTP_RETRY_429;
  } else if (strcmp(value, "5xx") == 0 || strcmp(value, "status_5xx") == 0) {
    *out = VECTIS_HTTP_RETRY_5XX;
  } else {
    return luaL_error(lua, "unsupported curl retry condition: %s", value);
  }
  return 0;
}

static int
vectis_lua_curl_retry_conditions_at(lua_State *lua, int index,
                                    vectis_http_retry_conditions fallback,
                                    vectis_http_retry_conditions *out) {
  vectis_http_retry_conditions conditions;
  vectis_http_retry_conditions current;
  size_t count;
  size_t i;

  index = lua_absindex(lua, index);
  if (lua_isnil(lua, index)) {
    *out = fallback;
    return 0;
  }
  if (lua_isnumber(lua, index)) {
    *out = (vectis_http_retry_conditions)lua_tointeger(lua, index);
    return 0;
  }
  if (lua_isstring(lua, index)) {
    return vectis_lua_curl_retry_condition_name(lua, lua_tostring(lua, index),
                                                out);
  }
  if (!lua_istable(lua, index)) {
    return luaL_error(lua, "curl retry conditions must be a string or table");
  }

  conditions = VECTIS_HTTP_RETRY_NONE;
  count = lua_rawlen(lua, index);
  for (i = 1u; i <= count; ++i) {
    lua_rawgeti(lua, index, (lua_Integer)i);
    if (lua_isnumber(lua, -1)) {
      current = (vectis_http_retry_conditions)lua_tointeger(lua, -1);
    } else {
      if (vectis_lua_curl_retry_condition_name(lua, luaL_checkstring(lua, -1),
                                               &current) != 0) {
        lua_pop(lua, 1);
        return 1;
      }
    }
    conditions |= current;
    lua_pop(lua, 1);
  }
  *out = conditions;
  return 0;
}

static int
vectis_lua_curl_retry_config_field(lua_State *lua, int index, const char *field,
                                   vectis_http_retry_conditions fallback,
                                   vectis_http_retry_conditions *out) {
  int status;

  lua_getfield(lua, index, field);
  status = vectis_lua_curl_retry_conditions_at(lua, -1, fallback, out);
  lua_pop(lua, 1);
  return status;
}

static int
vectis_lua_curl_parse_retry_config(lua_State *lua, int option_index,
                                   vectis_lua_curl_retry_config *config) {
  long value;
  int retry_index;

  vectis_lua_curl_retry_config_init(config);
  value = vectis_lua_table_long(lua, option_index, "retry_max_attempts", 0L);
  if (value > 0L) {
    config->max_attempts = (unsigned)value;
  }
  value =
      vectis_lua_table_long(lua, option_index, "retry_initial_delay_ms", -1L);
  if (value >= 0L) {
    config->initial_delay_ms = value;
  }
  value = vectis_lua_table_long(lua, option_index, "retry_max_delay_ms", -1L);
  if (value >= 0L) {
    config->max_delay_ms = value;
  }
  if (vectis_lua_curl_retry_config_field(lua, option_index, "retry_conditions",
                                         config->conditions,
                                         &config->conditions) != 0) {
    return 1;
  }

  lua_getfield(lua, option_index, "retry");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
  } else if (lua_isboolean(lua, -1)) {
    if (!lua_toboolean(lua, -1)) {
      config->max_attempts = 1u;
      config->conditions = VECTIS_HTTP_RETRY_NONE;
    }
    lua_pop(lua, 1);
  } else if (lua_istable(lua, -1)) {
    retry_index = lua_gettop(lua);
    value = vectis_lua_table_long(lua, retry_index, "max_attempts", 0L);
    if (value > 0L) {
      config->max_attempts = (unsigned)value;
    }
    value = vectis_lua_table_long(lua, retry_index, "initial_delay_ms", -1L);
    if (value >= 0L) {
      config->initial_delay_ms = value;
    }
    value = vectis_lua_table_long(lua, retry_index, "max_delay_ms", -1L);
    if (value >= 0L) {
      config->max_delay_ms = value;
    }
    if (vectis_lua_curl_retry_config_field(lua, retry_index, "conditions",
                                           config->conditions,
                                           &config->conditions) != 0) {
      lua_pop(lua, 1);
      return 1;
    }
    lua_pop(lua, 1);
  } else {
    return luaL_error(lua, "curl retry must be a table or false");
  }

  if (config->initial_delay_ms < 0L || config->max_delay_ms < 0L) {
    return luaL_error(lua, "curl retry delays must be non-negative");
  }
  if (config->conditions == VECTIS_HTTP_RETRY_NONE) {
    config->max_attempts = 1u;
  }
  return 0;
}

static int
vectis_lua_curl_retry_status(long status_code,
                             vectis_http_retry_conditions conditions) {
  if ((conditions & VECTIS_HTTP_RETRY_429) != 0u && status_code == 429L) {
    return 1;
  }
  return (conditions & VECTIS_HTTP_RETRY_5XX) != 0u && status_code >= 500L &&
         status_code <= 599L;
}

static int vectis_lua_curl_retry_code(CURLcode code,
                                      vectis_http_retry_conditions conditions) {
  return code != CURLE_OK && (conditions & VECTIS_HTTP_RETRY_TRANSPORT) != 0u;
}

static void
vectis_lua_curl_retry_delay_next(const vectis_lua_curl_retry_config *config,
                                 long *delay_ms) {
  if (*delay_ms <= 0L) {
    return;
  }
  if (*delay_ms > LONG_MAX / 2L) {
    if (config->max_delay_ms > 0L) {
      *delay_ms = config->max_delay_ms;
    }
    return;
  }
  *delay_ms *= 2L;
  if (config->max_delay_ms > 0L && *delay_ms > config->max_delay_ms) {
    *delay_ms = config->max_delay_ms;
  }
}

static void vectis_lua_curl_buffer_reset(vectis_lua_curl_buffer *buffer,
                                         size_t limit) {
  vectis_lua_curl_buffer_free(buffer);
  memset(buffer, 0, sizeof(*buffer));
  buffer->limit = limit;
}

static void vectis_lua_curl_push_result(lua_State *lua, CURLcode code,
                                        CURL *curl,
                                        const vectis_lua_curl_buffer *body,
                                        const vectis_lua_curl_buffer *headers,
                                        const char *error_buffer,
                                        unsigned attempts) {
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
  lua_pushinteger(lua, (lua_Integer)attempts);
  lua_setfield(lua, -2, "attempts");
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
  vectis_lua_curl_file_upload file_upload;
  vectis_lua_curl_retry_config retry_config;
  lonejson_curl_parse json_response;
  lonejson_curl_upload json_upload;
  lonejson_schema_view request_schema;
  lonejson_schema_view response_schema;
  lonejson_record_view request_record;
  lonejson_record_view response_record;
  const char *url;
  const char *username;
  const char *password;
  const char *upload_path;
  const char *download_path;
  char error_buffer[CURL_ERROR_SIZE];
  lonejson_error json_error;
  lonejson_status json_status;
  long timeout_ms;
  long retry_delay_ms;
  long response_code;
  unsigned attempt;
  int is_smtp;
  int is_upload;
  int request_schema_index;
  int request_value_index;
  int request_record_index;
  int response_schema_index;
  int response_record_index;
  int has_streaming_upload;
  int has_streaming_response;
  FILE *download_file;
  struct stat upload_stat;

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
  memset(&file_upload, 0, sizeof(file_upload));
  file_upload.size = (curl_off_t)-1;
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
  download_file = NULL;
  request_schema_index = 0;
  request_value_index = 0;
  request_record_index = 0;
  response_schema_index = 0;
  response_record_index = 0;
  has_streaming_upload = 0;
  has_streaming_response = 0;
  vectis_lua_curl_retry_config_init(&retry_config);
  if (vectis_lua_curl_parse_retry_config(lua, 1, &retry_config) != 0) {
    return 1;
  }
  upload_path = vectis_lua_table_string(lua, 1, "upload_path");
  if (upload_path == NULL) {
    upload_path = vectis_lua_table_string(lua, 1, "body_path");
  }
  if (upload_path != NULL) {
    if (upload_path[0] == '\0') {
      return luaL_error(lua, "curl upload_path must not be empty");
    }
    file_upload.file = fopen(upload_path, "rb");
    if (file_upload.file == NULL) {
      return luaL_error(lua, "curl failed to open upload_path: %s",
                        upload_path);
    }
    if (stat(upload_path, &upload_stat) == 0 && S_ISREG(upload_stat.st_mode)) {
      file_upload.size = (curl_off_t)upload_stat.st_size;
    }
  }
  download_path = vectis_lua_table_string(lua, 1, "download_path");
  if (download_path != NULL && download_path[0] == '\0') {
    if (file_upload.file != NULL) {
      (void)fclose(file_upload.file);
    }
    return luaL_error(lua, "curl download_path must not be empty");
  }
  (void)pthread_once(&vectis_lua_curl_once, vectis_lua_curl_global_init_once);
  curl = curl_easy_init();
  if (curl == NULL) {
    if (file_upload.file != NULL) {
      (void)fclose(file_upload.file);
    }
    return luaL_error(lua, "curl_easy_init failed");
  }

  (void)curl_easy_setopt(curl, CURLOPT_URL, url);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  lua_getfield(lua, 1, "request_schema");
  if (!lua_isnil(lua, -1)) {
    if (file_upload.file != NULL) {
      curl_easy_cleanup(curl);
      (void)fclose(file_upload.file);
      return luaL_error(lua, "curl request_schema cannot be used with "
                             "upload_path");
    }
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
    if (download_path != NULL) {
      curl_easy_cleanup(curl);
      if (has_streaming_upload) {
        lonejson_curl_upload_cleanup(&json_upload);
      }
      if (file_upload.file != NULL) {
        (void)fclose(file_upload.file);
      }
      return luaL_error(lua, "curl response_schema cannot be used with "
                             "download_path");
    }
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
    if (download_path != NULL) {
      download_file = fopen(download_path, "wb");
      if (download_file == NULL) {
        curl_easy_cleanup(curl);
        if (has_streaming_upload) {
          lonejson_curl_upload_cleanup(&json_upload);
        }
        if (file_upload.file != NULL) {
          (void)fclose(file_upload.file);
        }
        return luaL_error(lua, "curl failed to open download_path: %s",
                          download_path);
      }
      (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                             vectis_lua_curl_write_file);
      (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, download_file);
    } else {
      (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                             vectis_lua_curl_write);
      (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    }
  }
  if (retry_config.max_attempts > 1u && has_streaming_response) {
    curl_easy_cleanup(curl);
    lonejson_curl_parse_cleanup(&json_response);
    if (has_streaming_upload) {
      lonejson_curl_upload_cleanup(&json_upload);
    }
    vectis_lua_curl_buffer_free(&body);
    vectis_lua_curl_buffer_free(&response);
    vectis_lua_curl_buffer_free(&response_headers);
    return luaL_error(lua, "curl streaming responses cannot be retried safely");
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
                                   has_streaming_upload, &file_upload) != 0) {
    is_upload = 1;
  }
  if (has_streaming_upload) {
    is_upload = 1;
  }
  vectis_lua_curl_apply_method(lua, curl, 1,
                               is_smtp || (is_upload && !has_streaming_upload),
                               has_streaming_upload, &json_upload);

  retry_delay_ms = retry_config.initial_delay_ms;
  if (retry_config.max_delay_ms > 0L &&
      retry_delay_ms > retry_config.max_delay_ms) {
    retry_delay_ms = retry_config.max_delay_ms;
  }
  attempt = 1u;
  for (;;) {
    code = curl_easy_perform(curl);
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (attempt >= retry_config.max_attempts) {
      break;
    }
    if (!response.limit_exceeded && !response_headers.limit_exceeded &&
        vectis_lua_curl_retry_code(code, retry_config.conditions)) {
      vectis_lua_curl_buffer_reset(&response,
                                   VECTIS_LUA_CURL_RESPONSE_BODY_LIMIT);
      vectis_lua_curl_buffer_reset(&response_headers,
                                   VECTIS_LUA_CURL_RESPONSE_HEADER_LIMIT);
    } else if (code == CURLE_OK &&
               vectis_lua_curl_retry_status(response_code,
                                            retry_config.conditions)) {
      vectis_lua_curl_buffer_reset(&response,
                                   VECTIS_LUA_CURL_RESPONSE_BODY_LIMIT);
      vectis_lua_curl_buffer_reset(&response_headers,
                                   VECTIS_LUA_CURL_RESPONSE_HEADER_LIMIT);
    } else {
      break;
    }
    body.offset = 0u;
    if (file_upload.file != NULL &&
        fseek(file_upload.file, 0L, SEEK_SET) != 0) {
      curl_slist_free_all(headers);
      curl_slist_free_all(recipients);
      curl_easy_cleanup(curl);
      if (download_file != NULL) {
        (void)fclose(download_file);
      }
      (void)fclose(file_upload.file);
      vectis_lua_curl_buffer_free(&body);
      vectis_lua_curl_buffer_free(&response);
      vectis_lua_curl_buffer_free(&response_headers);
      return luaL_error(lua, "curl failed to rewind upload_path");
    }
    if (download_file != NULL) {
      if (fclose(download_file) != 0) {
        download_file = NULL;
        curl_slist_free_all(headers);
        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);
        if (file_upload.file != NULL) {
          (void)fclose(file_upload.file);
        }
        vectis_lua_curl_buffer_free(&body);
        vectis_lua_curl_buffer_free(&response);
        vectis_lua_curl_buffer_free(&response_headers);
        return luaL_error(lua, "curl failed to close download_path");
      }
      download_file = fopen(download_path, "wb");
      if (download_file == NULL) {
        curl_slist_free_all(headers);
        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);
        if (file_upload.file != NULL) {
          (void)fclose(file_upload.file);
        }
        vectis_lua_curl_buffer_free(&body);
        vectis_lua_curl_buffer_free(&response);
        vectis_lua_curl_buffer_free(&response_headers);
        return luaL_error(lua, "curl failed to reopen download_path: %s",
                          download_path);
      }
      (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, download_file);
    }
    if (has_streaming_upload) {
      lonejson_curl_upload_cleanup(&json_upload);
      memset(&json_upload, 0, sizeof(json_upload));
      json_status =
          lonejson_curl_upload_init(&json_upload, request_schema.runtime,
                                    request_schema.map, request_record.record);
      if (json_status != LONEJSON_STATUS_OK) {
        curl_slist_free_all(headers);
        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);
        vectis_lua_curl_buffer_free(&body);
        vectis_lua_curl_buffer_free(&response);
        vectis_lua_curl_buffer_free(&response_headers);
        return luaL_error(lua, "lonejson curl upload retry init failed");
      }
    }
    vectis_lua_curl_sleep_ms(retry_delay_ms);
    vectis_lua_curl_retry_delay_next(&retry_config, &retry_delay_ms);
    ++attempt;
  }
  if (stream_response.limit_exceeded) {
    response.limit_exceeded = 1;
  }
  if (download_file != NULL) {
    if (fclose(download_file) != 0 && code == CURLE_OK) {
      code = CURLE_WRITE_ERROR;
      (void)snprintf(error_buffer, sizeof(error_buffer),
                     "failed to close download_path");
    }
    download_file = NULL;
  }
  if (has_streaming_response && code == CURLE_OK) {
    json_status = lonejson_curl_parse_finish(&json_response);
  } else {
    json_status = LONEJSON_STATUS_OK;
  }
  vectis_lua_curl_push_result(lua, code, curl, &response, &response_headers,
                              error_buffer, attempt);
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
  if (file_upload.file != NULL) {
    (void)fclose(file_upload.file);
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
  const char *state_path;

  vectis_auth_store_config_init(config);
  index = lua_absindex(lua, index);
  path = vectis_lua_table_string(lua, index, "credentials_path");
  if (path == NULL) {
    path = vectis_lua_table_string(lua, index, "path");
  }
  config->credentials_path = path;
  state_path = vectis_lua_table_string(lua, index, "state_path");
  if (state_path == NULL) {
    state_path = vectis_lua_table_string(lua, index, "auth_state_path");
  }
  config->state_path = state_path;
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

static int vectis_lua_auth_email_token_issue(lua_State *lua) {
  vectis_auth_email_token_issue_config config;
  vectis_auth_email_token token;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_auth_email_token_issue_config_init(&config);
  vectis_lua_auth_store_config(lua, 1, &config.store);
  config.username = vectis_lua_table_string(lua, 1, "username");
  config.realm = vectis_lua_table_string(lua, 1, "realm");
  config.email = vectis_lua_table_string(lua, 1, "email");
  config.pending_transaction_id =
      vectis_lua_table_string(lua, 1, "pending_transaction_id");
  config.transaction_id = vectis_lua_table_string(lua, 1, "transaction_id");
  config.token = vectis_lua_table_string(lua, 1, "token");
  config.now_seconds = (uint64_t)vectis_lua_table_size(lua, 1, "now", 0u);
  if (config.now_seconds == 0u) {
    config.now_seconds = (uint64_t)vectis_lua_table_size(lua, 1, "time", 0u);
  }
  config.ttl_seconds =
      (uint64_t)vectis_lua_table_size(lua, 1, "ttl_seconds", 0u);
  config.max_attempts =
      (unsigned int)vectis_lua_table_size(lua, 1, "max_attempts", 0u);
  vectis_auth_email_token_init(&token);
  status = vectis_auth_email_token_issue(&config, &token, &error);
  if (status != VECTIS_OK) {
    vectis_auth_email_token_cleanup(&token);
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_newtable(lua);
  if (token.transaction_id != NULL) {
    lua_pushstring(lua, token.transaction_id);
    lua_setfield(lua, -2, "transaction_id");
  }
  if (token.token != NULL) {
    lua_pushstring(lua, token.token);
    lua_setfield(lua, -2, "token");
  }
  lua_pushinteger(lua, (lua_Integer)token.expires_at);
  lua_setfield(lua, -2, "expires_at");
  vectis_auth_email_token_cleanup(&token);
  return 1;
}

static int vectis_lua_auth_email_token_verify(lua_State *lua) {
  vectis_auth_email_token_verify_config config;
  vectis_auth_email_token_result result;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_auth_email_token_verify_config_init(&config);
  vectis_lua_auth_store_config(lua, 1, &config.store);
  config.transaction_id = vectis_lua_table_string(lua, 1, "transaction_id");
  config.username = vectis_lua_table_string(lua, 1, "username");
  config.realm = vectis_lua_table_string(lua, 1, "realm");
  config.pending_transaction_id =
      vectis_lua_table_string(lua, 1, "pending_transaction_id");
  config.token = vectis_lua_table_string(lua, 1, "token");
  config.now_seconds = (uint64_t)vectis_lua_table_size(lua, 1, "now", 0u);
  if (config.now_seconds == 0u) {
    config.now_seconds = (uint64_t)vectis_lua_table_size(lua, 1, "time", 0u);
  }
  vectis_auth_email_token_result_init(&result);
  status = vectis_auth_email_token_verify(&config, &result, &error);
  if (status != VECTIS_OK) {
    vectis_auth_email_token_result_cleanup(&result);
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_newtable(lua);
  lua_pushboolean(lua, result.verified);
  lua_setfield(lua, -2, "verified");
  lua_pushboolean(lua, result.expired);
  lua_setfield(lua, -2, "expired");
  if (result.username != NULL) {
    lua_pushstring(lua, result.username);
    lua_setfield(lua, -2, "username");
  }
  if (result.realm != NULL) {
    lua_pushstring(lua, result.realm);
    lua_setfield(lua, -2, "realm");
  }
  if (result.email != NULL) {
    lua_pushstring(lua, result.email);
    lua_setfield(lua, -2, "email");
  }
  if (result.pending_transaction_id != NULL) {
    lua_pushstring(lua, result.pending_transaction_id);
    lua_setfield(lua, -2, "pending_transaction_id");
  }
  lua_pushinteger(lua, (lua_Integer)result.failed_attempts);
  lua_setfield(lua, -2, "failed_attempts");
  lua_pushinteger(lua, (lua_Integer)result.max_attempts);
  lua_setfield(lua, -2, "max_attempts");
  vectis_auth_email_token_result_cleanup(&result);
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

static int vectis_lua_cert_generate_bundle(lua_State *lua) {
  vectis_cert_bundle_config config;
  vectis_error error;
  vectis_status status;
  int subject_index;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_cert_bundle_config_init(&config);
  lua_getfield(lua, 1, "subject");
  subject_index = lua_absindex(lua, -1);
  if (lua_istable(lua, subject_index)) {
    config.subject.common_name =
        vectis_lua_table_string(lua, subject_index, "common_name");
    config.subject.organization =
        vectis_lua_table_string(lua, subject_index, "organization");
    config.subject.organizational_unit =
        vectis_lua_table_string(lua, subject_index, "organizational_unit");
    config.subject.country =
        vectis_lua_table_string(lua, subject_index, "country");
    config.subject.state = vectis_lua_table_string(lua, subject_index, "state");
    config.subject.locality =
        vectis_lua_table_string(lua, subject_index, "locality");
  }
  lua_pop(lua, 1);
  if (config.subject.common_name == NULL) {
    config.subject.common_name = vectis_lua_table_string(lua, 1, "common_name");
  }
  config.dns_names = vectis_lua_table_string(lua, 1, "dns_names");
  config.ip_addresses = vectis_lua_table_string(lua, 1, "ip_addresses");
  config.ca_cert_path = vectis_lua_table_string(lua, 1, "ca_cert_path");
  config.ca_key_path = vectis_lua_table_string(lua, 1, "ca_key_path");
  config.is_ca = vectis_lua_table_bool(lua, 1, "is_ca", 0);
  config.output_bundle_path =
      vectis_lua_table_string(lua, 1, "output_bundle_path");
  if (config.output_bundle_path == NULL) {
    config.output_bundle_path = vectis_lua_table_string(lua, 1, "bundle_path");
  }
  config.output_cert_path = vectis_lua_table_string(lua, 1, "output_cert_path");
  config.output_key_path = vectis_lua_table_string(lua, 1, "output_key_path");
  config.key_bits =
      (unsigned)vectis_lua_table_size(lua, 1, "key_bits", config.key_bits);
  config.valid_days =
      vectis_lua_table_long(lua, 1, "valid_days", config.valid_days);

  vectis_error_clear(&error);
  status = vectis_cert_generate_bundle(&config, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static void vectis_lua_cert_subject_from_table(lua_State *lua, int index,
                                               vectis_cert_subject *subject) {
  int subject_index;

  lua_getfield(lua, index, "subject");
  subject_index = lua_absindex(lua, -1);
  if (lua_istable(lua, subject_index)) {
    subject->common_name =
        vectis_lua_table_string(lua, subject_index, "common_name");
    subject->organization =
        vectis_lua_table_string(lua, subject_index, "organization");
    subject->organizational_unit =
        vectis_lua_table_string(lua, subject_index, "organizational_unit");
    subject->country = vectis_lua_table_string(lua, subject_index, "country");
    subject->state = vectis_lua_table_string(lua, subject_index, "state");
    subject->locality =
        vectis_lua_table_string(lua, subject_index, "locality");
  }
  lua_pop(lua, 1);
  if (subject->common_name == NULL) {
    subject->common_name = vectis_lua_table_string(lua, index, "common_name");
  }
}

static int vectis_lua_cert_generate_private_key(lua_State *lua) {
  vectis_private_key_config config;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_private_key_config_init(&config);
  config.output_key_path = vectis_lua_table_string(lua, 1, "output_key_path");
  if (config.output_key_path == NULL) {
    config.output_key_path = vectis_lua_table_string(lua, 1, "key_path");
  }
  config.key_bits =
      (unsigned)vectis_lua_table_size(lua, 1, "key_bits", config.key_bits);

  vectis_error_clear(&error);
  status = vectis_cert_generate_private_key(&config, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_cert_generate_csr(lua_State *lua) {
  vectis_csr_config config;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_csr_config_init(&config);
  vectis_lua_cert_subject_from_table(lua, 1, &config.subject);
  config.dns_names = vectis_lua_table_string(lua, 1, "dns_names");
  config.ip_addresses = vectis_lua_table_string(lua, 1, "ip_addresses");
  config.private_key_path =
      vectis_lua_table_string(lua, 1, "private_key_path");
  if (config.private_key_path == NULL) {
    config.private_key_path = vectis_lua_table_string(lua, 1, "key_path");
  }
  config.output_csr_path = vectis_lua_table_string(lua, 1, "output_csr_path");
  if (config.output_csr_path == NULL) {
    config.output_csr_path = vectis_lua_table_string(lua, 1, "csr_path");
  }

  vectis_error_clear(&error);
  status = vectis_cert_generate_csr(&config, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static const char *vectis_lua_cert_path_arg(lua_State *lua, int index,
                                            const char *primary,
                                            const char *fallback,
                                            const char *label) {
  const char *path;

  if (lua_isstring(lua, index)) {
    return lua_tostring(lua, index);
  }
  luaL_checktype(lua, index, LUA_TTABLE);
  path = vectis_lua_table_string(lua, index, primary);
  if (path == NULL && fallback != NULL) {
    path = vectis_lua_table_string(lua, index, fallback);
  }
  if (path == NULL || path[0] == '\0') {
    luaL_error(lua, "cert %s path is required", label);
  }
  return path;
}

static int vectis_lua_cert_validate_bundle(lua_State *lua) {
  vectis_source bundle;
  vectis_error error;
  vectis_status status;
  const char *path;

  path = vectis_lua_cert_path_arg(lua, 1, "bundle_path", "path", "bundle");
  bundle = vectis_source_from_path(path);
  vectis_error_clear(&error);
  status = vectis_cert_validate_bundle(&bundle, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static X509 *vectis_lua_cert_read_x509_file(lua_State *lua, const char *path) {
  unsigned char *pem;
  size_t pem_size;
  BIO *bio;
  X509 *cert;

  pem = NULL;
  pem_size = 0u;
  if (vectis_read_all(path, &pem, &pem_size) != 0 || pem_size == 0u) {
    free(pem);
    (void)vectis_lua_push_error_text(lua, VECTIS_ERR_INVALID,
                                     "failed to read certificate bundle");
    return NULL;
  }
  bio = BIO_new_mem_buf(pem, (int)pem_size);
  if (bio == NULL) {
    free(pem);
    (void)vectis_lua_push_error_text(lua, VECTIS_ERR_NOMEM,
                                     "failed to allocate certificate reader");
    return NULL;
  }
  cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  free(pem);
  if (cert == NULL) {
    (void)vectis_lua_push_error_text(lua, VECTIS_ERR_INVALID,
                                     "failed to parse certificate from bundle");
    return NULL;
  }
  return cert;
}

static void vectis_lua_cert_name_field(lua_State *lua, X509_NAME *name,
                                       int nid, const char *field) {
  char value[512];
  int len;

  if (name == NULL) {
    return;
  }
  len = X509_NAME_get_text_by_NID(name, nid, value, (int)sizeof(value));
  if (len < 0) {
    return;
  }
  lua_pushlstring(lua, value, (size_t)len);
  lua_setfield(lua, -2, field);
}

static void vectis_lua_cert_push_name(lua_State *lua, X509_NAME *name) {
  lua_newtable(lua);
  vectis_lua_cert_name_field(lua, name, NID_commonName, "common_name");
  vectis_lua_cert_name_field(lua, name, NID_organizationName, "organization");
  vectis_lua_cert_name_field(lua, name, NID_organizationalUnitName,
                             "organizational_unit");
  vectis_lua_cert_name_field(lua, name, NID_countryName, "country");
  vectis_lua_cert_name_field(lua, name, NID_stateOrProvinceName, "state");
  vectis_lua_cert_name_field(lua, name, NID_localityName, "locality");
}

static void vectis_lua_cert_time_field(lua_State *lua, const ASN1_TIME *time,
                                       const char *field) {
  BIO *bio;
  char *data;
  long len;

  if (time == NULL) {
    return;
  }
  bio = BIO_new(BIO_s_mem());
  if (bio == NULL) {
    return;
  }
  if (ASN1_TIME_print(bio, time) != 1) {
    BIO_free(bio);
    return;
  }
  len = BIO_get_mem_data(bio, &data);
  if (len > 0 && data != NULL) {
    lua_pushlstring(lua, data, (size_t)len);
    lua_setfield(lua, -2, field);
  }
  BIO_free(bio);
}

static const char *vectis_lua_cert_public_key_type(EVP_PKEY *key) {
  if (key == NULL) {
    return "unknown";
  }
  switch (EVP_PKEY_base_id(key)) {
  case EVP_PKEY_RSA:
    return "rsa";
  case EVP_PKEY_EC:
    return "ec";
#ifdef EVP_PKEY_ED25519
  case EVP_PKEY_ED25519:
    return "ed25519";
#endif
#ifdef EVP_PKEY_ED448
  case EVP_PKEY_ED448:
    return "ed448";
#endif
  default:
    return "unknown";
  }
}

static void vectis_lua_cert_push_san(lua_State *lua, X509 *cert) {
  GENERAL_NAMES *names;
  GENERAL_NAME *name;
  const ASN1_STRING *string;
  const unsigned char *data;
  char ip[INET6_ADDRSTRLEN];
  int dns_count;
  int ip_count;
  int i;
  int count;
  int len;

  lua_newtable(lua);
  lua_newtable(lua);
  lua_setfield(lua, -2, "dns_names");
  lua_newtable(lua);
  lua_setfield(lua, -2, "ip_addresses");

  names = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
  if (names == NULL) {
    return;
  }
  count = sk_GENERAL_NAME_num(names);
  dns_count = 0;
  ip_count = 0;
  for (i = 0; i < count; ++i) {
    name = sk_GENERAL_NAME_value(names, i);
    if (name == NULL) {
      continue;
    }
    if (name->type == GEN_DNS) {
      string = name->d.dNSName;
      data = ASN1_STRING_get0_data(string);
      len = ASN1_STRING_length(string);
      if (data == NULL || len < 0) {
        continue;
      }
      lua_getfield(lua, -1, "dns_names");
      lua_pushlstring(lua, (const char *)data, (size_t)len);
      lua_rawseti(lua, -2, ++dns_count);
      lua_pop(lua, 1);
    } else if (name->type == GEN_IPADD) {
      string = name->d.iPAddress;
      data = ASN1_STRING_get0_data(string);
      len = ASN1_STRING_length(string);
      if (data == NULL ||
          ((len != 4 || inet_ntop(AF_INET, data, ip, sizeof(ip)) == NULL) &&
           (len != 16 || inet_ntop(AF_INET6, data, ip, sizeof(ip)) == NULL))) {
        continue;
      }
      lua_getfield(lua, -1, "ip_addresses");
      lua_pushstring(lua, ip);
      lua_rawseti(lua, -2, ++ip_count);
      lua_pop(lua, 1);
    }
  }
  GENERAL_NAMES_free(names);
}

static int vectis_lua_cert_inspect_bundle(lua_State *lua) {
  X509 *cert;
  EVP_PKEY *key;
  BASIC_CONSTRAINTS *constraints;
  BIGNUM *serial_bn;
  char *serial_hex;
  const char *path;

  path = vectis_lua_cert_path_arg(lua, 1, "bundle_path", "path", "bundle");
  cert = vectis_lua_cert_read_x509_file(lua, path);
  if (cert == NULL) {
    return 2;
  }
  key = X509_get_pubkey(cert);
  constraints = X509_get_ext_d2i(cert, NID_basic_constraints, NULL, NULL);
  serial_bn = ASN1_INTEGER_to_BN(X509_get_serialNumber(cert), NULL);
  serial_hex = serial_bn != NULL ? BN_bn2hex(serial_bn) : NULL;

  lua_newtable(lua);
  lua_pushstring(lua, path);
  lua_setfield(lua, -2, "path");
  lua_pushinteger(lua, (lua_Integer)(X509_get_version(cert) + 1L));
  lua_setfield(lua, -2, "version");
  if (serial_hex != NULL) {
    lua_pushstring(lua, serial_hex);
    lua_setfield(lua, -2, "serial_hex");
  }
  vectis_lua_cert_time_field(lua, X509_get0_notBefore(cert), "not_before");
  vectis_lua_cert_time_field(lua, X509_get0_notAfter(cert), "not_after");
  lua_pushboolean(lua, constraints != NULL && constraints->ca);
  lua_setfield(lua, -2, "is_ca");
  lua_pushstring(lua, vectis_lua_cert_public_key_type(key));
  lua_setfield(lua, -2, "public_key_type");
  if (key != NULL) {
    lua_pushinteger(lua, (lua_Integer)EVP_PKEY_bits(key));
    lua_setfield(lua, -2, "public_key_bits");
  }
  vectis_lua_cert_push_name(lua, X509_get_subject_name(cert));
  lua_setfield(lua, -2, "subject");
  vectis_lua_cert_push_name(lua, X509_get_issuer_name(cert));
  lua_setfield(lua, -2, "issuer");
  vectis_lua_cert_push_san(lua, cert);
  lua_setfield(lua, -2, "subject_alt_names");

  OPENSSL_free(serial_hex);
  BN_free(serial_bn);
  BASIC_CONSTRAINTS_free(constraints);
  EVP_PKEY_free(key);
  X509_free(cert);
  return 1;
}

static int vectis_lua_cert_validate_pair(lua_State *lua) {
  vectis_source certificate;
  vectis_source private_key;
  vectis_source ca_bundle;
  vectis_source *ca_bundle_ptr;
  vectis_error error;
  vectis_status status;
  const char *certificate_path;
  const char *private_key_path;
  const char *ca_bundle_path;

  luaL_checktype(lua, 1, LUA_TTABLE);
  certificate_path = vectis_lua_cert_path_arg(lua, 1, "certificate_path",
                                              "cert_path", "certificate");
  private_key_path = vectis_lua_cert_path_arg(lua, 1, "private_key_path",
                                              "key_path", "private key");
  ca_bundle_path = vectis_lua_table_string(lua, 1, "ca_bundle_path");
  certificate = vectis_source_from_path(certificate_path);
  private_key = vectis_source_from_path(private_key_path);
  ca_bundle_ptr = NULL;
  if (ca_bundle_path != NULL && ca_bundle_path[0] != '\0') {
    ca_bundle = vectis_source_from_path(ca_bundle_path);
    ca_bundle_ptr = &ca_bundle;
  }
  vectis_error_clear(&error);
  status = vectis_cert_validate_pair(&certificate, &private_key, ca_bundle_ptr,
                                     &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static void vectis_lua_push_cert_table(lua_State *lua) {
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_lua_cert_generate_bundle);
  lua_setfield(lua, -2, "generate_bundle");
  lua_pushcfunction(lua, vectis_lua_cert_generate_private_key);
  lua_setfield(lua, -2, "generate_private_key");
  lua_pushcfunction(lua, vectis_lua_cert_generate_csr);
  lua_setfield(lua, -2, "generate_csr");
  lua_pushcfunction(lua, vectis_lua_cert_inspect_bundle);
  lua_setfield(lua, -2, "inspect_bundle");
  lua_pushcfunction(lua, vectis_lua_cert_validate_bundle);
  lua_setfield(lua, -2, "validate_bundle");
  lua_pushcfunction(lua, vectis_lua_cert_validate_pair);
  lua_setfield(lua, -2, "validate_pair");
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

static void vectis_lua_register_embedded_chunks(lua_State *lua) {
  if (luaL_newmetatable(lua, VECTIS_LUA_EMBEDDED_CHUNKS)) {
    lua_pushcfunction(lua, vectis_lua_embedded_chunks_gc);
    lua_setfield(lua, -2, "__gc");
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
  lua_pushcfunction(lua, vectis_lua_auth_email_token_issue);
  lua_setfield(lua, -2, "email_token_issue");
  lua_pushcfunction(lua, vectis_lua_auth_email_token_verify);
  lua_setfield(lua, -2, "email_token_verify");
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

static void vectis_lua_register_server(lua_State *lua) {
  if (luaL_newmetatable(lua, VECTIS_LUA_SERVER)) {
    lua_newtable(lua);
    lua_pushcfunction(lua, vectis_lua_server_static_embedded);
    lua_setfield(lua, -2, "static_embedded");
    lua_pushcfunction(lua, vectis_lua_server_static_directory);
    lua_setfield(lua, -2, "static_directory");
    lua_pushcfunction(lua, vectis_lua_server_webdav);
    lua_setfield(lua, -2, "webdav");
    lua_pushcfunction(lua, vectis_lua_server_webdav_embedded_site);
    lua_setfield(lua, -2, "webdav_embedded_site");
    lua_pushcfunction(lua, vectis_lua_server_auth_routes);
    lua_setfield(lua, -2, "auth_routes");
    lua_pushcfunction(lua, vectis_lua_server_json);
    lua_setfield(lua, -2, "json");
    lua_pushcfunction(lua, vectis_lua_server_auth_json);
    lua_setfield(lua, -2, "auth_json");
    lua_pushcfunction(lua, vectis_lua_server_consumer_service);
    lua_setfield(lua, -2, "consumer_service");
    lua_pushcfunction(lua, vectis_lua_server_start);
    lua_setfield(lua, -2, "start");
    lua_pushcfunction(lua, vectis_lua_server_stop);
    lua_setfield(lua, -2, "stop");
    lua_pushcfunction(lua, vectis_lua_server_close);
    lua_setfield(lua, -2, "close");
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, vectis_lua_server_close);
    lua_setfield(lua, -2, "__gc");
  }
  lua_pop(lua, 1);
}

static void vectis_lua_push_server_table(lua_State *lua) {
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_lua_server_new);
  lua_setfield(lua, -2, "new");
}

static void vectis_lua_push_ssh_table(lua_State *lua) {
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_lua_ssh_exec);
  lua_setfield(lua, -2, "exec");
}

static int luaopen_vectis(lua_State *lua) {
  vectis_lua_register_totp_qr(lua);
  vectis_lua_register_embedded_chunks(lua);
  vectis_lua_register_server(lua);
  lua_newtable(lua);
  lua_pushliteral(lua, VECTIS_VERSION);
  lua_setfield(lua, -2, "version");
  lua_pushinteger(lua, VECTIS_OK);
  lua_setfield(lua, -2, "OK");
  lua_pushinteger(lua, VECTIS_ERR_INVALID);
  lua_setfield(lua, -2, "ERR_INVALID");
  lua_pushinteger(lua, VECTIS_ERR_NOT_IMPLEMENTED);
  lua_setfield(lua, -2, "ERR_NOT_IMPLEMENTED");
  lua_pushinteger(lua, VECTIS_ERR_TIMEOUT);
  lua_setfield(lua, -2, "ERR_TIMEOUT");
  lua_pushcfunction(lua, vectis_lua_status_string);
  lua_setfield(lua, -2, "status_string");
  lua_pushcfunction(lua, vectis_lua_has_embedded_lockd_bundle);
  lua_setfield(lua, -2, "has_embedded_lockd_bundle");
  lua_pushcfunction(lua, vectis_lua_embedded_lockd_bundle_size);
  lua_setfield(lua, -2, "embedded_lockd_bundle_size");
  lua_pushcfunction(lua, vectis_lua_embedded_lockd_bundle_source);
  lua_setfield(lua, -2, "embedded_lockd_bundle_source");
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_lua_embedded_has_assets);
  lua_setfield(lua, -2, "has_assets");
  lua_pushcfunction(lua, vectis_lua_embedded_default_extract_policy);
  lua_setfield(lua, -2, "default_extract_policy");
  lua_pushcfunction(lua, vectis_lua_embedded_tree_sha256);
  lua_setfield(lua, -2, "tree_sha256");
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
  vectis_lua_push_server_table(lua);
  lua_setfield(lua, -2, "server");
  vectis_lua_push_cert_table(lua);
  lua_setfield(lua, -2, "cert");
  vectis_lua_push_ssh_table(lua);
  lua_setfield(lua, -2, "ssh");
  lua_getglobal(lua, "require");
  lua_pushliteral(lua, "vectis.http");
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    return lua_error(lua);
  }
  lua_setfield(lua, -2, "http");
  lua_getglobal(lua, "require");
  lua_pushliteral(lua, "vectis.webdav");
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    return lua_error(lua);
  }
  lua_setfield(lua, -2, "webdav");
  lua_getglobal(lua, "require");
  lua_pushliteral(lua, "vectis.lockd");
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    return lua_error(lua);
  }
  lua_setfield(lua, -2, "lockd");
  lua_getglobal(lua, "require");
  lua_pushliteral(lua, "vectis.dsv");
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    return lua_error(lua);
  }
  lua_setfield(lua, -2, "dsv");
  lua_getglobal(lua, "require");
  lua_pushliteral(lua, "vectis.xml");
  if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
    return lua_error(lua);
  }
  lua_setfield(lua, -2, "xml");
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

static int luaopen_vectis_xml_core(lua_State *lua) {
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_lua_xml_parse);
  lua_setfield(lua, -2, "parse");
  lua_pushcfunction(lua, vectis_lua_xml_parse_record);
  lua_setfield(lua, -2, "parse_record");
  return 1;
}

static int vectis_luaopen_vectis_xml_core(void *lua_state) {
  return luaopen_vectis_xml_core((lua_State *)lua_state);
}

static int luaopen_vectis_dsv_core(lua_State *lua) {
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_lua_dsv_parse_json);
  lua_setfield(lua, -2, "parse_json");
  lua_pushcfunction(lua, vectis_lua_dsv_parse_spill);
  lua_setfield(lua, -2, "parse_spill");
  lua_pushcfunction(lua, vectis_lua_dsv_parse_typed);
  lua_setfield(lua, -2, "parse_typed");
  lua_pushcfunction(lua, vectis_lua_dsv_each);
  lua_setfield(lua, -2, "each");
  lua_pushcfunction(lua, vectis_lua_dsv_to_string);
  lua_setfield(lua, -2, "to_string");
  return 1;
}

static int vectis_luaopen_vectis_dsv_core(void *lua_state) {
  return luaopen_vectis_dsv_core((lua_State *)lua_state);
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

static int vectis_luaopen_lql_core(void *lua_state) {
  return luaopen_lql_core((lua_State *)lua_state);
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

static int vectis_luaopen_opcua(void *lua_state) {
  return luaopen_opcua((lua_State *)lua_state);
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

static char *vectis_lua_script_dir(const char *script_name) {
  const char *slash;
  size_t size;
  char *dir;

  if (script_name == NULL || script_name[0] == '\0') {
    return vectis_cli_strdup(".");
  }
  slash = strrchr(script_name, '/');
  if (slash == NULL) {
    return vectis_cli_strdup(".");
  }
  if (slash == script_name) {
    return vectis_cli_strdup("/");
  }
  size = (size_t)(slash - script_name);
  dir = (char *)malloc(size + 1u);
  if (dir == NULL) {
    return NULL;
  }
  memcpy(dir, script_name, size);
  dir[size] = '\0';
  return dir;
}

static char *vectis_lua_join_package_pattern(const char *dir,
                                             const char *pattern) {
  const char *separator;
  size_t dir_size;
  size_t pattern_size;
  size_t separator_size;
  char *joined;

  if (dir == NULL || pattern == NULL) {
    return NULL;
  }
  separator = strcmp(dir, "/") == 0 ? "" : "/";
  dir_size = strlen(dir);
  pattern_size = strlen(pattern);
  separator_size = strlen(separator);
  if (dir_size > ((size_t)-1) - separator_size ||
      dir_size + separator_size > ((size_t)-1) - pattern_size - 1u) {
    return NULL;
  }
  joined = (char *)malloc(dir_size + separator_size + pattern_size + 1u);
  if (joined == NULL) {
    return NULL;
  }
  memcpy(joined, dir, dir_size);
  memcpy(joined + dir_size, separator, separator_size);
  memcpy(joined + dir_size + separator_size, pattern, pattern_size);
  joined[dir_size + separator_size + pattern_size] = '\0';
  return joined;
}

static cpkt_lua_runtime_status
vectis_lua_prepend_script_package_path(cpkt_lua_runtime *runtime,
                                       const char *dir, const char *pattern,
                                       int native_module) {
  cpkt_lua_runtime_status status;
  char *path;

  path = vectis_lua_join_package_pattern(dir, pattern);
  if (path == NULL) {
    return CPKT_LUA_RUNTIME_ERR_ALLOC;
  }
  status = native_module ? cpkt_lua_runtime_prepend_package_cpath(runtime, path)
                         : cpkt_lua_runtime_prepend_package_path(runtime, path);
  free(path);
  return status;
}

static cpkt_lua_runtime_status
vectis_lua_configure_script_package_paths(cpkt_lua_runtime *runtime,
                                          const char *script_name) {
  cpkt_lua_runtime_status status;
  char *dir;

  dir = vectis_lua_script_dir(script_name);
  if (dir == NULL) {
    return CPKT_LUA_RUNTIME_ERR_ALLOC;
  }

  status =
      vectis_lua_prepend_script_package_path(runtime, dir, "?/init.lua", 0);
  if (status == CPKT_LUA_RUNTIME_OK) {
    status = vectis_lua_prepend_script_package_path(runtime, dir, "?.lua", 0);
  }
  if (status == CPKT_LUA_RUNTIME_OK) {
    status =
        vectis_lua_prepend_script_package_path(runtime, dir, "?/init.so", 1);
  }
  if (status == CPKT_LUA_RUNTIME_OK) {
    status = vectis_lua_prepend_script_package_path(runtime, dir, "?.so", 1);
  }
  free(dir);
  return status;
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
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "vectis.http", vectis_http_lua_init,
      sizeof(vectis_http_lua_init), "vectis.http");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "vectis.webdav", vectis_webdav_lua_init,
      sizeof(vectis_webdav_lua_init), "vectis.webdav");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "vectis.lockd", vectis_lockd_lua_init,
      sizeof(vectis_lockd_lua_init), "vectis.lockd");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_c_module(runtime, "vectis.dsv.core",
                                              vectis_luaopen_vectis_dsv_core);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "vectis.dsv", vectis_dsv_lua_init, sizeof(vectis_dsv_lua_init),
      "vectis.dsv");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_c_module(runtime, "vectis.xml.core",
                                              vectis_luaopen_vectis_xml_core);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "vectis.xml", vectis_xml_lua_init, sizeof(vectis_xml_lua_init),
      "vectis.xml");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status =
      cpkt_lua_runtime_register_c_module(runtime, "cai", vectis_luaopen_cai);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_c_module(runtime, "lql.core",
                                              vectis_luaopen_lql_core);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "lql", vectis_liblql_lua_init, sizeof(vectis_liblql_lua_init),
      "lql.init");
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
  status = cpkt_lua_runtime_register_c_module(runtime, "softline",
                                              vectis_luaopen_softline);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  return cpkt_lua_runtime_register_c_module(runtime, "opcua",
                                            vectis_luaopen_opcua);
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
  if (asset_payload != NULL && manifest != NULL && manifest_size > 0u) {
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
  status = vectis_lua_configure_script_package_paths(runtime, script_name);
  if (status != CPKT_LUA_RUNTIME_OK) {
    rc = vectis_lua_report_status(runtime, status);
    cpkt_lua_runtime_free(runtime);
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

  status =
      vectis_lua_configure_script_package_paths(runtime, argv[script_index]);
  if (status != CPKT_LUA_RUNTIME_OK) {
    rc = vectis_lua_report_status(runtime, status);
    cpkt_lua_runtime_free(runtime);
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
