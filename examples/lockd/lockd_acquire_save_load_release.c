#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lc/lc.h>
#include <lonejson.h>
#include <vectis/vectis.h>

typedef struct account_doc {
  char id[64];
  char status[32];
  lonejson_int64 version;
} account_doc;

static const lonejson_field account_doc_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(account_doc, id, "id",
                                    LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(account_doc, status, "status",
                                    LONEJSON_OVERFLOW_FAIL),
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

static void print_vectis_error(const char *operation,
                               const vectis_error *error) {
  fprintf(stderr, "%s failed", operation);
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, ": %s", error->message);
  }
  if (error != NULL && error->detail[0] != '\0') {
    fprintf(stderr, " (%s)", error->detail);
  }
  fprintf(stderr, "\n");
}

int main(void) {
  lc_client_config config;
  lc_client *client;
  lc_error error;
  vectis_error verror;
  account_doc saved = {"", "", 0};
  account_doc loaded = {"", "", 0};
  const char *endpoints[1];
  vectis_status status;

  lc_client_config_init(&config);
  lc_error_init(&error);
  vectis_error_clear(&verror);
  client = NULL;

  endpoints[0] = getenv("LOCKD_ENDPOINT") != NULL ? getenv("LOCKD_ENDPOINT")
                                                  : "https://127.0.0.1:8443";
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

  (void)snprintf(saved.id, sizeof(saved.id), "%s", "1001");
  (void)snprintf(saved.status, sizeof(saved.status), "%s", "active");
  saved.version = 1;
  status =
      vectis_lockd_state_save(client, "accounts/1001", "vectis-lockd-example",
                              30L, &account_doc_map, &saved, &verror);
  if (status == VECTIS_OK) {
    status =
        vectis_lockd_state_load(client, "accounts/1001", "vectis-lockd-example",
                                30L, &account_doc_map, &loaded, &verror);
  }
  if (status != VECTIS_OK) {
    print_vectis_error("vectis_lockd_state_save/load", &verror);
    lc_client_close(client);
    lc_error_cleanup(&error);
    return 1;
  }

  lc_client_close(client);
  lc_error_cleanup(&error);
  return 0;
}
