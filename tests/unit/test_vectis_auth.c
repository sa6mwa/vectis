#include "vectis_internal.h"

#include <vectis/auth.h>
#include <vectis/totp_qr.h>
#include <vectis/webdav.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;

static char *test_strdup(const char *value) {
  char *copy;
  size_t len;

  if (value == NULL) {
    return NULL;
  }
  len = strlen(value) + 1u;
  copy = (char *)malloc(len);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, len);
  return copy;
}

static void expect(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "failure: %s\n", message);
    failures++;
  }
}

static void remove_tree(const char *path) {
  DIR *directory;
  struct dirent *item;
  struct stat st;
  char child[4096];
  int written;

  directory = opendir(path);
  if (directory == NULL) {
    return;
  }
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
      continue;
    }
    written = snprintf(child, sizeof(child), "%s/%s", path, item->d_name);
    if (written < 0 || (size_t)written >= sizeof(child) ||
        lstat(child, &st) == -1) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      remove_tree(child);
    } else {
      (void)unlink(child);
    }
  }
  (void)closedir(directory);
  (void)rmdir(path);
}

static char base64_digit(unsigned value) {
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  return table[value & 63u];
}

static int base64_encode(const char *input, char *out, size_t out_size) {
  size_t input_len;
  size_t offset;
  size_t written;
  unsigned a;
  unsigned b;
  unsigned c;

  input_len = strlen(input);
  written = 0u;
  for (offset = 0u; offset < input_len; offset += 3u) {
    if (written + 4u >= out_size) {
      return 0;
    }
    a = (unsigned)(unsigned char)input[offset];
    b = offset + 1u < input_len ? (unsigned)(unsigned char)input[offset + 1u]
                                : 0u;
    c = offset + 2u < input_len ? (unsigned)(unsigned char)input[offset + 2u]
                                : 0u;
    out[written++] = base64_digit(a >> 2u);
    out[written++] = base64_digit(((a & 3u) << 4u) | (b >> 4u));
    out[written++] = offset + 1u < input_len
                         ? base64_digit(((b & 15u) << 2u) | (c >> 6u))
                         : '=';
    out[written++] = offset + 2u < input_len ? base64_digit(c) : '=';
  }
  out[written] = '\0';
  return 1;
}

static void expect_ok(vectis_status status, const vectis_error *error,
                      const char *message) {
  if (status != VECTIS_OK) {
    fprintf(stderr, "failure: %s: %s\n", message,
            error != NULL ? error->message : "unknown error");
    failures++;
  }
}

static vectis_status
sample_auth_provider(const vectis_auth_provider_request *request,
                     vectis_auth_provider_response *response, void *userdata,
                     vectis_error *error) {
  const char *mode;

  mode = (const char *)userdata;
  if (request == NULL || response == NULL || mode == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "sample auth provider input is required");
    return VECTIS_ERR_INVALID;
  }
  if (strcmp(mode, "required") == 0) {
    vectis_auth_provider_response_cleanup(response);
    response->action = VECTIS_AUTH_REQUIRED;
    response->status_code = 401;
    (void)snprintf(response->www_authenticate,
                   sizeof(response->www_authenticate), "Basic realm=\"unit\"");
    return VECTIS_OK;
  }
  if (strcmp(mode, "redirect") == 0) {
    vectis_auth_provider_response_cleanup(response);
    response->action = VECTIS_AUTH_REDIRECT;
    response->status_code = 303;
    response->location = "/auth/login?next=/dav/docs";
    response->content_type = "text/plain";
    response->body = "login required";
    response->body_size = strlen((const char *)response->body);
    return VECTIS_OK;
  }
  if (strcmp(mode, "header") == 0) {
    expect(request->authorization != NULL &&
               strcmp(request->authorization, "Bearer webdav-token") == 0,
           "provider receives forwarded authorization header");
  }
  expect(request->purpose != NULL && strcmp(request->purpose, "webdav") == 0,
         "provider receives purpose");
  expect(request->resource != NULL && strcmp(request->resource, "/docs") == 0,
         "provider receives resource");
  return vectis_auth_provider_response_set_authenticated(
      response, "unit-user", "client-1", "{\"sub\":\"unit-user\"}",
      VECTIS_AUTH_MODE_BASIC, error);
}

static vectis_status
oauth2_mock_transport(const vectis_auth_oauth2_http_request *request,
                      vectis_auth_oauth2_http_response *response,
                      void *userdata, vectis_error *error) {
  const char *mode;
  const char *body;
  const char *json;

  mode = (const char *)userdata;
  if (request == NULL || response == NULL || request->body == NULL ||
      mode == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "OAuth2 mock transport input is required");
    return VECTIS_ERR_INVALID;
  }
  body = (const char *)request->body;
  expect(strcmp(request->method, "POST") == 0,
         "OAuth2 transport receives POST");
  expect(strcmp(request->url, "https://idp.example.test/token") == 0,
         "OAuth2 transport receives token endpoint");
  expect(request->content_type != NULL &&
             strcmp(request->content_type,
                    "application/x-www-form-urlencoded") == 0,
         "OAuth2 transport receives form content type");
  if (strcmp(mode, "fail") == 0) {
    expect(strstr(body, "grant_type=refresh_token") != NULL,
           "OAuth2 failed refresh body carries grant type");
    vectis_set_error(error, VECTIS_ERR_INVALID, "mock OAuth2 refresh failed");
    return VECTIS_ERR_INVALID;
  }
  if (strcmp(mode, "code") == 0) {
    expect(strstr(body, "grant_type=authorization_code") != NULL,
           "OIDC code exchange body carries grant type");
    expect(strstr(body, "code=auth-code") != NULL,
           "OIDC code exchange body carries code");
    expect(strstr(body, "client_id=vectis-client") != NULL,
           "OIDC code exchange body carries client id");
    expect(strstr(body, "code_verifier=") != NULL,
           "OIDC code exchange body carries PKCE verifier");
    json = "{\"access_token\":\"browser-token\",\"token_type\":\"Bearer\","
           "\"refresh_token\":\"browser-refresh\",\"scope\":\"openid dav\","
           "\"id_token\":\"id-token\",\"expires_in\":4200}";
  } else if (strcmp(mode, "client") == 0) {
    expect(strstr(body, "grant_type=client_credentials") != NULL,
           "OAuth2 client credentials body carries grant type");
    expect(strstr(body, "client_id=vectis-client") != NULL,
           "OAuth2 client credentials body carries client id");
    expect(strstr(body, "client_secret=vectis-secret") != NULL,
           "OAuth2 client credentials body carries client secret");
    json = "{\"access_token\":\"m2m-token\",\"token_type\":\"Bearer\","
           "\"refresh_token\":\"m2m-refresh\",\"scope\":\"dav\","
           "\"expires_in\":3600}";
  } else {
    expect(strstr(body, "grant_type=refresh_token") != NULL,
           "OAuth2 refresh body carries grant type");
    expect(strstr(body, "refresh_token=old-refresh") != NULL,
           "OAuth2 refresh body carries refresh token");
    json = "{\"access_token\":\"refreshed-token\",\"token_type\":\"Bearer\","
           "\"refresh_token\":\"new-refresh\",\"scope\":\"dav\","
           "\"expires_in\":7200}";
  }
  response->status_code = 200;
  response->content_type = test_strdup("application/json");
  response->body = test_strdup(json);
  response->body_size = strlen(json);
  if (response->content_type == NULL || response->body == NULL) {
    vectis_auth_oauth2_http_response_cleanup(response);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate OAuth2 mock response");
    return VECTIS_ERR_NOMEM;
  }
  return VECTIS_OK;
}

int main(void) {
  char temp[] = "/tmp/vectis-auth-unit.XXXXXX";
  char credentials_path[4096];
  char bearer_header[1024];
  char basic_clear[1024];
  char basic_token[1400];
  char basic_header[1500];
  char login_basic_clear[1024];
  char login_basic_token[1400];
  char login_basic_header[1500];
  char oauth_basic_clear[1024];
  char oauth_basic_token[1400];
  char oauth_basic_header[1500];
  char totp_code[VECTIS_TOTP_CODE_LENGTH + 1u];
  int written;
  vectis_auth_store_config store;
  vectis_auth_user_config user;
  vectis_auth_user_enrollment enrollment;
  vectis_auth_login_config login;
  vectis_auth_oauth2_client_credentials_config oauth2_client;
  vectis_auth_oauth2_token_response token_response;
  vectis_auth_oauth2_token_flow token_flow;
  vectis_auth_oauth2_token_flow_policy token_policy;
  vectis_auth_oauth2_token_flow_result token_flow_result;
  vectis_auth_oauth2_token_flow_store_config token_store;
  vectis_auth_oauth2_stored_token_flow stored_flow;
  vectis_auth_oauth2_stored_token_flow_policy stored_policy;
  vectis_auth_oauth2_webdav_key_config oauth_webdav_config;
  vectis_auth_oidc_authorization_config oidc_authorization_config;
  vectis_auth_oidc_authorization oidc_authorization;
  vectis_auth_oidc_token_exchange_config oidc_exchange_config;
  vectis_auth_oidc_token_exchange oidc_exchange;
  vectis_auth_issue_config issue;
  vectis_auth_issued_credential bearer;
  vectis_auth_issued_credential basic;
  vectis_auth_issued_credential webdav_key;
  vectis_auth_issued_credential oauth_webdav_key;
  vectis_auth_result result;
  vectis_auth_native_provider_config native_provider_config;
  vectis_auth_provider provider;
  vectis_auth_provider custom_provider;
  vectis_auth_provider_request provider_request;
  vectis_auth_provider_response provider_response;
  vectis_webdav_auth_provider_config webdav_auth_config;
  vectis_webdav_auth_request webdav_request;
  vectis_webdav_auth_response webdav_response;
  vectis_request *webdav_vectis_request;
  vectis_error error;
  vectis_status status;
  vectis_totp totp;

  vectis_auth_user_config_init(&user);
  vectis_auth_user_enrollment_init(&enrollment);
  vectis_auth_login_config_init(&login);
  vectis_auth_oauth2_client_credentials_config_init(&oauth2_client);
  vectis_auth_oauth2_token_response_init(&token_response);
  vectis_auth_oauth2_token_flow_init(&token_flow);
  vectis_auth_oauth2_token_flow_policy_init(&token_policy);
  vectis_auth_oauth2_token_flow_result_init(&token_flow_result);
  vectis_auth_oauth2_token_flow_store_config_init(&token_store);
  vectis_auth_oauth2_stored_token_flow_init(&stored_flow);
  vectis_auth_oauth2_stored_token_flow_policy_init(&stored_policy);
  vectis_auth_oauth2_webdav_key_config_init(&oauth_webdav_config);
  vectis_auth_oidc_authorization_config_init(&oidc_authorization_config);
  vectis_auth_oidc_authorization_init(&oidc_authorization);
  vectis_auth_oidc_token_exchange_config_init(&oidc_exchange_config);
  vectis_auth_oidc_token_exchange_init(&oidc_exchange);
  vectis_auth_issued_credential_init(&bearer);
  vectis_auth_issued_credential_init(&basic);
  vectis_auth_issued_credential_init(&webdav_key);
  vectis_auth_issued_credential_init(&oauth_webdav_key);
  vectis_auth_result_init(&result);
  vectis_auth_provider_init(&provider);
  vectis_auth_provider_init(&custom_provider);
  vectis_auth_provider_request_init(&provider_request);
  vectis_auth_provider_response_init(&provider_response);
  vectis_webdav_auth_provider_config_init(&webdav_auth_config);
  vectis_webdav_auth_response_init(&webdav_response);
  webdav_vectis_request = NULL;
  vectis_error_clear(&error);

  if (mkdtemp(temp) == NULL) {
    perror("mkdtemp");
    return 1;
  }
  written = snprintf(credentials_path, sizeof(credentials_path),
                     "%s/credentials.json", temp);
  if (written < 0 || (size_t)written >= sizeof(credentials_path)) {
    remove_tree(temp);
    return 1;
  }

  vectis_auth_store_config_init(&store);
  store.credentials_path = credentials_path;
  status = vectis_auth_store_init(&store, &error);
  expect_ok(status, &error, "initializes credentials store");

  vectis_auth_oidc_authorization_config_init(&oidc_authorization_config);
  oidc_authorization_config.authorization_endpoint =
      "https://idp.example.test/authorize";
  oidc_authorization_config.client_id = "vectis-client";
  oidc_authorization_config.redirect_uri = "http://127.0.0.1/callback";
  oidc_authorization_config.scope = "openid dav";
  oidc_authorization_config.state = "state-1";
  oidc_authorization_config.nonce = "nonce-1";
  oidc_authorization_config.code_verifier =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~abc";
  status = vectis_auth_oidc_authorization_start(&oidc_authorization_config,
                                                &oidc_authorization, &error);
  expect_ok(status, &error, "builds OIDC authorization URL");
  expect(oidc_authorization.authorization_url != NULL &&
             strstr(oidc_authorization.authorization_url,
                    "https://idp.example.test/authorize?") != NULL,
         "OIDC authorization URL uses endpoint");
  expect(oidc_authorization.authorization_url != NULL &&
             strstr(oidc_authorization.authorization_url,
                    "response_type=code") != NULL,
         "OIDC authorization URL requests code flow");
  expect(oidc_authorization.authorization_url != NULL &&
             strstr(oidc_authorization.authorization_url,
                    "code_challenge_method=S256") != NULL,
         "OIDC authorization URL uses PKCE S256");
  expect(oidc_authorization.code_verifier != NULL &&
             strcmp(oidc_authorization.code_verifier,
                    oidc_authorization_config.code_verifier) == 0,
         "OIDC authorization returns verifier");
  expect(oidc_authorization.code_challenge != NULL &&
             oidc_authorization.code_challenge[0] != '\0',
         "OIDC authorization returns challenge");
  expect(oidc_authorization.state != NULL &&
             strcmp(oidc_authorization.state, "state-1") == 0,
         "OIDC authorization returns state");
  expect(oidc_authorization.nonce != NULL &&
             strcmp(oidc_authorization.nonce, "nonce-1") == 0,
         "OIDC authorization returns nonce");

  vectis_auth_oidc_token_exchange_config_init(&oidc_exchange_config);
  oidc_exchange_config.transport.request = oauth2_mock_transport;
  oidc_exchange_config.transport.request_userdata = (void *)"code";
  oidc_exchange_config.transport.user_agent = "vectis-unit";
  oidc_exchange_config.token_endpoint = "https://idp.example.test/token";
  oidc_exchange_config.client_id = "vectis-client";
  oidc_exchange_config.client_secret = "vectis-secret";
  oidc_exchange_config.redirect_uri = "http://127.0.0.1/callback";
  oidc_exchange_config.code_verifier = oidc_authorization.code_verifier;
  oidc_exchange_config.callback_query = "?code=auth-code&state=state-1";
  oidc_exchange_config.expected_state = oidc_authorization.state;
  oidc_exchange_config.now = 1000;
  status = vectis_auth_oidc_exchange_callback(&oidc_exchange_config,
                                              &oidc_exchange, &error);
  expect_ok(status, &error, "exchanges OIDC callback code");
  expect(oidc_exchange.code != NULL &&
             strcmp(oidc_exchange.code, "auth-code") == 0,
         "OIDC exchange returns parsed code");
  expect(oidc_exchange.state != NULL &&
             strcmp(oidc_exchange.state, "state-1") == 0,
         "OIDC exchange returns parsed state");
  expect(oidc_exchange.token.access_token != NULL &&
             strcmp(oidc_exchange.token.access_token, "browser-token") == 0,
         "OIDC exchange returns access token");
  expect(oidc_exchange.token.id_token != NULL &&
             strcmp(oidc_exchange.token.id_token, "id-token") == 0,
         "OIDC exchange returns ID token");
  expect(oidc_exchange.flow.access_token != NULL &&
             strcmp(oidc_exchange.flow.access_token, "browser-token") == 0,
         "OIDC exchange initializes token flow");
  expect(oidc_exchange.flow.has_expires_at &&
             oidc_exchange.flow.expires_at == 5200,
         "OIDC exchange initializes absolute expiry");
  vectis_auth_oidc_token_exchange_cleanup(&oidc_exchange);
  vectis_auth_oidc_authorization_cleanup(&oidc_authorization);

  vectis_auth_oauth2_client_credentials_config_init(&oauth2_client);
  oauth2_client.transport.request = oauth2_mock_transport;
  oauth2_client.transport.request_userdata = (void *)"client";
  oauth2_client.transport.user_agent = "vectis-unit";
  oauth2_client.token_endpoint = "https://idp.example.test/token";
  oauth2_client.client_id = "vectis-client";
  oauth2_client.client_secret = "vectis-secret";
  oauth2_client.scope = "dav";
  status = vectis_auth_oauth2_client_credentials_request(
      &oauth2_client, &token_response, &error);
  expect_ok(status, &error, "executes OAuth2 client credentials flow");
  expect(token_response.access_token != NULL &&
             strcmp(token_response.access_token, "m2m-token") == 0,
         "OAuth2 client credentials returns access token");
  expect(token_response.refresh_token != NULL &&
             strcmp(token_response.refresh_token, "m2m-refresh") == 0,
         "OAuth2 client credentials returns refresh token");
  expect(token_response.has_expires_in && token_response.expires_in == 3600,
         "OAuth2 client credentials returns expiry");
  vectis_auth_oauth2_token_response_cleanup(&token_response);

  token_flow.access_token = test_strdup("expired-token");
  token_flow.token_type = test_strdup("Bearer");
  token_flow.refresh_token = test_strdup("old-refresh");
  token_flow.scope = test_strdup("dav");
  token_flow.expires_at = 10;
  token_flow.has_expires_at = 1;
  expect(token_flow.access_token != NULL && token_flow.token_type != NULL &&
             token_flow.refresh_token != NULL && token_flow.scope != NULL,
         "allocates OAuth2 token flow fixture");
  vectis_auth_oauth2_token_flow_policy_init(&token_policy);
  token_policy.transport.request = oauth2_mock_transport;
  token_policy.transport.request_userdata = (void *)"refresh";
  token_policy.transport.user_agent = "vectis-unit";
  token_policy.token_endpoint = "https://idp.example.test/token";
  token_policy.client_id = "vectis-client";
  token_policy.client_secret = "vectis-secret";
  token_policy.scope = "dav";
  token_policy.now = 1000;
  token_policy.disable_retry = 1;
  status = vectis_auth_oauth2_token_flow_ensure(&token_flow, &token_policy,
                                                &token_flow_result, &error);
  expect_ok(status, &error, "refreshes OAuth2 token flow");
  expect(token_flow_result.state == VECTIS_AUTH_OAUTH2_TOKEN_FLOW_REFRESHED,
         "OAuth2 token flow reports refreshed state");
  expect(token_flow_result.refreshed, "OAuth2 token flow reports update");
  expect(token_flow.access_token != NULL &&
             strcmp(token_flow.access_token, "refreshed-token") == 0,
         "OAuth2 token flow updates access token");
  expect(token_flow.refresh_token != NULL &&
             strcmp(token_flow.refresh_token, "new-refresh") == 0,
         "OAuth2 token flow updates refresh token");
  expect(token_flow.has_expires_at && token_flow.expires_at == 8200,
         "OAuth2 token flow updates absolute expiry");
  vectis_auth_oauth2_token_flow_cleanup(&token_flow);

  token_flow.access_token = test_strdup("expired-token");
  token_flow.token_type = test_strdup("Bearer");
  token_flow.refresh_token = test_strdup("old-refresh");
  token_flow.scope = test_strdup("dav");
  token_flow.expires_at = 10;
  token_flow.has_expires_at = 1;
  expect(token_flow.access_token != NULL && token_flow.token_type != NULL &&
             token_flow.refresh_token != NULL && token_flow.scope != NULL,
         "allocates stored OAuth2 token flow fixture");
  vectis_auth_oauth2_token_flow_store_config_init(&token_store);
  token_store.store = store;
  token_store.flow_id = "oidc-flow-1";
  token_store.subject = "oidc-user@example.com";
  token_store.flow = token_flow;
  status = vectis_auth_oauth2_token_flow_upsert(&token_store, &error);
  expect_ok(status, &error, "stores OAuth2 token flow");
  vectis_auth_oauth2_token_flow_cleanup(&token_flow);

  status = vectis_auth_oauth2_token_flow_load(&store, "oidc-flow-1",
                                              &stored_flow, &error);
  expect_ok(status, &error, "loads OAuth2 token flow");
  expect(stored_flow.found, "stored OAuth2 token flow is found");
  expect(stored_flow.subject != NULL &&
             strcmp(stored_flow.subject, "oidc-user@example.com") == 0,
         "stored OAuth2 token flow carries subject");
  expect(stored_flow.flow.refresh_token != NULL &&
             strcmp(stored_flow.flow.refresh_token, "old-refresh") == 0,
         "stored OAuth2 token flow carries refresh token");
  vectis_auth_oauth2_stored_token_flow_cleanup(&stored_flow);

  vectis_auth_oauth2_webdav_key_config_init(&oauth_webdav_config);
  oauth_webdav_config.store = store;
  oauth_webdav_config.flow_id = "oidc-flow-1";
  oauth_webdav_config.subject = "oidc-user@example.com";
  status = vectis_auth_issue_webdav_key_for_oauth2_flow(
      &oauth_webdav_config, &oauth_webdav_key, &error);
  expect_ok(status, &error, "issues OAuth2-linked WebDAV key");
  expect(oauth_webdav_key.client_id != NULL,
         "OAuth2 WebDAV key includes client_id");
  expect(oauth_webdav_key.client_secret != NULL,
         "OAuth2 WebDAV key includes client_secret");
  expect(oauth_webdav_key.claim_json != NULL &&
             strstr(oauth_webdav_key.claim_json,
                    "\"oauth2_flow_id\":\"oidc-flow-1\"") != NULL,
         "OAuth2 WebDAV key claim carries flow id");
  written = snprintf(
      oauth_basic_clear, sizeof(oauth_basic_clear), "%s:%s",
      oauth_webdav_key.client_id != NULL ? oauth_webdav_key.client_id : "",
      oauth_webdav_key.client_secret != NULL ? oauth_webdav_key.client_secret
                                             : "");
  expect(written > 0 && (size_t)written < sizeof(oauth_basic_clear),
         "formats OAuth2 WebDAV key cleartext");
  expect(base64_encode(oauth_basic_clear, oauth_basic_token,
                       sizeof(oauth_basic_token)),
         "encodes OAuth2 WebDAV key");
  written = snprintf(oauth_basic_header, sizeof(oauth_basic_header), "Basic %s",
                     oauth_basic_token);
  expect(written > 0 && (size_t)written < sizeof(oauth_basic_header),
         "formats OAuth2 WebDAV Basic header");
  status = vectis_auth_verify_authorization(
      &store, oauth_basic_header, VECTIS_AUTH_MODE_BASIC, &result, &error);
  expect_ok(status, &error, "verifies OAuth2-linked WebDAV key");
  expect(result.authenticated, "authenticates OAuth2-linked WebDAV key");
  expect(result.claim_json != NULL &&
             strstr(result.claim_json, "\"oauth2_flow_id\":\"oidc-flow-1\"") !=
                 NULL,
         "verified OAuth2 WebDAV key carries flow id");
  vectis_auth_result_cleanup(&result);

  vectis_auth_oauth2_stored_token_flow_policy_init(&stored_policy);
  stored_policy.store = store;
  stored_policy.flow_id = "oidc-flow-1";
  stored_policy.flow_policy.transport.request = oauth2_mock_transport;
  stored_policy.flow_policy.transport.request_userdata = (void *)"fail";
  stored_policy.flow_policy.transport.user_agent = "vectis-unit";
  stored_policy.flow_policy.token_endpoint = "https://idp.example.test/token";
  stored_policy.flow_policy.client_id = "vectis-client";
  stored_policy.flow_policy.client_secret = "vectis-secret";
  stored_policy.flow_policy.scope = "dav";
  stored_policy.flow_policy.now = 1000;
  stored_policy.flow_policy.disable_retry = 1;
  status = vectis_auth_oauth2_stored_token_flow_ensure(
      &stored_policy, &stored_flow, &token_flow_result, &error);
  expect(status != VECTIS_OK,
         "failed stored OAuth2 refresh reports hard failure");
  vectis_auth_oauth2_stored_token_flow_cleanup(&stored_flow);
  status = vectis_auth_verify_authorization(
      &store, oauth_basic_header, VECTIS_AUTH_MODE_BASIC, &result, &error);
  expect_ok(status, &error, "checks OAuth2 WebDAV key after failed refresh");
  expect(!result.authenticated,
         "failed OAuth2 refresh revokes linked WebDAV key");
  vectis_auth_result_cleanup(&result);

  vectis_auth_user_config_init(&user);
  user.username = "dav-user@example.com";
  user.password = "correct horse battery staple";
  user.enable_totp = 1;
  user.totp_secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
  user.totp_label = "Vectis:dav-user@example.com";
  user.totp_issuer = "Vectis";
  status = vectis_auth_user_add_or_update(&store, &user, &enrollment, &error);
  expect_ok(status, &error, "adds TOTP user");
  expect(enrollment.username != NULL &&
             strcmp(enrollment.username, "dav-user@example.com") == 0,
         "user enrollment returns username");
  expect(enrollment.generated_password == NULL,
         "user enrollment omits provided password");
  expect(enrollment.totp_secret != NULL &&
             strcmp(enrollment.totp_secret,
                    "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ") == 0,
         "user enrollment returns normalized TOTP secret");
  expect(enrollment.totp_uri != NULL &&
             strstr(enrollment.totp_uri, "otpauth://totp/") != NULL,
         "user enrollment returns TOTP URI");
  expect(enrollment.totp_qr_ansi != NULL &&
             strstr(enrollment.totp_qr_ansi, "\342\226\210") != NULL,
         "user enrollment returns terminal QR");
  vectis_auth_user_enrollment_cleanup(&enrollment);

  vectis_auth_issue_config_init(&issue);
  issue.subject = "mike@example.com";
  issue.purpose = "api";
  issue.auth_modes = VECTIS_AUTH_MODE_BEARER;
  status = vectis_auth_issue_credential(&store, &issue, &bearer, &error);
  expect_ok(status, &error, "issues bearer credential");
  expect(bearer.client_id != NULL, "bearer includes client_id");
  expect(bearer.api_key != NULL, "bearer includes api_key");
  expect(bearer.client_secret == NULL, "bearer omits client_secret");
  expect(bearer.claim_json != NULL &&
             strstr(bearer.claim_json, "\"sub\":\"mike@example.com\"") != NULL,
         "bearer claim carries subject");
  expect(bearer.claim_json != NULL &&
             strstr(bearer.claim_json, "\"purpose\":\"api\"") != NULL,
         "bearer claim carries purpose");

  written = snprintf(bearer_header, sizeof(bearer_header), "Bearer %s",
                     bearer.api_key != NULL ? bearer.api_key : "");
  expect(written > 0 && (size_t)written < sizeof(bearer_header),
         "formats bearer header");
  status = vectis_auth_verify_authorization(
      &store, bearer_header, VECTIS_AUTH_MODE_BEARER, &result, &error);
  expect_ok(status, &error, "verifies bearer credential");
  expect(result.authenticated, "authenticates bearer credential");
  expect(result.auth_mode == VECTIS_AUTH_MODE_BEARER, "reports bearer mode");
  expect(result.client_id != NULL && bearer.client_id != NULL &&
             strcmp(result.client_id, bearer.client_id) == 0,
         "reports bearer client_id");
  expect(result.claim_json != NULL &&
             strstr(result.claim_json, "\"purpose\":\"api\"") != NULL,
         "reports bearer claim");
  vectis_auth_result_cleanup(&result);

  status = vectis_auth_verify_authorization(
      &store, "Bearer not-the-key", VECTIS_AUTH_MODE_BEARER, &result, &error);
  expect_ok(status, &error, "rejects invalid bearer without hard failure");
  expect(!result.authenticated, "does not authenticate invalid bearer");
  vectis_auth_result_cleanup(&result);

  vectis_auth_issue_config_init(&issue);
  issue.subject = "mike@example.com";
  issue.purpose = "webdav";
  issue.auth_modes = VECTIS_AUTH_MODE_BASIC;
  status = vectis_auth_issue_credential(&store, &issue, &basic, &error);
  expect_ok(status, &error, "issues WebDAV Basic credential");
  expect(basic.client_id != NULL, "basic includes client_id");
  expect(basic.client_secret != NULL, "basic includes client_secret");
  expect(basic.api_key == NULL, "basic omits bearer api_key");

  written = snprintf(basic_clear, sizeof(basic_clear), "%s:%s",
                     basic.client_id != NULL ? basic.client_id : "",
                     basic.client_secret != NULL ? basic.client_secret : "");
  expect(written > 0 && (size_t)written < sizeof(basic_clear),
         "formats basic cleartext");
  expect(base64_encode(basic_clear, basic_token, sizeof(basic_token)),
         "encodes basic token");
  written =
      snprintf(basic_header, sizeof(basic_header), "Basic %s", basic_token);
  expect(written > 0 && (size_t)written < sizeof(basic_header),
         "formats basic header");

  status = vectis_auth_verify_authorization(
      &store, basic_header, VECTIS_AUTH_MODE_BASIC, &result, &error);
  expect_ok(status, &error, "verifies WebDAV Basic credential");
  expect(result.authenticated, "authenticates WebDAV Basic credential");
  expect(result.auth_mode == VECTIS_AUTH_MODE_BASIC, "reports Basic mode");
  expect(result.claim_json != NULL &&
             strstr(result.claim_json, "\"purpose\":\"webdav\"") != NULL,
         "reports WebDAV purpose");
  vectis_auth_result_cleanup(&result);

  vectis_auth_login_config_init(&login);
  login.username = "dav-user@example.com";
  login.password = "correct horse battery staple";
  status = vectis_auth_user_login(&store, &login, &result, &error);
  expect_ok(status, &error, "checks TOTP user login without TOTP code");
  expect(!result.authenticated, "rejects TOTP user without TOTP code");
  vectis_auth_result_cleanup(&result);

  expect(vectis_totp_init(&totp, "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ") ==
             VECTIS_TOTP_QR_OK,
         "initializes test TOTP");
  expect(vectis_totp_generate(&totp, 59u, totp_code) == VECTIS_TOTP_QR_OK,
         "generates deterministic TOTP code");
  vectis_auth_login_config_init(&login);
  login.username = "dav-user@example.com";
  login.password = "correct horse battery staple";
  login.totp_code = totp_code;
  login.unix_seconds = 59u;
  login.totp_window = 0u;
  status = vectis_auth_user_login(&store, &login, &result, &error);
  expect_ok(status, &error, "authenticates TOTP user");
  expect(result.authenticated, "allows TOTP user with password and code");
  expect(result.claim_json != NULL &&
             strstr(result.claim_json, "\"purpose\":\"login\"") != NULL,
         "login result carries login purpose");
  vectis_auth_result_cleanup(&result);

  status = vectis_auth_issue_webdav_key_for_login(&store, &login, &webdav_key,
                                                  &error);
  expect_ok(status, &error, "issues WebDAV key after TOTP login");
  expect(webdav_key.client_id != NULL, "login WebDAV key includes client_id");
  expect(webdav_key.client_secret != NULL,
         "login WebDAV key includes client_secret");
  written = snprintf(login_basic_clear, sizeof(login_basic_clear), "%s:%s",
                     webdav_key.client_id != NULL ? webdav_key.client_id : "",
                     webdav_key.client_secret != NULL ? webdav_key.client_secret
                                                      : "");
  expect(written > 0 && (size_t)written < sizeof(login_basic_clear),
         "formats login WebDAV key cleartext");
  expect(base64_encode(login_basic_clear, login_basic_token,
                       sizeof(login_basic_token)),
         "encodes login WebDAV key");
  written = snprintf(login_basic_header, sizeof(login_basic_header), "Basic %s",
                     login_basic_token);
  expect(written > 0 && (size_t)written < sizeof(login_basic_header),
         "formats login WebDAV Basic header");
  status = vectis_auth_verify_authorization(
      &store, login_basic_header, VECTIS_AUTH_MODE_BASIC, &result, &error);
  expect_ok(status, &error, "verifies login-issued WebDAV key");
  expect(result.authenticated, "authenticates login-issued WebDAV key");
  expect(result.claim_json != NULL &&
             strstr(result.claim_json, "\"sub\":\"dav-user@example.com\"") !=
                 NULL,
         "login-issued WebDAV key carries user subject");
  expect(result.claim_json != NULL &&
             strstr(result.claim_json, "\"purpose\":\"webdav\"") != NULL,
         "login-issued WebDAV key carries webdav purpose");
  vectis_auth_result_cleanup(&result);

  vectis_auth_native_provider_config_init(&native_provider_config);
  native_provider_config.store = store;
  native_provider_config.allowed_auth_modes = VECTIS_AUTH_MODE_BASIC;
  native_provider_config.realm = "unit";
  status = vectis_auth_provider_from_native_store(
      &provider, &native_provider_config, &error);
  expect_ok(status, &error, "creates native auth provider");

  vectis_auth_provider_request_init(&provider_request);
  provider_request.authorization = basic_header;
  provider_request.purpose = "webdav";
  status = vectis_auth_provider_authenticate(&provider, &provider_request,
                                             &provider_response, &error);
  expect_ok(status, &error, "authenticates native provider");
  expect(provider_response.action == VECTIS_AUTH_ALLOW,
         "native provider allows valid WebDAV key");
  expect(strcmp(provider_response.principal, "mike@example.com") == 0,
         "native provider maps principal from claim subject");
  expect(provider_response.result.authenticated,
         "native provider carries result");
  vectis_auth_provider_response_cleanup(&provider_response);

  vectis_auth_provider_request_init(&provider_request);
  provider_request.purpose = "webdav";
  status = vectis_auth_provider_authenticate(&provider, &provider_request,
                                             &provider_response, &error);
  expect_ok(status, &error, "challenges missing native provider credentials");
  expect(provider_response.action == VECTIS_AUTH_REQUIRED,
         "native provider requires missing credentials");
  expect(provider_response.status_code == 401,
         "native provider challenge status");
  expect(strcmp(provider_response.www_authenticate, "Basic realm=\"unit\"") ==
             0,
         "native provider challenge realm");
  vectis_auth_provider_response_cleanup(&provider_response);

  vectis_auth_provider_request_init(&provider_request);
  provider_request.authorization = basic_header;
  provider_request.purpose = "admin";
  status = vectis_auth_provider_authenticate(&provider, &provider_request,
                                             &provider_response, &error);
  expect_ok(status, &error, "checks native provider purpose mismatch");
  expect(provider_response.action == VECTIS_AUTH_DENY,
         "native provider denies purpose mismatch");
  vectis_auth_provider_response_cleanup(&provider_response);

  webdav_vectis_request = vectis_internal_request_new(&error);
  expect(webdav_vectis_request != NULL,
         "creates internal request for native WebDAV auth");
  if (webdav_vectis_request != NULL) {
    status = vectis_internal_request_add_header(
        webdav_vectis_request, "authorization", basic_header, &error);
    expect_ok(status, &error, "adds Basic Authorization header");
    vectis_webdav_auth_provider_config_init(&webdav_auth_config);
    webdav_auth_config.provider = &provider;
    webdav_auth_config.purpose = "webdav";
    webdav_auth_config.allowed_auth_modes = VECTIS_AUTH_MODE_BASIC;
    memset(&webdav_request, 0, sizeof(webdav_request));
    webdav_request.request = webdav_vectis_request;
    webdav_request.method = VECTIS_HTTP_PROPFIND;
    webdav_request.mount_path_prefix = "/dav";
    webdav_request.resource_path = "/docs";
    status = vectis_webdav_auth_provider(&webdav_request, &webdav_response,
                                         &webdav_auth_config, &error);
    expect_ok(status, &error, "maps native Basic provider into WebDAV");
    expect(webdav_response.action == VECTIS_WEBDAV_AUTH_ALLOW,
           "WebDAV adapter allows native Basic credential");
    expect(strcmp(webdav_response.principal, "mike@example.com") == 0,
           "WebDAV adapter maps native principal");
    vectis_internal_request_free(webdav_vectis_request);
    webdav_vectis_request = NULL;
  }

  status = vectis_auth_provider_from_callback(
      &custom_provider, sample_auth_provider, (void *)"allow", &error);
  expect_ok(status, &error, "creates callback auth provider");
  vectis_webdav_auth_provider_config_init(&webdav_auth_config);
  webdav_auth_config.provider = &custom_provider;
  webdav_auth_config.purpose = "webdav";
  webdav_auth_config.allowed_auth_modes = VECTIS_AUTH_MODE_BASIC;
  memset(&webdav_request, 0, sizeof(webdav_request));
  webdav_request.resource_path = "/docs";
  status = vectis_webdav_auth_provider(&webdav_request, &webdav_response,
                                       &webdav_auth_config, &error);
  expect_ok(status, &error, "maps callback auth provider into WebDAV");
  expect(webdav_response.action == VECTIS_WEBDAV_AUTH_ALLOW,
         "WebDAV adapter allows provider success");
  expect(strcmp(webdav_response.principal, "unit-user") == 0,
         "WebDAV adapter copies provider principal");

  status = vectis_auth_provider_from_callback(
      &custom_provider, sample_auth_provider, (void *)"required", &error);
  expect_ok(status, &error, "creates required callback auth provider");
  status = vectis_webdav_auth_provider(&webdav_request, &webdav_response,
                                       &webdav_auth_config, &error);
  expect_ok(status, &error, "maps auth-required provider into WebDAV");
  expect(webdav_response.action == VECTIS_WEBDAV_AUTH_REQUIRED,
         "WebDAV adapter preserves auth required");
  expect(webdav_response.www_authenticate != NULL &&
             strcmp(webdav_response.www_authenticate, "Basic realm=\"unit\"") ==
                 0,
         "WebDAV adapter preserves auth challenge");

  status = vectis_auth_provider_from_callback(
      &custom_provider, sample_auth_provider, (void *)"redirect", &error);
  expect_ok(status, &error, "creates redirect callback auth provider");
  status = vectis_webdav_auth_provider(&webdav_request, &webdav_response,
                                       &webdav_auth_config, &error);
  expect_ok(status, &error, "maps auth redirect provider into WebDAV");
  expect(webdav_response.action == VECTIS_WEBDAV_AUTH_REDIRECT,
         "WebDAV adapter preserves auth redirect");
  expect(webdav_response.status_code == 303,
         "WebDAV adapter preserves redirect status");
  expect(webdav_response.location != NULL &&
             strcmp(webdav_response.location, "/auth/login?next=/dav/docs") ==
                 0,
         "WebDAV adapter preserves redirect location");
  expect(webdav_response.content_type != NULL &&
             strcmp(webdav_response.content_type, "text/plain") == 0 &&
             webdav_response.body_size == strlen("login required") &&
             memcmp(webdav_response.body, "login required",
                    strlen("login required")) == 0,
         "WebDAV adapter preserves auth response body");

  status = vectis_auth_provider_from_callback(
      &custom_provider, sample_auth_provider, (void *)"header", &error);
  expect_ok(status, &error, "creates header callback auth provider");
  webdav_vectis_request = vectis_internal_request_new(&error);
  expect(webdav_vectis_request != NULL,
         "creates internal request for WebDAV callback auth");
  if (webdav_vectis_request != NULL) {
    status = vectis_internal_request_add_header(
        webdav_vectis_request, "authorization", "Bearer webdav-token", &error);
    expect_ok(status, &error, "adds callback Authorization header");
    webdav_request.request = webdav_vectis_request;
    status = vectis_webdav_auth_provider(&webdav_request, &webdav_response,
                                         &webdav_auth_config, &error);
    expect_ok(status, &error,
              "forwards Authorization header through WebDAV adapter");
    expect(webdav_response.action == VECTIS_WEBDAV_AUTH_ALLOW,
           "WebDAV adapter allows header-aware provider");
    vectis_internal_request_free(webdav_vectis_request);
    webdav_vectis_request = NULL;
    webdav_request.request = NULL;
  }

  status = vectis_auth_revoke_client(
      &store, basic.client_id != NULL ? basic.client_id : "", &error);
  expect_ok(status, &error, "revokes Basic credential");
  status = vectis_auth_verify_authorization(
      &store, basic_header, VECTIS_AUTH_MODE_BASIC, &result, &error);
  expect_ok(status, &error, "checks revoked Basic credential");
  expect(!result.authenticated, "revoked Basic credential is rejected");
  vectis_auth_result_cleanup(&result);

  status = vectis_auth_verify_authorization(
      &store, bearer_header, VECTIS_AUTH_MODE_BEARER, &result, &error);
  expect_ok(status, &error, "checks retained bearer credential");
  expect(result.authenticated, "retains unrelated bearer credential");

  vectis_auth_result_cleanup(&result);
  vectis_auth_issued_credential_cleanup(&oauth_webdav_key);
  vectis_auth_issued_credential_cleanup(&webdav_key);
  vectis_auth_issued_credential_cleanup(&basic);
  vectis_auth_issued_credential_cleanup(&bearer);
  vectis_internal_request_free(webdav_vectis_request);
  remove_tree(temp);
  return failures == 0 ? 0 : 1;
}
