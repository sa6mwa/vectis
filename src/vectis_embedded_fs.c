#include <vectis/embedded_fs.h>

#include "vectis_internal.h"

#include <errno.h>
#include <lonejson.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VECTIS_EMBEDDED_FS_DEFAULT_CHUNK_SIZE (64u * 1024u)

typedef struct vectis_embedded_fs_impl_entry {
  char *path;
  char *content_type;
  char *sha256;
  const unsigned char *data;
  size_t size;
} vectis_embedded_fs_impl_entry;

typedef struct vectis_embedded_fs_impl {
  vectis_embedded_fs api;
  vectis_embedded_fs_impl_entry *entries;
  size_t count;
  size_t capacity;
  vectis_embedded_fs_extract_policy default_extract_policy;
  char *index_path;
  char *not_found_path;
} vectis_embedded_fs_impl;

typedef struct vectis_embedded_manifest_asset {
  char *path;
  char *content_type;
  char *sha256;
  lonejson_uint64 offset;
  lonejson_uint64 size;
} vectis_embedded_manifest_asset;

typedef struct vectis_embedded_manifest {
  char *format;
  char *extract_mode;
  lonejson_mapped_array_stream assets;
} vectis_embedded_manifest;

typedef struct vectis_embedded_parse_state {
  vectis_embedded_fs_impl *impl;
  const unsigned char *payload;
  size_t payload_size;
  vectis_error *error;
} vectis_embedded_parse_state;

static const lonejson_field vectis_embedded_manifest_asset_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(vectis_embedded_manifest_asset, path,
                                    "path"),
    LONEJSON_FIELD_U64_REQ(vectis_embedded_manifest_asset, offset, "offset"),
    LONEJSON_FIELD_U64_REQ(vectis_embedded_manifest_asset, size, "size"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(vectis_embedded_manifest_asset, sha256,
                                    "sha256"),
    LONEJSON_FIELD_STRING_ALLOC(vectis_embedded_manifest_asset, content_type,
                                "content_type")};

LONEJSON_MAP_DEFINE(vectis_embedded_manifest_asset_map,
                    vectis_embedded_manifest_asset,
                    vectis_embedded_manifest_asset_fields);

static const lonejson_field vectis_embedded_manifest_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(vectis_embedded_manifest, format, "format"),
    LONEJSON_FIELD_STRING_ALLOC(vectis_embedded_manifest, extract_mode,
                                "extract_mode"),
    LONEJSON_FIELD_MAPPED_ARRAY_STREAM_REQ(vectis_embedded_manifest, assets,
                                           "assets")};

LONEJSON_MAP_DEFINE(vectis_embedded_manifest_map, vectis_embedded_manifest,
                    vectis_embedded_manifest_fields);

static void vectis_embedded_set_errorf(vectis_error *error, vectis_status code,
                                       const char *fmt, ...) {
  va_list ap;

  vectis_error_clear(error);
  if (error == NULL) {
    return;
  }
  error->code = code;
  error->source = VECTIS_ERROR_SOURCE_VECTIS;
  va_start(ap, fmt);
  (void)vsnprintf(error->message, sizeof(error->message), fmt, ap);
  va_end(ap);
}

static vectis_status
vectis_embedded_lonejson_error(vectis_error *error, lonejson_status status,
                               const lonejson_error *json_error,
                               const char *message) {
  vectis_error_clear(error);
  if (error != NULL) {
    error->code = status == LONEJSON_STATUS_ALLOCATION_FAILED
                      ? VECTIS_ERR_NOMEM
                      : VECTIS_ERR_INVALID;
    error->source = VECTIS_ERROR_SOURCE_LONEJSON;
    error->dependency_code = (long)status;
    (void)snprintf(error->message, sizeof(error->message), "%s: %s",
                   message != NULL ? message : "embedded manifest error",
                   json_error != NULL && json_error->message[0] != '\0'
                       ? json_error->message
                       : lonejson_status_string(status));
  }
  return status == LONEJSON_STATUS_ALLOCATION_FAILED ? VECTIS_ERR_NOMEM
                                                     : VECTIS_ERR_INVALID;
}

static char *vectis_embedded_strdup(const char *value) {
  char *copy;
  size_t size;

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

static int vectis_embedded_path_valid(const char *path) {
  const char *segment;
  size_t segment_size;

  if (path == NULL || path[0] != '/' || path[1] == '\0') {
    return 0;
  }
  segment = path + 1;
  while (*segment != '\0') {
    segment_size = 0u;
    while (segment[segment_size] != '\0' && segment[segment_size] != '/') {
      segment_size++;
    }
    if (segment_size == 0u || (segment_size == 1u && segment[0] == '.') ||
        (segment_size == 2u && segment[0] == '.' && segment[1] == '.')) {
      return 0;
    }
    segment += segment_size;
    if (*segment == '/') {
      segment++;
    }
  }
  return 1;
}

static int vectis_embedded_hex_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

static int vectis_embedded_sha256_matches(const unsigned char *data,
                                          size_t size, const char *hex) {
  unsigned char actual[SHA256_DIGEST_LENGTH];
  unsigned char expected[SHA256_DIGEST_LENGTH];
  size_t i;
  int hi;
  int lo;

  if (hex == NULL || strlen(hex) != SHA256_DIGEST_LENGTH * 2u) {
    return 0;
  }
  for (i = 0u; i < SHA256_DIGEST_LENGTH; ++i) {
    hi = vectis_embedded_hex_value(hex[i * 2u]);
    lo = vectis_embedded_hex_value(hex[i * 2u + 1u]);
    if (hi < 0 || lo < 0) {
      return 0;
    }
    expected[i] = (unsigned char)((hi << 4) | lo);
  }
  SHA256(data, size, actual);
  return memcmp(actual, expected, SHA256_DIGEST_LENGTH) == 0;
}

static void
vectis_embedded_sha256_hex(const unsigned char sha[SHA256_DIGEST_LENGTH],
                           char out[SHA256_DIGEST_LENGTH * 2u + 1u]) {
  static const char hex[] = "0123456789abcdef";
  size_t i;

  for (i = 0u; i < SHA256_DIGEST_LENGTH; ++i) {
    out[i * 2u] = hex[(sha[i] >> 4) & 0x0f];
    out[i * 2u + 1u] = hex[sha[i] & 0x0f];
  }
  out[SHA256_DIGEST_LENGTH * 2u] = '\0';
}

const char *vectis_embedded_fs_extract_policy_string(
    vectis_embedded_fs_extract_policy policy) {
  switch (policy) {
  case VECTIS_EMBEDDED_FS_EXTRACT_FAIL_EXISTS:
    return "fail_exists";
  case VECTIS_EMBEDDED_FS_EXTRACT_SKIP_EXISTING:
    return "skip_existing";
  case VECTIS_EMBEDDED_FS_EXTRACT_OVERWRITE:
    return "overwrite";
  case VECTIS_EMBEDDED_FS_EXTRACT_VERIFY:
    return "verify";
  case VECTIS_EMBEDDED_FS_EXTRACT_REPAIR:
    return "repair";
  }
  return "unknown";
}

int vectis_embedded_fs_extract_policy_parse(
    const char *value, vectis_embedded_fs_extract_policy *out) {
  vectis_embedded_fs_extract_policy policy;

  if (value == NULL || out == NULL) {
    return 0;
  }
  if (strcmp(value, "fail_exists") == 0 || strcmp(value, "fail-exists") == 0 ||
      strcmp(value, "fail") == 0) {
    policy = VECTIS_EMBEDDED_FS_EXTRACT_FAIL_EXISTS;
  } else if (strcmp(value, "skip_existing") == 0 ||
             strcmp(value, "skip-existing") == 0 ||
             strcmp(value, "skip") == 0) {
    policy = VECTIS_EMBEDDED_FS_EXTRACT_SKIP_EXISTING;
  } else if (strcmp(value, "overwrite") == 0) {
    policy = VECTIS_EMBEDDED_FS_EXTRACT_OVERWRITE;
  } else if (strcmp(value, "verify") == 0) {
    policy = VECTIS_EMBEDDED_FS_EXTRACT_VERIFY;
  } else if (strcmp(value, "repair") == 0) {
    policy = VECTIS_EMBEDDED_FS_EXTRACT_REPAIR;
  } else {
    return 0;
  }
  *out = policy;
  return 1;
}

static int vectis_embedded_reserve(vectis_embedded_fs_impl *impl,
                                   size_t capacity) {
  vectis_embedded_fs_impl_entry *grown;
  size_t next_capacity;

  if (capacity <= impl->capacity) {
    return 1;
  }
  next_capacity = impl->capacity == 0u ? 8u : impl->capacity;
  while (next_capacity < capacity) {
    if (next_capacity > ((size_t)-1) / 2u) {
      return 0;
    }
    next_capacity *= 2u;
  }
  grown = (vectis_embedded_fs_impl_entry *)realloc(
      impl->entries, next_capacity * sizeof(impl->entries[0]));
  if (grown == NULL) {
    return 0;
  }
  impl->entries = grown;
  impl->capacity = next_capacity;
  return 1;
}

static int vectis_embedded_entry_compare(const void *left, const void *right) {
  const vectis_embedded_fs_impl_entry *a;
  const vectis_embedded_fs_impl_entry *b;

  a = (const vectis_embedded_fs_impl_entry *)left;
  b = (const vectis_embedded_fs_impl_entry *)right;
  return strcmp(a->path, b->path);
}

static vectis_status vectis_embedded_add(
    vectis_embedded_fs_impl *impl, const vectis_embedded_manifest_asset *asset,
    const unsigned char *payload, size_t payload_size, vectis_error *error) {
  vectis_embedded_fs_impl_entry *entry;
  size_t offset;
  size_t size;

  if (!vectis_embedded_path_valid(asset->path)) {
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "embedded asset path is invalid: %s",
                               asset->path != NULL ? asset->path : "(null)");
    return VECTIS_ERR_INVALID;
  }
  if (asset->offset > (lonejson_uint64)payload_size ||
      asset->size > (lonejson_uint64)payload_size - asset->offset) {
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "embedded asset exceeds payload bounds: %s",
                               asset->path);
    return VECTIS_ERR_INVALID;
  }
  offset = (size_t)asset->offset;
  size = (size_t)asset->size;
  if (!vectis_embedded_sha256_matches(payload + offset, size, asset->sha256)) {
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "embedded asset hash mismatch: %s", asset->path);
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_embedded_reserve(impl, impl->count + 1u)) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to grow embedded asset index");
    return VECTIS_ERR_NOMEM;
  }
  entry = impl->entries + impl->count;
  memset(entry, 0, sizeof(*entry));
  entry->path = vectis_embedded_strdup(asset->path);
  entry->sha256 = vectis_embedded_strdup(asset->sha256);
  if (asset->content_type != NULL) {
    entry->content_type = vectis_embedded_strdup(asset->content_type);
  }
  if (entry->path == NULL || entry->sha256 == NULL ||
      (asset->content_type != NULL && entry->content_type == NULL)) {
    free(entry->path);
    free(entry->content_type);
    free(entry->sha256);
    memset(entry, 0, sizeof(*entry));
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy embedded asset metadata");
    return VECTIS_ERR_NOMEM;
  }
  entry->data = payload + offset;
  entry->size = size;
  impl->count++;
  return VECTIS_OK;
}

static lonejson_status
vectis_embedded_manifest_asset_item(void *user, void *item,
                                    lonejson_error *json_error) {
  vectis_embedded_parse_state *state;
  vectis_status status;

  (void)json_error;
  state = (vectis_embedded_parse_state *)user;
  status = vectis_embedded_add(
      state->impl, (const vectis_embedded_manifest_asset *)item, state->payload,
      state->payload_size, state->error);
  if (status == VECTIS_ERR_NOMEM) {
    return LONEJSON_STATUS_ALLOCATION_FAILED;
  }
  if (status != VECTIS_OK) {
    return LONEJSON_STATUS_INVALID_JSON;
  }
  return LONEJSON_STATUS_OK;
}

static void vectis_embedded_impl_destroy(vectis_embedded_fs_impl *impl) {
  size_t i;

  if (impl == NULL) {
    return;
  }
  for (i = 0u; i < impl->count; ++i) {
    free(impl->entries[i].path);
    free(impl->entries[i].content_type);
    free(impl->entries[i].sha256);
  }
  free(impl->entries);
  free(impl->index_path);
  free(impl->not_found_path);
  free(impl);
}

static vectis_embedded_fs_extract_policy
vectis_embedded_default_extract_policy_impl(const vectis_embedded_fs *self) {
  const vectis_embedded_fs_impl *impl;

  if (self == NULL || self->impl == NULL) {
    return VECTIS_EMBEDDED_FS_EXTRACT_FAIL_EXISTS;
  }
  impl = (const vectis_embedded_fs_impl *)self->impl;
  return impl->default_extract_policy;
}

static vectis_status vectis_embedded_lookup_impl(const vectis_embedded_fs *self,
                                                 const char *path, int *found,
                                                 vectis_embedded_fs_entry *out,
                                                 vectis_error *error) {
  const vectis_embedded_fs_impl *impl;
  const char *lookup_path;
  size_t i;

  if (found == NULL || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs lookup output is required");
    return VECTIS_ERR_INVALID;
  }
  *found = 0;
  memset(out, 0, sizeof(*out));
  if (self == NULL || self->impl == NULL || path == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs and path are required");
    return VECTIS_ERR_INVALID;
  }
  impl = (const vectis_embedded_fs_impl *)self->impl;
  lookup_path = strcmp(path, "/") == 0 ? impl->index_path : path;
  if (!vectis_embedded_path_valid(lookup_path)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "embedded fs path is invalid");
    return VECTIS_ERR_INVALID;
  }
  for (i = 0u; i < impl->count; ++i) {
    if (strcmp(impl->entries[i].path, lookup_path) == 0) {
      *found = 1;
      out->path = impl->entries[i].path;
      out->content_type = impl->entries[i].content_type;
      out->sha256 = impl->entries[i].sha256;
      out->data = impl->entries[i].data;
      out->size = impl->entries[i].size;
      return VECTIS_OK;
    }
  }
  if (impl->not_found_path != NULL &&
      strcmp(lookup_path, impl->not_found_path) != 0) {
    return vectis_embedded_lookup_impl(self, impl->not_found_path, found, out,
                                       error);
  }
  return VECTIS_OK;
}

static vectis_status vectis_embedded_read_impl(const vectis_embedded_fs *self,
                                               const char *path, int *found,
                                               vectis_bytes *out,
                                               vectis_error *error) {
  vectis_embedded_fs_entry entry;
  vectis_status status;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs read output is required");
    return VECTIS_ERR_INVALID;
  }
  out->data = NULL;
  out->size = 0u;
  status = vectis_embedded_lookup_impl(self, path, found, &entry, error);
  if (status != VECTIS_OK || (found != NULL && !*found)) {
    return status;
  }
  out->data = entry.data;
  out->size = entry.size;
  return VECTIS_OK;
}

static vectis_status
vectis_embedded_list_impl(const vectis_embedded_fs *self, const char *prefix,
                          vectis_embedded_fs_list_fn callback, void *userdata,
                          vectis_error *error) {
  const vectis_embedded_fs_impl *impl;
  vectis_embedded_fs_entry entry;
  size_t prefix_size;
  size_t i;
  vectis_status status;

  if (self == NULL || self->impl == NULL || callback == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs list callback is required");
    return VECTIS_ERR_INVALID;
  }
  if (prefix == NULL) {
    prefix = "/";
  }
  if (strcmp(prefix, "/") != 0 && !vectis_embedded_path_valid(prefix)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs list prefix is invalid");
    return VECTIS_ERR_INVALID;
  }
  impl = (const vectis_embedded_fs_impl *)self->impl;
  prefix_size = strlen(prefix);
  for (i = 0u; i < impl->count; ++i) {
    if (strncmp(impl->entries[i].path, prefix, prefix_size) != 0) {
      continue;
    }
    entry.path = impl->entries[i].path;
    entry.content_type = impl->entries[i].content_type;
    entry.sha256 = impl->entries[i].sha256;
    entry.data = impl->entries[i].data;
    entry.size = impl->entries[i].size;
    status = callback(&entry, userdata, error);
    if (status != VECTIS_OK) {
      return status;
    }
  }
  return VECTIS_OK;
}

static vectis_status
vectis_embedded_stream_impl(const vectis_embedded_fs *self, const char *path,
                            size_t chunk_size, int *found,
                            vectis_embedded_fs_chunk_fn callback,
                            void *userdata, vectis_error *error) {
  vectis_embedded_fs_entry entry;
  size_t offset;
  size_t remaining;
  size_t amount;
  vectis_status status;

  if (callback == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs stream callback is required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_embedded_lookup_impl(self, path, found, &entry, error);
  if (status != VECTIS_OK || (found != NULL && !*found)) {
    return status;
  }
  if (chunk_size == 0u) {
    chunk_size = VECTIS_EMBEDDED_FS_DEFAULT_CHUNK_SIZE;
  }
  offset = 0u;
  while (offset < entry.size) {
    remaining = entry.size - offset;
    amount = remaining < chunk_size ? remaining : chunk_size;
    status = callback((const unsigned char *)entry.data + offset, amount,
                      userdata, error);
    if (status != VECTIS_OK) {
      return status;
    }
    offset += amount;
  }
  return VECTIS_OK;
}

static int vectis_embedded_mkdir_p(const char *path) {
  char *copy;
  char *p;

  copy = vectis_embedded_strdup(path);
  if (copy == NULL) {
    return 0;
  }
  for (p = copy + 1; *p != '\0'; ++p) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        free(copy);
        return 0;
      }
      *p = '/';
    }
  }
  if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
    free(copy);
    return 0;
  }
  free(copy);
  return 1;
}

static int vectis_embedded_parent_dirs(const char *path) {
  char *copy;
  char *slash;
  int ok;

  copy = vectis_embedded_strdup(path);
  if (copy == NULL) {
    return 0;
  }
  slash = strrchr(copy, '/');
  if (slash == NULL || slash == copy) {
    free(copy);
    return 1;
  }
  *slash = '\0';
  ok = vectis_embedded_mkdir_p(copy);
  free(copy);
  return ok;
}

static char *vectis_embedded_output_path(const char *root,
                                         const char *asset_path) {
  size_t root_size;
  size_t rel_size;
  size_t separator;
  char *out;

  root_size = strlen(root);
  rel_size = strlen(asset_path + 1);
  separator = root_size > 0u && root[root_size - 1u] == '/' ? 0u : 1u;
  out = (char *)malloc(root_size + separator + rel_size + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, root, root_size);
  if (separator) {
    out[root_size] = '/';
  }
  memcpy(out + root_size + separator, asset_path + 1, rel_size + 1u);
  return out;
}

static vectis_status
vectis_embedded_write_file(const char *path,
                           const vectis_embedded_fs_impl_entry *entry,
                           vectis_error *error) {
  char *tmp_path;
  FILE *fp;
  int written;

  if (!vectis_embedded_parent_dirs(path)) {
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to create embedded asset directory: %s",
                               path);
    return VECTIS_ERR_INVALID;
  }
  tmp_path = (char *)malloc(strlen(path) + 64u);
  if (tmp_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate embedded asset temp path");
    return VECTIS_ERR_NOMEM;
  }
  written = snprintf(tmp_path, strlen(path) + 64u, "%s.tmp.%ld", path,
                     (long)getpid());
  if (written <= 0 || (size_t)written >= strlen(path) + 64u) {
    free(tmp_path);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded asset temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  fp = fopen(tmp_path, "wb");
  if (fp == NULL) {
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to create embedded asset: %s", path);
    free(tmp_path);
    return VECTIS_ERR_INVALID;
  }
  if (entry->size > 0u &&
      fwrite(entry->data, 1u, entry->size, fp) != entry->size) {
    (void)fclose(fp);
    (void)remove(tmp_path);
    free(tmp_path);
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to write embedded asset: %s", path);
    return VECTIS_ERR_INVALID;
  }
  if (fclose(fp) != 0) {
    (void)remove(tmp_path);
    free(tmp_path);
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to write embedded asset: %s", path);
    return VECTIS_ERR_INVALID;
  }
  if (rename(tmp_path, path) != 0) {
    (void)remove(tmp_path);
    free(tmp_path);
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to publish embedded asset: %s", path);
    return VECTIS_ERR_INVALID;
  }
  free(tmp_path);
  return VECTIS_OK;
}

static vectis_status
vectis_embedded_file_matches_entry(const char *path,
                                   const vectis_embedded_fs_impl_entry *entry,
                                   int *matches, vectis_error *error) {
  EVP_MD_CTX *ctx;
  FILE *fp;
  unsigned char buffer[64u * 1024u];
  unsigned char digest[SHA256_DIGEST_LENGTH];
  char digest_hex[SHA256_DIGEST_LENGTH * 2u + 1u];
  unsigned int digest_size;
  size_t nread;
  int failed;

  if (matches == NULL || entry == NULL || entry->sha256 == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded asset verification input is required");
    return VECTIS_ERR_INVALID;
  }
  *matches = 0;
  fp = fopen(path, "rb");
  if (fp == NULL) {
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to open embedded asset for verify: %s",
                               path);
    return VECTIS_ERR_INVALID;
  }
  ctx = EVP_MD_CTX_new();
  if (ctx == NULL) {
    (void)fclose(fp);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate embedded asset verifier");
    return VECTIS_ERR_NOMEM;
  }
  failed = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1;
  while (!failed && (nread = fread(buffer, 1u, sizeof(buffer), fp)) > 0u) {
    if (EVP_DigestUpdate(ctx, buffer, nread) != 1) {
      failed = 1;
    }
  }
  if (ferror(fp)) {
    failed = 1;
  }
  digest_size = 0u;
  if (!failed && (EVP_DigestFinal_ex(ctx, digest, &digest_size) != 1 ||
                  digest_size != SHA256_DIGEST_LENGTH)) {
    failed = 1;
  }
  EVP_MD_CTX_free(ctx);
  if (fclose(fp) != 0) {
    failed = 1;
  }
  if (failed) {
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to verify embedded asset: %s", path);
    return VECTIS_ERR_INVALID;
  }
  vectis_embedded_sha256_hex(digest, digest_hex);
  *matches = strcmp(digest_hex, entry->sha256) == 0;
  return VECTIS_OK;
}

static vectis_status
vectis_embedded_extract_impl(const vectis_embedded_fs *self,
                             const vectis_embedded_fs_extract_config *config,
                             vectis_error *error) {
  const vectis_embedded_fs_impl *impl;
  char *path;
  struct stat st;
  size_t i;
  vectis_status status;
  int matches;

  if (self == NULL || self->impl == NULL || config == NULL ||
      config->output_dir == NULL || config->output_dir[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs extract output_dir is required");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_embedded_mkdir_p(config->output_dir)) {
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to create embedded fs output dir: %s",
                               config->output_dir);
    return VECTIS_ERR_INVALID;
  }
  impl = (const vectis_embedded_fs_impl *)self->impl;
  for (i = 0u; i < impl->count; ++i) {
    path =
        vectis_embedded_output_path(config->output_dir, impl->entries[i].path);
    if (path == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to allocate embedded asset output path");
      return VECTIS_ERR_NOMEM;
    }
    if (stat(path, &st) == 0) {
      if (!S_ISREG(st.st_mode)) {
        vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                   "embedded asset output is not a file: %s",
                                   path);
        free(path);
        return VECTIS_ERR_CONFLICT;
      }
      if (config->policy == VECTIS_EMBEDDED_FS_EXTRACT_SKIP_EXISTING) {
        free(path);
        continue;
      }
      if (config->policy == VECTIS_EMBEDDED_FS_EXTRACT_VERIFY ||
          config->policy == VECTIS_EMBEDDED_FS_EXTRACT_REPAIR) {
        matches = 0;
        status = vectis_embedded_file_matches_entry(path, &impl->entries[i],
                                                    &matches, error);
        if (status != VECTIS_OK) {
          free(path);
          return status;
        }
        if (matches) {
          free(path);
          continue;
        }
        if (config->policy == VECTIS_EMBEDDED_FS_EXTRACT_VERIFY) {
          vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                     "embedded asset verification failed: %s",
                                     path);
          free(path);
          return VECTIS_ERR_CONFLICT;
        }
      }
      if (config->policy != VECTIS_EMBEDDED_FS_EXTRACT_OVERWRITE) {
        if (config->policy != VECTIS_EMBEDDED_FS_EXTRACT_REPAIR) {
          vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                     "embedded asset already exists: %s", path);
          free(path);
          return VECTIS_ERR_CONFLICT;
        }
      }
    } else if (errno != ENOENT) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                                 "failed to inspect embedded asset: %s", path);
      free(path);
      return VECTIS_ERR_INVALID;
    } else if (config->policy == VECTIS_EMBEDDED_FS_EXTRACT_VERIFY) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                 "embedded asset is missing: %s", path);
      free(path);
      return VECTIS_ERR_CONFLICT;
    }
    status = vectis_embedded_write_file(path, &impl->entries[i], error);
    free(path);
    if (status != VECTIS_OK) {
      return status;
    }
  }
  return VECTIS_OK;
}

static void vectis_embedded_close_impl(vectis_embedded_fs *self) {
  if (self != NULL) {
    vectis_embedded_impl_destroy((vectis_embedded_fs_impl *)self->impl);
  }
}

void vectis_embedded_fs_config_init(vectis_embedded_fs_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->index_path = "/index.html";
}

void vectis_embedded_fs_extract_config_init(
    vectis_embedded_fs_extract_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->policy = VECTIS_EMBEDDED_FS_EXTRACT_FAIL_EXISTS;
}

vectis_status
vectis_embedded_fs_from_pack(const vectis_embedded_fs_config *config,
                             vectis_embedded_fs **out, vectis_error *error) {
  vectis_embedded_fs_impl *impl;
  vectis_embedded_manifest doc;
  vectis_embedded_manifest_asset item;
  vectis_embedded_parse_state state;
  lonejson_mapped_array_stream_handler handler;
  lonejson_error json_error;
  lonejson_status json_status;
  lonejson *runtime;
  size_t i;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs output handle is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  if (config == NULL || config->manifest_json == NULL ||
      config->manifest_json_size == 0u || config->payload == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs manifest and payload are required");
    return VECTIS_ERR_INVALID;
  }
  impl = (vectis_embedded_fs_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate embedded fs");
    return VECTIS_ERR_NOMEM;
  }
  impl->api.lookup = vectis_embedded_lookup_impl;
  impl->api.default_extract_policy =
      vectis_embedded_default_extract_policy_impl;
  impl->api.read = vectis_embedded_read_impl;
  impl->api.list = vectis_embedded_list_impl;
  impl->api.stream = vectis_embedded_stream_impl;
  impl->api.extract = vectis_embedded_extract_impl;
  impl->api.close = vectis_embedded_close_impl;
  impl->api.impl = impl;
  impl->default_extract_policy = VECTIS_EMBEDDED_FS_EXTRACT_FAIL_EXISTS;
  impl->index_path = vectis_embedded_strdup(
      config->index_path != NULL ? config->index_path : "/index.html");
  if (config->not_found_path != NULL) {
    impl->not_found_path = vectis_embedded_strdup(config->not_found_path);
  }
  if (impl->index_path == NULL ||
      (config->not_found_path != NULL && impl->not_found_path == NULL)) {
    vectis_embedded_impl_destroy(impl);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy embedded fs defaults");
    return VECTIS_ERR_NOMEM;
  }
  if (!vectis_embedded_path_valid(impl->index_path) ||
      (impl->not_found_path != NULL &&
       !vectis_embedded_path_valid(impl->not_found_path))) {
    vectis_embedded_impl_destroy(impl);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs default paths are invalid");
    return VECTIS_ERR_INVALID;
  }
  runtime = lonejson_new(NULL, &json_error);
  if (runtime == NULL) {
    vectis_embedded_impl_destroy(impl);
    return vectis_embedded_lonejson_error(
        error, LONEJSON_STATUS_ALLOCATION_FAILED, &json_error,
        "failed to initialize embedded fs manifest parser");
  }
  memset(&doc, 0, sizeof(doc));
  memset(&item, 0, sizeof(item));
  doc.assets = (lonejson_mapped_array_stream)LONEJSON_MAPPED_ARRAY_STREAM_INIT;
  state.impl = impl;
  state.payload = (const unsigned char *)config->payload;
  state.payload_size = config->payload_size;
  state.error = error;
  handler.item_map = &vectis_embedded_manifest_asset_map;
  handler.item_dst = &item;
  handler.item = vectis_embedded_manifest_asset_item;
  handler.user = &state;
  lonejson_error_init(&json_error);
  json_status = lonejson_mapped_array_stream_set_handler(&doc.assets, &handler,
                                                         &json_error);
  if (json_status == LONEJSON_STATUS_OK) {
    json_status = lonejson_parse_buffer(
        runtime, &vectis_embedded_manifest_map, &doc, config->manifest_json,
        config->manifest_json_size, &json_error);
  }
  if (json_status != LONEJSON_STATUS_OK) {
    lonejson_cleanup(&vectis_embedded_manifest_map, &doc);
    lonejson_cleanup(&vectis_embedded_manifest_asset_map, &item);
    lonejson_free(runtime);
    vectis_embedded_impl_destroy(impl);
    if (error != NULL && error->message[0] != '\0') {
      return error->code;
    }
    return vectis_embedded_lonejson_error(
        error, json_status, &json_error,
        "failed to parse embedded fs manifest");
  }
  if (strcmp(doc.format, "vectis-pack") != 0) {
    lonejson_cleanup(&vectis_embedded_manifest_map, &doc);
    lonejson_cleanup(&vectis_embedded_manifest_asset_map, &item);
    lonejson_free(runtime);
    vectis_embedded_impl_destroy(impl);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs manifest format is unsupported");
    return VECTIS_ERR_INVALID;
  }
  if (doc.extract_mode != NULL &&
      !vectis_embedded_fs_extract_policy_parse(doc.extract_mode,
                                               &impl->default_extract_policy)) {
    lonejson_cleanup(&vectis_embedded_manifest_map, &doc);
    lonejson_cleanup(&vectis_embedded_manifest_asset_map, &item);
    lonejson_free(runtime);
    vectis_embedded_impl_destroy(impl);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs manifest extract_mode is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (impl->count > 1u) {
    qsort(impl->entries, impl->count, sizeof(impl->entries[0]),
          vectis_embedded_entry_compare);
    for (i = 1u; i < impl->count; ++i) {
      if (strcmp(impl->entries[i - 1u].path, impl->entries[i].path) == 0) {
        lonejson_cleanup(&vectis_embedded_manifest_map, &doc);
        lonejson_cleanup(&vectis_embedded_manifest_asset_map, &item);
        lonejson_free(runtime);
        vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                   "duplicate embedded asset path: %s",
                                   impl->entries[i].path);
        vectis_embedded_impl_destroy(impl);
        return VECTIS_ERR_CONFLICT;
      }
    }
  }
  lonejson_cleanup(&vectis_embedded_manifest_map, &doc);
  lonejson_cleanup(&vectis_embedded_manifest_asset_map, &item);
  lonejson_free(runtime);
  *out = &impl->api;
  return VECTIS_OK;
}

vectis_embedded_fs_extract_policy
vectis_embedded_fs_default_extract_policy(const vectis_embedded_fs *fs) {
  if (fs == NULL || fs->default_extract_policy == NULL) {
    return VECTIS_EMBEDDED_FS_EXTRACT_FAIL_EXISTS;
  }
  return fs->default_extract_policy(fs);
}

vectis_status vectis_embedded_fs_lookup(const vectis_embedded_fs *fs,
                                        const char *path, int *found,
                                        vectis_embedded_fs_entry *out,
                                        vectis_error *error) {
  if (fs == NULL || fs->lookup == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs lookup handle is required");
    return VECTIS_ERR_INVALID;
  }
  return fs->lookup(fs, path, found, out, error);
}

vectis_status vectis_embedded_fs_read(const vectis_embedded_fs *fs,
                                      const char *path, int *found,
                                      vectis_bytes *out, vectis_error *error) {
  if (fs == NULL || fs->read == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs read handle is required");
    return VECTIS_ERR_INVALID;
  }
  return fs->read(fs, path, found, out, error);
}

vectis_status vectis_embedded_fs_list(const vectis_embedded_fs *fs,
                                      const char *prefix,
                                      vectis_embedded_fs_list_fn callback,
                                      void *userdata, vectis_error *error) {
  if (fs == NULL || fs->list == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs list handle is required");
    return VECTIS_ERR_INVALID;
  }
  return fs->list(fs, prefix, callback, userdata, error);
}

vectis_status vectis_embedded_fs_stream(const vectis_embedded_fs *fs,
                                        const char *path, size_t chunk_size,
                                        int *found,
                                        vectis_embedded_fs_chunk_fn callback,
                                        void *userdata, vectis_error *error) {
  if (fs == NULL || fs->stream == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs stream handle is required");
    return VECTIS_ERR_INVALID;
  }
  return fs->stream(fs, path, chunk_size, found, callback, userdata, error);
}

vectis_status
vectis_embedded_fs_extract(const vectis_embedded_fs *fs,
                           const vectis_embedded_fs_extract_config *config,
                           vectis_error *error) {
  if (fs == NULL || fs->extract == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs extract handle is required");
    return VECTIS_ERR_INVALID;
  }
  return fs->extract(fs, config, error);
}

void vectis_embedded_fs_close(vectis_embedded_fs *fs) {
  if (fs != NULL && fs->close != NULL) {
    fs->close(fs);
  }
}
