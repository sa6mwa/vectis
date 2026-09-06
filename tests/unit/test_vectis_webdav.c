#include "vectis_internal.h"
#include <sys/wait.h>
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

static vectis_status subtree_auth(const vectis_webdav_auth_request *request,
                                  vectis_webdav_auth_response *response,
                                  void *userdata, vectis_error *error) {
  int *calls;
  calls = (int *)userdata;
  ++*calls;
  vectis_error_clear(error);
  vectis_webdav_auth_response_init(response);
  response->action = strncmp(request->resource_path, "/allowed/", 9u) == 0
                         ? VECTIS_WEBDAV_AUTH_ALLOW
                         : VECTIS_WEBDAV_AUTH_DENY;
  return VECTIS_OK;
}

static void test_destination_auth(const vectis_webdav_config *storage) {
  vectis_app_config config;
  vectis_webdav_mount_config mount;
  vectis_app *app;
  vectis_request *request;
  vectis_response *response;
  vectis_error error;
  vectis_webdav_entry entry;
  unsigned char *body;
  size_t size;
  int calls;
  int move;
  vectis_http_method method;

  expect(vectis_webdav_put(storage, "/allowed/source",
                           (const unsigned char *)"new",
                           3u) == VECTIS_WEBDAV_OK,
         "create authorized source");
  expect(vectis_webdav_put(storage, "/private/target",
                           (const unsigned char *)"old",
                           3u) == VECTIS_WEBDAV_OK,
         "create protected destination");
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app = vectis_app_new(&config, &error);
  expect(app != NULL, "create authorization regression app");
  if (app == NULL)
    return;
  vectis_webdav_mount_config_init(&mount);
  mount.path_prefix = "/dav";
  mount.storage = *storage;
  mount.auth = subtree_auth;
  mount.auth_userdata = &calls;
  expect(app->webdav(app, &mount, &error) == VECTIS_OK,
         "mount restricted subtree");
  for (move = 0; move < 2; ++move) {
    calls = 0;
    method = move ? VECTIS_HTTP_MOVE : VECTIS_HTTP_COPY;
    request = vectis_internal_request_new(&error);
    response = vectis_internal_response_new(&error);
    vectis_internal_request_set_method(request, method);
    expect(vectis_internal_request_set_path(request, "/dav/allowed/source",
                                            &error) == VECTIS_OK,
           "set source path");
    expect(vectis_internal_request_add_header(request, "destination",
                                              "/dav/private//target",
                                              &error) == VECTIS_OK,
           "set destination");
    expect(vectis_internal_dispatch_route(app, method, "/dav/allowed/source",
                                          request, response,
                                          &error) == VECTIS_OK,
           "dispatch restricted transfer");
    expect(calls == 2, "authorize source and normalized destination");
    expect(vectis_internal_response_status_code(response) == 404,
           "conceal denied destination");
    expect(vectis_webdav_lookup(storage, "/allowed/source", &entry) ==
               VECTIS_WEBDAV_OK,
           "denial preserves source");
    body = NULL;
    size = 0u;
    expect(vectis_webdav_read(storage, "/private/target", &body, &size,
                              &entry) == VECTIS_WEBDAV_OK &&
               size == 3u && memcmp(body, "old", 3u) == 0,
           "denial preserves destination contents");
    free(body);
    vectis_internal_request_free(request);
    vectis_internal_response_free(response);
  }
  app->close(app);
}

static void test_failed_move(const vectis_webdav_config *storage) {
  char parent[4096];
  unsigned char *body;
  size_t size;
  vectis_webdav_entry entry;
  int overwrite;
  pid_t child;
  int result;

  /* Drop root privileges in a child so permission failures are reproducible. */
  child = fork();
  expect(child >= 0, "fork permission regression");
  if (child == 0) {
    if (geteuid() == 0 && setuid(65534) != 0)
      _exit(2);
    for (overwrite = 0; overwrite < 2; ++overwrite) {
      expect(vectis_webdav_put(storage, "/locked/source/file",
                               (const unsigned char *)"precious",
                               8u) == VECTIS_WEBDAV_OK,
             "create move source");
      if (overwrite)
        expect(vectis_webdav_put(storage, "/destination/old",
                                 (const unsigned char *)"old",
                                 3u) == VECTIS_WEBDAV_OK,
               "create overwrite destination");
      (void)snprintf(parent, sizeof(parent), "%s/locked", storage->root_dir);
      expect(chmod(parent, 0500) == 0, "lock source parent");
      expect(vectis_webdav_move(storage, "/locked/source", "/destination",
                                overwrite) == VECTIS_WEBDAV_IO,
             "report incomplete source removal");
      body = NULL;
      size = 0u;
      expect(vectis_webdav_read(storage, "/destination/file", &body, &size,
                                &entry) == VECTIS_WEBDAV_OK &&
                 size == 8u && memcmp(body, "precious", 8u) == 0,
             "failed move preserves complete destination");
      free(body);
      expect(chmod(parent, 0700) == 0, "restore source parent permissions");
      expect(vectis_webdav_delete(storage, "/destination") == VECTIS_WEBDAV_OK,
             "clean regression destination");
    }
    _exit(failures ? 1 : 0);
  }
  if (child > 0) {
    expect(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
               WEXITSTATUS(result) == 0,
           "permission failure regressions pass");
  }
}

int main(void) {
  char temp[4096];
  char cwd[2048];
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  char content_dir[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char root_dir[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char direct_file[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char large_file[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char outside_dir[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char outside_file[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char outside_write[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char outside_copy[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char outside_collection[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char symlink_path[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char intermediate_root[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char intermediate_root_link[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char intermediate_outside_file[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char txn_dir[VECTIS_WEBDAV_STORAGE_PATH_MAX];
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
  int i;

  if (getcwd(cwd, sizeof(cwd)) == NULL)
    return 1;
  (void)snprintf(temp, sizeof(temp), "%s/vectis-webdav-unit.XXXXXX", cwd);
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
  expect(mkdir(root_dir, 0777) == 0, "create regression root");
  expect(chmod(temp, 0755) == 0 && chmod(root_dir, 0777) == 0,
         "allow permission regression child access");
  test_failed_move(&direct_config);
  test_destination_auth(&direct_config);
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
  expect(snprintf(large_file, sizeof(large_file), "%s/large.txt", root_dir) > 0,
         "formats external oversized direct root file");
  expect(write_test_file(large_file, "0123456789abcdef!"),
         "writes external oversized direct root file");
  body = NULL;
  body_size = 0u;
  status = vectis_webdav_read(&direct_config, "/large.txt", &body, &body_size,
                              &entry);
  expect(status != VECTIS_WEBDAV_OK,
         "rejects external oversized direct root reads");
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
  status = vectis_webdav_mkcol(&direct_config, "/preserve");
  expect(status == VECTIS_WEBDAV_OK, "creates overwrite rollback destination");
  status = vectis_webdav_put(&direct_config, "/preserve/old.txt",
                             (const unsigned char *)"old", 3u);
  expect(status == VECTIS_WEBDAV_OK, "writes overwrite rollback destination");
  for (i = 0; i < 1000; ++i) {
    expect(snprintf(txn_dir, sizeof(txn_dir), "%s/preserve/.vectis-txn-%lu-%d",
                    root_dir, (unsigned long)getpid(), i) > 0,
           "formats direct root transaction collision");
    expect(mkdir(txn_dir, 0700) == 0,
           "creates direct root transaction collision");
  }
  status = vectis_webdav_copy(&direct_config, "/public/readme.txt",
                              "/preserve/old.txt", 1);
  expect(status != VECTIS_WEBDAV_OK,
         "rejects direct root overwrite when staged copy fails");
  body = NULL;
  body_size = 0u;
  status = vectis_webdav_read(&direct_config, "/preserve/old.txt", &body,
                              &body_size, &entry);
  expect(status == VECTIS_WEBDAV_OK && body_size == 3u &&
             memcmp(body, "old", 3u) == 0,
         "preserves direct root overwrite destination on copy failure");
  free(body);
  expect(symlink(outside_dir, symlink_path) == 0,
         "creates direct root symlink");
  status = vectis_webdav_put(&direct_config, "/public/after-link.txt",
                             (const unsigned char *)"ok", 2u);
  expect(status == VECTIS_WEBDAV_OK,
         "allows unrelated direct root mutation with symlink present");
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
  expect(snprintf(intermediate_root_link, sizeof(intermediate_root_link),
                  "%s/root-link", temp) > 0,
         "formats intermediate direct root symlink");
  expect(snprintf(intermediate_root, sizeof(intermediate_root), "%s/site",
                  intermediate_root_link) > 0,
         "formats direct root below intermediate symlink");
  expect(snprintf(intermediate_outside_file, sizeof(intermediate_outside_file),
                  "%s/site/escape.txt", outside_dir) > 0,
         "formats intermediate-symlink outside target");
  expect(symlink(outside_dir, intermediate_root_link) == 0,
         "creates intermediate direct root symlink");
  direct_config.root_dir = intermediate_root;
  status = vectis_webdav_put(&direct_config, "/escape.txt",
                             (const unsigned char *)"escape", 6u);
  expect(status != VECTIS_WEBDAV_OK,
         "rejects direct root with an intermediate symlink");
  expect(access(intermediate_outside_file, F_OK) != 0,
         "intermediate direct root symlink does not write outside root");
  direct_config.root_dir = root_dir;

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
