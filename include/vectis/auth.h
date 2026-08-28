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
#define VECTIS_AUTH_EMAIL_MAX 319u
#define VECTIS_AUTH_CHALLENGE_MAX 255u
#define VECTIS_AUTH_GENERATED_PASSWORD_MAX 64u
#define VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_TTL_SECONDS 300u
#define VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_MAX_ATTEMPTS 5u
#define VECTIS_AUTH_PENDING_LOGIN_DEFAULT_TTL_SECONDS 300u
#define VECTIS_AUTH_BROWSER_SESSION_DEFAULT_TTL_SECONDS (30u * 24u * 60u * 60u)

typedef enum vectis_auth_mode {
  VECTIS_AUTH_MODE_DEFAULT = 0,
  VECTIS_AUTH_MODE_BASIC = 1u << 0,
  VECTIS_AUTH_MODE_BEARER = 1u << 1,
  /* Server-side browser session authenticated through a Vectis cookie. */
  VECTIS_AUTH_MODE_BROWSER_SESSION = 1u << 2
} vectis_auth_mode;

typedef enum vectis_auth_browser_session_mode {
  /* Preserve M2M-only authentication and never issue or accept cookies. */
  VECTIS_AUTH_BROWSER_SESSION_M2M_ONLY = 0,
  /* Accept the configured cookie and issue it only to browser navigation. */
  VECTIS_AUTH_BROWSER_SESSION_M2M_AND_BROWSER = 1
} vectis_auth_browser_session_mode;

/*
 * Browser sessions are owned by libvectis. The signing key and per-session
 * records are persisted through the configured app Lockd client; Lua and C
 * callers configure their scope but never manage the signing key.
 */
typedef struct vectis_auth_browser_session_config {
  vectis_auth_browser_session_mode mode;
  /* Cookie name. Zero/default is "vectis_session". */
  const char *cookie_name;
  /* Cookie Path attribute. Zero/default is "/". */
  const char *cookie_path;
  /* Session audience. Zero/default is "browser". */
  const char *purpose;
  /* Lockd key prefix. Zero/default is "auth.browser_session.v1". */
  const char *state_key;
  /* Zero uses VECTIS_AUTH_BROWSER_SESSION_DEFAULT_TTL_SECONDS. */
  uint64_t ttl_seconds;
} vectis_auth_browser_session_config;

typedef struct vectis_auth_browser_session_result {
  /* Nonzero only when the opaque cookie resolved to an active session. */
  int authenticated;
  /* Borrow-free principal and audience copied from Lockd-backed state. */
  char principal[VECTIS_AUTH_PRINCIPAL_MAX + 1u];
  char purpose[128];
  /* Unix expiry time; zero when unauthenticated. */
  uint64_t expires_at;
} vectis_auth_browser_session_result;

typedef struct vectis_auth_store_config {
  /* JSON credential store path. Callers choose the config directory and file.
   */
  const char *credentials_path;
  /*
   * Optional JSON auth state path for high-churn pending_login and email_token
   * records. When NULL or empty, transient auth state is stored in
   * credentials_path for single-file deployments.
   */
  const char *state_path;
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
  /* Borrowed raw HTTP Cookie header, when the caller already has it. */
  const char *cookie;
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
  /* Required only when browser_session.mode is M2M_AND_BROWSER. */
  vectis_app *app;
  vectis_auth_browser_session_config browser_session;
  const char *purpose;
  const char *realm;
  unsigned allowed_auth_modes;
} vectis_auth_native_provider_config;

typedef struct vectis_auth_email_message {
  const char *username;
  const char *realm;
  const char *email;
  const char *transaction_id;
  const char *token;
  uint64_t expires_at;
} vectis_auth_email_message;

typedef struct vectis_auth_smtp_config {
  const char *url;
  const char *mail_from;
  const char *username;
  const char *password;
  const char *subject;
  const char *ca_bundle_path;
  long timeout_ms;
  long connect_timeout_ms;
  int use_ssl;
  int tls_verify_peer_disabled;
  int tls_verify_host_disabled;
  const char *allowed_recipient_domain;
  const char *const *allowed_recipients;
  size_t allowed_recipient_count;
  vectis_curl_configure_fn configure_curl;
  void *configure_curl_userdata;
} vectis_auth_smtp_config;

#define VECTIS_AUTH_ROUTE_FACTOR_PASSWORD 0x01u
#define VECTIS_AUTH_ROUTE_FACTOR_EMAIL_TOKEN 0x02u
#define VECTIS_AUTH_ROUTE_FACTOR_TOTP 0x04u

/*
 * Native browser/login route set. Registers:
 *   GET  <path_prefix>/login
 *   POST <path_prefix>/login
 *   POST <path_prefix>/email-token
 *   POST <path_prefix>/continue
 *   POST <path_prefix>/webdav-key
 *   POST <path_prefix>/logout
 * The email-token endpoint accepts application/x-www-form-urlencoded fields
 * username and email. The email must exactly match that user's enrolled
 * recipient; it cannot select a delivery address.
 * The endpoint returns transaction data unless SMTP delivery is configured.
 * The login, continue, and webdav-key POST endpoints accept username,
 * password, optional totp_code, and when require_email_token is set,
 * email_transaction_id plus email_token. They then issue a WebDAV Basic app
 * key through the native credentials store. If password is valid but required
 * TOTP or email-token factors are missing, they return a short-lived
 * pending_transaction_id. A later POST may provide username,
 * pending_transaction_id, and the remaining factor fields without resending
 * the password.
 * The logout endpoint verifies the presented Authorization header and revokes
 * that client_id from the native credentials store.
 */
struct vectis_auth_routes_config {
  const char *path_prefix;
  vectis_auth_store_config store;
  const char *realm;
  const char *login_title;
  /*
   * Login form template sources. Set at most one. login_template_html is an
   * inline HTML string; login_template_path is read from the local filesystem
   * during registration; login_template_embedded_path is read from
   * login_template_fs during registration. Custom login templates support
   * HTML-escaped substitutions for {{login_title}}, {{realm}},
   * {{path_prefix}}, {{email_token_action}}, {{continue_action}}, and
   * {{webdav_key_action}}.
   */
  const char *login_template_html;
  const char *login_template_path;
  const char *login_template_embedded_path;
  const vectis_embedded_fs *login_template_fs;
  size_t max_body_bytes;
  /* Zero uses current time. Non-zero supports deterministic TOTP checks. */
  uint64_t unix_seconds;
  /* Zero uses the login default. */
  unsigned int totp_window;
  /*
   * WebDAV-key factor policy. Zero preserves the default password factor.
   * The password factor validates the password and any enrolled TOTP for the
   * user. The TOTP factor makes TOTP enrollment mandatory for the user and
   * must be combined with the password factor. The email-token factor requires
   * a verified email token transaction.
   */
  unsigned int required_factors;
  /* Require email_transaction_id and email_token before issuing WebDAV keys. */
  int require_email_token;
  /* Zero uses VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_TTL_SECONDS. */
  uint64_t email_token_ttl_seconds;
  /* Zero uses VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_MAX_ATTEMPTS. */
  unsigned int email_token_max_attempts;
  /* Zero uses VECTIS_AUTH_PENDING_LOGIN_DEFAULT_TTL_SECONDS. */
  uint64_t pending_login_ttl_seconds;
  /* Optional browser-session policy. The default remains M2M-only. */
  vectis_auth_browser_session_config browser_session;
  /*
   * Optional SMTP delivery for email-token issuance. When email_smtp.url is
   * set, <path_prefix>/email-token sends the token through SMTP and omits it
   * from the HTTP response.
   */
  vectis_auth_smtp_config email_smtp;
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

typedef struct vectis_auth_password_check_config {
  vectis_auth_store_config store;
  const char *username;
  const char *password;
} vectis_auth_password_check_config;

typedef struct vectis_auth_password_check_result {
  int authenticated;
  int totp_required;
} vectis_auth_password_check_result;

typedef struct vectis_auth_pending_login_issue_config {
  vectis_auth_store_config store;
  const char *username;
  const char *password;
  const char *realm;
  /* Optional deterministic transaction id for tests or external flows. */
  const char *transaction_id;
  uint64_t now_seconds;
  uint64_t ttl_seconds;
} vectis_auth_pending_login_issue_config;

typedef struct vectis_auth_pending_login {
  int authenticated;
  int totp_required;
  /* Owned strings when authenticated. Release with cleanup. */
  char *transaction_id;
  char *username;
  char *realm;
  uint64_t expires_at;
} vectis_auth_pending_login;

typedef struct vectis_auth_pending_login_consume_config {
  vectis_auth_store_config store;
  const char *transaction_id;
  const char *username;
  const char *realm;
  const char *totp_code;
  uint64_t now_seconds;
  unsigned int totp_window;
} vectis_auth_pending_login_consume_config;

typedef struct vectis_auth_pending_login_result {
  int authenticated;
  int expired;
  int totp_required;
  /* Owned strings when the transaction is found. Release with cleanup. */
  char *username;
  char *realm;
} vectis_auth_pending_login_result;

typedef struct vectis_auth_email_token_issue_config {
  vectis_auth_store_config store;
  const char *username;
  const char *realm;
  /* Required. Must exactly match the user's enrolled email recipient. */
  const char *email;
  /* Optional pending auth transaction this email token is bound to. */
  const char *pending_transaction_id;
  /* Zero uses VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_MAX_ATTEMPTS. */
  unsigned int max_attempts;
  /* Optional deterministic values for tests or externally created flows. */
  const char *transaction_id;
  const char *token;
  uint64_t now_seconds;
  uint64_t ttl_seconds;
} vectis_auth_email_token_issue_config;

typedef struct vectis_auth_email_token {
  /* Owned strings. Release with vectis_auth_email_token_cleanup(). */
  char *transaction_id;
  char *token;
  uint64_t expires_at;
} vectis_auth_email_token;

typedef struct vectis_auth_email_token_verify_config {
  vectis_auth_store_config store;
  const char *transaction_id;
  const char *username;
  const char *realm;
  /* When set, the token must be bound to this pending auth transaction. */
  const char *pending_transaction_id;
  const char *token;
  uint64_t now_seconds;
} vectis_auth_email_token_verify_config;

typedef struct vectis_auth_email_token_result {
  int verified;
  int expired;
  /* Owned strings when the transaction is found. Release with
   * vectis_auth_email_token_result_cleanup(). */
  char *username;
  char *realm;
  char *email;
  char *pending_transaction_id;
  unsigned int failed_attempts;
  unsigned int max_attempts;
} vectis_auth_email_token_result;

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
void vectis_auth_password_check_config_init(
    vectis_auth_password_check_config *config);
void vectis_auth_password_check_result_init(
    vectis_auth_password_check_result *result);
void vectis_auth_pending_login_issue_config_init(
    vectis_auth_pending_login_issue_config *config);
void vectis_auth_pending_login_init(vectis_auth_pending_login *pending);
void vectis_auth_pending_login_cleanup(vectis_auth_pending_login *pending);
void vectis_auth_pending_login_consume_config_init(
    vectis_auth_pending_login_consume_config *config);
void vectis_auth_pending_login_result_init(
    vectis_auth_pending_login_result *result);
void vectis_auth_pending_login_result_cleanup(
    vectis_auth_pending_login_result *result);
void vectis_auth_email_token_issue_config_init(
    vectis_auth_email_token_issue_config *config);
void vectis_auth_email_token_init(vectis_auth_email_token *token);
void vectis_auth_email_token_cleanup(vectis_auth_email_token *token);
void vectis_auth_email_token_verify_config_init(
    vectis_auth_email_token_verify_config *config);
void vectis_auth_email_token_result_init(
    vectis_auth_email_token_result *result);
void vectis_auth_email_token_result_cleanup(
    vectis_auth_email_token_result *result);
void vectis_auth_smtp_config_init(vectis_auth_smtp_config *config);
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

/*
 * Formats an HTTP Basic Authorization header for an issued Basic credential.
 * `out` receives an owned NUL-terminated byte buffer containing
 * "Basic <base64(client_id:client_secret)>"; release it with
 * vectis_mutable_bytes_cleanup(). On failure, `out` is left empty.
 */
vectis_status vectis_auth_basic_authorization(const char *client_id,
                                              const char *client_secret,
                                              vectis_mutable_bytes *out,
                                              vectis_error *error);
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
/* Validates an email-token enrollment recipient. An email must be non-empty
 * and no longer than VECTIS_AUTH_EMAIL_MAX. */
vectis_status vectis_auth_user_email_validate(const char *email,
                                              vectis_error *error);
/* Sets the enrolled recipient for email-token authentication. The username
 * must already exist; email must be non-empty and no longer than
 * VECTIS_AUTH_EMAIL_MAX. */
vectis_status
vectis_auth_user_email_set(const vectis_auth_store_config *store_config,
                           const char *username, const char *email,
                           vectis_error *error);
/* Checks whether a username is present in the credentials store. */
vectis_status
vectis_auth_user_exists(const vectis_auth_store_config *store_config,
                        const char *username, int *out_exists,
                        vectis_error *error);
vectis_status
vectis_auth_user_login(const vectis_auth_store_config *store_config,
                       const vectis_auth_login_config *login_config,
                       vectis_auth_result *out, vectis_error *error);
vectis_status
vectis_auth_user_password_check(const vectis_auth_password_check_config *config,
                                vectis_auth_password_check_result *out,
                                vectis_error *error);
vectis_status vectis_auth_pending_login_issue(
    const vectis_auth_pending_login_issue_config *config,
    vectis_auth_pending_login *out, vectis_error *error);
/* Verifies a pending login without removing it from the store. Use this before
 * checking later factors that may fail independently, then call consume after
 * every required factor has passed.
 */
vectis_status vectis_auth_pending_login_verify(
    const vectis_auth_pending_login_consume_config *config,
    vectis_auth_pending_login_result *out, vectis_error *error);
/* Verifies and removes terminal pending-login records from the store. A
 * matching accepted, expired, or wrong-TOTP transaction is single-use.
 */
vectis_status vectis_auth_pending_login_consume(
    const vectis_auth_pending_login_consume_config *config,
    vectis_auth_pending_login_result *out, vectis_error *error);
vectis_status vectis_auth_email_token_issue(
    const vectis_auth_email_token_issue_config *config,
    vectis_auth_email_token *out, vectis_error *error);
vectis_status vectis_auth_email_token_verify(
    const vectis_auth_email_token_verify_config *config,
    vectis_auth_email_token_result *out, vectis_error *error);
vectis_status
vectis_auth_email_token_deliver_smtp(const vectis_auth_smtp_config *config,
                                     const vectis_auth_email_message *message,
                                     vectis_error *error);
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

/* Initializes browser-session defaults; the default mode is M2M-only. */
void vectis_auth_browser_session_config_init(
    vectis_auth_browser_session_config *config);
/* Validates the declarative policy without creating keys or sessions. */
vectis_status vectis_auth_browser_session_config_validate(
    const vectis_auth_browser_session_config *config, vectis_error *error);
/* Clears a result to its unauthenticated state. */
void vectis_auth_browser_session_result_init(
    vectis_auth_browser_session_result *result);
/*
 * Validates the configured cookie in cookie_header against Lockd-backed
 * server-side state. Missing, malformed, expired, revoked, and purpose-
 * mismatched cookies produce an unauthenticated result, not an error.
 */
vectis_status vectis_auth_browser_session_verify(
    vectis_app *app, const vectis_auth_browser_session_config *config,
    const char *cookie_header, uint64_t unix_seconds,
    vectis_auth_browser_session_result *out, vectis_error *error);
/*
 * Creates an opaque browser session and appends its HttpOnly, Secure,
 * SameSite=Strict cookie to response. The secret and server-side state remain
 * owned by libvectis.
 */
vectis_status vectis_auth_browser_session_issue(
    vectis_app *app, const vectis_auth_browser_session_config *config,
    const char *principal, uint64_t unix_seconds, vectis_response *response,
    vectis_error *error);
/* Revokes the session named by cookie_header and clears its cookie on response.
 */
vectis_status vectis_auth_browser_session_revoke(
    vectis_app *app, const vectis_auth_browser_session_config *config,
    const char *cookie_header, vectis_response *response, vectis_error *error);

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
