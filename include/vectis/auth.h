#ifndef VECTIS_AUTH_H
#define VECTIS_AUTH_H

#include <stddef.h>
#include <vectis/vectis.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VECTIS_AUTH_DEFAULT_MAX_STORE_BYTES (1024u * 1024u)
#define VECTIS_AUTH_PRINCIPAL_MAX 254u
#define VECTIS_AUTH_CHALLENGE_MAX 255u

typedef enum vectis_auth_mode {
  VECTIS_AUTH_MODE_DEFAULT = 0,
  VECTIS_AUTH_MODE_BASIC = 1u << 0,
  VECTIS_AUTH_MODE_BEARER = 1u << 1
} vectis_auth_mode;

typedef struct vectis_auth_store_config {
  /* JSON credential store path. Callers choose the config directory and file.
   */
  const char *credentials_path;
  /* Maximum store bytes accepted when reading. Zero uses the default. */
  size_t max_store_bytes;
} vectis_auth_store_config;

typedef struct vectis_auth_issue_config {
  /* Authenticated principal/user identifier embedded into the claim JSON. */
  const char *subject;
  /* Credential purpose embedded into the claim JSON, for example "webdav". */
  const char *purpose;
  /* VECTIS_AUTH_MODE_* flags to issue. Defaults to Bearer. */
  unsigned auth_modes;
  /* Optional lonejson record-size cap. Zero uses the lonejson default. */
  size_t max_record_bytes;
} vectis_auth_issue_config;

typedef struct vectis_auth_issued_credential {
  /* Owned strings. Release with vectis_auth_issued_credential_cleanup(). */
  char *client_id;
  char *client_secret;
  char *api_key;
  char *claim_json;
} vectis_auth_issued_credential;

typedef struct vectis_auth_result {
  /* False for missing, malformed, revoked, or non-matching credentials. */
  int authenticated;
  unsigned auth_mode;
  /* Owned strings when authenticated. Release with
   * vectis_auth_result_cleanup(). */
  char *client_id;
  char *claim_json;
} vectis_auth_result;

typedef enum vectis_auth_action {
  VECTIS_AUTH_DENY = 0,
  VECTIS_AUTH_ALLOW = 1,
  VECTIS_AUTH_REQUIRED = 2,
  VECTIS_AUTH_REDIRECT = 3
} vectis_auth_action;

typedef struct vectis_auth_provider_request {
  vectis_request *request;
  const char *authorization;
  const char *purpose;
  const char *resource;
  unsigned allowed_auth_modes;
} vectis_auth_provider_request;

typedef struct vectis_auth_provider_response {
  vectis_auth_action action;
  int status_code;
  const char *location;
  const char *content_type;
  const void *body;
  size_t body_size;
  char www_authenticate[VECTIS_AUTH_CHALLENGE_MAX + 1u];
  char principal[VECTIS_AUTH_PRINCIPAL_MAX + 1u];
  vectis_auth_result result;
} vectis_auth_provider_response;

typedef vectis_status (*vectis_auth_provider_fn)(
    const vectis_auth_provider_request *request,
    vectis_auth_provider_response *response, void *userdata,
    vectis_error *error);

typedef struct vectis_auth_provider {
  vectis_auth_provider_fn authenticate;
  void *userdata;
} vectis_auth_provider;

typedef struct vectis_auth_native_provider_config {
  vectis_auth_store_config store;
  const char *purpose;
  const char *realm;
  unsigned allowed_auth_modes;
} vectis_auth_native_provider_config;

void vectis_auth_store_config_init(vectis_auth_store_config *config);
void vectis_auth_issue_config_init(vectis_auth_issue_config *config);
void vectis_auth_issued_credential_init(
    vectis_auth_issued_credential *credential);
void vectis_auth_issued_credential_cleanup(
    vectis_auth_issued_credential *credential);
void vectis_auth_result_init(vectis_auth_result *result);
void vectis_auth_result_cleanup(vectis_auth_result *result);
void vectis_auth_provider_request_init(vectis_auth_provider_request *request);
void vectis_auth_provider_response_init(
    vectis_auth_provider_response *response);
void vectis_auth_provider_response_cleanup(
    vectis_auth_provider_response *response);
void vectis_auth_provider_init(vectis_auth_provider *provider);
void vectis_auth_native_provider_config_init(
    vectis_auth_native_provider_config *config);

vectis_status vectis_auth_store_init(const vectis_auth_store_config *config,
                                     vectis_error *error);
/* Issues a lonejson-backed Basic and/or Bearer credential, appends the hashed
 * record to the JSON store under a file lock, and returns one-time secrets.
 */
vectis_status
vectis_auth_issue_credential(const vectis_auth_store_config *store_config,
                             const vectis_auth_issue_config *issue_config,
                             vectis_auth_issued_credential *out,
                             vectis_error *error);
/* Verifies a raw HTTP Authorization header. Rejected credentials return
 * VECTIS_OK with out->authenticated == 0; malformed stores/runtime failures
 * return an error status.
 */
vectis_status vectis_auth_verify_authorization(
    const vectis_auth_store_config *store_config, const char *authorization,
    unsigned allowed_auth_modes, vectis_auth_result *out, vectis_error *error);
/* Idempotently removes a client_id from the JSON credentials array. */
vectis_status
vectis_auth_revoke_client(const vectis_auth_store_config *store_config,
                          const char *client_id, vectis_error *error);

vectis_status
vectis_auth_provider_from_callback(vectis_auth_provider *provider,
                                   vectis_auth_provider_fn authenticate,
                                   void *userdata, vectis_error *error);
vectis_status vectis_auth_provider_from_native_store(
    vectis_auth_provider *provider, vectis_auth_native_provider_config *config,
    vectis_error *error);
vectis_status
vectis_auth_provider_authenticate(const vectis_auth_provider *provider,
                                  const vectis_auth_provider_request *request,
                                  vectis_auth_provider_response *response,
                                  vectis_error *error);
vectis_status vectis_auth_provider_response_set_authenticated(
    vectis_auth_provider_response *response, const char *principal,
    const char *client_id, const char *claim_json, unsigned auth_mode,
    vectis_error *error);

#ifdef __cplusplus
}
#endif

#endif
