#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <lc/lc.h>
#include <lonejson.h>

typedef struct account_doc {
  char id[64];
  char status[32];
  lonejson_int64 version;
} account_doc;

static const lonejson_field account_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(account_doc, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(account_doc, status, "status", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_I64(account_doc, version, "version")};

LONEJSON_MAP_DEFINE(account_doc_map, account_doc, account_doc_fields);

static void print_error(const char *operation, const lc_error *error) {
  fprintf(stderr, "%s failed", operation);
  if (error != NULL && error->message != NULL) {
    fprintf(stderr, ": %s", error->message);
  }
  if (error != NULL && error->detail != NULL) {
    fprintf(stderr, " (%s)", error->detail);
  }
  fprintf(stderr, "\n");
}

int main(void) {
  lc_client_config config;
  lc_client *client;
  lc_lease *lease;
  lc_acquire_req acquire;
  lc_release_req release;
  lc_get_res get_response;
  lc_error error;
  account_doc saved;
  account_doc loaded;
  const char *endpoints[1];

  lc_client_config_init(&config);
  lc_acquire_req_init(&acquire);
  lc_release_req_init(&release);
  lc_error_init(&error);
  client = NULL;
  lease = NULL;
  memset(&get_response, 0, sizeof(get_response));
  memset(&saved, 0, sizeof(saved));
  memset(&loaded, 0, sizeof(loaded));

  endpoints[0] = getenv("LOCKD_ENDPOINT") != NULL ? getenv("LOCKD_ENDPOINT") : "https://127.0.0.1:8443";
  config.endpoints = endpoints;
  config.endpoint_count = 1u;
  config.client_bundle_path = getenv("LOCKD_CLIENT_BUNDLE");
  config.default_namespace = "examples";
  config.disable_mtls = config.client_bundle_path == NULL;
  config.insecure_skip_verify = config.client_bundle_path == NULL;

  if (lc_client_open(&config, &client, &error) != LC_OK) {
    print_error("lc_client_open", &error);
    lc_error_cleanup(&error);
    return 1;
  }

  acquire.key = "accounts/1001";
  acquire.owner = "vectis-lockd-example";
  acquire.ttl_seconds = 30L;
  if (lc_acquire(client, &acquire, &lease, &error) != LC_OK) {
    print_error("lc_acquire", &error);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }

  (void)snprintf(saved.id, sizeof(saved.id), "%s", "1001");
  (void)snprintf(saved.status, sizeof(saved.status), "%s", "active");
  saved.version = 1;
  if (lease->save(lease, &account_doc_map, &saved, NULL, &error) != LC_OK) {
    print_error("lease.save", &error);
    lc_lease_close(lease);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }
  if (lease->load(lease, &account_doc_map, &loaded, NULL, NULL, &get_response, &error) != LC_OK) {
    print_error("lease.load", &error);
    lc_lease_close(lease);
    lc_client_close(client);
    lc_get_res_cleanup(&get_response);
    lc_error_cleanup(&error);
    return 1;
  }
  lc_get_res_cleanup(&get_response);
  memset(&get_response, 0, sizeof(get_response));
  if (lease->release(lease, &release, &error) != LC_OK) {
    print_error("lease.release", &error);
    lc_lease_close(lease);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }
  lease = NULL;

  lc_client_close(client);
  lc_error_cleanup(&error);
  return 0;
}
