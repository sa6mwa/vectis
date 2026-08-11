#ifndef VECTIS_EMBEDDED_FS_H
#define VECTIS_EMBEDDED_FS_H

#include <stddef.h>
#include <vectis/vectis.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extraction behavior for embedded assets:
 * fail_exists writes only missing files and fails on existing files;
 * skip_existing writes missing files and preserves existing files;
 * overwrite writes every embedded file;
 * verify writes nothing and requires every embedded file to exist and match;
 * repair writes missing or mismatched embedded files and preserves unrelated
 * files.
 */
typedef enum vectis_embedded_fs_extract_policy {
  VECTIS_EMBEDDED_FS_EXTRACT_FAIL_EXISTS = 0,
  VECTIS_EMBEDDED_FS_EXTRACT_SKIP_EXISTING = 1,
  VECTIS_EMBEDDED_FS_EXTRACT_OVERWRITE = 2,
  VECTIS_EMBEDDED_FS_EXTRACT_VERIFY = 3,
  VECTIS_EMBEDDED_FS_EXTRACT_REPAIR = 4
} vectis_embedded_fs_extract_policy;

/*
 * Borrowed packed-asset input. `manifest_json` and `payload` must remain valid
 * until the returned embedded fs handle is closed. NULL index_path defaults to
 * "/index.html"; NULL not_found_path disables fallback lookup.
 */
typedef struct vectis_embedded_fs_config {
  const void *manifest_json;
  size_t manifest_json_size;
  const void *payload;
  size_t payload_size;
  const char *index_path;
  const char *not_found_path;
} vectis_embedded_fs_config;

/* Embedded entry kind. Current packed assets are files; directory metadata is
 * reserved for explicit directory entries in a future manifest extension.
 */
typedef enum vectis_embedded_fs_entry_kind {
  VECTIS_EMBEDDED_FS_ENTRY_FILE = 1,
  VECTIS_EMBEDDED_FS_ENTRY_DIRECTORY = 2
} vectis_embedded_fs_entry_kind;

/* Borrowed view of one embedded file. Pointers remain valid while the owning
 * embedded fs handle remains open.
 */
typedef struct vectis_embedded_fs_entry {
  vectis_embedded_fs_entry_kind kind;
  const char *path;
  const char *content_type;
  const char *sha256;
  const char *etag;
  const void *data;
  size_t size;
  unsigned mode;
} vectis_embedded_fs_entry;

/* Extraction target. The output directory is created when needed. */
typedef struct vectis_embedded_fs_extract_config {
  const char *output_dir;
  vectis_embedded_fs_extract_policy policy;
} vectis_embedded_fs_extract_config;

typedef vectis_status (*vectis_embedded_fs_list_fn)(
    const vectis_embedded_fs_entry *entry, void *userdata, vectis_error *error);
typedef vectis_status (*vectis_embedded_fs_chunk_fn)(const void *data,
                                                     size_t size,
                                                     void *userdata,
                                                     vectis_error *error);

struct vectis_embedded_fs {
  /* Return the manifest-recorded default extraction policy. */
  vectis_embedded_fs_extract_policy (*default_extract_policy)(
      const vectis_embedded_fs *self);
  /* Return the manifest-recorded aggregate asset tree SHA-256 hex digest, or
   * NULL when the manifest does not provide one.
   */
  const char *(*tree_sha256)(const vectis_embedded_fs *self);
  /* Lookup a file by absolute embedded path. "/" resolves to index_path. */
  vectis_status (*lookup)(const vectis_embedded_fs *self, const char *path,
                          int *found, vectis_embedded_fs_entry *out,
                          vectis_error *error);
  /* Return borrowed bytes for one embedded file. Missing files set found=0. */
  vectis_status (*read)(const vectis_embedded_fs *self, const char *path,
                        int *found, vectis_bytes *out, vectis_error *error);
  /* Open an owned rewindable source for one embedded file. Missing files set
   * found=0 and leave *out NULL. The caller closes the returned source with
   * lc_source_close().
   */
  vectis_status (*open_source)(const vectis_embedded_fs *self, const char *path,
                               int *found, struct lc_source **out,
                               vectis_error *error);
  /* Visit files at prefix or under prefix as a path segment boundary. */
  vectis_status (*list)(const vectis_embedded_fs *self, const char *prefix,
                        vectis_embedded_fs_list_fn callback, void *userdata,
                        vectis_error *error);
  /* Visit one embedded file in bounded chunks. chunk_size=0 selects a default.
   */
  vectis_status (*stream)(const vectis_embedded_fs *self, const char *path,
                          size_t chunk_size, int *found,
                          vectis_embedded_fs_chunk_fn callback, void *userdata,
                          vectis_error *error);
  /* Extract every embedded file under config->output_dir. */
  vectis_status (*extract)(const vectis_embedded_fs *self,
                           const vectis_embedded_fs_extract_config *config,
                           vectis_error *error);
  /* Close the handle and copied index metadata. Borrowed payload is not freed.
   */
  void (*close)(vectis_embedded_fs *self);
  void *impl;
};

/* Initialize config defaults. */
void vectis_embedded_fs_config_init(vectis_embedded_fs_config *config);
/* Initialize extraction defaults. */
void vectis_embedded_fs_extract_config_init(
    vectis_embedded_fs_extract_config *config);
/* Return the canonical string for an extraction policy. Invalid values return
 * "unknown".
 */
const char *vectis_embedded_fs_extract_policy_string(
    vectis_embedded_fs_extract_policy policy);
/* Parse an extraction policy string. Accepts canonical underscore spellings and
 * CLI-friendly hyphen aliases. Returns non-zero on success.
 */
int vectis_embedded_fs_extract_policy_parse(
    const char *value, vectis_embedded_fs_extract_policy *out);
/* Parse a Vectis pack manifest and return an owned receiver-shell handle. */
vectis_status
vectis_embedded_fs_from_pack(const vectis_embedded_fs_config *config,
                             vectis_embedded_fs **out, vectis_error *error);
/* Free-function wrapper for fs->default_extract_policy. NULL fs returns
 * fail_exists.
 */
vectis_embedded_fs_extract_policy
vectis_embedded_fs_default_extract_policy(const vectis_embedded_fs *fs);
/* Free-function wrapper for fs->tree_sha256. NULL fs returns NULL. */
const char *vectis_embedded_fs_tree_sha256(const vectis_embedded_fs *fs);
/* Free-function wrapper for fs->lookup. */
vectis_status vectis_embedded_fs_lookup(const vectis_embedded_fs *fs,
                                        const char *path, int *found,
                                        vectis_embedded_fs_entry *out,
                                        vectis_error *error);
/* Free-function wrapper for fs->read. */
vectis_status vectis_embedded_fs_read(const vectis_embedded_fs *fs,
                                      const char *path, int *found,
                                      vectis_bytes *out, vectis_error *error);
/* Free-function wrapper for fs->open_source. */
vectis_status vectis_embedded_fs_open_source(const vectis_embedded_fs *fs,
                                             const char *path, int *found,
                                             struct lc_source **out,
                                             vectis_error *error);
/* Free-function wrapper for fs->list. */
vectis_status vectis_embedded_fs_list(const vectis_embedded_fs *fs,
                                      const char *prefix,
                                      vectis_embedded_fs_list_fn callback,
                                      void *userdata, vectis_error *error);
/* Free-function wrapper for fs->stream. */
vectis_status vectis_embedded_fs_stream(const vectis_embedded_fs *fs,
                                        const char *path, size_t chunk_size,
                                        int *found,
                                        vectis_embedded_fs_chunk_fn callback,
                                        void *userdata, vectis_error *error);
/* Free-function wrapper for fs->extract. */
vectis_status
vectis_embedded_fs_extract(const vectis_embedded_fs *fs,
                           const vectis_embedded_fs_extract_config *config,
                           vectis_error *error);
/* Close an embedded fs handle. Accepts NULL. */
void vectis_embedded_fs_close(vectis_embedded_fs *fs);

#ifdef __cplusplus
}
#endif

#endif
