#include <vectis/embedded_fs.h>

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned failures;

static void expect(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

static void remove_tree(const char *path) {
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  char child[1024];
  int written;

  if (path == NULL || path[0] == '\0') {
    return;
  }
  dir = opendir(path);
  if (dir == NULL) {
    (void)remove(path);
    return;
  }
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof(child)) {
      continue;
    }
    if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
      remove_tree(child);
    } else {
      (void)remove(child);
    }
  }
  (void)closedir(dir);
  (void)rmdir(path);
}

typedef struct list_state {
  unsigned count;
  int saw_index;
  int saw_app;
} list_state;

typedef struct chunk_state {
  char bytes[16];
  size_t size;
  unsigned count;
} chunk_state;

static vectis_status list_entry(const vectis_embedded_fs_entry *entry,
                                void *userdata, vectis_error *error) {
  list_state *state;

  (void)error;
  state = (list_state *)userdata;
  state->count++;
  if (strcmp(entry->path, "/index.html") == 0) {
    state->saw_index = 1;
  }
  if (strcmp(entry->path, "/assets/app.txt") == 0) {
    state->saw_app = 1;
  }
  return VECTIS_OK;
}

static vectis_status chunk_entry(const void *data, size_t size, void *userdata,
                                 vectis_error *error) {
  chunk_state *state;

  (void)error;
  state = (chunk_state *)userdata;
  if (state->size + size >= sizeof(state->bytes)) {
    return VECTIS_ERR_INVALID;
  }
  memcpy(state->bytes + state->size, data, size);
  state->size += size;
  state->count++;
  return VECTIS_OK;
}

static vectis_embedded_fs *new_fixture_fs(vectis_error *error) {
  static const unsigned char payload[] = "hello\napp\n";
  static const char manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/index.html\",\"offset\":0,\"size\":6,"
      "\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\","
      "\"content_type\":\"text/html\"},"
      "{\"path\":\"/assets/app.txt\",\"offset\":6,\"size\":4,"
      "\"sha256\":"
      "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\","
      "\"content_type\":\"text/plain\"}]}";
  vectis_embedded_fs_config config;
  vectis_embedded_fs *fs;

  vectis_embedded_fs_config_init(&config);
  config.manifest_json = manifest;
  config.manifest_json_size = sizeof(manifest) - 1u;
  config.payload = payload;
  config.payload_size = sizeof(payload) - 1u;
  fs = NULL;
  expect(vectis_embedded_fs_from_pack(&config, &fs, error) == VECTIS_OK &&
             fs != NULL,
         "creates embedded fs from manifest and payload");
  return fs;
}

static void read_file(const char *path, char *buffer, size_t buffer_size) {
  FILE *fp;
  size_t nread;

  fp = fopen(path, "rb");
  assert(fp != NULL);
  nread = fread(buffer, 1u, buffer_size - 1u, fp);
  buffer[nread] = '\0';
  (void)fclose(fp);
}

int main(void) {
  vectis_error error;
  vectis_embedded_fs *fs;
  vectis_embedded_fs_entry entry;
  vectis_embedded_fs_extract_config extract;
  vectis_bytes body;
  list_state listed;
  chunk_state chunked;
  char temp[] = "/tmp/vectis-embedded-fs.XXXXXX";
  char extracted[512];
  char buffer[32];
  int found;
  vectis_status status;
  static const unsigned char bad_payload[] = "hullo\nbad\n";
  static const char duplicate_manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/dup.txt\",\"offset\":0,\"size\":6,"
      "\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\"},"
      "{\"path\":\"/dup.txt\",\"offset\":6,\"size\":4,"
      "\"sha256\":"
      "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\"}]}";
  static const char bounds_manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/bad.txt\",\"offset\":50,\"size\":4,"
      "\"sha256\":"
      "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\"}]}";
  vectis_embedded_fs_config config;

  vectis_error_clear(&error);
  fs = new_fixture_fs(&error);
  if (fs == NULL) {
    return 1;
  }

  found = 0;
  memset(&entry, 0, sizeof(entry));
  status = fs->lookup(fs, "/", &found, &entry, &error);
  expect(status == VECTIS_OK && found && strcmp(entry.path, "/index.html") == 0,
         "lookup maps root to index");
  expect(entry.content_type != NULL &&
             strcmp(entry.content_type, "text/html") == 0,
         "lookup exposes content type");

  found = 0;
  body.data = NULL;
  body.size = 0u;
  status =
      vectis_embedded_fs_read(fs, "/assets/app.txt", &found, &body, &error);
  expect(status == VECTIS_OK && found && body.size == 4u &&
             memcmp(body.data, "app\n", 4u) == 0,
         "reads borrowed embedded bytes");

  memset(&listed, 0, sizeof(listed));
  status = fs->list(fs, "/", list_entry, &listed, &error);
  expect(status == VECTIS_OK && listed.count == 2u && listed.saw_index &&
             listed.saw_app,
         "lists embedded entries under prefix");

  found = 0;
  memset(&chunked, 0, sizeof(chunked));
  status = vectis_embedded_fs_stream(fs, "/assets/app.txt", 2u, &found,
                                     chunk_entry, &chunked, &error);
  expect(status == VECTIS_OK && found && chunked.count == 2u &&
             chunked.size == 4u && memcmp(chunked.bytes, "app\n", 4u) == 0,
         "streams embedded entry in bounded chunks");

  found = 1;
  status =
      vectis_embedded_fs_lookup(fs, "/missing.txt", &found, &entry, &error);
  expect(status == VECTIS_OK && !found, "missing lookup is not an error");

  expect(mkdtemp(temp) != NULL, "creates extraction temp directory");
  vectis_embedded_fs_extract_config_init(&extract);
  extract.output_dir = temp;
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_OK, "extracts embedded fs to disk");
  (void)snprintf(extracted, sizeof(extracted), "%s/index.html", temp);
  read_file(extracted, buffer, sizeof(buffer));
  expect(strcmp(buffer, "hello\n") == 0, "extracted index content matches");

  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_ERR_CONFLICT,
         "default extract policy rejects existing files");
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_SKIP_EXISTING;
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_OK, "skip-existing extract policy succeeds");
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_OVERWRITE;
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_OK, "overwrite extract policy succeeds");
  remove_tree(temp);

  vectis_embedded_fs_close(fs);

  vectis_embedded_fs_config_init(&config);
  config.payload = "hello\napp\n";
  config.payload_size = 10u;
  config.manifest_json = duplicate_manifest;
  config.manifest_json_size = sizeof(duplicate_manifest) - 1u;
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_ERR_CONFLICT && fs == NULL,
         "rejects duplicate embedded paths");

  vectis_embedded_fs_config_init(&config);
  config.payload = "hello\napp\n";
  config.payload_size = 10u;
  config.manifest_json = bounds_manifest;
  config.manifest_json_size = sizeof(bounds_manifest) - 1u;
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_ERR_INVALID && fs == NULL,
         "rejects out-of-bounds embedded assets");

  vectis_embedded_fs_config_init(&config);
  config.payload = bad_payload;
  config.payload_size = sizeof(bad_payload) - 1u;
  config.manifest_json =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/index.html\",\"offset\":0,\"size\":6,"
      "\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\"}]}";
  config.manifest_json_size = strlen((const char *)config.manifest_json);
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_ERR_INVALID && fs == NULL,
         "rejects hash-mismatched embedded assets");

  return failures == 0u ? 0 : 1;
}
