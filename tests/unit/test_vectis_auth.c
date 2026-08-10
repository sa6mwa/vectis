#include <vectis/auth.h>
#include <vectis/webdav.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;

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
  expect(request->purpose != NULL && strcmp(request->purpose, "webdav") == 0,
         "provider receives purpose");
  expect(request->resource != NULL && strcmp(request->resource, "/docs") == 0,
         "provider receives resource");
  return vectis_auth_provider_response_set_authenticated(
      response, "unit-user", "client-1", "{\"sub\":\"unit-user\"}",
      VECTIS_AUTH_MODE_BASIC, error);
}

int main(void) {
  char temp[] = "/tmp/vectis-auth-unit.XXXXXX";
  char credentials_path[4096];
  char bearer_header[1024];
  char basic_clear[1024];
  char basic_token[1400];
  char basic_header[1500];
  int written;
  vectis_auth_store_config store;
  vectis_auth_issue_config issue;
  vectis_auth_issued_credential bearer;
  vectis_auth_issued_credential basic;
  vectis_auth_result result;
  vectis_auth_native_provider_config native_provider_config;
  vectis_auth_provider provider;
  vectis_auth_provider custom_provider;
  vectis_auth_provider_request provider_request;
  vectis_auth_provider_response provider_response;
  vectis_webdav_auth_provider_config webdav_auth_config;
  vectis_webdav_auth_request webdav_request;
  vectis_webdav_auth_response webdav_response;
  vectis_error error;
  vectis_status status;

  vectis_auth_issued_credential_init(&bearer);
  vectis_auth_issued_credential_init(&basic);
  vectis_auth_result_init(&result);
  vectis_auth_provider_init(&provider);
  vectis_auth_provider_init(&custom_provider);
  vectis_auth_provider_request_init(&provider_request);
  vectis_auth_provider_response_init(&provider_response);
  vectis_webdav_auth_provider_config_init(&webdav_auth_config);
  vectis_webdav_auth_response_init(&webdav_response);
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
  vectis_auth_issued_credential_cleanup(&basic);
  vectis_auth_issued_credential_cleanup(&bearer);
  remove_tree(temp);
  return failures == 0 ? 0 : 1;
}
