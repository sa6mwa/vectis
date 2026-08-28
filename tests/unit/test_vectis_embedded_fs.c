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
    if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
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
  int saw_assets_dir;
  int saw_skip;
  int app_metadata;
  int dir_metadata;
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
    state->app_metadata =
        entry->kind == VECTIS_EMBEDDED_FS_ENTRY_FILE && entry->mode == 0444u &&
        entry->etag != NULL &&
        strcmp(entry->etag, "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f9"
                            "95eae6d078875901\"") == 0 &&
        entry->data != NULL && entry->size == 4u &&
        memcmp(entry->data, "app\n", 4u) == 0;
  }
  if (strcmp(entry->path, "/assets") == 0) {
    state->saw_assets_dir = 1;
    state->dir_metadata = entry->kind == VECTIS_EMBEDDED_FS_ENTRY_DIRECTORY &&
                          entry->mode == 0555u && entry->etag == NULL &&
                          entry->sha256 == NULL && entry->data == NULL &&
                          entry->size == 0u;
  }
  if (strcmp(entry->path, "/assets2/skip.txt") == 0) {
    state->saw_skip = 1;
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
      "\"kind\":\"file\",\"mode\":292,"
      "\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\","
      "\"etag\":"
      "\"\\\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\\"
      "\"\","
      "\"content_type\":\"text/html\"},"
      "{\"path\":\"/assets/app.txt\",\"offset\":6,\"size\":4,"
      "\"kind\":\"file\",\"mode\":292,"
      "\"sha256\":"
      "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\","
      "\"etag\":"
      "\"\\\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\\"
      "\"\","
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

static vectis_embedded_fs *new_adjacent_prefix_fs(vectis_error *error) {
  static const unsigned char payload[] = "a\nb\n";
  static const char manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/assets/app.txt\",\"kind\":\"file\",\"mode\":292,"
      "\"offset\":0,\"size\":2,"
      "\"sha256\":"
      "\"87428fc522803d31065e7bce3cf03fe475096631e5e07bbd7a0fde60c4cf25c7\"},"
      "{\"path\":\"/assets2/skip.txt\",\"kind\":\"file\",\"mode\":292,"
      "\"offset\":2,\"size\":2,"
      "\"sha256\":"
      "\"0263829989b6fd954f72baaf2fc64bc2e2f01d692d4de72986ea808f6e99813f\"}]}";
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
         "creates adjacent-prefix embedded fs fixture");
  return fs;
}

static vectis_embedded_fs *new_directory_fs(vectis_error *error) {
  static const unsigned char payload[] = "app\n";
  static const char manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/assets\",\"kind\":\"directory\",\"mode\":365},"
      "{\"path\":\"/assets/app.txt\",\"kind\":\"file\",\"mode\":292,"
      "\"offset\":0,\"size\":4,"
      "\"sha256\":"
      "\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\"}]}";
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
         "creates directory-entry embedded fs fixture");
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
  vectis_embedded_fs *prefix_fs;
  vectis_embedded_fs *directory_fs;
  vectis_embedded_fs_entry entry;
  vectis_embedded_fs_extract_config extract;
  vectis_bytes body;
  lc_source *source;
  lc_error lcerr;
  list_state listed;
  chunk_state chunked;
  struct stat st;
  char temp[] = "/tmp/vectis-embedded-fs.XXXXXX";
  char symlink_temp[] = "/tmp/vectis-embedded-fs-link.XXXXXX";
  char outside_temp[] = "/tmp/vectis-embedded-fs-outside.XXXXXX";
  char root_symlink_temp[] = "/tmp/vectis-embedded-fs-root-link.XXXXXX";
  char root_outside_temp[] = "/tmp/vectis-embedded-fs-root-outside.XXXXXX";
  char tmp_symlink_temp[] = "/tmp/vectis-embedded-fs-tmp-link.XXXXXX";
  char tmp_outside_temp[] = "/tmp/vectis-embedded-fs-tmp-outside.XXXXXX";
  char directory_temp[] = "/tmp/vectis-embedded-fs-dir.XXXXXX";
  char verify_temp[] = "/tmp/vectis-embedded-fs-verify.XXXXXX";
  char verify_directory_temp[] = "/tmp/vectis-embedded-fs-verify-dir.XXXXXX";
  char extracted[512];
  char extracted_app[512];
  char user_file[512];
  char symlink_dir[512];
  char outside_app[512];
  char tmp_symlink[512];
  char tmp_outside_app[512];
  char directory_path[512];
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
  static const char trailing_path_manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/assets/\",\"kind\":\"directory\",\"mode\":365}]}";
  static const char file_ancestor_manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/foo\",\"offset\":0,\"size\":6,"
      "\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\"},"
      "{\"path\":\"/foo-bar\",\"kind\":\"directory\",\"mode\":365},"
      "{\"path\":\"/foo/bar\",\"offset\":6,\"size\":4,"
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
      "\"\\\"8a8f60ecb09b7e64c6d5214a8043865e608507db8c3f61f995eae6d078875901\\"
      "\"\"}]}";
  static const char invalid_kind_manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/index.html\",\"kind\":\"symlink\",\"offset\":0,"
      "\"size\":6,\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\"}]}";
  static const char missing_file_metadata_manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/index.html\",\"kind\":\"file\",\"offset\":0,"
      "\"size\":6}]}";
  static const char invalid_directory_metadata_manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/assets\",\"kind\":\"directory\",\"mode\":365,"
      "\"content_type\":\"text/plain\"}]}";
  static const char invalid_mode_manifest[] =
      "{\"format\":\"vectis-pack\",\"assets\":["
      "{\"path\":\"/index.html\",\"kind\":\"file\",\"mode\":420,"
      "\"offset\":0,\"size\":6,\"sha256\":"
      "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03\"}]}";
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
             strcmp(fs->tree_sha256(fs), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                         "aaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0,
         "receiver exposes manifest asset tree hash");
  expect(
      strcmp(
          vectis_embedded_fs_tree_sha256(fs),
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
  expect(entry.kind == VECTIS_EMBEDDED_FS_ENTRY_FILE && entry.mode == 0444u,
         "lookup exposes file kind and portable mode metadata");
  expect(entry.etag != NULL &&
             strcmp(entry.etag, "\"5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af"
                                "34d08286a2e846f6be03\"") == 0,
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
  expect(listed.app_metadata,
         "list callback exposes full embedded entry metadata");

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
  (void)snprintf(extracted_app, sizeof(extracted_app), "%s/assets/app.txt",
                 temp);
  expect(stat(extracted_app, &st) == 0 && (st.st_mode & 0777u) == 0644u,
         "extract applies manifest file mode with owner write");

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
  expect(stat(extracted_app, &st) == 0 && (st.st_mode & 0777u) == 0644u,
         "repair reapplies manifest file mode with owner write");
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

  expect(mkdtemp(verify_temp) != NULL,
         "creates verify no-mutation temp directory");
  (void)snprintf(directory_path, sizeof(directory_path), "%s/missing-output",
                 verify_temp);
  vectis_embedded_fs_extract_config_init(&extract);
  extract.output_dir = directory_path;
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_VERIFY;
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_ERR_CONFLICT,
         "verify rejects a missing output directory");
  expect(lstat(directory_path, &st) != 0,
         "verify does not create a missing output directory");
  remove_tree(verify_temp);

  expect(mkdtemp(symlink_temp) != NULL,
         "creates symlink extraction temp directory");
  expect(mkdtemp(outside_temp) != NULL, "creates outside temp directory");
  (void)snprintf(symlink_dir, sizeof(symlink_dir), "%s/assets", symlink_temp);
  (void)snprintf(outside_app, sizeof(outside_app), "%s/app.txt", outside_temp);
  expect(symlink(outside_temp, symlink_dir) == 0,
         "creates extraction parent symlink fixture");
  vectis_embedded_fs_extract_config_init(&extract);
  extract.output_dir = symlink_temp;
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_REPAIR;
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_ERR_CONFLICT,
         "extract refuses to follow symlink parent directories");
  expect(stat(outside_app, &st) != 0,
         "symlink-denied extraction does not write outside docroot");
  remove_tree(symlink_temp);
  remove_tree(outside_temp);

  expect(mkdtemp(root_symlink_temp) != NULL,
         "creates intermediate-root symlink temp directory");
  expect(mkdtemp(root_outside_temp) != NULL,
         "creates intermediate-root symlink outside directory");
  (void)snprintf(symlink_dir, sizeof(symlink_dir), "%s/root-link",
                 root_symlink_temp);
  (void)snprintf(directory_path, sizeof(directory_path), "%s/verified-output",
                 root_outside_temp);
  expect(mkdir(directory_path, 0755) == 0,
         "creates intermediate-root verify fixture");
  (void)snprintf(extracted, sizeof(extracted), "%s/index.html", directory_path);
  write_file(extracted, "hello\n");
  (void)snprintf(extracted_app, sizeof(extracted_app), "%s/assets",
                 directory_path);
  expect(mkdir(extracted_app, 0755) == 0,
         "creates intermediate-root verify assets directory");
  (void)snprintf(extracted_app, sizeof(extracted_app), "%s/assets/app.txt",
                 directory_path);
  write_file(extracted_app, "app\n");
  expect(symlink(root_outside_temp, symlink_dir) == 0,
         "creates intermediate output-root symlink");
  (void)snprintf(extracted, sizeof(extracted), "%s/verified-output",
                 symlink_dir);
  vectis_embedded_fs_extract_config_init(&extract);
  extract.output_dir = extracted;
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_VERIFY;
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_ERR_CONFLICT,
         "verify refuses an output root with an intermediate symlink");
  (void)snprintf(extracted, sizeof(extracted), "%s/created-output",
                 symlink_dir);
  (void)snprintf(outside_app, sizeof(outside_app), "%s/created-output",
                 root_outside_temp);
  extract.output_dir = extracted;
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_REPAIR;
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_ERR_CONFLICT,
         "extract refuses an intermediate-symlink output root");
  expect(lstat(outside_app, &st) != 0,
         "intermediate-root extraction does not create outside output");
  remove_tree(root_symlink_temp);
  remove_tree(root_outside_temp);

  expect(mkdtemp(tmp_symlink_temp) != NULL,
         "creates temp-symlink extraction directory");
  expect(mkdtemp(tmp_outside_temp) != NULL,
         "creates temp-symlink outside directory");
  (void)snprintf(symlink_dir, sizeof(symlink_dir), "%s/assets",
                 tmp_symlink_temp);
  expect(mkdir(symlink_dir, 0755) == 0,
         "creates parent directory for temp symlink fixture");
  (void)snprintf(extracted_app, sizeof(extracted_app), "%s/assets/app.txt",
                 tmp_symlink_temp);
  (void)snprintf(tmp_symlink, sizeof(tmp_symlink), "%s.tmp.%ld", extracted_app,
                 (long)getpid());
  (void)snprintf(tmp_outside_app, sizeof(tmp_outside_app), "%s/app.txt",
                 tmp_outside_temp);
  write_file(tmp_outside_app, "outside\n");
  expect(symlink(tmp_outside_app, tmp_symlink) == 0,
         "creates extraction temp symlink fixture");
  vectis_embedded_fs_extract_config_init(&extract);
  extract.output_dir = tmp_symlink_temp;
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_REPAIR;
  status = vectis_embedded_fs_extract(fs, &extract, &error);
  expect(status == VECTIS_ERR_CONFLICT,
         "extract refuses preexisting temp publish symlink");
  read_file(tmp_outside_app, buffer, sizeof(buffer));
  expect(strcmp(buffer, "outside\n") == 0,
         "temp-symlink extraction does not write outside docroot");
  remove_tree(tmp_symlink_temp);
  remove_tree(tmp_outside_temp);

  vectis_embedded_fs_close(fs);

  prefix_fs = new_adjacent_prefix_fs(&error);
  if (prefix_fs == NULL) {
    return 1;
  }
  memset(&listed, 0, sizeof(listed));
  status = vectis_embedded_fs_list(prefix_fs, "/assets", list_entry, &listed,
                                   &error);
  expect(status == VECTIS_OK && listed.count == 1u && listed.saw_app &&
             !listed.saw_skip,
         "embedded list prefix respects path segment boundaries");
  vectis_embedded_fs_close(prefix_fs);

  directory_fs = new_directory_fs(&error);
  if (directory_fs == NULL) {
    return 1;
  }
  found = 0;
  memset(&entry, 0, sizeof(entry));
  status = vectis_embedded_fs_lookup(directory_fs, "/assets", &found, &entry,
                                     &error);
  expect(status == VECTIS_OK && found &&
             entry.kind == VECTIS_EMBEDDED_FS_ENTRY_DIRECTORY &&
             entry.mode == 0555u && entry.size == 0u && entry.data == NULL &&
             entry.sha256 == NULL && entry.etag == NULL,
         "lookup exposes embedded directory metadata");
  found = 0;
  body.data = NULL;
  body.size = 0u;
  status =
      vectis_embedded_fs_read(directory_fs, "/assets", &found, &body, &error);
  expect(status == VECTIS_ERR_INVALID && found,
         "embedded directory cannot be read as file bytes");
  found = 0;
  source = NULL;
  status = vectis_embedded_fs_open_source(directory_fs, "/assets", &found,
                                          &source, &error);
  expect(status == VECTIS_ERR_INVALID && found && source == NULL,
         "embedded directory cannot be opened as a source");
  found = 0;
  memset(&chunked, 0, sizeof(chunked));
  status = vectis_embedded_fs_stream(directory_fs, "/assets", 2u, &found,
                                     chunk_entry, &chunked, &error);
  expect(status == VECTIS_ERR_INVALID && found && chunked.count == 0u,
         "embedded directory cannot be streamed as file chunks");
  memset(&listed, 0, sizeof(listed));
  status = vectis_embedded_fs_list(directory_fs, "/assets", list_entry, &listed,
                                   &error);
  expect(status == VECTIS_OK && listed.count == 2u && listed.saw_assets_dir &&
             listed.saw_app && listed.dir_metadata,
         "lists embedded directory entries under prefix");
  expect(mkdtemp(verify_directory_temp) != NULL,
         "creates explicit-directory verify temp directory");
  vectis_embedded_fs_extract_config_init(&extract);
  extract.output_dir = verify_directory_temp;
  extract.policy = VECTIS_EMBEDDED_FS_EXTRACT_VERIFY;
  status = vectis_embedded_fs_extract(directory_fs, &extract, &error);
  expect(status == VECTIS_ERR_CONFLICT,
         "verify rejects a missing embedded directory");
  (void)snprintf(directory_path, sizeof(directory_path), "%s/assets",
                 verify_directory_temp);
  expect(lstat(directory_path, &st) != 0,
         "verify does not create missing embedded directories");
  expect(mkdir(directory_path, 0700) == 0,
         "creates verify directory mode fixture");
  status = vectis_embedded_fs_extract(directory_fs, &extract, &error);
  expect(status == VECTIS_ERR_CONFLICT,
         "verify rejects a missing file in an existing directory");
  expect(stat(directory_path, &st) == 0 && (st.st_mode & 0777u) == 0700u,
         "verify does not change existing directory permissions");
  (void)snprintf(extracted_app, sizeof(extracted_app), "%s/app.txt",
                 directory_path);
  expect(lstat(extracted_app, &st) != 0,
         "verify does not create missing files in existing directories");
  remove_tree(verify_directory_temp);
  expect(mkdtemp(directory_temp) != NULL,
         "creates directory-entry extraction temp directory");
  vectis_embedded_fs_extract_config_init(&extract);
  extract.output_dir = directory_temp;
  status = vectis_embedded_fs_extract(directory_fs, &extract, &error);
  expect(status == VECTIS_OK, "extracts explicit embedded directories");
  (void)snprintf(directory_path, sizeof(directory_path), "%s/assets",
                 directory_temp);
  expect(stat(directory_path, &st) == 0 && S_ISDIR(st.st_mode) &&
             (st.st_mode & 0777u) == 0755u,
         "extract applies directory mode with owner write");
  (void)snprintf(extracted_app, sizeof(extracted_app), "%s/assets/app.txt",
                 directory_temp);
  read_file(extracted_app, buffer, sizeof(buffer));
  expect(strcmp(buffer, "app\n") == 0,
         "extracts file inside explicit embedded directory");
  remove_tree(directory_temp);
  vectis_embedded_fs_close(directory_fs);

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
  config.payload = "";
  config.payload_size = 0u;
  config.manifest_json = trailing_path_manifest;
  config.manifest_json_size = sizeof(trailing_path_manifest) - 1u;
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_ERR_INVALID && fs == NULL,
         "rejects trailing-slash embedded manifest paths");

  vectis_embedded_fs_config_init(&config);
  config.payload = "hello\napp\n";
  config.payload_size = 10u;
  config.manifest_json = file_ancestor_manifest;
  config.manifest_json_size = sizeof(file_ancestor_manifest) - 1u;
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_ERR_CONFLICT && fs == NULL,
         "rejects files that parent embedded manifest assets");

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
  config.payload = "hello\n";
  config.payload_size = 6u;
  config.manifest_json = invalid_kind_manifest;
  config.manifest_json_size = sizeof(invalid_kind_manifest) - 1u;
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_ERR_INVALID && fs == NULL,
         "rejects unsupported manifest asset kind");

  vectis_embedded_fs_config_init(&config);
  config.payload = "hello\n";
  config.payload_size = 6u;
  config.manifest_json = missing_file_metadata_manifest;
  config.manifest_json_size = sizeof(missing_file_metadata_manifest) - 1u;
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_ERR_INVALID && fs == NULL,
         "rejects incomplete embedded file metadata");

  vectis_embedded_fs_config_init(&config);
  config.payload = "hello\n";
  config.payload_size = 6u;
  config.manifest_json = invalid_directory_metadata_manifest;
  config.manifest_json_size = sizeof(invalid_directory_metadata_manifest) - 1u;
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_ERR_INVALID && fs == NULL,
         "rejects file-only metadata on embedded directories");

  vectis_embedded_fs_config_init(&config);
  config.payload = "hello\n";
  config.payload_size = 6u;
  config.manifest_json = invalid_mode_manifest;
  config.manifest_json_size = sizeof(invalid_mode_manifest) - 1u;
  fs = NULL;
  status = vectis_embedded_fs_from_pack(&config, &fs, &error);
  expect(status == VECTIS_ERR_INVALID && fs == NULL,
         "rejects writable manifest asset mode");

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
