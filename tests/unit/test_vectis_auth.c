#include "vectis_internal.h"

#include <vectis/auth.h>
#include <vectis/totp_qr.h>
#include <vectis/webdav.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;

typedef struct smtp_mock_server {
  int listen_fd;
  unsigned short port;
  pthread_t thread;
  char data[8192];
  size_t data_size;
  int started;
} smtp_mock_server;

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

static int smtp_mock_send_all(int fd, const char *text) {
  size_t len;
  size_t sent;
  ssize_t n;

  len = strlen(text);
  sent = 0u;
  while (sent < len) {
    n = send(fd, text + sent, len - sent, 0);
    if (n <= 0) {
      return 0;
    }
    sent += (size_t)n;
  }
  return 1;
}

static int smtp_mock_read_line(int fd, char *line, size_t line_size) {
  size_t used;
  char ch;
  ssize_t n;

  used = 0u;
  while (used + 1u < line_size) {
    n = recv(fd, &ch, 1u, 0);
    if (n <= 0) {
      return 0;
    }
    line[used++] = ch;
    if (ch == '\n') {
      break;
    }
  }
  line[used] = '\0';
  return used > 0u;
}

static void smtp_mock_capture(smtp_mock_server *server, const char *line) {
  size_t len;
  size_t copy;

  if (server == NULL || line == NULL) {
    return;
  }
  len = strlen(line);
  if (server->data_size >= sizeof(server->data) - 1u) {
    return;
  }
  copy = len;
  if (copy > sizeof(server->data) - 1u - server->data_size) {
    copy = sizeof(server->data) - 1u - server->data_size;
  }
  memcpy(server->data + server->data_size, line, copy);
  server->data_size += copy;
  server->data[server->data_size] = '\0';
}

static void *smtp_mock_thread(void *userdata) {
  smtp_mock_server *server;
  struct sockaddr_in peer;
  socklen_t peer_len;
  char line[1024];
  int client_fd;
  int in_data;

  server = (smtp_mock_server *)userdata;
  peer_len = (socklen_t)sizeof(peer);
  client_fd = accept(server->listen_fd, (struct sockaddr *)&peer, &peer_len);
  if (client_fd < 0) {
    return NULL;
  }
  in_data = 0;
  (void)smtp_mock_send_all(client_fd, "220 vectis smtp mock\r\n");
  while (smtp_mock_read_line(client_fd, line, sizeof(line))) {
    if (in_data) {
      if (strcmp(line, ".\r\n") == 0 || strcmp(line, ".\n") == 0) {
        in_data = 0;
        (void)smtp_mock_send_all(client_fd, "250 queued\r\n");
      } else {
        smtp_mock_capture(server, line);
      }
    } else if (strncmp(line, "EHLO", 4u) == 0 ||
               strncmp(line, "HELO", 4u) == 0) {
      (void)smtp_mock_send_all(client_fd, "250-localhost\r\n250 OK\r\n");
    } else if (strncmp(line, "MAIL FROM:", 10u) == 0 ||
               strncmp(line, "RCPT TO:", 8u) == 0) {
      (void)smtp_mock_send_all(client_fd, "250 OK\r\n");
    } else if (strncmp(line, "DATA", 4u) == 0) {
      in_data = 1;
      (void)smtp_mock_send_all(client_fd, "354 end with dot\r\n");
    } else if (strncmp(line, "QUIT", 4u) == 0) {
      (void)smtp_mock_send_all(client_fd, "221 bye\r\n");
      break;
    } else {
      (void)smtp_mock_send_all(client_fd, "250 OK\r\n");
    }
  }
  (void)close(client_fd);
  return NULL;
}

static int smtp_mock_start(smtp_mock_server *server) {
  struct sockaddr_in addr;
  socklen_t addr_len;
  int enabled;

  memset(server, 0, sizeof(*server));
  server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server->listen_fd < 0) {
    return 0;
  }
  enabled = 1;
  (void)setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   (socklen_t)sizeof(enabled));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(server->listen_fd, 1) != 0) {
    (void)close(server->listen_fd);
    server->listen_fd = -1;
    return 0;
  }
  addr_len = (socklen_t)sizeof(addr);
  if (getsockname(server->listen_fd, (struct sockaddr *)&addr, &addr_len) !=
      0) {
    (void)close(server->listen_fd);
    server->listen_fd = -1;
    return 0;
  }
  server->port = ntohs(addr.sin_port);
  if (pthread_create(&server->thread, NULL, smtp_mock_thread, server) != 0) {
    (void)close(server->listen_fd);
    server->listen_fd = -1;
    return 0;
  }
  server->started = 1;
  return 1;
}

static void smtp_mock_stop(smtp_mock_server *server) {
  if (server == NULL) {
    return;
  }
  if (server->started) {
    (void)pthread_join(server->thread, NULL);
    server->started = 0;
  }
  if (server->listen_fd >= 0) {
    (void)close(server->listen_fd);
    server->listen_fd = -1;
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
  char smtp_url[128];
  char totp_code[VECTIS_TOTP_CODE_LENGTH + 1u];
  int written;
  int user_exists;
  vectis_auth_store_config store;
  vectis_auth_user_config user;
  vectis_auth_user_enrollment enrollment;
  vectis_auth_login_config login;
  vectis_auth_password_check_config password_check;
  vectis_auth_password_check_result password_result;
  vectis_auth_pending_login_issue_config pending_issue;
  vectis_auth_pending_login pending_login;
  vectis_auth_pending_login_consume_config pending_consume;
  vectis_auth_pending_login_result pending_result;
  vectis_auth_oauth2_client_credentials_config oauth2_client;
  vectis_auth_oauth2_token_response token_response;
  vectis_auth_oauth2_token_flow token_flow;
  vectis_auth_oauth2_token_flow_policy token_policy;
  vectis_auth_oauth2_token_flow_result token_flow_result;
  vectis_auth_oauth2_token_flow_store_config token_store;
  vectis_auth_oauth2_stored_token_flow stored_flow;
  vectis_auth_oauth2_stored_token_flow_policy stored_policy;
  vectis_auth_oauth2_webdav_key_config oauth_webdav_config;
  vectis_auth_email_token_issue_config email_issue;
  vectis_auth_email_token email_token;
  vectis_auth_email_token_verify_config email_verify;
  vectis_auth_email_token_result email_result;
  vectis_auth_smtp_config smtp_config;
  vectis_auth_email_message email_message;
  smtp_mock_server smtp_server;
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
  vectis_auth_password_check_config_init(&password_check);
  vectis_auth_password_check_result_init(&password_result);
  vectis_auth_pending_login_issue_config_init(&pending_issue);
  vectis_auth_pending_login_init(&pending_login);
  vectis_auth_pending_login_consume_config_init(&pending_consume);
  vectis_auth_pending_login_result_init(&pending_result);
  vectis_auth_oauth2_client_credentials_config_init(&oauth2_client);
  vectis_auth_oauth2_token_response_init(&token_response);
  vectis_auth_oauth2_token_flow_init(&token_flow);
  vectis_auth_oauth2_token_flow_policy_init(&token_policy);
  vectis_auth_oauth2_token_flow_result_init(&token_flow_result);
  vectis_auth_oauth2_token_flow_store_config_init(&token_store);
  vectis_auth_oauth2_stored_token_flow_init(&stored_flow);
  vectis_auth_oauth2_stored_token_flow_policy_init(&stored_policy);
  vectis_auth_oauth2_webdav_key_config_init(&oauth_webdav_config);
  vectis_auth_email_token_issue_config_init(&email_issue);
  vectis_auth_email_token_init(&email_token);
  vectis_auth_email_token_verify_config_init(&email_verify);
  vectis_auth_email_token_result_init(&email_result);
  vectis_auth_smtp_config_init(&smtp_config);
  memset(&email_message, 0, sizeof(email_message));
  memset(&smtp_server, 0, sizeof(smtp_server));
  smtp_server.listen_fd = -1;
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
  user_exists = 1;
  status = vectis_auth_user_exists(&store, "dav-user@example.com", &user_exists,
                                   &error);
  expect_ok(status, &error, "checks absent auth user");
  expect(!user_exists, "absent auth user is reported missing");

  vectis_auth_email_token_issue_config_init(&email_issue);
  email_issue.store = store;
  email_issue.username = "email-user@example.com";
  email_issue.realm = "unit";
  email_issue.email = "email-user@example.com";
  email_issue.transaction_id = "email-tx-1";
  email_issue.token = "123456";
  email_issue.now_seconds = 1000;
  email_issue.ttl_seconds = 300;
  status = vectis_auth_email_token_issue(&email_issue, &email_token, &error);
  expect_ok(status, &error, "issues email auth token");
  expect(email_token.transaction_id != NULL &&
             strcmp(email_token.transaction_id, "email-tx-1") == 0,
         "email token carries transaction id");
  expect(email_token.token != NULL && strcmp(email_token.token, "123456") == 0,
         "email token carries configured token");
  expect(email_token.expires_at == 1300, "email token applies ttl");

  vectis_auth_email_token_verify_config_init(&email_verify);
  email_verify.store = store;
  email_verify.transaction_id = "email-tx-1";
  email_verify.username = "email-user@example.com";
  email_verify.realm = "unit";
  email_verify.token = "000000";
  email_verify.now_seconds = 1100;
  status = vectis_auth_email_token_verify(&email_verify, &email_result, &error);
  expect_ok(status, &error, "checks wrong email token");
  expect(!email_result.verified && !email_result.expired,
         "wrong email token is rejected without consuming transaction");
  expect(email_result.failed_attempts == 1u,
         "wrong email token increments failed attempts");
  expect(email_result.max_attempts ==
             VECTIS_AUTH_EMAIL_TOKEN_DEFAULT_MAX_ATTEMPTS,
         "email token reports default max attempts");
  vectis_auth_email_token_result_cleanup(&email_result);

  email_verify.token = "123456";
  status = vectis_auth_email_token_verify(&email_verify, &email_result, &error);
  expect_ok(status, &error, "verifies email auth token");
  expect(email_result.verified && !email_result.expired,
         "email token verifies before expiry");
  expect(email_result.username != NULL &&
             strcmp(email_result.username, "email-user@example.com") == 0,
         "email token result carries username");
  expect(email_result.realm != NULL && strcmp(email_result.realm, "unit") == 0,
         "email token result carries realm");
  expect(email_result.email != NULL &&
             strcmp(email_result.email, "email-user@example.com") == 0,
         "email token result carries email");
  vectis_auth_email_token_result_cleanup(&email_result);

  status = vectis_auth_email_token_verify(&email_verify, &email_result, &error);
  expect_ok(status, &error, "checks replayed email token");
  expect(!email_result.verified && !email_result.expired,
         "verified email token is consumed");
  vectis_auth_email_token_result_cleanup(&email_result);
  vectis_auth_email_token_cleanup(&email_token);

  vectis_auth_email_token_issue_config_init(&email_issue);
  email_issue.store = store;
  email_issue.username = "email-user@example.com";
  email_issue.realm = "unit";
  email_issue.email = "email-user@example.com";
  email_issue.transaction_id = "email-tx-limited";
  email_issue.token = "limited";
  email_issue.now_seconds = 1300;
  email_issue.ttl_seconds = 300;
  email_issue.max_attempts = 2u;
  status = vectis_auth_email_token_issue(&email_issue, &email_token, &error);
  expect_ok(status, &error, "issues limited-attempt email auth token");

  vectis_auth_email_token_verify_config_init(&email_verify);
  email_verify.store = store;
  email_verify.transaction_id = "email-tx-limited";
  email_verify.username = "email-user@example.com";
  email_verify.realm = "unit";
  email_verify.token = "wrong-one";
  email_verify.now_seconds = 1310;
  status = vectis_auth_email_token_verify(&email_verify, &email_result, &error);
  expect_ok(status, &error, "checks first limited-attempt wrong token");
  expect(!email_result.verified && !email_result.expired,
         "first limited wrong token is rejected");
  expect(email_result.failed_attempts == 1u && email_result.max_attempts == 2u,
         "first limited wrong token records attempt budget");
  vectis_auth_email_token_result_cleanup(&email_result);

  email_verify.token = "wrong-two";
  status = vectis_auth_email_token_verify(&email_verify, &email_result, &error);
  expect_ok(status, &error, "checks second limited-attempt wrong token");
  expect(!email_result.verified && !email_result.expired,
         "second limited wrong token is rejected");
  expect(email_result.failed_attempts == 2u && email_result.max_attempts == 2u,
         "second limited wrong token reaches attempt budget");
  vectis_auth_email_token_result_cleanup(&email_result);

  email_verify.token = "limited";
  status = vectis_auth_email_token_verify(&email_verify, &email_result, &error);
  expect_ok(status, &error, "checks limited-attempt token after budget");
  expect(!email_result.verified && !email_result.expired,
         "limited-attempt token is consumed after failed budget");
  vectis_auth_email_token_result_cleanup(&email_result);
  vectis_auth_email_token_cleanup(&email_token);

  vectis_auth_email_token_issue_config_init(&email_issue);
  email_issue.store = store;
  email_issue.username = "email-user@example.com";
  email_issue.realm = "unit";
  email_issue.email = "email-user@example.com";
  email_issue.pending_transaction_id = "pending-email-1";
  email_issue.transaction_id = "email-tx-scoped";
  email_issue.token = "abcdef";
  email_issue.now_seconds = 1200;
  email_issue.ttl_seconds = 300;
  status = vectis_auth_email_token_issue(&email_issue, &email_token, &error);
  expect_ok(status, &error, "issues pending-scoped email auth token");

  vectis_auth_email_token_verify_config_init(&email_verify);
  email_verify.store = store;
  email_verify.transaction_id = "email-tx-scoped";
  email_verify.username = "email-user@example.com";
  email_verify.realm = "unit";
  email_verify.pending_transaction_id = "pending-email-other";
  email_verify.token = "abcdef";
  email_verify.now_seconds = 1210;
  status = vectis_auth_email_token_verify(&email_verify, &email_result, &error);
  expect_ok(status, &error,
            "rejects wrong pending transaction for email token");
  expect(!email_result.verified && !email_result.expired,
         "wrong pending transaction does not verify scoped email token");
  vectis_auth_email_token_result_cleanup(&email_result);

  email_verify.pending_transaction_id = "pending-email-1";
  status = vectis_auth_email_token_verify(&email_verify, &email_result, &error);
  expect_ok(status, &error, "verifies pending-scoped email auth token");
  expect(email_result.verified && !email_result.expired,
         "pending-scoped email token verifies");
  expect(email_result.pending_transaction_id != NULL &&
             strcmp(email_result.pending_transaction_id, "pending-email-1") ==
                 0,
         "email token result carries pending transaction id");
  vectis_auth_email_token_result_cleanup(&email_result);
  vectis_auth_email_token_cleanup(&email_token);

  vectis_auth_email_token_issue_config_init(&email_issue);
  email_issue.store = store;
  email_issue.username = "email-user@example.com";
  email_issue.realm = "unit";
  email_issue.email = "email-user@example.com";
  email_issue.transaction_id = "email-tx-expired";
  email_issue.token = "654321";
  email_issue.now_seconds = 2000;
  email_issue.ttl_seconds = 60;
  status = vectis_auth_email_token_issue(&email_issue, &email_token, &error);
  expect_ok(status, &error, "issues expiring email auth token");
  vectis_auth_email_token_verify_config_init(&email_verify);
  email_verify.store = store;
  email_verify.transaction_id = "email-tx-expired";
  email_verify.username = "email-user@example.com";
  email_verify.realm = "unit";
  email_verify.token = "654321";
  email_verify.now_seconds = 2061;
  status = vectis_auth_email_token_verify(&email_verify, &email_result, &error);
  expect_ok(status, &error, "checks expired email token");
  expect(!email_result.verified && email_result.expired,
         "expired email token is rejected and reported");
  vectis_auth_email_token_result_cleanup(&email_result);
  status = vectis_auth_email_token_verify(&email_verify, &email_result, &error);
  expect_ok(status, &error, "checks consumed expired email token");
  expect(!email_result.verified && !email_result.expired,
         "expired email token is consumed");
  vectis_auth_email_token_result_cleanup(&email_result);
  vectis_auth_email_token_cleanup(&email_token);

  vectis_auth_smtp_config_init(&smtp_config);
  smtp_config.url = "smtp://127.0.0.1:9";
  smtp_config.mail_from = "<sender@example.test>";
  smtp_config.allowed_recipient_domain = "example.test";
  memset(&email_message, 0, sizeof(email_message));
  email_message.username = "email-user@example.com";
  email_message.realm = "unit";
  email_message.email = "blocked@example.invalid";
  email_message.transaction_id = "email-tx-smtp-blocked";
  email_message.token = "999999";
  email_message.expires_at = 1300;
  status = vectis_auth_email_token_deliver_smtp(&smtp_config, &email_message,
                                                &error);
  expect(status == VECTIS_ERR_INVALID, "SMTP helper enforces recipient domain");
  vectis_error_clear(&error);

  if (smtp_mock_start(&smtp_server)) {
    written = snprintf(smtp_url, sizeof(smtp_url), "smtp://127.0.0.1:%u",
                       (unsigned)smtp_server.port);
    expect(written > 0 && (size_t)written < sizeof(smtp_url),
           "formats SMTP mock URL");
    smtp_config.url = smtp_url;
    smtp_config.timeout_ms = 5000L;
    smtp_config.connect_timeout_ms = 2000L;
    email_message.email = "email-user@example.test";
    email_message.transaction_id = "email-tx-smtp";
    email_message.token = "135790";
    email_message.expires_at = 1600;
    status = vectis_auth_email_token_deliver_smtp(&smtp_config, &email_message,
                                                  &error);
    expect_ok(status, &error, "delivers email token through SMTP");
    smtp_mock_stop(&smtp_server);
    expect(strstr(smtp_server.data, "Subject: Vectis login token") != NULL,
           "SMTP delivery includes subject");
    expect(strstr(smtp_server.data, "135790") != NULL,
           "SMTP delivery includes token");
    expect(strstr(smtp_server.data, "email-tx-smtp") != NULL,
           "SMTP delivery includes transaction id");
  } else {
    expect(0, "starts SMTP mock server");
  }

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

  vectis_auth_oauth2_webdav_key_config_init(&oauth_webdav_config);
  oauth_webdav_config.store = store;
  oauth_webdav_config.flow_id = "missing-oidc-flow";
  oauth_webdav_config.subject = "oidc-user@example.com";
  status = vectis_auth_issue_webdav_key_for_oauth2_flow(
      &oauth_webdav_config, &oauth_webdav_key, &error);
  expect(status == VECTIS_ERR_STATE,
         "OAuth2 WebDAV key rejects missing stored flow");
  expect(error.message != NULL &&
             strcmp(error.message, "OAuth2 token flow was not found") == 0,
         "OAuth2 WebDAV key missing-flow error is diagnostic");
  expect(oauth_webdav_key.client_id == NULL &&
             oauth_webdav_key.client_secret == NULL,
         "OAuth2 WebDAV key missing-flow rejection issues no credential");
  vectis_error_clear(&error);
  vectis_auth_issued_credential_cleanup(&oauth_webdav_key);

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
  vectis_error_clear(&error);

  vectis_auth_issued_credential_cleanup(&oauth_webdav_key);
  vectis_auth_oauth2_webdav_key_config_init(&oauth_webdav_config);
  oauth_webdav_config.store = store;
  oauth_webdav_config.flow_id = "oidc-flow-1";
  oauth_webdav_config.subject = "oidc-user@example.com";
  status = vectis_auth_issue_webdav_key_for_oauth2_flow(
      &oauth_webdav_config, &oauth_webdav_key, &error);
  expect_ok(status, &error, "issues OAuth2 WebDAV key for revoke opt-out");
  written = snprintf(
      oauth_basic_clear, sizeof(oauth_basic_clear), "%s:%s",
      oauth_webdav_key.client_id != NULL ? oauth_webdav_key.client_id : "",
      oauth_webdav_key.client_secret != NULL ? oauth_webdav_key.client_secret
                                             : "");
  expect(written > 0 && (size_t)written < sizeof(oauth_basic_clear),
         "formats OAuth2 opt-out WebDAV key cleartext");
  expect(base64_encode(oauth_basic_clear, oauth_basic_token,
                       sizeof(oauth_basic_token)),
         "encodes OAuth2 opt-out WebDAV key");
  written = snprintf(oauth_basic_header, sizeof(oauth_basic_header), "Basic %s",
                     oauth_basic_token);
  expect(written > 0 && (size_t)written < sizeof(oauth_basic_header),
         "formats OAuth2 opt-out WebDAV Basic header");

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
  stored_policy.revoke_webdav_keys_on_failure = 0;
  status = vectis_auth_oauth2_stored_token_flow_ensure(
      &stored_policy, &stored_flow, &token_flow_result, &error);
  expect(status != VECTIS_OK,
         "failed stored OAuth2 refresh still reports failure when revoke "
         "opt-out is set");
  vectis_auth_oauth2_stored_token_flow_cleanup(&stored_flow);
  vectis_error_clear(&error);
  status = vectis_auth_verify_authorization(
      &store, oauth_basic_header, VECTIS_AUTH_MODE_BASIC, &result, &error);
  expect_ok(status, &error,
            "checks OAuth2 WebDAV key after opt-out failed refresh");
  expect(result.authenticated,
         "failed OAuth2 refresh preserves linked WebDAV key when revoke "
         "opt-out is set");
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
  user_exists = 0;
  status = vectis_auth_user_exists(&store, "dav-user@example.com", &user_exists,
                                   &error);
  expect_ok(status, &error, "checks present auth user");
  expect(user_exists, "present auth user is reported present");
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

  vectis_auth_password_check_config_init(&password_check);
  password_check.store = store;
  password_check.username = "dav-user@example.com";
  password_check.password = "wrong";
  status = vectis_auth_user_password_check(&password_check, &password_result,
                                           &error);
  expect_ok(status, &error, "checks wrong password before pending login");
  expect(!password_result.authenticated,
         "wrong password does not pass password check");

  password_check.password = "correct horse battery staple";
  status = vectis_auth_user_password_check(&password_check, &password_result,
                                           &error);
  expect_ok(status, &error, "checks correct password before pending login");
  expect(password_result.authenticated, "correct password passes check");
  expect(password_result.totp_required, "password check reports TOTP required");

  vectis_auth_pending_login_issue_config_init(&pending_issue);
  pending_issue.store = store;
  pending_issue.username = "dav-user@example.com";
  pending_issue.password = "wrong";
  pending_issue.realm = "unit";
  pending_issue.transaction_id = "pending-login-wrong";
  pending_issue.now_seconds = 59u;
  status =
      vectis_auth_pending_login_issue(&pending_issue, &pending_login, &error);
  expect_ok(status, &error, "rejects pending login issue with wrong password");
  expect(!pending_login.authenticated,
         "wrong password does not create pending login");
  vectis_auth_pending_login_cleanup(&pending_login);

  pending_issue.password = "correct horse battery staple";
  pending_issue.transaction_id = "pending-login-wrong-totp";
  pending_issue.ttl_seconds = 300u;
  status =
      vectis_auth_pending_login_issue(&pending_issue, &pending_login, &error);
  expect_ok(status, &error, "issues pending login for TOTP user");
  expect(pending_login.authenticated, "pending login is authenticated");
  expect(pending_login.totp_required, "pending login reports TOTP required");
  expect(pending_login.transaction_id != NULL &&
             strcmp(pending_login.transaction_id, "pending-login-wrong-totp") ==
                 0,
         "pending login carries transaction id");
  expect(pending_login.expires_at == 359u, "pending login carries expiry");
  vectis_auth_pending_login_cleanup(&pending_login);

  vectis_auth_pending_login_consume_config_init(&pending_consume);
  pending_consume.store = store;
  pending_consume.transaction_id = "pending-login-wrong-totp";
  pending_consume.username = "dav-user@example.com";
  pending_consume.realm = "unit";
  pending_consume.totp_code = "000000";
  pending_consume.now_seconds = 59u;
  pending_consume.totp_window = 0u;
  status = vectis_auth_pending_login_consume(&pending_consume, &pending_result,
                                             &error);
  expect_ok(status, &error, "rejects wrong pending-login TOTP");
  expect(!pending_result.authenticated, "wrong TOTP does not consume as login");
  expect(pending_result.totp_required, "wrong TOTP reports TOTP requirement");
  vectis_auth_pending_login_result_cleanup(&pending_result);

  pending_consume.totp_code = totp_code;
  status = vectis_auth_pending_login_consume(&pending_consume, &pending_result,
                                             &error);
  expect_ok(status, &error, "wrong TOTP consumes pending transaction");
  expect(!pending_result.authenticated,
         "wrong-TOTP pending transaction cannot be replayed");
  vectis_auth_pending_login_result_cleanup(&pending_result);

  pending_issue.transaction_id = "pending-login-verify";
  status =
      vectis_auth_pending_login_issue(&pending_issue, &pending_login, &error);
  expect_ok(status, &error, "issues pending login for verify preflight");
  expect(pending_login.authenticated,
         "verify preflight pending login is authenticated");
  vectis_auth_pending_login_cleanup(&pending_login);

  pending_consume.transaction_id = "pending-login-verify";
  pending_consume.totp_code = totp_code;
  status = vectis_auth_pending_login_verify(&pending_consume, &pending_result,
                                            &error);
  expect_ok(status, &error, "verifies pending login without consuming it");
  expect(pending_result.authenticated,
         "pending login verify accepts correct TOTP");
  vectis_auth_pending_login_result_cleanup(&pending_result);

  status = vectis_auth_pending_login_consume(&pending_consume, &pending_result,
                                             &error);
  expect_ok(status, &error,
            "consume still accepts pending login after verify preflight");
  expect(pending_result.authenticated,
         "pending login verify preflight is non-consuming");
  vectis_auth_pending_login_result_cleanup(&pending_result);

  pending_issue.transaction_id = "pending-login-first";
  status =
      vectis_auth_pending_login_issue(&pending_issue, &pending_login, &error);
  expect_ok(status, &error, "issues first duplicate-user pending login");
  expect(pending_login.authenticated,
         "first duplicate-user pending login is authenticated");
  vectis_auth_pending_login_cleanup(&pending_login);

  pending_issue.transaction_id = "pending-login-second";
  status =
      vectis_auth_pending_login_issue(&pending_issue, &pending_login, &error);
  expect_ok(status, &error, "issues second duplicate-user pending login");
  expect(pending_login.authenticated,
         "second duplicate-user pending login is authenticated");
  vectis_auth_pending_login_cleanup(&pending_login);

  pending_consume.transaction_id = "pending-login-first";
  pending_consume.totp_code = totp_code;
  status = vectis_auth_pending_login_verify(&pending_consume, &pending_result,
                                            &error);
  expect_ok(status, &error, "verifies first duplicate-user pending login");
  expect(pending_result.authenticated,
         "duplicate-user pending lookup verifies requested transaction");
  vectis_auth_pending_login_result_cleanup(&pending_result);

  pending_issue.transaction_id = "pending-login-ok";
  status =
      vectis_auth_pending_login_issue(&pending_issue, &pending_login, &error);
  expect_ok(status, &error, "issues second pending login for success");
  expect(pending_login.authenticated, "second pending login is authenticated");
  vectis_auth_pending_login_cleanup(&pending_login);

  pending_consume.transaction_id = "pending-login-ok";
  pending_consume.totp_code = totp_code;
  status = vectis_auth_pending_login_consume(&pending_consume, &pending_result,
                                             &error);
  expect_ok(status, &error, "consumes pending login with TOTP");
  expect(pending_result.authenticated, "pending login accepts correct TOTP");
  expect(pending_result.username != NULL &&
             strcmp(pending_result.username, "dav-user@example.com") == 0,
         "pending result carries username");
  vectis_auth_pending_login_result_cleanup(&pending_result);

  status = vectis_auth_pending_login_consume(&pending_consume, &pending_result,
                                             &error);
  expect_ok(status, &error, "checks pending login replay");
  expect(!pending_result.authenticated, "pending login is single-use");
  vectis_auth_pending_login_result_cleanup(&pending_result);

  pending_issue.transaction_id = "pending-login-expired";
  pending_issue.now_seconds = 59u;
  pending_issue.ttl_seconds = 1u;
  status =
      vectis_auth_pending_login_issue(&pending_issue, &pending_login, &error);
  expect_ok(status, &error, "issues expiring pending login");
  expect(pending_login.authenticated, "expiring pending login is created");
  vectis_auth_pending_login_cleanup(&pending_login);

  pending_consume.transaction_id = "pending-login-expired";
  pending_consume.totp_code = totp_code;
  pending_consume.now_seconds = 61u;
  status = vectis_auth_pending_login_consume(&pending_consume, &pending_result,
                                             &error);
  expect_ok(status, &error, "rejects expired pending login");
  expect(!pending_result.authenticated,
         "expired pending login is not accepted");
  expect(pending_result.expired, "expired pending login reports expiry");
  vectis_auth_pending_login_result_cleanup(&pending_result);

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
  vectis_auth_pending_login_result_cleanup(&pending_result);
  vectis_auth_pending_login_cleanup(&pending_login);
  vectis_auth_email_token_result_cleanup(&email_result);
  vectis_auth_email_token_cleanup(&email_token);
  vectis_auth_issued_credential_cleanup(&oauth_webdav_key);
  vectis_auth_issued_credential_cleanup(&webdav_key);
  vectis_auth_issued_credential_cleanup(&basic);
  vectis_auth_issued_credential_cleanup(&bearer);
  vectis_internal_request_free(webdav_vectis_request);
  remove_tree(temp);
  return failures == 0 ? 0 : 1;
}
