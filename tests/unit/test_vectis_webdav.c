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

static int write_test_file(const char *path, const char *body) {
  size_t body_size;
  size_t written_size;
  FILE *file;

  if (path == NULL || body == NULL) {
    return 0;
  }
  file = fopen(path, "wb");
  if (file == NULL) {
    return 0;
  }
  body_size = strlen(body);
  written_size = fwrite(body, 1u, body_size, file);
  return fclose(file) == 0 && written_size == body_size;
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
  char root_dir[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char direct_file[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char outside_dir[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char outside_file[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char outside_write[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char outside_copy[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char outside_collection[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char symlink_path[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  unsigned char *body;
  size_t body_size;
  vectis_webdav_config config;
  vectis_webdav_config direct_config;
  vectis_webdav_config limited_config;
  vectis_webdav_mount_config mount;
  vectis_webdav_embedded_mount_config embedded_mount;
  vectis_webdav_auth_response auth_response;
  vectis_webdav_entry entry;
  vectis_webdav_status status;
  vectis_status cstatus;
  vectis_app_config app_config;
  vectis_error error;
  vectis_app *app;
  vectis_embedded_fs fake_fs;
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
  vectis_webdav_embedded_mount_config_init(&embedded_mount);
  expect(embedded_mount.auth_required == 1 &&
             embedded_mount.conceal_unauthorized == 1 &&
             embedded_mount.auth == NULL && embedded_mount.path_prefix != NULL,
         "defaults embedded WebDAV mounts to protected concealment");
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
    expect(app->webdav(app, &mount, &error) == VECTIS_OK &&
               vectis_route_count(app) == 1u,
           "registers mounted WebDAV route with auth adapter");
    memset(&fake_fs, 0, sizeof(fake_fs));
    vectis_webdav_embedded_mount_config_init(&embedded_mount);
    embedded_mount.path_prefix = "/embedded-dav";
    embedded_mount.fs = &fake_fs;
    embedded_mount.auth = allow_webdav_auth;
    expect(app->webdav_embedded(app, &embedded_mount, &error) == VECTIS_OK &&
               vectis_route_count(app) == 2u,
           "registers read-only embedded WebDAV route with auth adapter");
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
    expect(app->webdav(app, &mount, &error) == VECTIS_ERR_INVALID,
           "rejects WebDAV mount without storage");
    mount.storage = config;
    expect(app->webdav(app, &mount, &error) == VECTIS_ERR_INVALID,
           "rejects protected WebDAV mount without auth adapter");
    mount.auth_required = 0;
    expect(app->webdav(app, &mount, &error) == VECTIS_OK,
           "registers explicitly public WebDAV mount without auth adapter");
    memset(&fake_fs, 0, sizeof(fake_fs));
    vectis_webdav_embedded_mount_config_init(&embedded_mount);
    embedded_mount.path_prefix = "/embedded-dav";
    expect(app->webdav_embedded(app, &embedded_mount, &error) ==
               VECTIS_ERR_INVALID,
           "rejects embedded WebDAV mount without embedded fs");
    embedded_mount.fs = &fake_fs;
    expect(app->webdav_embedded(app, &embedded_mount, &error) ==
               VECTIS_ERR_INVALID,
           "rejects protected embedded WebDAV mount without auth adapter");
    embedded_mount.auth_required = 0;
    expect(app->webdav_embedded(app, &embedded_mount, &error) == VECTIS_OK,
           "registers explicitly public embedded WebDAV mount without auth");
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

  expect(snprintf(root_dir, sizeof(root_dir), "%s/direct-root", temp) > 0,
         "formats direct root path");
  direct_config = config;
  direct_config.site_id = "direct";
  direct_config.root_dir = root_dir;
  cstatus = vectis_webdav_content_dir(&direct_config, content_dir, &error);
  expect(cstatus == VECTIS_OK && strcmp(content_dir, root_dir) == 0,
         "reports direct WebDAV root as content directory");
  status = vectis_webdav_mkcol(&direct_config, "/public");
  expect(status == VECTIS_WEBDAV_OK, "creates direct root collection");
  status = vectis_webdav_put(&direct_config, "/public/readme.txt",
                             (const unsigned char *)"direct", 6u);
  expect(status == VECTIS_WEBDAV_OK, "writes direct root file");
  expect(snprintf(direct_file, sizeof(direct_file), "%s/public/readme.txt",
                  root_dir) > 0,
         "formats direct root file path");
  expect(access(direct_file, F_OK) == 0, "direct root write reaches disk");
  body = NULL;
  body_size = 0u;
  status = vectis_webdav_read(&direct_config, "/public/readme.txt", &body,
                              &body_size, &entry);
  expect(status == VECTIS_WEBDAV_OK && body_size == 6u &&
             memcmp(body, "direct", 6u) == 0,
         "reads direct root file");
  free(body);
  status =
      vectis_webdav_copy(&direct_config, "/public/readme.txt", "/copy.txt", 0);
  expect(status == VECTIS_WEBDAV_OK, "copies direct root file");
  status = vectis_webdav_move(&direct_config, "/copy.txt", "/moved.txt", 1);
  expect(status == VECTIS_WEBDAV_OK, "moves direct root file");
  status = vectis_webdav_lookup(&direct_config, "/copy.txt", &entry);
  expect(status == VECTIS_WEBDAV_NOT_FOUND,
         "direct root move does not tombstone source");
  status = vectis_webdav_delete(&direct_config, "/moved.txt");
  expect(status == VECTIS_WEBDAV_OK, "deletes direct root file");
  status = vectis_webdav_lookup(&direct_config, "/moved.txt", &entry);
  expect(status == VECTIS_WEBDAV_NOT_FOUND,
         "direct root delete does not tombstone resource");
  expect(snprintf(outside_dir, sizeof(outside_dir), "%s/outside", temp) > 0,
         "formats outside directory path");
  expect(snprintf(outside_file, sizeof(outside_file), "%s/secret.txt",
                  outside_dir) > 0,
         "formats outside file path");
  expect(snprintf(outside_write, sizeof(outside_write), "%s/write.txt",
                  outside_dir) > 0,
         "formats outside write path");
  expect(snprintf(outside_copy, sizeof(outside_copy), "%s/copy.txt",
                  outside_dir) > 0,
         "formats outside copy path");
  expect(snprintf(outside_collection, sizeof(outside_collection), "%s/newdir",
                  outside_dir) > 0,
         "formats outside collection path");
  expect(snprintf(symlink_path, sizeof(symlink_path), "%s/link", root_dir) > 0,
         "formats direct root symlink path");
  expect(mkdir(outside_dir, 0700) == 0, "creates outside directory");
  expect(write_test_file(outside_file, "secret\n"), "creates outside file");
  expect(symlink(outside_dir, symlink_path) == 0,
         "creates direct root symlink");
  body = NULL;
  body_size = 0u;
  status = vectis_webdav_read(&direct_config, "/link/secret.txt", &body,
                              &body_size, &entry);
  expect(status != VECTIS_WEBDAV_OK,
         "rejects direct root reads through symlinked ancestors");
  free(body);
  status = vectis_webdav_put(&direct_config, "/link/write.txt",
                             (const unsigned char *)"write", 5u);
  expect(status != VECTIS_WEBDAV_OK && access(outside_write, F_OK) != 0,
         "rejects direct root writes through symlinked ancestors");
  status = vectis_webdav_mkcol(&direct_config, "/link/newdir");
  expect(status != VECTIS_WEBDAV_OK && access(outside_collection, F_OK) != 0,
         "rejects direct root collection creation through symlinked ancestors");
  status = vectis_webdav_copy(&direct_config, "/public/readme.txt",
                              "/link/copy.txt", 1);
  expect(status != VECTIS_WEBDAV_OK && access(outside_copy, F_OK) != 0,
         "rejects direct root copy destinations through symlinked ancestors");
  status = vectis_webdav_delete(&direct_config, "/link/secret.txt");
  expect(status != VECTIS_WEBDAV_OK && access(outside_file, F_OK) == 0,
         "rejects direct root deletes through symlinked ancestors");

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
