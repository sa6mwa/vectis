#include "vectis_acme_state.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <lc/lc.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VECTIS_ACME_STATE_MANIFEST "{\"schema\":1,\"kind\":\"acme\"}\n"

static int vectis_acme_state_domain_valid(const char *domain);

static char *vectis_acme_state_strdup(const char *value) {
  size_t size;
  char *copy;

  if (value == NULL) {
    return NULL;
  }
  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  if (copy != NULL) {
    memcpy(copy, value, size);
  }
  return copy;
}

static int vectis_acme_state_mkdirs(const char *path) {
  char copy[4096];
  char *cursor;
  size_t size;
  struct stat st;

  if (path == NULL || path[0] != '/') {
    return 0;
  }
  size = strlen(path);
  if (size == 0u || size >= sizeof(copy)) {
    return 0;
  }
  memcpy(copy, path, size + 1u);
  for (cursor = copy + 1; *cursor != '\0'; ++cursor) {
    if (*cursor == '/') {
      *cursor = '\0';
      if (mkdir(copy, 0700) != 0 &&
          (errno != EEXIST || stat(copy, &st) != 0 || !S_ISDIR(st.st_mode))) {
        return 0;
      }
      *cursor = '/';
    }
  }
  return mkdir(copy, 0700) == 0 ||
         (errno == EEXIST && stat(copy, &st) == 0 && S_ISDIR(st.st_mode));
}

char *vectis_acme_state_default_endpoint(vectis_error *error) {
  const char *state_home;
  const char *home;
  char path[4096];
  char endpoint[4110];
  int written;

  state_home = getenv("XDG_STATE_HOME");
  if (state_home != NULL && state_home[0] == '/') {
    written = snprintf(path, sizeof(path), "%s/vectis/storage", state_home);
  } else {
    home = getenv("HOME");
    if (home == NULL || home[0] != '/') {
      vectis_set_error(error, VECTIS_ERR_STATE,
                       "XDG_STATE_HOME or HOME is required for ACME state");
      return NULL;
    }
    written = snprintf(path, sizeof(path), "%s/.local/state/vectis/storage",
                       home);
  }
  if (written <= 0 || (size_t)written >= sizeof(path) ||
      !vectis_acme_state_mkdirs(path)) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to create default ACME Pouch state directory");
    return NULL;
  }
  written = snprintf(endpoint, sizeof(endpoint), "pouch://%s", path);
  if (written <= 0 || (size_t)written >= sizeof(endpoint)) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "default ACME Pouch endpoint path is too long");
    return NULL;
  }
  return vectis_acme_state_strdup(endpoint);
}

char *vectis_acme_state_default_key(const char *const *domains,
                                    size_t domain_count,
                                    vectis_error *error) {
  uint32_t hash;
  const unsigned char *cursor;
  char key[64];
  size_t i;
  int written;

  if (domains == NULL || domain_count == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "ACME domains are required for the default state key");
    return NULL;
  }
  hash = 2166136261UL;
  for (i = 0u; i < domain_count; ++i) {
    if (!vectis_acme_state_domain_valid(domains[i])) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "ACME domain is invalid for state storage");
      return NULL;
    }
    for (cursor = (const unsigned char *)domains[i]; *cursor != '\0';
         ++cursor) {
      hash ^= (uint32_t)*cursor;
      hash *= UINT32_C(16777619);
    }
    hash ^= (uint32_t)'\n';
    hash *= UINT32_C(16777619);
  }
  written = snprintf(key, sizeof(key), "acme-%08x", (unsigned int)hash);
  if (written <= 0 || (size_t)written >= sizeof(key)) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to format the default ACME state key");
    return NULL;
  }
  return vectis_acme_state_strdup(key);
}

char *vectis_acme_state_runtime_dir_new(vectis_error *error) {
  const char *cache_home;
  const char *home;
  char base[4096];
  char pattern[4096];
  int written;
  char *result;

  cache_home = getenv("XDG_CACHE_HOME");
  if (cache_home != NULL && cache_home[0] == '/') {
    written = snprintf(base, sizeof(base), "%s/vectis", cache_home);
  } else {
    home = getenv("HOME");
    if (home == NULL || home[0] != '/') {
      vectis_set_error(error, VECTIS_ERR_STATE,
                       "XDG_CACHE_HOME or HOME is required for ACME runtime");
      return NULL;
    }
    written = snprintf(base, sizeof(base), "%s/.cache/vectis", home);
  }
  if (written <= 0 || (size_t)written >= sizeof(base) ||
      !vectis_acme_state_mkdirs(base)) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to create ACME runtime cache directory");
    return NULL;
  }
  written = snprintf(pattern, sizeof(pattern), "%s/acme-XXXXXX", base);
  if (written <= 0 || (size_t)written >= sizeof(pattern) ||
      mkdtemp(pattern) == NULL || chmod(pattern, 0700) != 0) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to create private ACME runtime directory");
    return NULL;
  }
  result = vectis_acme_state_strdup(pattern);
  if (result == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy ACME runtime directory");
  }
  return result;
}

static int vectis_acme_state_remove_tree(const char *path) {
  DIR *directory;
  struct dirent *entry;
  struct stat st;
  char child[4096];
  int written;

  if (path == NULL || lstat(path, &st) != 0) {
    return path != NULL && errno == ENOENT;
  }
  if (!S_ISDIR(st.st_mode)) {
    return unlink(path) == 0;
  }
  directory = opendir(path);
  if (directory == NULL) {
    return 0;
  }
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof(child) ||
        !vectis_acme_state_remove_tree(child)) {
      (void)closedir(directory);
      return 0;
    }
  }
  return closedir(directory) == 0 && rmdir(path) == 0;
}

int vectis_acme_state_runtime_dir_remove(const char *path) {
  return path != NULL && path[0] == '/' && vectis_acme_state_remove_tree(path);
}

static int vectis_acme_state_domain_valid(const char *domain) {
  return domain != NULL && domain[0] != '\0' && strchr(domain, '/') == NULL &&
         strstr(domain, "..") == NULL;
}

static int vectis_acme_state_attachment_path(const char *domain,
                                             const char *leaf,
                                             char out[4096]) {
  int written;

  if (domain == NULL) {
    written = snprintf(out, 4096u, "%s", leaf);
  } else if (!vectis_acme_state_domain_valid(domain)) {
    return 0;
  } else {
    written = snprintf(out, 4096u, "certificates/%s/%s", domain, leaf);
  }
  return written > 0 && (size_t)written < 4096u;
}

static int vectis_acme_state_runtime_path(const vectis_acme_state_config *config,
                                          const char *attachment,
                                          char out[4096]) {
  int written;

  if (config == NULL || config->runtime_dir == NULL || attachment == NULL ||
      attachment[0] == '/' || strstr(attachment, "..") != NULL) {
    return 0;
  }
  written = snprintf(out, 4096u, "%s/%s", config->runtime_dir, attachment);
  return written > 0 && (size_t)written < 4096u;
}

static int vectis_acme_state_parent_dir(const char *path, char out[4096]) {
  const char *slash;
  size_t size;

  slash = strrchr(path, '/');
  if (slash == NULL || slash == path) {
    return 0;
  }
  size = (size_t)(slash - path);
  if (size >= 4096u) {
    return 0;
  }
  memcpy(out, path, size);
  out[size] = '\0';
  return 1;
}

static int vectis_acme_state_file_exists(const char *path) {
  struct stat st;

  return path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void vectis_acme_state_set_lockd_error(vectis_error *error,
                                              const char *operation,
                                              const lc_error *lcerr) {
  vectis_set_error(error, VECTIS_ERR_STATE,
                   lcerr != NULL && lcerr->message != NULL ? lcerr->message
                                                            : operation);
}

static int vectis_acme_state_endpoint_is_pouch(const char *endpoint) {
  return endpoint != NULL && strncmp(endpoint, "pouch://", 8u) == 0;
}

static int vectis_acme_state_endpoint_has_pouch_crypto_option(
    const char *endpoint) {
  return endpoint != NULL &&
         (strstr(endpoint, "pouch_crypto_key=") != NULL ||
          strstr(endpoint, "pouch_crypto_key_file=") != NULL);
}

static int vectis_acme_state_client_open(const vectis_acme_state_config *config,
                                         lc_client **client, lc_source **memory,
                                         lc_error *lcerr) {
  lc_client_config client_config;
  char *default_key_file;
  int rc;

  *client = NULL;
  *memory = NULL;
  default_key_file = NULL;
  lc_client_config_init(&client_config);
  client_config.endpoints = &config->endpoint;
  client_config.endpoint_count = 1u;
  client_config.default_namespace = config->namespace_name;
  client_config.timeout_ms = config->timeout_ms > 0L ? config->timeout_ms : 30000L;
  client_config.client_bundle_path = config->client_bundle_path;
  if (vectis_acme_state_endpoint_is_pouch(config->endpoint)) {
    client_config.pouch_crypto_key = config->pouch_crypto_key;
    client_config.pouch_crypto_key_file = config->pouch_crypto_key_file;
    client_config.pouch_crypto_generate_key_file =
        config->pouch_crypto_generate_key_file;
    client_config.pouch_crypto_generate_key_file_set =
        config->pouch_crypto_generate_key_file_set;
    client_config.pouch_compression = config->pouch_compression;
    if (client_config.pouch_crypto_key == NULL &&
        client_config.pouch_crypto_key_file == NULL &&
        !vectis_acme_state_endpoint_has_pouch_crypto_option(config->endpoint)) {
      rc = lc_pouch_crypto_default_key_file(&default_key_file, lcerr);
      if (rc != LC_OK) {
        return rc;
      }
      client_config.pouch_crypto_key_file = default_key_file;
      client_config.pouch_crypto_generate_key_file = 1;
      client_config.pouch_crypto_generate_key_file_set = 1;
    }
  }
  if (config->client_bundle_pem != NULL && config->client_bundle_pem_size > 0u) {
    rc = lc_source_from_memory(config->client_bundle_pem,
                               config->client_bundle_pem_size, memory, lcerr);
    if (rc != LC_OK) {
      if (default_key_file != NULL) {
        lc_pouch_crypto_key_string_free(default_key_file);
      }
      return rc;
    }
    client_config.client_bundle_source = *memory;
  }
  rc = lc_client_open(&client_config, client, lcerr);
  if (*memory != NULL) {
    (*memory)->close(*memory);
    *memory = NULL;
  }
  if (default_key_file != NULL) {
    lc_pouch_crypto_key_string_free(default_key_file);
  }
  return rc;
}

static int vectis_acme_state_list_has(const lc_attachment_list *list,
                                      const char *name) {
  size_t i;

  if (list == NULL || name == NULL) {
    return 0;
  }
  for (i = 0u; i < list->count; ++i) {
    if (list->items[i].name != NULL && strcmp(list->items[i].name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int vectis_acme_state_hydrate_attachment(
    lc_lease *lease, const vectis_acme_state_config *config,
    const lc_attachment_list *list, const char *attachment, lc_error *lcerr) {
  lc_attachment_get_req get;
  lc_attachment_get_res result;
  lc_sink *sink;
  char path[4096];
  char temporary[4096];
  char parent[4096];
  int written;
  int rc;

  if (!vectis_acme_state_list_has(list, attachment)) {
    return LC_OK;
  }
  if (!vectis_acme_state_runtime_path(config, attachment, path) ||
      !vectis_acme_state_parent_dir(path, parent) ||
      !vectis_acme_state_mkdirs(parent)) {
    return LC_ERR_INVALID;
  }
  written = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  if (written <= 0 || (size_t)written >= sizeof(temporary)) {
    return LC_ERR_INVALID;
  }
  (void)unlink(temporary);
  sink = NULL;
  memset(&result, 0, sizeof(result));
  lc_attachment_get_req_init(&get);
  get.selector.name = attachment;
  rc = lc_sink_to_file(temporary, &sink, lcerr);
  if (rc == LC_OK) {
    rc = lease->get_attachment(lease, &get, sink, &result, lcerr);
  }
  if (sink != NULL) {
    sink->close(sink);
  }
  lc_attachment_get_res_cleanup(&result);
  if (rc != LC_OK || chmod(temporary, 0600) != 0 ||
      rename(temporary, path) != 0) {
    (void)unlink(temporary);
    return rc == LC_OK ? LC_ERR_TRANSPORT : rc;
  }
  return LC_OK;
}

static int vectis_acme_state_hydrate_all(lc_lease *lease,
                                         const vectis_acme_state_config *config,
                                         const lc_attachment_list *list,
                                         lc_error *lcerr) {
  char attachment[4096];
  size_t i;
  int rc;

  rc = vectis_acme_state_hydrate_attachment(lease, config, list,
                                             "account-key.pem", lcerr);
  if (rc != LC_OK) {
    return rc;
  }
  for (i = 0u; i < config->domain_count; ++i) {
    if (!vectis_acme_state_attachment_path(config->domains[i], "fullchain.pem",
                                           attachment)) {
      return LC_ERR_INVALID;
    }
    rc = vectis_acme_state_hydrate_attachment(lease, config, list, attachment,
                                               lcerr);
    if (rc != LC_OK) {
      return rc;
    }
    if (!vectis_acme_state_attachment_path(config->domains[i], "key.pem",
                                           attachment)) {
      return LC_ERR_INVALID;
    }
    rc = vectis_acme_state_hydrate_attachment(lease, config, list, attachment,
                                               lcerr);
    if (rc != LC_OK) {
      return rc;
    }
  }
  return LC_OK;
}

vectis_status vectis_acme_state_hydrate(const vectis_acme_state_config *config,
                                        int *hydrated,
                                        vectis_error *error) {
  lc_client *client;
  lc_source *memory;
  lc_lease *lease;
  lc_acquire_req acquire;
  lc_release_req release;
  lc_attachment_list list;
  lc_error lcerr;
  int rc;

  if (hydrated != NULL) {
    *hydrated = 0;
  }
  if (config == NULL || config->endpoint == NULL || config->key == NULL ||
      config->runtime_dir == NULL || !vectis_acme_state_mkdirs(config->runtime_dir)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "ACME state hydration configuration is invalid");
    return VECTIS_ERR_INVALID;
  }
  client = NULL;
  memory = NULL;
  lease = NULL;
  memset(&list, 0, sizeof(list));
  lc_error_init(&lcerr);
  rc = vectis_acme_state_client_open(config, &client, &memory, &lcerr);
  if (rc == LC_OK) {
    lc_acquire_req_init(&acquire);
    acquire.namespace_name = config->namespace_name;
    acquire.key = config->key;
    acquire.owner = config->owner;
    acquire.ttl_seconds = 30L;
    acquire.block_seconds = 1L;
    rc = client->acquire(client, &acquire, &lease, &lcerr);
  }
  if (rc == LC_OK) {
    rc = lease->list_attachments(lease, &list, &lcerr);
  }
  if (rc == LC_OK) {
    rc = vectis_acme_state_hydrate_all(lease, config, &list, &lcerr);
  }
  if (rc == LC_OK && hydrated != NULL) {
    *hydrated = list.count > 0u;
  }
  lc_attachment_list_cleanup(&list);
  if (lease != NULL) {
    lc_release_req_init(&release);
    if (rc == LC_OK) {
      rc = lease->release(lease, &release, &lcerr);
    }
    if (rc != LC_OK) {
      lease->close(lease);
    }
  }
  if (client != NULL) {
    client->close(client);
  }
  if (rc != LC_OK) {
    vectis_acme_state_set_lockd_error(error, "failed to hydrate ACME state", &lcerr);
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}

typedef struct vectis_acme_state_persist_context {
  const vectis_acme_state_config *config;
} vectis_acme_state_persist_context;

static int vectis_acme_state_persist_attachment(
    lc_lease *lease, const vectis_acme_state_config *config,
    const char *attachment, lc_error *lcerr) {
  lc_attach_req attach;
  lc_attach_res result;
  lc_source *source;
  char path[4096];
  int rc;

  if (!vectis_acme_state_runtime_path(config, attachment, path)) {
    return LC_ERR_INVALID;
  }
  if (!vectis_acme_state_file_exists(path)) {
    return LC_OK;
  }
  source = NULL;
  memset(&result, 0, sizeof(result));
  rc = lc_source_from_file(path, &source, lcerr);
  if (rc != LC_OK) {
    return rc;
  }
  lc_attach_req_init(&attach);
  attach.name = attachment;
  attach.content_type = "application/x-pem-file";
  rc = lease->attach(lease, &attach, source, &result, lcerr);
  source->close(source);
  lc_attach_res_cleanup(&result);
  return rc;
}

static int vectis_acme_state_persist_update(void *context,
                                             lc_acquire_for_update_context *update,
                                             lc_error *lcerr) {
  vectis_acme_state_persist_context *persist;
  lc_source *manifest;
  lc_update_opts options;
  char attachment[4096];
  size_t i;
  int rc;

  persist = (vectis_acme_state_persist_context *)context;
  if (persist == NULL || persist->config == NULL || update == NULL ||
      update->lease == NULL) {
    return LC_ERR_INVALID;
  }
  rc = vectis_acme_state_persist_attachment(update->lease, persist->config,
                                             "account-key.pem", lcerr);
  if (rc != LC_OK) {
    return rc;
  }
  for (i = 0u; i < persist->config->domain_count; ++i) {
    if (!vectis_acme_state_attachment_path(persist->config->domains[i],
                                           "fullchain.pem", attachment)) {
      return LC_ERR_INVALID;
    }
    rc = vectis_acme_state_persist_attachment(update->lease, persist->config,
                                               attachment, lcerr);
    if (rc != LC_OK) {
      return rc;
    }
    if (!vectis_acme_state_attachment_path(persist->config->domains[i],
                                           "key.pem", attachment)) {
      return LC_ERR_INVALID;
    }
    rc = vectis_acme_state_persist_attachment(update->lease, persist->config,
                                               attachment, lcerr);
    if (rc != LC_OK) {
      return rc;
    }
  }
  manifest = NULL;
  rc = lc_source_from_memory(VECTIS_ACME_STATE_MANIFEST,
                             strlen(VECTIS_ACME_STATE_MANIFEST), &manifest,
                             lcerr);
  if (rc != LC_OK) {
    return rc;
  }
  lc_update_opts_init(&options);
  options.content_type = "application/json";
  rc = update->lease->update(update->lease, manifest, &options, lcerr);
  manifest->close(manifest);
  return rc;
}

vectis_status vectis_acme_state_persist(const vectis_acme_state_config *config,
                                        vectis_error *error) {
  lc_client *client;
  lc_source *memory;
  lc_acquire_req acquire;
  lc_error lcerr;
  vectis_acme_state_persist_context context;
  int rc;

  if (config == NULL || config->endpoint == NULL || config->key == NULL ||
      config->runtime_dir == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "ACME state persistence configuration is invalid");
    return VECTIS_ERR_INVALID;
  }
  lc_error_init(&lcerr);
  client = NULL;
  memory = NULL;
  rc = vectis_acme_state_client_open(config, &client, &memory, &lcerr);
  if (rc == LC_OK) {
    lc_acquire_req_init(&acquire);
    acquire.namespace_name = config->namespace_name;
    acquire.key = config->key;
    acquire.owner = config->owner;
    acquire.ttl_seconds = 30L;
    acquire.block_seconds = 1L;
    context.config = config;
    rc = client->acquire_for_update(client, &acquire,
                                    vectis_acme_state_persist_update, &context,
                                    &lcerr);
  }
  if (client != NULL) {
    client->close(client);
  }
  if (rc != LC_OK) {
    vectis_acme_state_set_lockd_error(error, "failed to persist ACME state", &lcerr);
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  vectis_error_clear(error);
  return VECTIS_OK;
}
