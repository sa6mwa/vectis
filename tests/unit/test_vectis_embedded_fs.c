#include <vectis/embedded_fs.h>

#include <assert.h>
#include <dirent.h>
#include <lc/lc.h>
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
      "{\"format\":\"vectis-pack\","
      "\"tree_sha256\":"
      "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
      "\"assets\":["
      "{\"path\":\"/index.html\",\"offset\":0,\"size\":6,"
      "\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\","
      "\"etag\":"
      "\"\\\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\\\"\","
      "\"content_type\":\"text/html\"},"
      "{\"path\":\"/assets/app.txt\",\"offset\":6,\"size\":4,"
      "\"sha256\":"
      "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\","
      "\"etag\":"
      "\"\\\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\\\"\","
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

static void write_file(const char *path, const char *body) {
  FILE *fp;

  fp = fopen(path, "wb");
  assert(fp != NULL);
  assert(fwrite(body, 1u, strlen(body), fp) == strlen(body));
  assert(fclose(fp) == 0);
}

int main(void) {
  vectis_error error;
  vectis_embedded_fs *fs;
  vectis_embedded_fs_entry entry;
  vectis_embedded_fs_extract_config extract;
  vectis_bytes body;
  lc_source *source;
  lc_error lcerr;
  list_state listed;
  chunk_state chunked;
  char temp[] = "/tmp/vectis-embedded-fs.XXXXXX";
  char extracted[512];
  char extracted_app[512];
  char user_file[512];
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
  static const char repair_manifest[] =
      "{\"format\":\"vectis-pack\",\"extract_mode\":\"repair\",\"assets\":["
      "{\"path\":\"/index.html\",\"offset\":0,\"size\":6,"
      "\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\"}]}";
  static const char invalid_extract_mode_manifest[] =
      "{\"format\":\"vectis-pack\",\"extract_mode\":\"invalid\",\"assets\":["
      "{\"path\":\"/index.html\",\"offset\":0,\"size\":6,"
      "\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\"}]}";
  static const char invalid_tree_hash_manifest[] =
      "{\"format\":\"vectis-pack\",\"tree_sha256\":\"bad\",\"assets\":["
      "{\"path\":\"/index.html\",\"offset\":0,\"size\":6,"
      "\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\"}]}";
  static const char invalid_etag_manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/index.html\",\"offset\":0,\"size\":6,"
      "\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\","
      "\"etag\":"
      "\"\\\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\\\"\"}]}";
  vectis_embedded_fs_config config;

  vectis_error_clear(&error);
  fs = new_fixture_fs(&error);
  if (fs == NULL) {
    return 1;
  }
  expect(vectis_embedded_fs_default_extract_policy(fs) ==
             VECTIS_EMBEDDED_FS_EXTRACT_FAIL_EXISTS,
         "manifest without extract_mode defaults to fail_exists");
  expect(fs->tree_sha256 != NULL &&
             strcmp(fs->tree_sha256(fs),
                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") ==
                 0,
         "receiver exposes manifest asset tree hash");
  expect(strcmp(vectis_embedded_fs_tree_sha256(fs),
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") ==
             0,
         "wrapper exposes manifest asset tree hash");
  expect(vectis_embedded_fs_tree_sha256(NULL) == NULL,
         "null embedded fs has no tree hash");
  expect(strcmp(vectis_embedded_fs_extract_policy_string(
                    VECTIS_EMBEDDED_FS_EXTRACT_SKIP_EXISTING),
                "skip_existing") == 0,
         "extract policy has canonical string");
  expect(vectis_embedded_fs_extract_policy_parse("skip-existing",
                                                 &extract.policy) &&
             extract.policy == VECTIS_EMBEDDED_FS_EXTRACT_SKIP_EXISTING,
         "extract policy parser accepts CLI hyphen alias");

  found = 0;
  memset(&entry, 0, sizeof(entry));
  status = fs->lookup(fs, "/", &found, &entry, &error);
  expect(status == VECTIS_OK && found && strcmp(entry.path, "/index.html") == 0,
         "lookup maps root to index");
  expect(entry.content_type != NULL &&
             strcmp(entry.content_type, "text/html") == 0,
         "lookup exposes content type");
  expect(entry.etag != NULL &&
             strcmp(entry.etag,
                    "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\"") ==
                 0,
         "lookup exposes strong ETag metadata");

  found = 0;
  body.data = NULL;
  body.size = 0u;
  status =
      vectis_embedded_fs_read(fs, "/assets/app.txt", &found, &body, &error);
  expect(status == VECTIS_OK && found && body.size == 4u &&
             memcmp(body.data, "app\n", 4u) == 0,
         "reads borrowed embedded bytes");

  found = 0;
  source = NULL;
  status = vectis_embedded_fs_open_source(fs, "/assets/app.txt", &found,
                                          &source, &error);
  expect(status == VECTIS_OK && found && source != NULL,
         "opens embedded entry as source");
  if (source != NULL) {
    lc_error_init(&lcerr);
    memset(buffer, 0, sizeof(buffer));
    expect(source->read(source, buffer, 2u, &lcerr) == 2u &&
               memcmp(buffer, "ap", 2u) == 0,
           "embedded source reads first bounded chunk");
    expect(source->read(source, buffer, sizeof(buffer), &lcerr) == 2u &&
               memcmp(buffer, "p\n", 2u) == 0,
           "embedded source reads remaining bytes");
    expect(source->read(source, buffer, sizeof(buffer), &lcerr) == 0u &&
               lcerr.code == LC_OK,
           "embedded source reports EOF");
    lc_error_cleanup(&lcerr);
    lc_source_close(source);
  }
  found = 1;
  source = NULL;
  status = vectis_embedded_fs_open_source(fs, "/missing.txt", &found, &source,
                                          &error);
  expect(status == VECTIS_OK && !found && source == NULL,
         "missing embedded source is not an error");

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
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_VERIFY;
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_OK, "verify extract policy accepts matching files");
  (void)snprintf(extracted_app, sizeof(extracted_app), "%s/assets/app.txt",
                 temp);
  write_file(extracted_app, "mutated\n");
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_ERR_CONFLICT,
         "verify extract policy rejects mismatched files");
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_REPAIR;
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_OK,
         "repair extract policy restores mismatched files");
  read_file(extracted_app, buffer, sizeof(buffer));
  expect(strcmp(buffer, "app\n") == 0, "repair restored app content");
  (void)snprintf(user_file, sizeof(user_file), "%s/user-created.txt", temp);
  write_file(user_file, "user\n");
  (void)remove(extracted_app);
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_VERIFY;
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_ERR_CONFLICT,
         "verify extract policy rejects missing files");
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_REPAIR;
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_OK, "repair extract policy restores missing files");
  read_file(extracted_app, buffer, sizeof(buffer));
  expect(strcmp(buffer, "app\n") == 0, "repair restored missing app content");
  read_file(user_file, buffer, sizeof(buffer));
  expect(strcmp(buffer, "user\n") == 0, "repair keeps user-created files");
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
  config.payload = "hello\n";
  config.payload_size = 6u;
  config.manifest_json = repair_manifest;
  config.manifest_json_size = sizeof(repair_manifest) - 1u;
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_OK && fs != NULL &&
             vectis_embedded_fs_default_extract_policy(fs) ==
                 VECTIS_EMBEDDED_FS_EXTRACT_REPAIR,
         "reads manifest default extract policy");
  vectis_embedded_fs_close(fs);

  vectis_embedded_fs_config_init(&config);
  config.payload = "hello\n";
  config.payload_size = 6u;
  config.manifest_json = invalid_extract_mode_manifest;
  config.manifest_json_size = sizeof(invalid_extract_mode_manifest) - 1u;
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_ERR_INVALID && fs == NULL,
         "rejects invalid manifest default extract policy");

  vectis_embedded_fs_config_init(&config);
  config.payload = "hello\n";
  config.payload_size = 6u;
  config.manifest_json = invalid_tree_hash_manifest;
  config.manifest_json_size = sizeof(invalid_tree_hash_manifest) - 1u;
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_ERR_INVALID && fs == NULL,
         "rejects invalid manifest asset tree hash");

  vectis_embedded_fs_config_init(&config);
  config.payload = "hello\n";
  config.payload_size = 6u;
  config.manifest_json = invalid_etag_manifest;
  config.manifest_json_size = sizeof(invalid_etag_manifest) - 1u;
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_ERR_INVALID && fs == NULL,
         "rejects invalid manifest asset etag");

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
