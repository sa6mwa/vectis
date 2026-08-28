#include <vectis/embedded_fs.h>

#include "vectis_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <lc/lc.h>
#include <lonejson.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define VECTIS_EMBEDDED_FS_DEFAULT_CHUNK_SIZE (64u * 1024u)

typedef struct vectis_embedded_fs_impl_entry {
  vectis_embedded_fs_entry_kind kind;
  char *path;
  char *content_type;
  char *sha256;
  char *etag;
  const unsigned char *data;
  size_t size;
  unsigned mode;
} vectis_embedded_fs_impl_entry;

typedef struct vectis_embedded_fs_impl {
  vectis_embedded_fs api;
  vectis_embedded_fs_impl_entry *entries;
  size_t count;
  size_t capacity;
  vectis_embedded_fs_extract_policy default_extract_policy;
  char *index_path;
  char *not_found_path;
  char *tree_sha256;
} vectis_embedded_fs_impl;

typedef struct vectis_embedded_manifest_asset {
  char *path;
  char *kind;
  char *content_type;
  char *sha256;
  char *etag;
  lonejson_uint64 mode;
  int mode_present;
  lonejson_uint64 offset;
  int offset_present;
  lonejson_uint64 size;
  int size_present;
} vectis_embedded_manifest_asset;

typedef struct vectis_embedded_manifest {
  char *format;
  char *extract_mode;
  char *tree_sha256;
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
    LONEJSON_FIELD_STRING_ALLOC(vectis_embedded_manifest_asset, kind, "kind"),
    LONEJSON_FIELD_U64_PRESENT(vectis_embedded_manifest_asset, mode,
                               mode_present, "mode"),
    LONEJSON_FIELD_U64_PRESENT(vectis_embedded_manifest_asset, offset,
                               offset_present, "offset"),
    LONEJSON_FIELD_U64_PRESENT(vectis_embedded_manifest_asset, size,
                               size_present, "size"),
    LONEJSON_FIELD_STRING_ALLOC(vectis_embedded_manifest_asset, sha256,
                                "sha256"),
    LONEJSON_FIELD_STRING_ALLOC(vectis_embedded_manifest_asset, etag, "etag"),
    LONEJSON_FIELD_STRING_ALLOC(vectis_embedded_manifest_asset, content_type,
                                "content_type")};

LONEJSON_MAP_DEFINE(vectis_embedded_manifest_asset_map,
                    vectis_embedded_manifest_asset,
                    vectis_embedded_manifest_asset_fields);

static const lonejson_field vectis_embedded_manifest_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(vectis_embedded_manifest, format, "format"),
    LONEJSON_FIELD_STRING_ALLOC(vectis_embedded_manifest, extract_mode,
                                "extract_mode"),
    LONEJSON_FIELD_STRING_ALLOC(vectis_embedded_manifest, tree_sha256,
                                "tree_sha256"),
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

static char *vectis_embedded_etag_from_sha256(const char *sha256) {
  char *etag;
  size_t size;

  if (sha256 == NULL) {
    return NULL;
  }
  size = strlen(sha256) + 3u;
  etag = (char *)malloc(size);
  if (etag != NULL) {
    (void)snprintf(etag, size, "\"%s\"", sha256);
  }
  return etag;
}

static int vectis_embedded_etag_matches_sha256(const char *etag,
                                               const char *sha256) {
  size_t sha_size;

  if (etag == NULL || sha256 == NULL) {
    return 0;
  }
  sha_size = strlen(sha256);
  if (sha_size != SHA256_DIGEST_LENGTH * 2u || strlen(etag) != sha_size + 2u ||
      etag[0] != '"' || etag[sha_size + 1u] != '"') {
    return 0;
  }
  return memcmp(etag + 1u, sha256, sha_size) == 0;
}

static int
vectis_embedded_manifest_kind_parse(const char *kind,
                                    vectis_embedded_fs_entry_kind *out) {
  if (out == NULL) {
    return 0;
  }
  if (kind == NULL || strcmp(kind, "file") == 0) {
    *out = VECTIS_EMBEDDED_FS_ENTRY_FILE;
    return 1;
  }
  if (strcmp(kind, "directory") == 0) {
    *out = VECTIS_EMBEDDED_FS_ENTRY_DIRECTORY;
    return 1;
  }
  return 0;
}

static int vectis_embedded_mode_valid(vectis_embedded_fs_entry_kind kind,
                                      unsigned mode) {
  if (kind == VECTIS_EMBEDDED_FS_ENTRY_DIRECTORY) {
    return mode == 0555u;
  }
  return mode == 0444u || mode == 0555u;
}

static mode_t vectis_embedded_extract_mode(unsigned mode) {
  return (mode_t)(mode | 0200u);
}

static int vectis_embedded_path_valid(const char *path) {
  const char *segment;
  size_t segment_size;

  if (path == NULL || path[0] != '/' || path[1] == '\0' ||
      path[strlen(path) - 1u] == '/') {
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

static int vectis_embedded_sha256_hex_valid(const char *hex) {
  size_t i;

  if (hex == NULL || strlen(hex) != SHA256_DIGEST_LENGTH * 2u) {
    return 0;
  }
  for (i = 0u; i < SHA256_DIGEST_LENGTH * 2u; ++i) {
    if (vectis_embedded_hex_value(hex[i]) < 0) {
      return 0;
    }
  }
  return 1;
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
  vectis_embedded_fs_entry_kind kind;
  size_t offset;
  size_t size;
  unsigned mode;

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
  if (!vectis_embedded_manifest_kind_parse(asset->kind, &kind) ||
      (kind != VECTIS_EMBEDDED_FS_ENTRY_FILE &&
       kind != VECTIS_EMBEDDED_FS_ENTRY_DIRECTORY)) {
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "embedded asset kind is unsupported: %s",
                               asset->path);
    return VECTIS_ERR_INVALID;
  }
  if (asset->mode_present) {
    if (asset->mode > 0777u ||
        !vectis_embedded_mode_valid(kind, (unsigned)asset->mode)) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                                 "embedded asset mode is invalid: %s",
                                 asset->path);
      return VECTIS_ERR_INVALID;
    }
    mode = (unsigned)asset->mode;
  } else if (kind == VECTIS_EMBEDDED_FS_ENTRY_DIRECTORY) {
    mode = 0555u;
  } else {
    mode = 0444u;
  }
  offset = 0u;
  size = 0u;
  if (kind == VECTIS_EMBEDDED_FS_ENTRY_FILE) {
    if (!asset->offset_present || !asset->size_present ||
        asset->sha256 == NULL) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                                 "embedded file metadata is incomplete: %s",
                                 asset->path);
      return VECTIS_ERR_INVALID;
    }
    offset = (size_t)asset->offset;
    size = (size_t)asset->size;
    if (!vectis_embedded_sha256_matches(payload + offset, size,
                                        asset->sha256)) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                                 "embedded asset hash mismatch: %s",
                                 asset->path);
      return VECTIS_ERR_INVALID;
    }
    if (asset->etag != NULL &&
        !vectis_embedded_etag_matches_sha256(asset->etag, asset->sha256)) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                                 "embedded asset etag is invalid: %s",
                                 asset->path);
      return VECTIS_ERR_INVALID;
    }
  } else {
    if (asset->offset_present || asset->size_present || asset->sha256 != NULL ||
        asset->etag != NULL || asset->content_type != NULL) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                                 "embedded directory metadata is invalid: %s",
                                 asset->path);
      return VECTIS_ERR_INVALID;
    }
  }
  if (!vectis_embedded_reserve(impl, impl->count + 1u)) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to grow embedded asset index");
    return VECTIS_ERR_NOMEM;
  }
  entry = impl->entries + impl->count;
  memset(entry, 0, sizeof(*entry));
  entry->kind = kind;
  entry->path = vectis_embedded_strdup(asset->path);
  if (asset->sha256 != NULL) {
    entry->sha256 = vectis_embedded_strdup(asset->sha256);
  }
  if (kind == VECTIS_EMBEDDED_FS_ENTRY_FILE) {
    entry->etag = asset->etag != NULL
                      ? vectis_embedded_strdup(asset->etag)
                      : vectis_embedded_etag_from_sha256(asset->sha256);
  }
  if (asset->content_type != NULL) {
    entry->content_type = vectis_embedded_strdup(asset->content_type);
  }
  if (entry->path == NULL || (asset->sha256 != NULL && entry->sha256 == NULL) ||
      (kind == VECTIS_EMBEDDED_FS_ENTRY_FILE && entry->etag == NULL) ||
      (asset->content_type != NULL && entry->content_type == NULL)) {
    free(entry->path);
    free(entry->content_type);
    free(entry->sha256);
    free(entry->etag);
    memset(entry, 0, sizeof(*entry));
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy embedded asset metadata");
    return VECTIS_ERR_NOMEM;
  }
  if (kind == VECTIS_EMBEDDED_FS_ENTRY_FILE) {
    entry->data = payload + offset;
  }
  entry->size = size;
  entry->mode = mode;
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
    free(impl->entries[i].etag);
  }
  free(impl->entries);
  free(impl->index_path);
  free(impl->not_found_path);
  free(impl->tree_sha256);
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

static const char *
vectis_embedded_tree_sha256_impl(const vectis_embedded_fs *self) {
  const vectis_embedded_fs_impl *impl;

  if (self == NULL || self->impl == NULL) {
    return NULL;
  }
  impl = (const vectis_embedded_fs_impl *)self->impl;
  return impl->tree_sha256;
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
      out->kind = impl->entries[i].kind;
      out->path = impl->entries[i].path;
      out->content_type = impl->entries[i].content_type;
      out->sha256 = impl->entries[i].sha256;
      out->etag = impl->entries[i].etag;
      out->data = impl->entries[i].data;
      out->size = impl->entries[i].size;
      out->mode = impl->entries[i].mode;
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
  if (entry.kind != VECTIS_EMBEDDED_FS_ENTRY_FILE) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs read path is not a file");
    return VECTIS_ERR_INVALID;
  }
  out->data = entry.data;
  out->size = entry.size;
  return VECTIS_OK;
}

static vectis_status
vectis_embedded_open_source_impl(const vectis_embedded_fs *self,
                                 const char *path, int *found, lc_source **out,
                                 vectis_error *error) {
  vectis_embedded_fs_entry entry;
  lc_error lcerr;
  vectis_status status;
  int rc;

  if (found == NULL || out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs source output is required");
    return VECTIS_ERR_INVALID;
  }
  *found = 0;
  *out = NULL;
  memset(&entry, 0, sizeof(entry));
  status = vectis_embedded_lookup_impl(self, path, found, &entry, error);
  if (status != VECTIS_OK || !*found) {
    return status;
  }
  if (entry.kind != VECTIS_EMBEDDED_FS_ENTRY_FILE) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs source path is not a file");
    return VECTIS_ERR_INVALID;
  }
  lc_error_init(&lcerr);
  rc = lc_source_from_memory(entry.data, entry.size, out, &lcerr);
  if (rc != LC_OK) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     lcerr.message != NULL
                         ? lcerr.message
                         : "failed to open embedded fs memory source");
    lc_error_cleanup(&lcerr);
    return error != NULL ? error->code : VECTIS_ERR_STATE;
  }
  lc_error_cleanup(&lcerr);
  return VECTIS_OK;
}

static int vectis_embedded_path_prefixed_by(const char *path,
                                            const char *prefix) {
  size_t prefix_size;

  if (path == NULL || prefix == NULL) {
    return 0;
  }
  if (strcmp(prefix, "/") == 0) {
    return 1;
  }
  prefix_size = strlen(prefix);
  return strncmp(path, prefix, prefix_size) == 0 &&
         (path[prefix_size] == '\0' || path[prefix_size] == '/');
}

static vectis_status
vectis_embedded_list_impl(const vectis_embedded_fs *self, const char *prefix,
                          vectis_embedded_fs_list_fn callback, void *userdata,
                          vectis_error *error) {
  const vectis_embedded_fs_impl *impl;
  vectis_embedded_fs_entry entry;
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
  for (i = 0u; i < impl->count; ++i) {
    if (!vectis_embedded_path_prefixed_by(impl->entries[i].path, prefix)) {
      continue;
    }
    memset(&entry, 0, sizeof(entry));
    entry.kind = impl->entries[i].kind;
    entry.path = impl->entries[i].path;
    entry.content_type = impl->entries[i].content_type;
    entry.sha256 = impl->entries[i].sha256;
    entry.etag = impl->entries[i].etag;
    entry.data = impl->entries[i].data;
    entry.size = impl->entries[i].size;
    entry.mode = impl->entries[i].mode;
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
  if (entry.kind != VECTIS_EMBEDDED_FS_ENTRY_FILE) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs stream path is not a file");
    return VECTIS_ERR_INVALID;
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

static void vectis_embedded_fd_close(int *fd) {
  if (fd != NULL && *fd >= 0) {
    (void)close(*fd);
    *fd = -1;
  }
}

static int vectis_embedded_open_child_dir_at(int parent_fd, const char *name) {
  struct stat st;
  int fd;

  fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
    vectis_embedded_fd_close(&fd);
    errno = ENOTDIR;
    return -1;
  }
  return fd;
}

static vectis_status vectis_embedded_open_root_fd(const char *path,
                                                  int create_missing,
                                                  int *out_fd,
                                                  vectis_error *error) {
  char *copy;
  char *next_segment;
  char *segment;
  int current_fd;
  int next_fd;
  int saved_errno;

  if (path == NULL || path[0] == '\0' || out_fd == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded asset output directory is required");
    return VECTIS_ERR_INVALID;
  }
  *out_fd = -1;
  current_fd = open(path[0] == '/' ? "/" : ".",
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (current_fd < 0) {
    vectis_embedded_set_errorf(
        error, VECTIS_ERR_INVALID,
        "failed to open embedded asset output directory: "
        "%s",
        path);
    return VECTIS_ERR_INVALID;
  }
  copy = vectis_embedded_strdup(path);
  if (copy == NULL) {
    vectis_embedded_fd_close(&current_fd);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate embedded asset output directory");
    return VECTIS_ERR_NOMEM;
  }
  segment = copy;
  while (*segment == '/') {
    segment++;
  }
  while (*segment != '\0') {
    next_segment = strchr(segment, '/');
    if (next_segment != NULL) {
      *next_segment = '\0';
    }
    next_fd = vectis_embedded_open_child_dir_at(current_fd, segment);
    saved_errno = errno;
    if (next_fd < 0 && create_missing && saved_errno == ENOENT) {
      if (mkdirat(current_fd, segment, 0755) != 0 && errno != EEXIST) {
        vectis_embedded_fd_close(&current_fd);
        free(copy);
        vectis_embedded_set_errorf(
            error, VECTIS_ERR_INVALID,
            "failed to create embedded asset output directory: %s", path);
        return VECTIS_ERR_INVALID;
      }
      next_fd = vectis_embedded_open_child_dir_at(current_fd, segment);
      saved_errno = errno;
    }
    if (next_fd < 0) {
      vectis_embedded_fd_close(&current_fd);
      free(copy);
      if (saved_errno == ENOENT) {
        vectis_embedded_set_errorf(
            error, VECTIS_ERR_CONFLICT,
            "embedded asset output directory is missing: "
            "%s",
            path);
        return VECTIS_ERR_CONFLICT;
      }
      if (saved_errno == ELOOP || saved_errno == ENOTDIR) {
        vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                   "embedded asset output directory is unsafe: "
                                   "%s",
                                   path);
        return VECTIS_ERR_CONFLICT;
      }
      vectis_embedded_set_errorf(
          error, VECTIS_ERR_INVALID,
          "failed to open embedded asset output directory: "
          "%s",
          path);
      return VECTIS_ERR_INVALID;
    }
    vectis_embedded_fd_close(&current_fd);
    current_fd = next_fd;
    if (next_segment == NULL) {
      break;
    }
    segment = next_segment + 1u;
    while (*segment == '/') {
      segment++;
    }
  }
  free(copy);
  *out_fd = current_fd;
  return VECTIS_OK;
}

static vectis_status vectis_embedded_open_parent_fd(int root_fd,
                                                    const char *asset_path,
                                                    int create_missing,
                                                    int *out_parent_fd,
                                                    vectis_error *error) {
  char *parent_path;
  char *segment;
  char *next_segment;
  int current_fd;
  int next_fd;
  int saved_errno;

  if (root_fd < 0 || asset_path == NULL || asset_path[0] != '/' ||
      asset_path[1] == '\0' || out_parent_fd == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded asset parent path is required");
    return VECTIS_ERR_INVALID;
  }
  *out_parent_fd = -1;
  parent_path = vectis_embedded_strdup(asset_path + 1u);
  if (parent_path == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate embedded asset parent path");
    return VECTIS_ERR_NOMEM;
  }
  next_segment = strrchr(parent_path, '/');
  if (next_segment == NULL) {
    free(parent_path);
    current_fd = dup(root_fd);
    if (current_fd < 0) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "failed to open embedded asset output directory");
      return VECTIS_ERR_INVALID;
    }
    *out_parent_fd = current_fd;
    return VECTIS_OK;
  }
  *next_segment = '\0';
  current_fd = dup(root_fd);
  if (current_fd < 0) {
    free(parent_path);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "failed to open embedded asset output directory");
    return VECTIS_ERR_INVALID;
  }
  segment = parent_path;
  for (;;) {
    next_segment = strchr(segment, '/');
    if (next_segment != NULL) {
      *next_segment = '\0';
    }
    next_fd = vectis_embedded_open_child_dir_at(current_fd, segment);
    saved_errno = errno;
    if (next_fd < 0 && create_missing && saved_errno == ENOENT) {
      if (mkdirat(current_fd, segment, 0755) != 0 && errno != EEXIST) {
        vectis_embedded_fd_close(&current_fd);
        free(parent_path);
        vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                                   "failed to create embedded asset parent "
                                   "directory: %s",
                                   asset_path);
        return VECTIS_ERR_INVALID;
      }
      next_fd = vectis_embedded_open_child_dir_at(current_fd, segment);
      saved_errno = errno;
    }
    if (next_fd < 0) {
      vectis_embedded_fd_close(&current_fd);
      free(parent_path);
      if (saved_errno == ENOENT) {
        vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                   "embedded asset parent directory is "
                                   "missing: %s",
                                   asset_path);
        return VECTIS_ERR_CONFLICT;
      }
      if (saved_errno == ELOOP || saved_errno == ENOTDIR) {
        vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                   "embedded asset parent directory is "
                                   "unsafe: %s",
                                   asset_path);
        return VECTIS_ERR_CONFLICT;
      }
      vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                                 "failed to open embedded asset parent "
                                 "directory: %s",
                                 asset_path);
      return VECTIS_ERR_INVALID;
    }
    vectis_embedded_fd_close(&current_fd);
    current_fd = next_fd;
    if (next_segment == NULL) {
      break;
    }
    segment = next_segment + 1u;
  }
  free(parent_path);
  *out_parent_fd = current_fd;
  return VECTIS_OK;
}

static vectis_status
vectis_embedded_open_directory_at(int parent_fd, const char *leaf,
                                  const char *asset_path, int create_missing,
                                  int *out_fd, vectis_error *error) {
  int fd;
  int saved_errno;

  *out_fd = -1;
  fd = vectis_embedded_open_child_dir_at(parent_fd, leaf);
  saved_errno = errno;
  if (fd < 0 && create_missing && saved_errno == ENOENT) {
    if (mkdirat(parent_fd, leaf, 0755) != 0 && errno != EEXIST) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                                 "failed to create embedded asset directory: "
                                 "%s",
                                 asset_path);
      return VECTIS_ERR_INVALID;
    }
    fd = vectis_embedded_open_child_dir_at(parent_fd, leaf);
    saved_errno = errno;
  }
  if (fd < 0) {
    if (saved_errno == ENOENT) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                 "embedded asset directory is missing: %s",
                                 asset_path);
      return VECTIS_ERR_CONFLICT;
    }
    if (saved_errno == ELOOP || saved_errno == ENOTDIR) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                 "embedded asset directory is unsafe: %s",
                                 asset_path);
      return VECTIS_ERR_CONFLICT;
    }
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to open embedded asset directory: %s",
                               asset_path);
    return VECTIS_ERR_INVALID;
  }
  *out_fd = fd;
  return VECTIS_OK;
}

static vectis_status vectis_embedded_write_file_at(
    int parent_fd, const char *leaf, const char *asset_path,
    const vectis_embedded_fs_impl_entry *entry, vectis_error *error) {
  char *temporary;
  FILE *fp;
  int fd;
  int open_errno;
  int written;
  size_t temporary_size;

  temporary_size = strlen(leaf) + 64u;
  temporary = (char *)malloc(temporary_size);
  if (temporary == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate embedded asset temp path");
    return VECTIS_ERR_NOMEM;
  }
  written =
      snprintf(temporary, temporary_size, "%s.tmp.%ld", leaf, (long)getpid());
  if (written <= 0 || (size_t)written >= temporary_size) {
    free(temporary);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded asset temp path is too long");
    return VECTIS_ERR_INVALID;
  }
  fd = openat(parent_fd, temporary,
              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    open_errno = errno;
    if (open_errno == EEXIST) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                 "embedded asset temp path already exists: %s",
                                 asset_path);
    } else {
      vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                                 "failed to create embedded asset: %s",
                                 asset_path);
    }
    free(temporary);
    return open_errno == EEXIST ? VECTIS_ERR_CONFLICT : VECTIS_ERR_INVALID;
  }
  fp = fdopen(fd, "wb");
  if (fp == NULL) {
    (void)close(fd);
    (void)unlinkat(parent_fd, temporary, 0);
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to create embedded asset: %s",
                               asset_path);
    free(temporary);
    return VECTIS_ERR_INVALID;
  }
  if (entry->size > 0u &&
      fwrite(entry->data, 1u, entry->size, fp) != entry->size) {
    (void)fclose(fp);
    (void)unlinkat(parent_fd, temporary, 0);
    free(temporary);
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to write embedded asset: %s",
                               asset_path);
    return VECTIS_ERR_INVALID;
  }
  if (fchmod(fd, vectis_embedded_extract_mode(entry->mode)) != 0) {
    (void)fclose(fp);
    (void)unlinkat(parent_fd, temporary, 0);
    free(temporary);
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to set embedded asset mode: %s",
                               asset_path);
    return VECTIS_ERR_INVALID;
  }
  if (fclose(fp) != 0) {
    (void)unlinkat(parent_fd, temporary, 0);
    free(temporary);
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to write embedded asset: %s",
                               asset_path);
    return VECTIS_ERR_INVALID;
  }
  if (renameat(parent_fd, temporary, parent_fd, leaf) != 0) {
    (void)unlinkat(parent_fd, temporary, 0);
    free(temporary);
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to publish embedded asset: %s",
                               asset_path);
    return VECTIS_ERR_INVALID;
  }
  free(temporary);
  return VECTIS_OK;
}

static vectis_status vectis_embedded_file_matches_entry_at(
    int parent_fd, const char *leaf, const char *asset_path,
    const vectis_embedded_fs_impl_entry *entry, int *matches,
    vectis_error *error) {
  EVP_MD_CTX *ctx;
  struct stat st;
  unsigned char buffer[64u * 1024u];
  unsigned char digest[SHA256_DIGEST_LENGTH];
  char digest_hex[SHA256_DIGEST_LENGTH * 2u + 1u];
  unsigned int digest_size;
  ssize_t nread;
  int fd;
  int failed;

  if (matches == NULL || entry == NULL || entry->sha256 == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded asset verification input is required");
    return VECTIS_ERR_INVALID;
  }
  *matches = 0;
  fd = openat(parent_fd, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                 "embedded asset is missing: %s", asset_path);
      return VECTIS_ERR_CONFLICT;
    }
    if (errno == ELOOP || errno == ENOTDIR) {
      vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                 "embedded asset output is unsafe: %s",
                                 asset_path);
      return VECTIS_ERR_CONFLICT;
    }
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to open embedded asset for verify: %s",
                               asset_path);
    return VECTIS_ERR_INVALID;
  }
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    vectis_embedded_fd_close(&fd);
    vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                               "embedded asset output is not a file: %s",
                               asset_path);
    return VECTIS_ERR_CONFLICT;
  }
  ctx = EVP_MD_CTX_new();
  if (ctx == NULL) {
    vectis_embedded_fd_close(&fd);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate embedded asset verifier");
    return VECTIS_ERR_NOMEM;
  }
  nread = 0;
  failed = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1;
  while (!failed && (nread = read(fd, buffer, sizeof(buffer))) > 0) {
    if (EVP_DigestUpdate(ctx, buffer, (size_t)nread) != 1) {
      failed = 1;
    }
  }
  if (nread < 0) {
    failed = 1;
  }
  digest_size = 0u;
  if (!failed && (EVP_DigestFinal_ex(ctx, digest, &digest_size) != 1 ||
                  digest_size != SHA256_DIGEST_LENGTH)) {
    failed = 1;
  }
  EVP_MD_CTX_free(ctx);
  if (close(fd) != 0) {
    failed = 1;
  }
  if (failed) {
    vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                               "failed to verify embedded asset: %s",
                               asset_path);
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
  const char *leaf;
  struct stat st;
  size_t i;
  vectis_status status;
  int root_fd;
  int parent_fd;
  int directory_fd;
  int exists;
  int matches;
  int create_missing;

  if (self == NULL || self->impl == NULL || config == NULL ||
      config->output_dir == NULL || config->output_dir[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs extract output_dir is required");
    return VECTIS_ERR_INVALID;
  }
  create_missing = config->policy != VECTIS_EMBEDDED_FS_EXTRACT_VERIFY;
  root_fd = -1;
  status = vectis_embedded_open_root_fd(config->output_dir, create_missing,
                                        &root_fd, error);
  if (status != VECTIS_OK) {
    return status;
  }
  impl = (const vectis_embedded_fs_impl *)self->impl;
  status = VECTIS_OK;
  for (i = 0u; i < impl->count; ++i) {
    leaf = strrchr(impl->entries[i].path, '/') + 1u;
    parent_fd = -1;
    status = vectis_embedded_open_parent_fd(root_fd, impl->entries[i].path,
                                            create_missing, &parent_fd, error);
    if (status != VECTIS_OK) {
      break;
    }
    if (impl->entries[i].kind == VECTIS_EMBEDDED_FS_ENTRY_DIRECTORY) {
      directory_fd = -1;
      status = vectis_embedded_open_directory_at(
          parent_fd, leaf, impl->entries[i].path, create_missing, &directory_fd,
          error);
      if (status == VECTIS_OK &&
          config->policy != VECTIS_EMBEDDED_FS_EXTRACT_VERIFY &&
          fchmod(directory_fd,
                 vectis_embedded_extract_mode(impl->entries[i].mode)) != 0) {
        vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                                   "failed to set embedded directory mode: %s",
                                   impl->entries[i].path);
        status = VECTIS_ERR_INVALID;
      }
      vectis_embedded_fd_close(&directory_fd);
      vectis_embedded_fd_close(&parent_fd);
      if (status != VECTIS_OK) {
        break;
      }
      continue;
    }
    exists = fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0;
    if (!exists && errno != ENOENT) {
      vectis_embedded_fd_close(&parent_fd);
      vectis_embedded_set_errorf(error, VECTIS_ERR_INVALID,
                                 "failed to inspect embedded asset: %s",
                                 impl->entries[i].path);
      status = VECTIS_ERR_INVALID;
      break;
    }
    if (exists) {
      if (S_ISLNK(st.st_mode)) {
        vectis_embedded_fd_close(&parent_fd);
        vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                   "embedded asset output is a symlink: %s",
                                   impl->entries[i].path);
        status = VECTIS_ERR_CONFLICT;
        break;
      }
      if (!S_ISREG(st.st_mode)) {
        vectis_embedded_fd_close(&parent_fd);
        vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                   "embedded asset output is not a file: %s",
                                   impl->entries[i].path);
        status = VECTIS_ERR_CONFLICT;
        break;
      }
      if (config->policy == VECTIS_EMBEDDED_FS_EXTRACT_SKIP_EXISTING) {
        vectis_embedded_fd_close(&parent_fd);
        continue;
      }
      if (config->policy == VECTIS_EMBEDDED_FS_EXTRACT_VERIFY ||
          config->policy == VECTIS_EMBEDDED_FS_EXTRACT_REPAIR) {
        matches = 0;
        status = vectis_embedded_file_matches_entry_at(
            parent_fd, leaf, impl->entries[i].path, &impl->entries[i], &matches,
            error);
        if (status != VECTIS_OK) {
          vectis_embedded_fd_close(&parent_fd);
          break;
        }
        if (matches) {
          vectis_embedded_fd_close(&parent_fd);
          continue;
        }
        if (config->policy == VECTIS_EMBEDDED_FS_EXTRACT_VERIFY) {
          vectis_embedded_fd_close(&parent_fd);
          vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                     "embedded asset verification failed: %s",
                                     impl->entries[i].path);
          status = VECTIS_ERR_CONFLICT;
          break;
        }
      }
      if (config->policy != VECTIS_EMBEDDED_FS_EXTRACT_OVERWRITE &&
          config->policy != VECTIS_EMBEDDED_FS_EXTRACT_REPAIR) {
        vectis_embedded_fd_close(&parent_fd);
        vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                   "embedded asset already exists: %s",
                                   impl->entries[i].path);
        status = VECTIS_ERR_CONFLICT;
        break;
      }
    } else if (config->policy == VECTIS_EMBEDDED_FS_EXTRACT_VERIFY) {
      vectis_embedded_fd_close(&parent_fd);
      vectis_embedded_set_errorf(error, VECTIS_ERR_CONFLICT,
                                 "embedded asset is missing: %s",
                                 impl->entries[i].path);
      status = VECTIS_ERR_CONFLICT;
      break;
    }
    status = vectis_embedded_write_file_at(
        parent_fd, leaf, impl->entries[i].path, &impl->entries[i], error);
    vectis_embedded_fd_close(&parent_fd);
    if (status != VECTIS_OK) {
      break;
    }
  }
  vectis_embedded_fd_close(&root_fd);
  return status;
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
  size_t j;
  size_t previous_path_size;

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
  impl->api.tree_sha256 = vectis_embedded_tree_sha256_impl;
  impl->api.read = vectis_embedded_read_impl;
  impl->api.open_source = vectis_embedded_open_source_impl;
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
  lonejson_mapped_array_stream_init(&doc.assets);
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
  if (doc.tree_sha256 != NULL &&
      !vectis_embedded_sha256_hex_valid(doc.tree_sha256)) {
    lonejson_cleanup(&vectis_embedded_manifest_map, &doc);
    lonejson_cleanup(&vectis_embedded_manifest_asset_map, &item);
    lonejson_free(runtime);
    vectis_embedded_impl_destroy(impl);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs manifest tree_sha256 is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (doc.tree_sha256 != NULL) {
    impl->tree_sha256 = vectis_embedded_strdup(doc.tree_sha256);
    if (impl->tree_sha256 == NULL) {
      lonejson_cleanup(&vectis_embedded_manifest_map, &doc);
      lonejson_cleanup(&vectis_embedded_manifest_asset_map, &item);
      lonejson_free(runtime);
      vectis_embedded_impl_destroy(impl);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy embedded fs tree hash");
      return VECTIS_ERR_NOMEM;
    }
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
      for (j = 0u; j < i; ++j) {
        previous_path_size = strlen(impl->entries[j].path);
        if (impl->entries[j].kind == VECTIS_EMBEDDED_FS_ENTRY_FILE &&
            strncmp(impl->entries[i].path, impl->entries[j].path,
                    previous_path_size) == 0 &&
            impl->entries[i].path[previous_path_size] == '/') {
          lonejson_cleanup(&vectis_embedded_manifest_map, &doc);
          lonejson_cleanup(&vectis_embedded_manifest_asset_map, &item);
          lonejson_free(runtime);
          vectis_embedded_set_errorf(
              error, VECTIS_ERR_CONFLICT,
              "embedded file path conflicts with descendant asset: %s",
              impl->entries[j].path);
          vectis_embedded_impl_destroy(impl);
          return VECTIS_ERR_CONFLICT;
        }
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

const char *vectis_embedded_fs_tree_sha256(const vectis_embedded_fs *fs) {
  if (fs == NULL || fs->tree_sha256 == NULL) {
    return NULL;
  }
  return fs->tree_sha256(fs);
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

vectis_status vectis_embedded_fs_open_source(const vectis_embedded_fs *fs,
                                             const char *path, int *found,
                                             lc_source **out,
                                             vectis_error *error) {
  if (fs == NULL || fs->open_source == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "embedded fs source handle is required");
    return VECTIS_ERR_INVALID;
  }
  return fs->open_source(fs, path, found, out, error);
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
