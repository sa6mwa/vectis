#ifndef VECTIS_EMBEDDED_FS_H
#define VECTIS_EMBEDDED_FS_H

#include <stddef.h>
#include <vectis/vectis.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vectis_embedded_fs vectis_embedded_fs;

/* Extraction behavior when an embedded asset already exists on disk. */
typedef enum vectis_embedded_fs_extract_policy {
  VECTIS_EMBEDDED_FS_EXTRACT_FAIL_EXISTS = 0,
  VECTIS_EMBEDDED_FS_EXTRACT_SKIP_EXISTING = 1,
  VECTIS_EMBEDDED_FS_EXTRACT_OVERWRITE = 2
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

/* Borrowed view of one embedded file. Pointers remain valid while the owning
 * embedded fs handle remains open.
 */
typedef struct vectis_embedded_fs_entry {
  const char *path;
  const char *content_type;
  const char *sha256;
  const void *data;
  size_t size;
} vectis_embedded_fs_entry;

/* Extraction target. The output directory is created when needed. */
typedef struct vectis_embedded_fs_extract_config {
  const char *output_dir;
  vectis_embedded_fs_extract_policy policy;
} vectis_embedded_fs_extract_config;

typedef vectis_status (*vectis_embedded_fs_list_fn)(
    const vectis_embedded_fs_entry *entry, void *userdata, vectis_error *error);

struct vectis_embedded_fs {
  /* Lookup a file by absolute embedded path. "/" resolves to index_path. */
  vectis_status (*lookup)(const vectis_embedded_fs *self, const char *path,
                          int *found, vectis_embedded_fs_entry *out,
                          vectis_error *error);
  /* Return borrowed bytes for one embedded file. Missing files set found=0. */
  vectis_status (*read)(const vectis_embedded_fs *self, const char *path,
                        int *found, vectis_bytes *out, vectis_error *error);
  /* Visit files whose absolute embedded path starts with prefix. */
  vectis_status (*list)(const vectis_embedded_fs *self, const char *prefix,
                        vectis_embedded_fs_list_fn callback, void *userdata,
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
/* Parse a Vectis pack manifest and return an owned receiver-shell handle. */
vectis_status
vectis_embedded_fs_from_pack(const vectis_embedded_fs_config *config,
                             vectis_embedded_fs **out, vectis_error *error);
/* Free-function wrapper for fs->lookup. */
vectis_status vectis_embedded_fs_lookup(const vectis_embedded_fs *fs,
                                        const char *path, int *found,
                                        vectis_embedded_fs_entry *out,
                                        vectis_error *error);
/* Free-function wrapper for fs->read. */
vectis_status vectis_embedded_fs_read(const vectis_embedded_fs *fs,
                                      const char *path, int *found,
                                      vectis_bytes *out, vectis_error *error);
/* Free-function wrapper for fs->list. */
vectis_status vectis_embedded_fs_list(const vectis_embedded_fs *fs,
                                      const char *prefix,
                                      vectis_embedded_fs_list_fn callback,
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
