#ifndef VECTIS_WEBDAV_H
#define VECTIS_WEBDAV_H

#include <stddef.h>
#include <time.h>
#include <vectis/auth.h>
#include <vectis/embedded_fs.h>
#include <vectis/vectis.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VECTIS_WEBDAV_PATH_MAX 1024u
#define VECTIS_WEBDAV_STORAGE_PATH_MAX 4096u
#define VECTIS_WEBDAV_PRINCIPAL_MAX 254u
#define VECTIS_WEBDAV_ETAG_LENGTH 64u

typedef enum vectis_webdav_status {
  VECTIS_WEBDAV_OK = 0,
  VECTIS_WEBDAV_NOT_FOUND = 1,
  VECTIS_WEBDAV_EXISTS = 2,
  VECTIS_WEBDAV_INVALID = 3,
  VECTIS_WEBDAV_LIMIT = 4,
  VECTIS_WEBDAV_IO = 5,
  VECTIS_WEBDAV_NOMEM = 6,
  VECTIS_WEBDAV_CONFLICT = 7,
  VECTIS_WEBDAV_TOMBSTONED = 8
} vectis_webdav_status;

typedef enum vectis_webdav_entry_kind {
  VECTIS_WEBDAV_ENTRY_NONE = 0,
  VECTIS_WEBDAV_ENTRY_FILE = 1,
  VECTIS_WEBDAV_ENTRY_TOMBSTONE = 2,
  VECTIS_WEBDAV_ENTRY_COLLECTION = 3
} vectis_webdav_entry_kind;

typedef struct vectis_webdav_config {
  const char *cache_dir;
  const char *site_id;
  /* Optional direct mutable disk docroot. When set, WebDAV content operations
     read/write root_dir directly while cache_dir/site_id still provide
     metadata such as locks and transaction scratch space. */
  const char *root_dir;
  size_t max_file_bytes;
  size_t max_total_bytes;
  size_t max_resources;
} vectis_webdav_config;

typedef struct vectis_webdav_entry {
  vectis_webdav_entry_kind kind;
  size_t size;
  char storage_path[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char etag[VECTIS_WEBDAV_ETAG_LENGTH + 1u];
} vectis_webdav_entry;

typedef int (*vectis_webdav_list_callback)(const char *path,
                                           vectis_webdav_entry_kind kind,
                                           size_t size, void *userdata);

/*
 * WebDAV auth is adapter-owned. Protected mounts call a registered adapter
 * callback, either application-provided or vectis_webdav_auth_provider() backed
 * by a vectis_auth_provider. Vectis-issued WebDAV app keys are verified by the
 * native auth provider; the WebDAV mount only depends on this callback
 * contract.
 */
typedef enum vectis_webdav_auth_action {
  /* Stop the WebDAV operation. Defaults to 404 when conceal_unauthorized is
     set. */
  VECTIS_WEBDAV_AUTH_DENY = 0,
  /* Continue the WebDAV operation for the authenticated principal. */
  VECTIS_WEBDAV_AUTH_ALLOW = 1,
  /* Return an authentication challenge/contract response such as 401. */
  VECTIS_WEBDAV_AUTH_REQUIRED = 2,
  /* Redirect the client to an adapter-defined auth endpoint/flow. */
  VECTIS_WEBDAV_AUTH_REDIRECT = 3
} vectis_webdav_auth_action;

typedef struct vectis_webdav_auth_request {
  vectis_request *request;
  vectis_http_method method;
  const char *mount_path_prefix;
  const char *resource_path;
} vectis_webdav_auth_request;

typedef struct vectis_webdav_auth_response {
  vectis_webdav_auth_action action;
  int status_code;
  /* Borrowed adapter-owned response fields. Vectis does not free them. */
  const char *location;
  const char *www_authenticate;
  const char *content_type;
  const void *body;
  size_t body_size;
  char www_authenticate_value[VECTIS_AUTH_CHALLENGE_MAX + 1u];
  char principal[VECTIS_WEBDAV_PRINCIPAL_MAX + 1u];
} vectis_webdav_auth_response;

typedef vectis_status (*vectis_webdav_auth_fn)(
    const vectis_webdav_auth_request *request,
    vectis_webdav_auth_response *response, void *userdata, vectis_error *error);

struct vectis_webdav_mount_config {
  const char *path_prefix;
  vectis_webdav_config storage;
  /* Protected mounts fail registration unless auth is set. */
  int auth_required;
  int conceal_unauthorized;
  vectis_webdav_auth_fn auth;
  void *auth_userdata;
};

/*
 * Extract an embedded filesystem into the WebDAV mutable content tree, then
 * register a normal WebDAV mount over that storage. The init helper defaults to
 * VECTIS_EMBEDDED_FS_EXTRACT_SKIP_EXISTING so user-edited files survive
 * restart; set extract_policy to OVERWRITE for repair/restore behavior.
 */
struct vectis_webdav_embedded_site_config {
  const char *path_prefix;
  vectis_webdav_config storage;
  const vectis_embedded_fs *fs;
  vectis_embedded_fs_extract_policy extract_policy;
  int auth_required;
  int conceal_unauthorized;
  vectis_webdav_auth_fn auth;
  void *auth_userdata;
};

/* Read-only WebDAV mount over a borrowed embedded filesystem. Unlike
 * vectis_webdav_embedded_site_config this does not extract or create mutable
 * overlay storage; mutating WebDAV methods return 405. */
struct vectis_webdav_embedded_mount_config {
  const char *path_prefix;
  const vectis_embedded_fs *fs;
  const char *content_type;
  const char *cache_control;
  int auth_required;
  int conceal_unauthorized;
  vectis_webdav_auth_fn auth;
  void *auth_userdata;
};

typedef struct vectis_webdav_auth_provider_config {
  const vectis_auth_provider *provider;
  const char *purpose;
  unsigned allowed_auth_modes;
} vectis_webdav_auth_provider_config;

void vectis_webdav_config_init(vectis_webdav_config *config);
int vectis_webdav_path_normalize(const char *path,
                                 char out[VECTIS_WEBDAV_PATH_MAX + 1u]);
const char *vectis_webdav_status_string(vectis_webdav_status status);
vectis_status
vectis_webdav_content_dir(const vectis_webdav_config *config,
                          char out[VECTIS_WEBDAV_STORAGE_PATH_MAX],
                          vectis_error *error);

vectis_webdav_status vectis_webdav_lookup(const vectis_webdav_config *config,
                                          const char *path,
                                          vectis_webdav_entry *entry);
vectis_webdav_status vectis_webdav_read(const vectis_webdav_config *config,
                                        const char *path, unsigned char **body,
                                        size_t *body_size,
                                        vectis_webdav_entry *entry);
vectis_webdav_status vectis_webdav_put(const vectis_webdav_config *config,
                                       const char *path,
                                       const unsigned char *body,
                                       size_t body_size);
vectis_webdav_status vectis_webdav_delete(const vectis_webdav_config *config,
                                          const char *path);
vectis_webdav_status vectis_webdav_mkcol(const vectis_webdav_config *config,
                                         const char *path);
vectis_webdav_status vectis_webdav_copy(const vectis_webdav_config *config,
                                        const char *source,
                                        const char *destination, int overwrite);
vectis_webdav_status vectis_webdav_move(const vectis_webdav_config *config,
                                        const char *source,
                                        const char *destination, int overwrite);
vectis_webdav_status vectis_webdav_list(const vectis_webdav_config *config,
                                        const char *path,
                                        vectis_webdav_list_callback callback,
                                        void *userdata);

void vectis_webdav_auth_response_init(vectis_webdav_auth_response *response);
void vectis_webdav_mount_config_init(vectis_webdav_mount_config *config);
void vectis_webdav_embedded_site_config_init(
    vectis_webdav_embedded_site_config *config);
void vectis_webdav_embedded_mount_config_init(
    vectis_webdav_embedded_mount_config *config);
void vectis_webdav_auth_provider_config_init(
    vectis_webdav_auth_provider_config *config);
vectis_status
vectis_webdav_auth_provider(const vectis_webdav_auth_request *request,
                            vectis_webdav_auth_response *response,
                            void *userdata, vectis_error *error);
vectis_status vectis_register_webdav(vectis_app *app,
                                     const vectis_webdav_mount_config *config,
                                     vectis_error *error);
vectis_status vectis_register_webdav_site(vectis_app *app,
                                          const char *path_prefix,
                                          const vectis_webdav_config *storage,
                                          vectis_error *error);
vectis_status vectis_register_webdav_embedded_site(
    vectis_app *app, const vectis_webdav_embedded_site_config *config,
    vectis_error *error);
vectis_status vectis_register_webdav_embedded(
    vectis_app *app, const vectis_webdav_embedded_mount_config *config,
    vectis_error *error);

#ifdef __cplusplus
}
#endif

#endif
