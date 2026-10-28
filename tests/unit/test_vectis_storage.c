#include <assert.h>
#include <lc/postgres.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vectis/storage.h>
typedef struct state_doc {
  char id[16];
} state_doc;
static const lonejson_field state_fields[] = {LONEJSON_FIELD_STRING_FIXED_REQ(
    state_doc, id, "id", LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(state_map, state_doc, state_fields);
static vectis_status change_state(lc_lease *lease, void *value, int *save,
                                  void *context, vectis_error *error) {
  state_doc *doc = value;
  (void)lease;
  (void)context;
  (void)error;
  assert(!strcmp(doc->id, "before"));
  strcpy(doc->id, "after");
  *save = 1;
  return VECTIS_OK;
}
static void retained_state(vectis_app *app) {
  state_doc value;
  lc_update_opts options;
  lc_acquire_req acquire;
  lc_release_req release;
  lc_keepalive_req renewal;
  lc_source *source = NULL;
  lc_version version;
  lc_lease *lease = NULL;
  lc_client *client = vectis_lockd_client(app);
  lc_error native;
  vectis_error error;
  lc_error_init(&native);
  lc_update_opts_init(&options);
  options.has_retention_seconds = 1;
  options.retention_seconds = 3600;
  strcpy(value.id, "before");
  assert(vectis_lockd_state_save(client, "retained", "test", 30L, &state_map,
                                 &value, &options, &error) == VECTIS_OK);
  lc_acquire_req_init(&acquire);
  acquire.key = "retained";
  acquire.owner = "test";
  acquire.ttl_seconds = 30L;
  assert(client->acquire(client, &acquire, &lease, &native) == LC_OK);
  assert(lease->describe(lease, &native) == LC_OK);
  assert(lease->has_retention_expires_at_unix &&
         lease->retention_expires_at_unix > 0);
  assert(lc_source_from_memory("{\"id\":\"before\"}", 15u, &source, &native) ==
         LC_OK);
  assert(lease->update(lease, source, NULL, &native) == LC_OK);
  source->close(source);
  version = lease->version;
  lc_keepalive_req_init(&renewal);
  renewal.ttl_seconds = 30L;
  assert(lease->keepalive(lease, &renewal, &native) == LC_OK);
  assert(lease->version == version);
  assert(lc_source_from_memory("{\"id\":\"before\"}", 15u, &source, &native) ==
         LC_OK);
  assert(lease->update(lease, source, NULL, &native) == LC_OK);
  source->close(source);
  lc_release_req_init(&release);
  assert(lease->release(lease, &release, &native) == LC_OK);
  options.retention_seconds = 0;
  memset(&value, 0, sizeof(value));
  assert(vectis_lockd_state_update(client, "retained", "test", 30L, &state_map,
                                   &value, change_state, NULL, &options,
                                   &error) == VECTIS_OK);
  assert(!strcmp(value.id, "after"));
  assert(client->acquire(client, &acquire, &lease, &native) == LC_OK);
  assert(lease->describe(lease, &native) == LC_OK);
  assert(lease->has_retention_expires_at_unix &&
         lease->retention_expires_at_unix == 0);
  assert(lease->release(lease, &release, &native) == LC_OK);
  options.retention_seconds = -1;
  assert(vectis_lockd_state_save(client, "invalid-retention", "test", 30L,
                                 &state_map, &value, &options,
                                 &error) != VECTIS_OK);
  lc_error_cleanup(&native);
}
static int count_collection(void *context, const lc_collection_info *info,
                            lc_error *error) {
  int *count = (int *)context;
  (void)error;
  assert(!strcmp(info->name, "journal"));
  ++*count;
  return LC_OK;
}
int main(int argc, char **argv) {
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  lc_error native;
  lc_collection_config collection_config;
  lc_collection *collection = NULL, *second = NULL;
  lc_collection_append_req request;
  lc_collection_append_res appended;
  lc_collection_capabilities capabilities;
  lc_collection_read_opts read_options;
  lc_collection_read_res progress;
  lc_collection_info info;
  lc_xa_coordinator *coordinator = NULL;
  lc_source *source = NULL;
  lc_sink *sink = NULL;
  const char *endpoints[1];
  char endpoint[4096], *key = NULL;
  int count = 0;
  assert(argc == 2);
  assert(strlen(argv[1]) + 9u < sizeof(endpoint));
  strcpy(endpoint, "pouch://");
  strcat(endpoint, argv[1]);
  endpoints[0] = endpoint;
  lc_error_init(&native);
  assert(lc_pouch_crypto_generate_key_string(&key, &native) == LC_OK);
  vectis_app_config_init(&config);
  config.app_name = "storage-c-test";
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.lockd.endpoints = endpoints;
  config.lockd.endpoint_count = 1;
  config.lockd.pouch_crypto_key = key;
  config.lockd.logger_disabled = 1;
  app = vectis_app_new(&config, &error);
  assert(app);
  lc_collection_config_init(&collection_config);
  collection_config.ns = "events";
  collection_config.name = "journal";
  assert(vectis_app_create_collection(app, &collection_config, &collection,
                                      &error) == VECTIS_ERR_STATE &&
         !collection);
  assert(vectis_app_create_collection(app, &collection_config, NULL, &error) ==
         VECTIS_ERR_INVALID);
  assert(vectis_start(app, &error) == VECTIS_OK);
  retained_state(app);
  assert(vectis_app_collection_capabilities(app, &capabilities, &error) ==
             VECTIS_OK &&
         capabilities.collections);
  assert(vectis_app_create_collection(app, &collection_config, &collection,
                                      &error) == VECTIS_OK);
  lc_collection_append_req_init(&request);
  request.append_id = "abc";
  assert(lc_source_from_memory("{\"n\":1}", 7u, &source, &native) == LC_OK);
  assert(collection->append(collection, &request, source, &appended, &native) ==
             LC_OK &&
         appended.position == 1u);
  lc_source_close(source);
  assert(vectis_app_open_collection(app, &collection_config, &second, &error) ==
         VECTIS_OK);
  assert(second->describe(second, &info, &native) == LC_OK &&
         info.live_count == 1u);
  assert(vectis_app_list_collections(app, "events", count_collection, &count,
                                     &error) == VECTIS_OK &&
         count == 1);
  lc_collection_read_opts_init(&read_options);
  lc_collection_read_res_init(&progress);
  assert(lc_sink_to_discard(&sink, &native) == LC_OK);
  assert(second->read(second, &read_options, sink, &progress, &native) ==
         LC_OK);
  assert(progress.complete && progress.completed_records == 1u);
  lc_collection_read_res_cleanup(&progress);
  lc_sink_close(sink);
  assert(vectis_app_xa_coordinator_open(app, NULL, &coordinator, &error) ==
         VECTIS_OK);
  assert(lc_postgres_driver() != NULL);
  assert(lc_xa_coordinator_close_blocking(coordinator, &native) == LC_OK);
  second->close(second);
  collection->close(collection);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  assert(vectis_app_collection_capabilities(app, &capabilities, &error) ==
         VECTIS_ERR_STATE);
  vectis_app_close(app);
  free(key);
  lc_error_cleanup(&native);
  return 0;
}
