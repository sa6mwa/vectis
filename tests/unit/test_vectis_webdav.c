#include <vectis/webdav.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;

static void expect(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "failure: %s\n", message);
    failures++;
  }
}

static void remove_tree(const char *path) {
  DIR *directory;
  struct dirent *item;
  struct stat st;
  char child[4096];
  int written;

  directory = opendir(path);
  if (directory == NULL) {
    return;
  }
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
      continue;
    }
    written = snprintf(child, sizeof(child), "%s/%s", path, item->d_name);
    if (written < 0 || (size_t)written >= sizeof(child) ||
        lstat(child, &st) == -1) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      remove_tree(child);
    } else {
      (void)unlink(child);
    }
  }
  (void)closedir(directory);
  (void)rmdir(path);
}

typedef struct list_state {
  int files;
  int collections;
} list_state;

static int count_entry(const char *path, vectis_webdav_entry_kind kind,
                       size_t size, void *userdata) {
  list_state *state;

  (void)path;
  (void)size;
  state = (list_state *)userdata;
  if (kind == VECTIS_WEBDAV_ENTRY_FILE) {
    state->files++;
  }
  if (kind == VECTIS_WEBDAV_ENTRY_COLLECTION) {
    state->collections++;
  }
  return 1;
}

static vectis_status
allow_webdav_auth(const vectis_webdav_auth_request *request,
                  vectis_webdav_auth_response *response, void *userdata,
                  vectis_error *error) {
  (void)request;
  (void)userdata;
  vectis_error_clear(error);
  vectis_webdav_auth_response_init(response);
  response->action = VECTIS_WEBDAV_AUTH_ALLOW;
  (void)snprintf(response->principal, sizeof(response->principal), "unit-user");
  return VECTIS_OK;
}

int main(void) {
  char temp[] = "/tmp/vectis-webdav-unit.XXXXXX";
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  char content_dir[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  unsigned char *body;
  size_t body_size;
  vectis_webdav_config config;
  vectis_webdav_config limited_config;
  vectis_webdav_mount_config mount;
  vectis_webdav_auth_response auth_response;
  vectis_webdav_entry entry;
  vectis_webdav_status status;
  vectis_status cstatus;
  vectis_app_config app_config;
  vectis_error error;
  vectis_app *app;
  list_state listed;

  if (mkdtemp(temp) == NULL) {
    perror("mkdtemp");
    return 1;
  }

  vectis_webdav_config_init(&config);
  config.cache_dir = temp;
  config.site_id = "test";
  config.max_file_bytes = 16u;
  config.max_total_bytes = 64u;
  cstatus = vectis_webdav_content_dir(&config, content_dir, &error);
  expect(cstatus == VECTIS_OK &&
             strstr(content_dir, "/webdav/test/content") != NULL,
         "reports WebDAV mutable content directory");

  vectis_webdav_mount_config_init(&mount);
  expect(mount.auth_required == 1 && mount.conceal_unauthorized == 1 &&
             mount.auth == NULL,
         "defaults WebDAV mounts to protected concealment");
  vectis_webdav_auth_response_init(&auth_response);
  expect(auth_response.action == VECTIS_WEBDAV_AUTH_DENY &&
             auth_response.status_code == 0 && auth_response.location == NULL &&
             auth_response.www_authenticate == NULL,
         "defaults WebDAV auth responses to deny");
  vectis_app_config_init(&app_config);
  app_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app_config.tls.port = 0u;
  vectis_error_clear(&error);
  app = vectis_app_new(&app_config, &error);
  expect(app != NULL, "creates app for WebDAV registration");
  if (app != NULL) {
    mount.path_prefix = "/dav";
    mount.storage = config;
    mount.auth = allow_webdav_auth;
    expect(vectis_register_webdav(app, &mount, &error) == VECTIS_OK &&
               vectis_route_count(app) == 1u,
           "registers mounted WebDAV route with auth adapter");
    app->close(app);
  }

  vectis_app_config_init(&app_config);
  app_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app_config.tls.port = 0u;
  app = vectis_app_new(&app_config, &error);
  expect(app != NULL, "creates app for invalid WebDAV registration");
  if (app != NULL) {
    vectis_webdav_mount_config_init(&mount);
    mount.path_prefix = "/dav";
    expect(vectis_register_webdav(app, &mount, &error) == VECTIS_ERR_INVALID,
           "rejects WebDAV mount without storage");
    mount.storage = config;
    expect(vectis_register_webdav(app, &mount, &error) == VECTIS_ERR_INVALID,
           "rejects protected WebDAV mount without auth adapter");
    mount.auth_required = 0;
    expect(vectis_register_webdav(app, &mount, &error) == VECTIS_OK,
           "registers explicitly public WebDAV mount without auth adapter");
    app->close(app);
  }

  status = vectis_webdav_lookup(&config, "/untouched.txt", &entry);
  expect(status == VECTIS_WEBDAV_NOT_FOUND,
         "missing resources are reported without creating content");

  expect(vectis_webdav_path_normalize("/docs//new-file.txt/", normalized) &&
             strcmp(normalized, "/docs/new-file.txt") == 0,
         "normalizes collection paths");
  expect(!vectis_webdav_path_normalize("/docs/../secret", normalized),
         "rejects traversal");
  expect(!vectis_webdav_path_normalize("/docs/space name", normalized),
         "rejects ambiguous paths");

  status = vectis_webdav_put(&config, "/draft.txt",
                             (const unsigned char *)"hello", 5u);
  expect(status == VECTIS_WEBDAV_OK, "writes file content");
  body = NULL;
  body_size = 0u;
  status = vectis_webdav_read(&config, "/draft.txt", &body, &body_size, &entry);
  expect(status == VECTIS_WEBDAV_OK && body_size == 5u &&
             memcmp(body, "hello", 5u) == 0 &&
             entry.kind == VECTIS_WEBDAV_ENTRY_FILE &&
             strlen(entry.etag) == VECTIS_WEBDAV_ETAG_LENGTH,
         "reads file content with metadata");
  free(body);

  status = vectis_webdav_mkcol(&config, "/docs");
  expect(status == VECTIS_WEBDAV_OK, "creates collection");
  status = vectis_webdav_put(&config, "/docs/a.txt",
                             (const unsigned char *)"doc", 3u);
  expect(status == VECTIS_WEBDAV_OK, "writes nested file");
  listed.files = 0;
  listed.collections = 0;
  status = vectis_webdav_list(&config, "/", count_entry, &listed);
  expect(status == VECTIS_WEBDAV_OK && listed.collections >= 1,
         "lists collections");
  status = vectis_webdav_copy(&config, "/docs", "/docs-copy", 0);
  expect(status == VECTIS_WEBDAV_OK, "copies collection");
  body = NULL;
  body_size = 0u;
  status = vectis_webdav_read(&config, "/docs-copy/a.txt", &body, &body_size,
                              &entry);
  expect(status == VECTIS_WEBDAV_OK && body_size == 3u &&
             memcmp(body, "doc", 3u) == 0,
         "reads copied nested file");
  free(body);
  status = vectis_webdav_move(&config, "/docs-copy", "/docs-moved", 1);
  expect(status == VECTIS_WEBDAV_OK, "moves collection");
  status = vectis_webdav_lookup(&config, "/docs-copy/a.txt", &entry);
  expect(status == VECTIS_WEBDAV_OK &&
             entry.kind == VECTIS_WEBDAV_ENTRY_TOMBSTONE,
         "tombstones moved source");
  status = vectis_webdav_lookup(&config, "/docs-moved/a.txt", &entry);
  expect(status == VECTIS_WEBDAV_OK, "keeps moved destination");

  status = vectis_webdav_delete(&config, "/draft.txt");
  expect(status == VECTIS_WEBDAV_OK, "deletes file");
  status = vectis_webdav_lookup(&config, "/draft.txt", &entry);
  expect(status == VECTIS_WEBDAV_OK &&
             entry.kind == VECTIS_WEBDAV_ENTRY_TOMBSTONE,
         "records tombstone");

  limited_config = config;
  limited_config.site_id = "limited";
  limited_config.max_total_bytes = 5u;
  limited_config.max_file_bytes = 4u;
  status = vectis_webdav_put(&limited_config, "/fits.txt",
                             (const unsigned char *)"abcd", 4u);
  expect(status == VECTIS_WEBDAV_OK, "allows file within quota");
  status = vectis_webdav_put(&limited_config, "/too-large.txt",
                             (const unsigned char *)"ab", 2u);
  expect(status == VECTIS_WEBDAV_LIMIT, "enforces aggregate quota");

  remove_tree(temp);
  return failures == 0 ? 0 : 1;
}
