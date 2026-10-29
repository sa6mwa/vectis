#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vectis_acme_state.h"
#include "vectis_lua_buffer.h"
#include "vectis_lua_compression.h"
#include "vectis_lua_crypto.h"
#include "vectis_lua_curl.h"
#include "vectis_lua_gz.h"
#include "vectis_lua_pdf.h"
#include "vectis_lua_png.h"
#include "vectis_lua_sqlite.h"
#include "vectis_lua_xml_objects.h"
#include "vectis_lua_zlib_raw.h"
#include "vectis_lua_zlib_stream.h"
#include <lauxlib.h>
#include <lc/lc.h>
#include <lualib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vectis/digest.h>
#include <vectis/embedded_fs.h>
#include <vectis/totp_qr.h>
#include <vectis/webdav.h>
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "stack worker: line %d: %s\n", __LINE__, #condition);    \
      return 1;                                                                \
    }                                                                          \
  } while (0)
typedef struct worker_case {
  const char *root, *script;
  int failed;
} worker_case;
int luaopen_libmdf_core(lua_State *lua);

static vectis_status read_lua(void *context, unsigned char *buffer,
                              size_t capacity, size_t *size,
                              vectis_error *error) {
  lua_State *L = (lua_State *)context;
  const char *bytes;
  lua_getglobal(L, "stack_read");
  lua_pushinteger(L, (lua_Integer)capacity);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "message");
      fprintf(stderr, "stack callback: %s\n", lua_tostring(L, -1));
      lua_pop(L, 1);
    } else
      fprintf(stderr, "stack callback: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    error->code = VECTIS_ERR_STATE;
    return VECTIS_ERR_STATE;
  }
  if (lua_isnil(L, -1))
    *size = 0u;
  else {
    bytes = lua_tolstring(L, -1, size);
    if (bytes == NULL || *size > capacity) {
      lua_pop(L, 1);
      return VECTIS_ERR_INVALID;
    }
    memcpy(buffer, bytes, *size);
  }
  lua_pop(L, 1);
  return VECTIS_OK;
}
static int lua_case(const worker_case *test) {
  lua_State *L;
  vectis_digest_result actual, expected;
  vectis_error error;
  unsigned char *payload;
  int status;
  L = luaL_newstate();
  CHECK(L != NULL);
  luaL_openlibs(L);
  luaL_requiref(L, "libmdf.core", luaopen_libmdf_core, 0);
  lua_setglobal(L, "stack_mdf");
  luaL_requiref(L, "vectis.buffer.core", luaopen_vectis_buffer_core, 0);
  lua_pop(L, 1);
  luaL_requiref(L, "vectis.sqlite.core", luaopen_vectis_sqlite_core, 0);
  lua_pop(L, 1);
  luaL_requiref(L, "vectis.png.core", luaopen_vectis_png_core, 0);
  lua_pop(L, 1);
  luaL_requiref(L, "vectis.pdf.core", luaopen_vectis_pdf_core, 0);
  lua_pop(L, 1);
  luaL_requiref(L, "vectis.xml.objects.core", luaopen_vectis_xml_objects_core,
                0);
  lua_pop(L, 1);
  luaL_requiref(L, "vectis.crypto.core", luaopen_vectis_crypto_core, 0);
  lua_pop(L, 1);
  lua_newtable(L);
  vectis_lua_zlib_raw_register(L);
  vectis_lua_zlib_stream_register(L);
  vectis_lua_gz_register(L);
  lua_setglobal(L, "stack_zlib");
  luaL_requiref(L, "vectis.compression.core", luaopen_vectis_compression_core,
                0);
  lua_pop(L, 1);
  luaL_requiref(L, "vectis.curl.handles.core", luaopen_vectis_curl_handles_core,
                0);
  lua_pop(L, 1);
  lua_pushstring(L, test->script);
  lua_setglobal(L, "stack_script_path");
  lua_pushstring(L, test->root);
  lua_setglobal(L, "stack_root");
  status = luaL_dofile(L, test->script);
  if (status != LUA_OK)
    fprintf(stderr, "stack Lua: %s\n", lua_tostring(L, -1));
  CHECK(status == LUA_OK);
  CHECK(vectis_digest_stream("SHA256", read_lua, L, &actual, &error) ==
        VECTIS_OK);
  payload = (unsigned char *)malloc(65536u);
  CHECK(payload != NULL);
  memset(payload, 'q', 65536u);
  CHECK(vectis_digest_materialized("SHA256", payload, 65536u, &expected,
                                   &error) == VECTIS_OK);
  CHECK(actual.size == expected.size &&
        memcmp(actual.data, expected.data, actual.size) == 0);
  free(payload);
  lua_close(L);
  return 0;
}
static int lockdc_case(const char *root) {
  lc_client_config config;
  lc_pouch_settings settings;
  lc_collection_config collection_config;
  lc_collection_append_req request;
  lc_collection_append_res result;
  lc_collection_read_opts options;
  lc_collection_read_res progress;
  lc_client *client = NULL;
  lc_collection *collection = NULL;
  lc_collection_writer *writer = NULL;
  lc_source *source = NULL;
  lc_sink *sink = NULL;
  lc_error error;
  const char *endpoints[1];
  char *endpoint, *chunk;
  size_t written, index;
  endpoint = (char *)malloc(strlen(root) + 32u);
  chunk = (char *)malloc(65536u);
  CHECK(endpoint != NULL && chunk != NULL);
  sprintf(endpoint, "pouch://%s/lockdc", root);
  memset(chunk, 'x', 65536u);
  endpoints[0] = endpoint;
  lc_error_init(&error);
  lc_client_config_init(&config);
  lc_pouch_settings_init(&settings);
  settings.set_mask =
      LC_POUCH_SETTING_BACKGROUND_COMPACTION | LC_POUCH_SETTING_COMPRESSION |
      LC_POUCH_SETTING_QUERY_ENGINE | LC_POUCH_SETTING_QUERY_INDEXING;
  settings.background_compaction_enabled = 0;
  settings.compression = "zlib";
  settings.query_engine = "scan";
  settings.query_indexing_enabled = 0;
  config.endpoints = endpoints;
  config.endpoint_count = 1u;
  config.pouch_settings = &settings;
  CHECK(lc_client_open(&config, &client, &error) == LC_OK);
  lc_collection_config_init(&collection_config);
  collection_config.name = "worker";
  collection_config.has_max_entries = 1;
  collection_config.max_entries = 2u;
  CHECK(lc_client_create_collection(client, &collection_config, &collection,
                                    &error) == LC_OK);
  lc_collection_append_req_init(&request);
  request.append_id = "worker-push";
  request.has_collection_retention_seconds = 1;
  request.collection_retention_seconds = 3600L;
  CHECK(lc_collection_append_begin(collection, &request, &writer, &error) ==
        LC_OK);
  CHECK(lc_collection_writer_write(writer, "{\"data\":\"", 9u, &error) ==
        LC_OK);
  for (index = 0u; index < 64u; ++index)
    CHECK(lc_collection_writer_write(writer, chunk, 4096u, &error) == LC_OK);
  CHECK(lc_collection_writer_write(writer, "\"}", 2u, &error) == LC_OK);
  CHECK(lc_collection_writer_commit(writer, &result, &error) == LC_OK &&
        result.position == 1u &&
        result.has_collection_retention_expires_at_unix);
  lc_collection_writer_close(writer);
  CHECK(lc_sink_to_discard(&sink, &error) == LC_OK);
  lc_collection_read_opts_init(&options);
  lc_collection_read_res_init(&progress);
  CHECK(lc_collection_read(collection, &options, sink, &progress, &error) ==
            LC_OK &&
        progress.completed_records == 1u);
  lc_collection_read_res_cleanup(&progress);
  lc_collection_read_res_init(&progress);
  options.selector_lql = "exists{/data}";
  CHECK(lc_collection_read(collection, &options, sink, &progress, &error) ==
            LC_OK &&
        progress.completed_records == 1u);
  lc_collection_read_res_cleanup(&progress);
  /* Exercise the newly heap-backed bulk copy in the same default-stack worker.
   */
  CHECK(lc_source_from_memory(chunk, 65536u, &source, &error) == LC_OK);
  CHECK(lc_copy(source, sink, &written, &error) == LC_OK && written == 65536u);
  lc_source_close(source);
  lc_sink_close(sink);
  lc_collection_close(collection);
  lc_client_close(client);
  lc_error_cleanup(&error);
  free(chunk);
  free(endpoint);
  return 0;
}
static int embedded_case(const char *root) {
  static const unsigned char payload[] = "app\n";
  static const char manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":[{\"path\":\"/"
      "app.txt\",\"kind\":\"file\",\"mode\":292,\"offset\":0,\"size\":4,"
      "\"sha256\":"
      "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\"}]}";
  vectis_embedded_fs_config config;
  vectis_embedded_fs_extract_config extract;
  vectis_embedded_fs *fs = NULL;
  vectis_error error;
  vectis_embedded_fs_config_init(&config);
  config.manifest_json = manifest;
  config.manifest_json_size = sizeof(manifest) - 1u;
  config.payload = payload;
  config.payload_size = sizeof(payload) - 1u;
  CHECK(vectis_embedded_fs_from_pack(&config, &fs, &error) == VECTIS_OK);
  vectis_embedded_fs_extract_config_init(&extract);
  extract.output_dir = root;
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_OVERWRITE;
  CHECK(fs->extract(fs, &extract, &error) == VECTIS_OK);
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_VERIFY;
  CHECK(fs->extract(fs, &extract, &error) == VECTIS_OK);
  fs->close(fs);
  return 0;
}
static int webdav_case(const char *root, int direct) {
  vectis_webdav_config config;
  vectis_webdav_entry *entry;
  char *cache, *disk, *path, *copied;
  unsigned char *body = NULL;
  size_t length, size;
  unsigned i;
  entry = (vectis_webdav_entry *)malloc(sizeof(*entry));
  CHECK(entry != NULL);
  vectis_webdav_config_init(&config);
  cache = (char *)malloc(65536u);
  disk = (char *)malloc(65536u);
  path = (char *)malloc(1025u);
  copied = (char *)malloc(1025u);
  CHECK(cache && disk && path && copied);
  CHECK(snprintf(cache, 4096u, "%s/cache-%d", root, direct) > 0);
  CHECK(snprintf(disk, 4096u, "%s/disk-%d", root, direct) > 0);
  if (direct)
    CHECK(mkdir(disk, 0700) == 0);
  config.cache_dir = cache;
  config.site_id = "stack";
  config.root_dir = direct ? disk : NULL;
  CHECK(vectis_webdav_mkcol(&config, "/src") == VECTIS_WEBDAV_OK);
  strcpy(path, "/src");
  for (i = 0u; i < 160u; ++i) {
    strcat(path, "/a");
    CHECK(vectis_webdav_mkcol(&config, path) == VECTIS_WEBDAV_OK);
  }
  strcat(path, "/file");
  length = strlen(path);
  CHECK(vectis_webdav_put(&config, path, (const unsigned char *)"payload",
                          7u) == VECTIS_WEBDAV_OK);
  CHECK(vectis_webdav_copy(&config, "/src", "/dst", 0) == VECTIS_WEBDAV_OK);
  strcpy(copied, path);
  memcpy(copied, "/dst", 4u);
  CHECK(vectis_webdav_read(&config, copied, &body, &size, entry) ==
            VECTIS_WEBDAV_OK &&
        size == 7u && memcmp(body, "payload", 7u) == 0);
  free(body);
  body = NULL;
  CHECK(vectis_webdav_copy(&config, "/src", "/dst", 0) == VECTIS_WEBDAV_EXISTS);
  CHECK(vectis_webdav_move(&config, "/dst", "/out", 0) == VECTIS_WEBDAV_OK);
  CHECK(vectis_webdav_delete(&config, "/src") == VECTIS_WEBDAV_OK);
  CHECK(vectis_webdav_delete(&config, "/out") == VECTIS_WEBDAV_OK);
  CHECK(strlen(copied) == length);
  free(entry);
  free(cache);
  free(disk);
  free(path);
  free(copied);
  return 0;
}
static void *worker(void *context) {
  worker_case *test = (worker_case *)context;
  vectis_qr *qr;
#if defined(__linux__)
  pthread_attr_t attributes;
  size_t stack_size;
  if (pthread_getattr_np(pthread_self(), &attributes) != 0)
    return NULL;
  if (pthread_attr_getstacksize(&attributes, &stack_size) != 0)
    return NULL;
  printf("worker stack: %lu bytes; pthread_create attributes: NULL\n",
         (unsigned long)stack_size);
  pthread_attr_destroy(&attributes);
#endif
  if (lua_case(test) || lockdc_case(test->root) || embedded_case(test->root) ||
      webdav_case(test->root, 0) || webdav_case(test->root, 1))
    return NULL;
  qr = (vectis_qr *)malloc(sizeof(*qr));
  if (qr == NULL)
    return NULL;
  if (vectis_qr_encode(qr, (const unsigned char *)"stack-budget", 12u) !=
      VECTIS_TOTP_QR_OK) {
    free(qr);
    return NULL;
  }
  free(qr);
  test->failed = 0;
  return NULL;
}
int main(int argc, char **argv) {
  pthread_t thread;
  worker_case test;
  char *root;
  CHECK(argc == 3);
  root = (char *)malloc(strlen(argv[2]) + 32u);
  CHECK(root != NULL);
  sprintf(root, "%s-XXXXXX", argv[2]);
  CHECK(mkdtemp(root) != NULL);
  test.root = root;
  test.script = argv[1];
  test.failed = 1;
  CHECK(pthread_create(&thread, NULL, worker, &test) == 0 &&
        pthread_join(thread, NULL) == 0);
  CHECK(test.failed == 0);
  CHECK(vectis_acme_state_runtime_dir_remove(root));
  free(root);
  puts("default-stack library and Lua worker behavior passed");
  return 0;
}
