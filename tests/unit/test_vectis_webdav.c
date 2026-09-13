#include "vectis_internal.h"
#include <sys/wait.h>
#include <vectis/webdav.h>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
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

static void test_special_file(const vectis_webdav_config *storage) {
  char path[4096];
  pid_t child;
  int result;
  vectis_webdav_entry entry;
  (void)snprintf(path, sizeof(path), "%s/fifo", storage->root_dir);
  expect(mkfifo(path, 0600) == 0, "create FIFO without writer");
  child = fork();
  expect(child >= 0, "fork bounded FIFO lookup");
  if (child == 0) {
    alarm(2u);
    _exit(vectis_webdav_lookup(storage, "/fifo", &entry) == VECTIS_WEBDAV_OK
              ? 1
              : 0);
  }
  if (child > 0) {
    expect(waitpid(child, &result, 0) == child && WIFEXITED(result) &&
               WEXITSTATUS(result) == 0,
           "FIFO lookup rejects special file without blocking");
  }
  expect(unlink(path) == 0, "remove test FIFO");
}

static void test_destination_authority(const vectis_webdav_config *storage) {
  static const char *destinations[] = {"https://other-host/dav/target",
                                       "https://local:444/dav/target",
                                       "ftp://local/dav/target",
                                       "//other-host/dav/target",
                                       "https://user@local/dav/target",
                                       "https://local?x/dav/target",
                                       "https://LOCAL:443/dav/target",
                                       "https://local/dav/target",
                                       "/dav/target"};
  vectis_app_config config;
  vectis_webdav_mount_config mount;
  vectis_app *app;
  vectis_request *request;
  vectis_response *response;
  vectis_error error;
  vectis_webdav_entry entry;
  vectis_http_method method;
  unsigned char *body;
  size_t size;
  size_t i;
  size_t h;
  int move;
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app = vectis_app_new(&config, &error);
  expect(app != NULL, "create authority regression app");
  if (app == NULL)
    return;
  vectis_webdav_mount_config_init(&mount);
  mount.path_prefix = "/dav";
  mount.storage = *storage;
  mount.auth_required = 0;
  expect(app->webdav(app, &mount, &error) == VECTIS_OK,
         "mount authority regression storage");
  for (move = 0; move < 2; ++move) {
    method = move ? VECTIS_HTTP_MOVE : VECTIS_HTTP_COPY;
    for (i = 0u; i < sizeof(destinations) / sizeof(destinations[0]); ++i) {
      expect(vectis_webdav_put(storage, "/source", (const unsigned char *)"new",
                               3u) == VECTIS_WEBDAV_OK,
             "create transfer source");
      expect(vectis_webdav_put(storage, "/target", (const unsigned char *)"old",
                               3u) == VECTIS_WEBDAV_OK,
             "create transfer target");
      request = vectis_internal_request_new(&error);
      response = vectis_internal_response_new(&error);
      vectis_internal_request_set_method(request, method);
      expect(vectis_internal_request_set_path(request, "/dav/source", &error) ==
                 VECTIS_OK,
             "set authority source path");
      expect(vectis_internal_request_add_header(request, "host", "local",
                                                &error) == VECTIS_OK,
             "set request authority");
      expect(vectis_internal_request_add_header(
                 request, "destination", destinations[i], &error) == VECTIS_OK,
             "set transfer destination");
      expect(vectis_internal_dispatch_route(app, method, "/dav/source", request,
                                            response, &error) == VECTIS_OK,
             "dispatch authority transfer");
      expect(vectis_internal_response_status_code(response) ==
                 (i < 6u ? 400 : 204),
             "reject unsupported destination and accept local authority");
      body = NULL;
      size = 0u;
      expect(vectis_webdav_read(storage, "/target", &body, &size, &entry) ==
                     VECTIS_WEBDAV_OK &&
                 size == 3u && memcmp(body, i < 6u ? "old" : "new", 3u) == 0,
             "destination reflects only accepted operations");
      free(body);
      if (i < 6u)
        expect(vectis_webdav_lookup(storage, "/source", &entry) ==
                   VECTIS_WEBDAV_OK,
               "rejected MOVE preserves source");
      vectis_internal_request_free(request);
      vectis_internal_response_free(response);
    }
  }
  for (move = 0; move < 2; ++move) {
    method = move ? VECTIS_HTTP_HEAD : VECTIS_HTTP_GET;
    request = vectis_internal_request_new(&error);
    response = vectis_internal_response_new(&error);
    vectis_internal_request_set_method(request, method);
    expect(vectis_internal_request_set_path(request, "/dav/target", &error) ==
               VECTIS_OK,
           "set retrieval path");
    expect(vectis_internal_dispatch_route(app, method, "/dav/target", request,
                                          response, &error) == VECTIS_OK &&
               vectis_internal_response_status_code(response) == 200,
           "retrieve regular file");
    for (h = 0u; h < vectis_internal_response_header_count(response); ++h) {
      if (strcmp(vectis_internal_response_header_name(response, h), "etag") ==
          0) {
        expect(strlen(vectis_internal_response_header_value(response, h)) ==
                       66u &&
                   vectis_internal_response_header_value(response, h)[0] == '"',
               "GET and HEAD expose a quoted content validator");
        break;
      }
    }
    expect(h < vectis_internal_response_header_count(response), "ETag present");
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

static int dotfile_entry(const char *path, vectis_webdav_entry_kind kind,
                         size_t size, void *userdata) {
  unsigned *seen;
  const char *name;
  seen = (unsigned *)userdata;
  name = strrchr(path, '/') + 1;
  if (strcmp(name, ".hidden") == 0) {
    expect(kind == VECTIS_WEBDAV_ENTRY_COLLECTION, "lists hidden collection");
    *seen |= 1u;
  } else {
    expect(kind == VECTIS_WEBDAV_ENTRY_FILE && size == 1u,
           "lists dotfile with its size");
    if (strcmp(name, ".config") == 0)
      *seen |= 2u;
    else if (strcmp(name, "visible.txt") == 0)
      *seen |= 4u;
    else if (strcmp(name, ".vectis-user") == 0)
      *seen |= 8u;
    else
      expect(0, "listing excludes internal entries and deleted dotfiles");
  }
  return 1;
}

static void test_dotfile_listing(const vectis_webdav_config *storage) {
  vectis_webdav_config config;
  vectis_app_config app_config;
  vectis_webdav_mount_config mount;
  vectis_app *app;
  vectis_request *request;
  vectis_response *response;
  vectis_error error;
  vectis_bytes body;
  char content[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  const char *files[] = {".config", "visible.txt", ".vectis-user", ".deleted"};
  char path[128];
  size_t i;
  unsigned seen;

  config = *storage;
  config.max_total_bytes = 1024u * 1024u;
  expect(vectis_webdav_mkcol(&config, "/dotfiles") == VECTIS_WEBDAV_OK,
         "creates dotfile test collection");
  expect(vectis_webdav_mkcol(&config, "/dotfiles/.hidden") == VECTIS_WEBDAV_OK,
         "creates hidden collection");
  for (i = 0u; i < sizeof(files) / sizeof(files[0]); ++i) {
    (void)snprintf(path, sizeof(path), "/dotfiles/%s", files[i]);
    expect(vectis_webdav_put(&config, path, (const unsigned char *)"x", 1u) ==
               VECTIS_WEBDAV_OK,
           "uploads dotfile listing fixture");
  }
  expect(vectis_webdav_delete(&config, "/dotfiles/.deleted") ==
             VECTIS_WEBDAV_OK,
         "deletes dotfile fixture");
  expect(vectis_webdav_content_dir(&config, content, &error) == VECTIS_OK,
         "gets dotfile content directory");
  (void)snprintf(disk, sizeof(disk), "%s/dotfiles/.vectis-tmp-test", content);
  expect(write_test_file(disk, "internal"),
         "creates internal temporary fixture");
  (void)snprintf(disk, sizeof(disk), "%s/dotfiles/.vectis-txn-test", content);
  expect(mkdir(disk, 0700) == 0, "creates internal transaction fixture");
  seen = 0u;
  expect(vectis_webdav_list(&config, "/dotfiles", dotfile_entry, &seen) ==
                 VECTIS_WEBDAV_OK &&
             seen == 15u,
         "lists all user entries including dotfiles");

  vectis_app_config_init(&app_config);
  app = vectis_app_new(&app_config, &error);
  expect(app != NULL, "creates dotfile PROPFIND app");
  if (app != NULL) {
    vectis_webdav_mount_config_init(&mount);
    mount.path_prefix = "/dav";
    mount.storage = config;
    mount.auth_required = 0;
    expect(app->webdav(app, &mount, &error) == VECTIS_OK,
           "mounts dotfile storage");
    request = vectis_internal_request_new(&error);
    response = vectis_internal_response_new(&error);
    expect(request != NULL && response != NULL, "creates PROPFIND request");
    if (request != NULL && response != NULL) {
      vectis_internal_request_set_method(request, VECTIS_HTTP_PROPFIND);
      expect(vectis_internal_request_set_path(request, "/dav/dotfiles",
                                              &error) == VECTIS_OK,
             "sets PROPFIND path");
      expect(vectis_internal_request_add_header(request, "depth", "1",
                                                &error) == VECTIS_OK,
             "sets PROPFIND depth");
      expect(vectis_internal_dispatch_route(app, VECTIS_HTTP_PROPFIND,
                                            "/dav/dotfiles", request, response,
                                            &error) == VECTIS_OK,
             "dispatches dotfile PROPFIND");
      expect(vectis_internal_response_status_code(response) == 207,
             "PROPFIND returns multistatus");
      body = vectis_internal_response_body(response);
      expect(body.data != NULL, "PROPFIND has XML body");
      if (body.data != NULL) {
        char *xml;
        xml = (char *)malloc(body.size + 1u);
        expect(xml != NULL, "allocates terminated PROPFIND test string");
        if (xml != NULL) {
          memcpy(xml, body.data, body.size);
          xml[body.size] = '\0';
          expect(strstr(xml, "/dav/dotfiles/.config") != NULL &&
                     strstr(xml, "/dav/dotfiles/.hidden") != NULL &&
                     strstr(xml, "/dav/dotfiles/.vectis-user") != NULL,
                 "PROPFIND exposes user dotfiles and hidden collections");
          expect(strstr(xml, ".vectis-tmp-") == NULL &&
                     strstr(xml, ".vectis-txn-") == NULL &&
                     strstr(xml, ".deleted") == NULL,
                 "PROPFIND excludes internal and deleted entries");
          free(xml);
        }
      }
    }
    vectis_internal_request_free(request);
    vectis_internal_response_free(response);
    app->close(app);
  }
  expect(vectis_webdav_delete(&config, "/dotfiles") == VECTIS_WEBDAV_OK,
         "removes dotfile fixtures");
}

static void test_root_separators(const char *temp) {
  static const char *suffixes[] = {"", "/", "///"};
  char root[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  vectis_webdav_config config;
  vectis_webdav_entry entry;
  unsigned char *body;
  size_t size;
  size_t i;
  list_state listed;

  for (i = 0u; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
    (void)snprintf(root, sizeof(root), "%s//separator-root-%lu%s", temp,
                   (unsigned long)i, suffixes[i]);
    vectis_webdav_config_init(&config);
    config.cache_dir = temp;
    config.site_id = "separators";
    config.root_dir = root;
    expect(vectis_webdav_put(&config, "/file", (const unsigned char *)"data",
                             4u) == VECTIS_WEBDAV_OK,
           "create missing root with separators and write file");
    expect(vectis_webdav_lookup(&config, "/file", &entry) == VECTIS_WEBDAV_OK &&
               entry.kind == VECTIS_WEBDAV_ENTRY_FILE,
           "lookup file through root with separators");
    body = NULL;
    size = 0u;
    expect(vectis_webdav_read(&config, "/file", &body, &size, &entry) ==
                   VECTIS_WEBDAV_OK &&
               size == 4u && memcmp(body, "data", 4u) == 0,
           "read file through root with separators");
    free(body);
    memset(&listed, 0, sizeof(listed));
    expect(vectis_webdav_list(&config, "/", count_entry, &listed) ==
                   VECTIS_WEBDAV_OK &&
               listed.files == 1,
           "list root with separators");
    expect(vectis_webdav_mkcol(&config, "/dir") == VECTIS_WEBDAV_OK,
           "create collection through root with separators");
    expect(vectis_webdav_copy(&config, "/file", "/dir/copy", 0) ==
               VECTIS_WEBDAV_OK,
           "copy through root with separators");
    expect(vectis_webdav_move(&config, "/dir/copy", "/moved", 0) ==
               VECTIS_WEBDAV_OK,
           "move through root with separators");
    expect(vectis_webdav_delete(&config, "/moved") == VECTIS_WEBDAV_OK,
           "delete through root with separators");
  }
}

typedef struct conditional_writer {
  const vectis_webdav_config *config;
  const char *etag;
  int gate;
  vectis_webdav_status result;
} conditional_writer;

static void *conditional_write(void *arg) {
  conditional_writer *writer = (conditional_writer *)arg;
  char token;
  if (read(writer->gate, &token, 1u) != 1)
    return NULL;
  writer->result = vectis_webdav_put_conditional(
      writer->config, "/conditional.txt", (const unsigned char *)"winner", 6u,
      writer->etag, writer->etag == NULL ? "*" : NULL);
  return NULL;
}

static void *conditional_delete(void *arg) {
  conditional_writer *writer = (conditional_writer *)arg;
  char token;
  if (read(writer->gate, &token, 1u) == 1)
    writer->result = vectis_webdav_delete_conditional(
        writer->config, "/conditional.txt", writer->etag, NULL);
  return NULL;
}

static void *conditional_move(void *arg) {
  conditional_writer *writer = (conditional_writer *)arg;
  char token;
  if (read(writer->gate, &token, 1u) == 1)
    writer->result = vectis_webdav_move_conditional(
        writer->config, "/conditional.txt", "/conditional-moved.txt", 1,
        writer->etag, NULL);
  return NULL;
}

static void test_delete_and_shallow_copy(const vectis_webdav_config *config) {
  vectis_webdav_entry entry;
  unsigned char *body;
  size_t size;
  char etag[67];
  conditional_writer writers[8];
  pthread_t threads[8];
  int gate[2], i, winners = 0;
  expect(vectis_webdav_mkcol_conditional(config, "/conditional-col", "*",
                                         NULL) == VECTIS_WEBDAV_PRECONDITION,
         "MKCOL If-Match requires existing resource");
  expect(vectis_webdav_lookup(config, "/conditional-col", &entry) ==
             VECTIS_WEBDAV_NOT_FOUND,
         "failed MKCOL leaves collection absent");
  expect(vectis_webdav_mkcol_conditional(config, "/conditional-col", "invalid",
                                         NULL) == VECTIS_WEBDAV_INVALID,
         "MKCOL rejects malformed condition");
  expect(vectis_webdav_mkcol_conditional(config, "/conditional-col", NULL,
                                         "*") == VECTIS_WEBDAV_OK,
         "MKCOL If-None-Match creates missing collection");
  expect(vectis_webdav_mkcol_conditional(config, "/conditional-col", NULL,
                                         "*") == VECTIS_WEBDAV_PRECONDITION,
         "MKCOL If-None-Match rejects existing collection");
  expect(vectis_webdav_mkcol_conditional(config, "/conditional-col", "*",
                                         NULL) == VECTIS_WEBDAV_EXISTS,
         "matching MKCOL condition retains existing-resource behavior");
  expect(vectis_webdav_delete(config, "/conditional-col") == VECTIS_WEBDAV_OK,
         "cleanup conditional collection");
  expect(vectis_webdav_put(config, "/conditional.txt",
                           (const unsigned char *)"new",
                           3u) == VECTIS_WEBDAV_OK,
         "seed delete");
  expect(vectis_webdav_delete_conditional(config, "/conditional.txt",
                                          "\"stale\"",
                                          NULL) == VECTIS_WEBDAV_PRECONDITION,
         "stale delete denied");
  expect(vectis_webdav_delete_conditional(config, "/conditional.txt", NULL,
                                          "*") == VECTIS_WEBDAV_PRECONDITION,
         "exclusive delete denied");
  expect(vectis_webdav_read(config, "/conditional.txt", &body, &size, &entry) ==
             VECTIS_WEBDAV_OK,
         "failed delete preserves file");
  expect(size == 3u && memcmp(body, "new", 3u) == 0, "preserves new revision");
  free(body);
  snprintf(etag, sizeof(etag), "\"%s\"", entry.etag);
  expect(pipe(gate) == 0, "delete race gate");
  for (i = 0; i < 8; ++i) {
    writers[i].config = config;
    writers[i].etag = etag;
    writers[i].gate = gate[0];
    writers[i].result = VECTIS_WEBDAV_IO;
    expect(pthread_create(&threads[i], NULL, conditional_delete, &writers[i]) ==
               0,
           "start conditional deleter");
  }
  expect(write(gate[1], "12345678", 8u) == 8, "release deleters");
  for (i = 0; i < 8; ++i) {
    expect(pthread_join(threads[i], NULL) == 0, "join deleter");
    if (writers[i].result == VECTIS_WEBDAV_OK)
      ++winners;
    else
      expect(writers[i].result == VECTIS_WEBDAV_PRECONDITION, "delete loser");
  }
  close(gate[0]);
  close(gate[1]);
  expect(winners == 1, "one atomic conditional delete wins");
  expect(vectis_webdav_put(config, "/conditional.txt",
                           (const unsigned char *)"new",
                           3u) == VECTIS_WEBDAV_OK,
         "seed conditional transfer");
  expect(vectis_webdav_copy_conditional(
             config, "/conditional.txt", "/conditional-moved.txt", 1, -1,
             "\"stale\"", NULL) == VECTIS_WEBDAV_PRECONDITION,
         "reject stale copy");
  expect(vectis_webdav_lookup(config, "/conditional-moved.txt", &entry) ==
             VECTIS_WEBDAV_NOT_FOUND,
         "rejected copy creates nothing");
  expect(vectis_webdav_copy_conditional(config, "/conditional.txt",
                                        "/conditional-moved.txt", 1, -1, etag,
                                        NULL) == VECTIS_WEBDAV_OK,
         "accept matching copy");
  expect(pipe(gate) == 0, "move race gate");
  winners = 0;
  for (i = 0; i < 8; ++i) {
    writers[i].gate = gate[0];
    writers[i].result = VECTIS_WEBDAV_IO;
    expect(pthread_create(&threads[i], NULL, conditional_move, &writers[i]) ==
               0,
           "start mover");
  }
  expect(write(gate[1], "12345678", 8u) == 8, "release movers");
  for (i = 0; i < 8; ++i) {
    expect(pthread_join(threads[i], NULL) == 0, "join mover");
    if (writers[i].result == VECTIS_WEBDAV_OK)
      ++winners;
    else
      expect(writers[i].result == VECTIS_WEBDAV_PRECONDITION, "move loser");
  }
  close(gate[0]);
  close(gate[1]);
  expect(winners == 1, "one atomic conditional move wins");
  expect(vectis_webdav_read(config, "/conditional-moved.txt", &body, &size,
                            &entry) == VECTIS_WEBDAV_OK,
         "read moved data");
  expect(size == 3u && memcmp(body, "new", 3u) == 0, "moved data preserved");
  free(body);
  expect(vectis_webdav_delete(config, "/conditional-moved.txt") ==
             VECTIS_WEBDAV_OK,
         "clean moved fixture");
  expect(vectis_webdav_mkcol(config, "/depth-source") == VECTIS_WEBDAV_OK,
         "depth source");
  expect(vectis_webdav_put(config, "/depth-source/child",
                           (const unsigned char *)"x", 1u) == VECTIS_WEBDAV_OK,
         "depth child");
  expect(vectis_webdav_copy_depth(config, "/depth-source", "/depth-copy", 0,
                                  0) == VECTIS_WEBDAV_OK,
         "shallow copy");
  expect(vectis_webdav_lookup(config, "/depth-copy", &entry) ==
                 VECTIS_WEBDAV_OK &&
             entry.kind == VECTIS_WEBDAV_ENTRY_COLLECTION,
         "copies collection");
  expect(vectis_webdav_lookup(config, "/depth-copy/child", &entry) ==
             VECTIS_WEBDAV_NOT_FOUND,
         "does not copy descendants");
  expect(vectis_webdav_copy(config, "/depth-source", "/depth-copy", 1) ==
             VECTIS_WEBDAV_OK,
         "recursive overwrite");
  expect(vectis_webdav_lookup(config, "/depth-copy/child", &entry) ==
             VECTIS_WEBDAV_OK,
         "recursive child exists");
  expect(vectis_webdav_copy_depth(config, "/depth-source", "/depth-copy", 1,
                                  0) == VECTIS_WEBDAV_OK,
         "shallow overwrite");
  expect(vectis_webdav_lookup(config, "/depth-copy/child", &entry) ==
             VECTIS_WEBDAV_NOT_FOUND,
         "shallow overwrite removes old descendants");
  expect(vectis_webdav_delete(config, "/depth-copy") == VECTIS_WEBDAV_OK,
         "clean copy");
  expect(vectis_webdav_delete(config, "/depth-source") == VECTIS_WEBDAV_OK,
         "clean source");
}

static void test_conditional_put(const vectis_webdav_config *config) {
  unsigned char *body;
  size_t size;
  vectis_webdav_entry entry;
  char etag[67], weak[70], list[90];
  pthread_t threads[8];
  pid_t children[8];
  conditional_writer writers[8];
  int gate[2], winners, phase, i, child_status;
  expect(vectis_webdav_put_conditional(config, "/conditional.txt",
                                       (const unsigned char *)"old", 3u, "*",
                                       NULL) == VECTIS_WEBDAV_PRECONDITION,
         "If-Match wildcard rejects missing resource");
  for (phase = 0; phase < 2; ++phase) {
    if (phase == 1) {
      expect(vectis_webdav_put(config, "/conditional.txt",
                               (const unsigned char *)"old",
                               3u) == VECTIS_WEBDAV_OK,
             "seed CAS");
      expect(vectis_webdav_read(config, "/conditional.txt", &body, &size,
                                &entry) == VECTIS_WEBDAV_OK,
             "read CAS tag");
      free(body);
      snprintf(etag, sizeof(etag), "\"%s\"", entry.etag);
    }
    expect(pipe(gate) == 0, "create writer gate");
    for (i = 0; i < 8; ++i) {
      writers[i].config = config;
      writers[i].etag = phase ? etag : NULL;
      writers[i].gate = gate[0];
      writers[i].result = VECTIS_WEBDAV_IO;
      expect(pthread_create(&threads[i], NULL, conditional_write,
                            &writers[i]) == 0,
             "start conditional writer");
    }
    expect(write(gate[1], "12345678", 8u) == 8, "release competing writers");
    winners = 0;
    for (i = 0; i < 8; ++i) {
      expect(pthread_join(threads[i], NULL) == 0, "join writer");
      if (writers[i].result == VECTIS_WEBDAV_OK)
        ++winners;
      else
        expect(writers[i].result == VECTIS_WEBDAV_PRECONDITION,
               "loser rejects without mutation");
    }
    close(gate[0]);
    close(gate[1]);
    expect(winners == 1, "exactly one competing conditional write commits");
  }
  expect(vectis_webdav_read(config, "/conditional.txt", &body, &size, &entry) ==
             VECTIS_WEBDAV_OK,
         "read winner");
  expect(size == 6u && memcmp(body, "winner", 6u) == 0,
         "winning body preserved");
  free(body);
  snprintf(etag, sizeof(etag), "\"%s\"", entry.etag);
  snprintf(weak, sizeof(weak), "W/%s", etag);
  expect(vectis_webdav_put_conditional(config, "/conditional.txt",
                                       (const unsigned char *)"bad", 3u, weak,
                                       NULL) == VECTIS_WEBDAV_PRECONDITION,
         "weak If-Match rejected");
  expect(vectis_webdav_put_conditional(config, "/conditional.txt",
                                       (const unsigned char *)"bad", 3u, NULL,
                                       weak) == VECTIS_WEBDAV_PRECONDITION,
         "weak If-None-Match matches");
  expect(vectis_webdav_put_conditional(config, "/conditional.txt",
                                       (const unsigned char *)"bad", 3u,
                                       "\"x\",", NULL) == VECTIS_WEBDAV_INVALID,
         "malformed list rejected");
  snprintf(list, sizeof(list), "\"other,tag\", %s", etag);
  expect(vectis_webdav_put_conditional(config, "/conditional.txt",
                                       (const unsigned char *)"new", 3u, list,
                                       NULL) == VECTIS_WEBDAV_OK,
         "quoted comma and matching tag list accepted");
  expect(vectis_webdav_delete(config, "/conditional.txt") == VECTIS_WEBDAV_OK,
         "delete conditional fixture");
  expect(vectis_webdav_put_conditional(config, "/conditional.txt",
                                       (const unsigned char *)"new", 3u, NULL,
                                       "*") == VECTIS_WEBDAV_OK,
         "exclusive create replaces tombstone");
  expect(vectis_webdav_delete(config, "/conditional.txt") == VECTIS_WEBDAV_OK,
         "clean conditional fixture");
  expect(pipe(gate) == 0, "create process writer gate");
  for (i = 0; i < 8; ++i) {
    children[i] = fork();
    expect(children[i] >= 0, "fork conditional writer");
    if (children[i] == 0) {
      conditional_writer writer;
      writer.config = config;
      writer.etag = NULL;
      writer.gate = gate[0];
      writer.result = VECTIS_WEBDAV_IO;
      conditional_write(&writer);
      _exit(writer.result == VECTIS_WEBDAV_OK             ? 0
            : writer.result == VECTIS_WEBDAV_PRECONDITION ? 1
                                                          : 2);
    }
  }
  expect(write(gate[1], "12345678", 8u) == 8, "release process writers");
  winners = 0;
  for (i = 0; i < 8; ++i) {
    expect(waitpid(children[i], &child_status, 0) == children[i],
           "join process writer");
    expect(WIFEXITED(child_status) && WEXITSTATUS(child_status) <= 1,
           "process write result");
    if (WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0)
      ++winners;
  }
  close(gate[0]);
  close(gate[1]);
  expect(winners == 1, "exactly one competing process commits");
  expect(vectis_webdav_delete(config, "/conditional.txt") == VECTIS_WEBDAV_OK,
         "clean process fixture");
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

  test_root_separators(temp);
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
  test_special_file(&direct_config);
  test_destination_authority(&direct_config);
  test_dotfile_listing(&config);
  test_dotfile_listing(&direct_config);
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
  test_conditional_put(&config);
  test_conditional_put(&direct_config);
  test_delete_and_shallow_copy(&config);
  test_delete_and_shallow_copy(&direct_config);
  limited_config.site_id = "limited";
  limited_config.max_total_bytes = 5u;
  limited_config.max_file_bytes = 4u;
  status = vectis_webdav_put(&limited_config, "/fits.txt",
                             (const unsigned char *)"abcd", 4u);
  expect(status == VECTIS_WEBDAV_OK, "allows file within quota");
  status = vectis_webdav_put(&limited_config, "/too-large.txt",
                             (const unsigned char *)"ab", 2u);
  expect(status == VECTIS_WEBDAV_LIMIT, "enforces aggregate quota");

  limited_config = config;
  limited_config.site_id = "delete-resource-quota";
  limited_config.max_resources = 1u;
  expect(vectis_webdav_put(&limited_config, "/a", (const unsigned char *)"a",
                           1u) == VECTIS_WEBDAV_OK,
         "fills resource quota");
  expect(vectis_webdav_delete(&limited_config, "/a") == VECTIS_WEBDAV_OK,
         "delete replaces content with tombstone at quota");
  expect(vectis_webdav_delete(&limited_config, "/a") == VECTIS_WEBDAV_OK,
         "repeated delete replaces tombstone at quota");
  expect(vectis_webdav_delete(&limited_config, "/b") == VECTIS_WEBDAV_LIMIT,
         "new tombstone still obeys quota");
  limited_config.site_id = "delete-subtree-quota";
  limited_config.max_resources = 3u;
  expect(vectis_webdav_put(&limited_config, "/dir/a",
                           (const unsigned char *)"a", 1u) == VECTIS_WEBDAV_OK,
         "creates subtree at quota");
  expect(vectis_webdav_delete(&limited_config, "/dir/a") == VECTIS_WEBDAV_OK,
         "nested delete counts new tombstone parent");
  expect(vectis_webdav_delete(&limited_config, "/dir") == VECTIS_WEBDAV_OK,
         "collection delete subtracts content and descendant tombstones");
  limited_config.max_resources = 1u;
  expect(vectis_webdav_delete(&limited_config, "/dir") == VECTIS_WEBDAV_OK,
         "collection tombstone leaves one resource");

  remove_tree(temp);
  return failures == 0 ? 0 : 1;
}
