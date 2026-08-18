#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "vectis_internal.h"

#include <lc/lc.h>
#include <vectis/vectis.h>

static vectis_status sample_handler(vectis_app *app, vectis_request *request,
                                    vectis_response *response, void *userdata,
                                    vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok", error);
}

static vectis_status https_get(const char *url, const char *ca_bundle_path,
                               const char *client_bundle_path,
                               vectis_http_response *response,
                               vectis_error *error) {
  vectis_http_client_config http;

  vectis_http_client_config_init(&http);
  http.ca_bundle_path = ca_bundle_path;
  http.client_bundle_path = client_bundle_path;
  http.timeout_ms = 1000L;
  http.connect_timeout_ms = 200L;
  return vectis_http_get(&http, url, response, error);
}

static vectis_status https_get_ca_source(const char *url,
                                         vectis_source ca_bundle,
                                         vectis_http_response *response,
                                         vectis_error *error) {
  vectis_http_client_config http;

  vectis_http_client_config_init(&http);
  http.ca_bundle = ca_bundle;
  http.timeout_ms = 1000L;
  http.connect_timeout_ms = 200L;
  return vectis_http_get(&http, url, response, error);
}

static unsigned short test_port_from_env(const char *name,
                                         unsigned short fallback) {
  const char *value;
  char *end;
  unsigned long parsed;

  value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  end = NULL;
  parsed = strtoul(value, &end, 10);
  if (end == value || *end != '\0' || parsed == 0ul || parsed > 65535ul) {
    fprintf(stderr, "%s must be a TCP port number\n", name);
    assert(0);
  }
  return (unsigned short)parsed;
}

static void format_secure_url(char *out, size_t out_size, const char *host,
                              unsigned short port) {
  int written;

  written =
      snprintf(out, out_size, "https://%s:%u/secure", host, (unsigned)port);
  assert(written > 0 && (size_t)written < out_size);
}

static void format_clear_url(char *out, size_t out_size, const char *host,
                             unsigned short port) {
  int written;

  written = snprintf(out, out_size, "http://%s:%u/secure?from=http", host,
                     (unsigned)port);
  assert(written > 0 && (size_t)written < out_size);
}

static void assert_http_redirect(unsigned short port, unsigned short tls_port) {
  struct sockaddr_in address;
  char location[256];
  char request[256];
  char response[2048];
  ssize_t received;
  int fd;
  int attempt;
  int written;

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
  written = snprintf(request, sizeof(request),
                     "GET /secure?from=http HTTP/1.1\r\n"
                     "Host: localhost:%u\r\nConnection: close\r\n\r\n",
                     (unsigned)port);
  assert(written > 0 && (size_t)written < sizeof(request));
  for (attempt = 0; attempt < 100; ++attempt) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
      break;
    }
    (void)close(fd);
    usleep(100000u);
  }
  assert(attempt < 100);
  assert(send(fd, request, (size_t)written, 0) == written);
  received = recv(fd, response, sizeof(response) - 1u, 0);
  assert(received > 0);
  response[received] = '\0';
  (void)close(fd);
  assert(strstr(response, " 308 ") != NULL);
  written = snprintf(location, sizeof(location),
                     "location: https://localhost:%u/secure?from=http\r\n",
                     (unsigned)tls_port);
  assert(written > 0 && (size_t)written < sizeof(location));
  assert(strstr(response, location) != NULL);
}

static void assert_http_redirect_rejected(unsigned short port) {
  struct sockaddr_in address;
  const char request[] = "GET /secure HTTP/1.1\r\n"
                         "Host: attacker.example\r\n"
                         "Connection: close\r\n\r\n";
  char response[2048];
  ssize_t received;
  int fd;
  int attempt;

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
  for (attempt = 0; attempt < 100; ++attempt) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
      break;
    }
    (void)close(fd);
    usleep(100000u);
  }
  assert(attempt < 100);
  assert(send(fd, request, sizeof(request) - 1u, 0) ==
         (ssize_t)(sizeof(request) - 1u));
  received = recv(fd, response, sizeof(response) - 1u, 0);
  assert(received > 0);
  response[received] = '\0';
  (void)close(fd);
  assert(strstr(response, " 400 ") != NULL);
  assert(strstr(response, "location:") == NULL);
}

static int read_file(const char *path, char **out, size_t *out_size) {
  FILE *fp;
  long length;
  char *buffer;

  *out = NULL;
  *out_size = 0u;
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return 0;
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    (void)fclose(fp);
    return 0;
  }
  length = ftell(fp);
  if (length <= 0L || fseek(fp, 0L, SEEK_SET) != 0) {
    (void)fclose(fp);
    return 0;
  }
  buffer = (char *)malloc((size_t)length);
  if (buffer == NULL) {
    (void)fclose(fp);
    return 0;
  }
  if (fread(buffer, 1u, (size_t)length, fp) != (size_t)length) {
    (void)fclose(fp);
    free(buffer);
    return 0;
  }
  (void)fclose(fp);
  *out = buffer;
  *out_size = (size_t)length;
  return 1;
}

static void assert_https_fails(const char *url, const char *ca_bundle_path,
                               const char *client_bundle_path) {
  vectis_http_response response;
  vectis_error error;
  vectis_status status;

  memset(&response, 0, sizeof(response));
  status =
      https_get(url, ca_bundle_path, client_bundle_path, &response, &error);
  assert(status != VECTIS_OK);
  assert(error.source == VECTIS_ERROR_SOURCE_CURL);
  vectis_http_response_cleanup(&response);
}

static void assert_https_ok(const char *url, const char *ca_bundle_path,
                            const char *client_bundle_path) {
  vectis_http_response response;
  vectis_error error;
  vectis_status status;
  int attempt;

  memset(&response, 0, sizeof(response));
  status = VECTIS_ERR_STATE;
  for (attempt = 0; attempt < 100 && status != VECTIS_OK; ++attempt) {
    vectis_http_response_cleanup(&response);
    status =
        https_get(url, ca_bundle_path, client_bundle_path, &response, &error);
    if (status != VECTIS_OK) {
      usleep(100000u);
    }
  }
  if (status != VECTIS_OK) {
    fprintf(stderr, "https smoke failed: status=%s error=%s detail=%s\n",
            vectis_status_string(status), error.message, error.detail);
  }
  assert(status == VECTIS_OK);
  assert(response.status_code == 200L);
  assert(response.body_size == 2u);
  assert(memcmp(response.body, "ok", 2u) == 0);
  vectis_http_response_cleanup(&response);
}

static void assert_https_ca_source_ok(const char *url, vectis_source ca_source,
                                      const char *label) {
  vectis_http_response response;
  vectis_error error;
  vectis_status status;
  int attempt;

  memset(&response, 0, sizeof(response));
  status = VECTIS_ERR_STATE;
  for (attempt = 0; attempt < 100 && status != VECTIS_OK; ++attempt) {
    vectis_http_response_cleanup(&response);
    status = https_get_ca_source(url, ca_source, &response, &error);
    if (status != VECTIS_OK) {
      usleep(100000u);
    }
  }
  if (status != VECTIS_OK) {
    fprintf(stderr, "https %s CA smoke failed: status=%s error=%s detail=%s\n",
            label, vectis_status_string(status), error.message, error.detail);
  }
  assert(status == VECTIS_OK);
  assert(response.status_code == 200L);
  assert(response.body_size == 2u);
  assert(memcmp(response.body, "ok", 2u) == 0);
  vectis_http_response_cleanup(&response);
}

int main(void) {
  vectis_cert_bundle_config certs;
  vectis_app_config config;
  vectis_route_config route;
  vectis_error error;
  vectis_status status;
  vectis_app *app;
  const char root_bundle_path[] = "/tmp/vectis-runtime-root-bundle.pem";
  const char root_cert_path[] = "/tmp/vectis-runtime-root-cert.pem";
  const char root_key_path[] = "/tmp/vectis-runtime-root-key.pem";
  const char wrong_root_bundle_path[] =
      "/tmp/vectis-runtime-wrong-root-bundle.pem";
  const char wrong_root_cert_path[] = "/tmp/vectis-runtime-wrong-root-cert.pem";
  const char wrong_root_key_path[] = "/tmp/vectis-runtime-wrong-root-key.pem";
  const char intermediate_cert_path[] =
      "/tmp/vectis-runtime-intermediate-cert.pem";
  const char intermediate_key_path[] =
      "/tmp/vectis-runtime-intermediate-key.pem";
  const char server_cert_path[] = "/tmp/vectis-runtime-server-cert.pem";
  const char server_key_path[] = "/tmp/vectis-runtime-server-key.pem";
  char *root_cert_pem;
  size_t root_cert_pem_size;
  lc_source *root_cert_source;
  unsigned short port;
  unsigned short redirect_port;
  char localhost_url[128];
  char loopback_url[128];
  char redirect_url[128];

  app = NULL;
  assert(vectis_internal_kore_autoblock_mutex_recovers_worker_death());
  root_cert_pem = NULL;
  root_cert_pem_size = 0u;
  root_cert_source = NULL;
  port = test_port_from_env("VECTIS_TEST_HTTPS_PORT", 28443u);
  redirect_port = test_port_from_env("VECTIS_TEST_HTTPS_REDIRECT_PORT", 28080u);
  assert(redirect_port != port);
  format_secure_url(localhost_url, sizeof(localhost_url), "localhost", port);
  format_secure_url(loopback_url, sizeof(loopback_url), "127.0.0.1", port);
  format_clear_url(redirect_url, sizeof(redirect_url), "localhost",
                   redirect_port);
  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "Vectis Runtime Root CA";
  certs.output_bundle_path = root_bundle_path;
  certs.output_cert_path = root_cert_path;
  certs.output_key_path = root_key_path;
  certs.is_ca = 1;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_OK);

  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "Vectis Runtime Wrong Root CA";
  certs.output_bundle_path = wrong_root_bundle_path;
  certs.output_cert_path = wrong_root_cert_path;
  certs.output_key_path = wrong_root_key_path;
  certs.is_ca = 1;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_OK);

  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "Vectis Runtime Intermediate CA";
  certs.output_cert_path = intermediate_cert_path;
  certs.output_key_path = intermediate_key_path;
  certs.ca_cert_path = root_cert_path;
  certs.ca_key_path = root_key_path;
  certs.is_ca = 1;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_OK);

  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "localhost";
  certs.dns_names = "localhost";
  certs.ip_addresses = "127.0.0.1";
  certs.output_cert_path = server_cert_path;
  certs.output_key_path = server_key_path;
  certs.ca_cert_path = intermediate_cert_path;
  certs.ca_key_path = intermediate_key_path;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_OK);

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_MANUAL;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  config.tls.http_redirect_enabled = 1;
  config.tls.http_redirect_port = redirect_port;
  config.tls.domain = "localhost";
  config.tls.certificate_path = server_cert_path;
  config.tls.private_key_path = server_key_path;
  config.tls.ca_bundle_path = intermediate_cert_path;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/secure", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);

  assert_https_ok(localhost_url, root_cert_path, NULL);
  assert_http_redirect(redirect_port, port);
  assert_http_redirect_rejected(redirect_port);
  assert_https_ok(redirect_url, root_cert_path, NULL);
  assert(read_file(root_cert_path, &root_cert_pem, &root_cert_pem_size));
  assert_https_ca_source_ok(
      localhost_url,
      vectis_source_from_memory(root_cert_pem, root_cert_pem_size), "memory");
  assert(lc_source_from_memory(root_cert_pem, root_cert_pem_size,
                               &root_cert_source, NULL) == LC_OK);
  assert_https_ca_source_ok(
      localhost_url, vectis_source_from_lc(root_cert_source), "lc_source");
  lc_source_close(root_cert_source);
  root_cert_source = NULL;
  free(root_cert_pem);
  root_cert_pem = NULL;
  assert_https_fails(loopback_url, NULL, NULL);
  assert_https_fails(localhost_url, wrong_root_cert_path, NULL);
  assert_https_fails(loopback_url, root_cert_path, NULL);

  status = vectis_stop(app, &error);
  assert(status == VECTIS_OK);
  app->close(app);

  (void)remove(root_bundle_path);
  (void)remove(root_cert_path);
  (void)remove(root_key_path);
  (void)remove(wrong_root_bundle_path);
  (void)remove(wrong_root_cert_path);
  (void)remove(wrong_root_key_path);
  (void)remove(intermediate_cert_path);
  (void)remove(intermediate_key_path);
  (void)remove(server_cert_path);
  (void)remove(server_key_path);
  if (root_cert_source != NULL) {
    lc_source_close(root_cert_source);
  }
  free(root_cert_pem);
  return 0;
}
