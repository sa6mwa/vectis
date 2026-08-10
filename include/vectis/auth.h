#ifndef VECTIS_AUTH_H
#define VECTIS_AUTH_H

#include <stddef.h>
#include <stdint.h>
#include <vectis/vectis.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VECTIS_AUTH_DEFAULT_MAX_STORE_BYTES (1024u * 1024u)
#define VECTIS_AUTH_PRINCIPAL_MAX 254u
#define VECTIS_AUTH_CHALLENGE_MAX 255u
#define VECTIS_AUTH_GENERATED_PASSWORD_MAX 64u

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
  /* Optional OAuth2/OIDC token-flow id embedded into the claim JSON. */
  const char *oauth2_flow_id;
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
  /* Borrowed raw HTTP Authorization header, when the caller already has it. */
  const char *authorization;
  const char *purpose;
  const char *resource;
  unsigned allowed_auth_modes;
} vectis_auth_provider_request;

typedef struct vectis_auth_provider_response {
  vectis_auth_action action;
  int status_code;
  /* Borrowed adapter-owned response fields. Vectis does not free them. */
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

/*
 * Native browser/login route set. Registers:
 *   GET  <path_prefix>/login
 *   POST <path_prefix>/webdav-key
 * The POST endpoint accepts application/x-www-form-urlencoded fields
 * username, password, and optional totp_code, then issues a WebDAV Basic app
 * key through the native credentials store.
 */
struct vectis_auth_routes_config {
  const char *path_prefix;
  vectis_auth_store_config store;
  const char *realm;
  const char *login_title;
  const char *login_template_html;
  size_t max_body_bytes;
  /* Zero uses current time. Non-zero supports deterministic TOTP checks. */
  uint64_t unix_seconds;
  /* Zero uses the login default. */
  unsigned int totp_window;
};

typedef struct vectis_auth_user_config {
  /* Stable local username/principal. */
  const char *username;
  /* Optional password. Empty or NULL generates one and returns it in out. */
  const char *password;
  /* Enables TOTP enrollment for this user. */
  int enable_totp;
  /* Optional existing base32 TOTP secret. Empty or NULL generates one. */
  const char *totp_secret;
  /* Optional enrollment label/issuer used for otpauth URI and QR rendering. */
  const char *totp_label;
  const char *totp_issuer;
} vectis_auth_user_config;

typedef struct vectis_auth_user_enrollment {
  /* Owned strings. Release with vectis_auth_user_enrollment_cleanup(). */
  char *username;
  char *generated_password;
  char *totp_secret;
  char *totp_uri;
  char *totp_qr_ansi;
} vectis_auth_user_enrollment;

typedef struct vectis_auth_login_config {
  const char *username;
  const char *password;
  /* Required when the stored user has TOTP enabled. */
  const char *totp_code;
  /* Zero uses current time. Non-zero supports deterministic verification. */
  uint64_t unix_seconds;
  /* Defaults to 1 time-step. */
  unsigned int totp_window;
} vectis_auth_login_config;

typedef struct vectis_auth_oauth2_http_request {
  const char *method;
  const char *url;
  const char *content_type;
  const char *authorization;
  const char *user_agent;
  const void *body;
  size_t body_size;
  size_t max_response_bytes;
} vectis_auth_oauth2_http_request;

typedef struct vectis_auth_oauth2_http_response {
  /* Owned strings/body. Release with
   * vectis_auth_oauth2_http_response_cleanup(). */
  long status_code;
  char *content_type;
  void *body;
  size_t body_size;
} vectis_auth_oauth2_http_response;

typedef vectis_status (*vectis_auth_oauth2_http_fn)(
    const vectis_auth_oauth2_http_request *request,
    vectis_auth_oauth2_http_response *response, void *userdata,
    vectis_error *error);

typedef struct vectis_auth_oauth2_transport_config {
  /* Optional Vectis HTTP client config. NULL uses Vectis HTTP defaults. */
  const vectis_http_client_config *http_client;
  /* Optional test/application transport callback. When set, it takes
   * precedence over http_client.
   */
  vectis_auth_oauth2_http_fn request;
  void *request_userdata;
  const char *user_agent;
} vectis_auth_oauth2_transport_config;

typedef struct vectis_auth_oauth2_client_credentials_config {
  vectis_auth_oauth2_transport_config transport;
  const char *token_endpoint;
  const char *client_id;
  const char *client_secret;
  const char *scope;
  const char *audience;
  const char *resource;
  size_t max_response_bytes;
  size_t max_body_bytes;
} vectis_auth_oauth2_client_credentials_config;

typedef struct vectis_auth_oauth2_token_response {
  /* Owned token strings. Release with
   * vectis_auth_oauth2_token_response_cleanup(). */
  char *access_token;
  char *token_type;
  char *refresh_token;
  char *scope;
  char *id_token;
  int64_t expires_in;
  int has_expires_in;
} vectis_auth_oauth2_token_response;

typedef enum vectis_auth_oauth2_token_flow_state {
  VECTIS_AUTH_OAUTH2_TOKEN_FLOW_READY = 0,
  VECTIS_AUTH_OAUTH2_TOKEN_FLOW_REFRESHED = 1,
  VECTIS_AUTH_OAUTH2_TOKEN_FLOW_NEEDS_INTERACTION = 2,
  VECTIS_AUTH_OAUTH2_TOKEN_FLOW_FAILED = 3
} vectis_auth_oauth2_token_flow_state;

typedef struct vectis_auth_oauth2_token_flow {
  /* Owned token strings. Release with vectis_auth_oauth2_token_flow_cleanup().
   */
  char *access_token;
  char *token_type;
  char *refresh_token;
  char *scope;
  char *id_token;
  int64_t expires_at;
  int has_expires_at;
} vectis_auth_oauth2_token_flow;

typedef struct vectis_auth_oauth2_token_flow_policy {
  vectis_auth_oauth2_transport_config transport;
  const char *token_endpoint;
  const char *client_id;
  const char *client_secret;
  const char *scope;
  int64_t now;
  int64_t refresh_skew_seconds;
  size_t max_response_bytes;
  unsigned max_retries;
  int disable_refresh;
  int disable_retry;
} vectis_auth_oauth2_token_flow_policy;

typedef struct vectis_auth_oauth2_token_flow_result {
  vectis_auth_oauth2_token_flow_state state;
  unsigned attempts;
  int refreshed;
} vectis_auth_oauth2_token_flow_result;

typedef struct vectis_auth_oauth2_stored_token_flow {
  /* Owned strings and nested token flow. Release with
   * vectis_auth_oauth2_stored_token_flow_cleanup().
   */
  char *flow_id;
  char *subject;
  char *webdav_client_id;
  vectis_auth_oauth2_token_flow flow;
  int found;
} vectis_auth_oauth2_stored_token_flow;

typedef struct vectis_auth_oauth2_token_flow_store_config {
  vectis_auth_store_config store;
  const char *flow_id;
  const char *subject;
  const char *webdav_client_id;
  vectis_auth_oauth2_token_flow flow;
} vectis_auth_oauth2_token_flow_store_config;

typedef struct vectis_auth_oauth2_stored_token_flow_policy {
  vectis_auth_store_config store;
  const char *flow_id;
  vectis_auth_oauth2_token_flow_policy flow_policy;
  int revoke_webdav_keys_on_failure;
} vectis_auth_oauth2_stored_token_flow_policy;

typedef struct vectis_auth_oauth2_webdav_key_config {
  vectis_auth_store_config store;
  const char *flow_id;
  const char *subject;
  size_t max_record_bytes;
} vectis_auth_oauth2_webdav_key_config;

typedef struct vectis_auth_oidc_authorization_config {
  const char *authorization_endpoint;
  const char *client_id;
  const char *redirect_uri;
  const char *scope;
  const char *state;
  const char *nonce;
  const char *code_verifier;
  const char *code_challenge;
  const char *audience;
  const char *resource;
  size_t verifier_bytes;
  size_t max_url_bytes;
} vectis_auth_oidc_authorization_config;

typedef struct vectis_auth_oidc_authorization {
  /* Owned strings. Release with vectis_auth_oidc_authorization_cleanup(). */
  char *authorization_url;
  char *code_verifier;
  char *code_challenge;
  char *state;
  char *nonce;
} vectis_auth_oidc_authorization;

typedef struct vectis_auth_oidc_token_exchange_config {
  vectis_auth_oauth2_transport_config transport;
  const char *token_endpoint;
  const char *client_id;
  const char *client_secret;
  const char *redirect_uri;
  const char *code_verifier;
  const char *callback_query;
  const char *expected_state;
  int64_t now;
  size_t max_query_bytes;
  size_t max_response_bytes;
  size_t max_body_bytes;
} vectis_auth_oidc_token_exchange_config;

typedef struct vectis_auth_oidc_token_exchange {
  /* Owned strings and nested OAuth2 outputs. Release with
   * vectis_auth_oidc_token_exchange_cleanup().
   */
  char *code;
  char *state;
  vectis_auth_oauth2_token_response token;
  vectis_auth_oauth2_token_flow flow;
} vectis_auth_oidc_token_exchange;

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
void vectis_auth_routes_config_init(vectis_auth_routes_config *config);
void vectis_auth_user_config_init(vectis_auth_user_config *config);
void vectis_auth_user_enrollment_init(vectis_auth_user_enrollment *enrollment);
void vectis_auth_user_enrollment_cleanup(
    vectis_auth_user_enrollment *enrollment);
void vectis_auth_login_config_init(vectis_auth_login_config *config);
void vectis_auth_oauth2_http_response_init(
    vectis_auth_oauth2_http_response *response);
void vectis_auth_oauth2_http_response_cleanup(
    vectis_auth_oauth2_http_response *response);
void vectis_auth_oauth2_transport_config_init(
    vectis_auth_oauth2_transport_config *config);
void vectis_auth_oauth2_client_credentials_config_init(
    vectis_auth_oauth2_client_credentials_config *config);
void vectis_auth_oauth2_token_response_init(
    vectis_auth_oauth2_token_response *response);
void vectis_auth_oauth2_token_response_cleanup(
    vectis_auth_oauth2_token_response *response);
void vectis_auth_oauth2_token_flow_init(vectis_auth_oauth2_token_flow *flow);
void vectis_auth_oauth2_token_flow_cleanup(vectis_auth_oauth2_token_flow *flow);
void vectis_auth_oauth2_token_flow_policy_init(
    vectis_auth_oauth2_token_flow_policy *policy);
void vectis_auth_oauth2_token_flow_result_init(
    vectis_auth_oauth2_token_flow_result *result);
void vectis_auth_oauth2_stored_token_flow_init(
    vectis_auth_oauth2_stored_token_flow *flow);
void vectis_auth_oauth2_stored_token_flow_cleanup(
    vectis_auth_oauth2_stored_token_flow *flow);
void vectis_auth_oauth2_token_flow_store_config_init(
    vectis_auth_oauth2_token_flow_store_config *config);
void vectis_auth_oauth2_stored_token_flow_policy_init(
    vectis_auth_oauth2_stored_token_flow_policy *policy);
void vectis_auth_oauth2_webdav_key_config_init(
    vectis_auth_oauth2_webdav_key_config *config);
void vectis_auth_oidc_authorization_config_init(
    vectis_auth_oidc_authorization_config *config);
void vectis_auth_oidc_authorization_init(
    vectis_auth_oidc_authorization *authorization);
void vectis_auth_oidc_authorization_cleanup(
    vectis_auth_oidc_authorization *authorization);
void vectis_auth_oidc_token_exchange_config_init(
    vectis_auth_oidc_token_exchange_config *config);
void vectis_auth_oidc_token_exchange_init(
    vectis_auth_oidc_token_exchange *exchange);
void vectis_auth_oidc_token_exchange_cleanup(
    vectis_auth_oidc_token_exchange *exchange);

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
vectis_auth_user_add_or_update(const vectis_auth_store_config *store_config,
                               const vectis_auth_user_config *user_config,
                               vectis_auth_user_enrollment *out,
                               vectis_error *error);
vectis_status
vectis_auth_user_login(const vectis_auth_store_config *store_config,
                       const vectis_auth_login_config *login_config,
                       vectis_auth_result *out, vectis_error *error);
vectis_status vectis_auth_issue_webdav_key_for_login(
    const vectis_auth_store_config *store_config,
    const vectis_auth_login_config *login_config,
    vectis_auth_issued_credential *out, vectis_error *error);
vectis_status vectis_auth_oauth2_client_credentials_request(
    const vectis_auth_oauth2_client_credentials_config *config,
    vectis_auth_oauth2_token_response *out, vectis_error *error);
vectis_status vectis_auth_oauth2_token_flow_ensure(
    vectis_auth_oauth2_token_flow *flow,
    const vectis_auth_oauth2_token_flow_policy *policy,
    vectis_auth_oauth2_token_flow_result *result, vectis_error *error);
vectis_status vectis_auth_oauth2_token_flow_upsert(
    const vectis_auth_oauth2_token_flow_store_config *config,
    vectis_error *error);
vectis_status vectis_auth_oauth2_token_flow_load(
    const vectis_auth_store_config *store_config, const char *flow_id,
    vectis_auth_oauth2_stored_token_flow *out, vectis_error *error);
vectis_status vectis_auth_oauth2_stored_token_flow_ensure(
    const vectis_auth_oauth2_stored_token_flow_policy *policy,
    vectis_auth_oauth2_stored_token_flow *out,
    vectis_auth_oauth2_token_flow_result *result, vectis_error *error);
vectis_status vectis_auth_issue_webdav_key_for_oauth2_flow(
    const vectis_auth_oauth2_webdav_key_config *config,
    vectis_auth_issued_credential *out, vectis_error *error);
vectis_status vectis_auth_oidc_authorization_start(
    const vectis_auth_oidc_authorization_config *config,
    vectis_auth_oidc_authorization *out, vectis_error *error);
vectis_status vectis_auth_oidc_exchange_callback(
    const vectis_auth_oidc_token_exchange_config *config,
    vectis_auth_oidc_token_exchange *out, vectis_error *error);

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
vectis_status
vectis_register_auth_routes(vectis_app *app,
                            const vectis_auth_routes_config *config,
                            vectis_error *error);

#ifdef __cplusplus
}
#endif

#endif
