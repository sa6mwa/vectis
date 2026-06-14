#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static vectis_status https_get_client_source(const char *url,
                                             const char *ca_bundle_path,
                                             vectis_source client_bundle,
                                             vectis_http_response *response,
                                             vectis_error *error) {
  vectis_http_client_config http;

  vectis_http_client_config_init(&http);
  http.ca_bundle_path = ca_bundle_path;
  http.client_bundle = client_bundle;
  http.timeout_ms = 1000L;
  http.connect_timeout_ms = 200L;
  return vectis_http_get(&http, url, response, error);
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
    fprintf(stderr, "https mtls smoke failed: status=%s error=%s detail=%s\n",
            vectis_status_string(status), error.message, error.detail);
  }
  assert(status == VECTIS_OK);
  assert(response.status_code == 200L);
  assert(response.body_size == 2u);
  assert(memcmp(response.body, "ok", 2u) == 0);
  vectis_http_response_cleanup(&response);
}

static void assert_https_client_source_ok(const char *url,
                                          const char *ca_bundle_path,
                                          vectis_source client_source,
                                          const char *label) {
  vectis_http_response response;
  vectis_error error;
  vectis_status status;
  int attempt;

  memset(&response, 0, sizeof(response));
  status = VECTIS_ERR_STATE;
  for (attempt = 0; attempt < 100 && status != VECTIS_OK; ++attempt) {
    vectis_http_response_cleanup(&response);
    status = https_get_client_source(url, ca_bundle_path, client_source,
                                     &response, &error);
    if (status != VECTIS_OK) {
      usleep(100000u);
    }
  }
  if (status != VECTIS_OK) {
    fprintf(
        stderr,
        "https mtls %s client source failed: status=%s error=%s detail=%s\n",
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
  const char root_bundle_path[] = "/tmp/vectis-mtls-root-bundle.pem";
  const char root_cert_path[] = "/tmp/vectis-mtls-root-cert.pem";
  const char root_key_path[] = "/tmp/vectis-mtls-root-key.pem";
  const char wrong_root_bundle_path[] =
      "/tmp/vectis-mtls-wrong-root-bundle.pem";
  const char wrong_root_cert_path[] = "/tmp/vectis-mtls-wrong-root-cert.pem";
  const char wrong_root_key_path[] = "/tmp/vectis-mtls-wrong-root-key.pem";
  const char server_cert_path[] = "/tmp/vectis-mtls-server-cert.pem";
  const char server_key_path[] = "/tmp/vectis-mtls-server-key.pem";
  const char client_bundle_path[] = "/tmp/vectis-mtls-client-bundle.pem";
  const char wrong_client_bundle_path[] =
      "/tmp/vectis-mtls-wrong-client-bundle.pem";
  char *client_bundle_pem;
  size_t client_bundle_pem_size;
  lc_source *client_bundle_source;

  app = NULL;
  client_bundle_pem = NULL;
  client_bundle_pem_size = 0u;
  client_bundle_source = NULL;
  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "Vectis Runtime mTLS Root CA";
  certs.output_bundle_path = root_bundle_path;
  certs.output_cert_path = root_cert_path;
  certs.output_key_path = root_key_path;
  certs.is_ca = 1;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_OK);

  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "Vectis Runtime mTLS Wrong Root CA";
  certs.output_bundle_path = wrong_root_bundle_path;
  certs.output_cert_path = wrong_root_cert_path;
  certs.output_key_path = wrong_root_key_path;
  certs.is_ca = 1;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_OK);

  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "127.0.0.1";
  certs.ip_addresses = "127.0.0.1";
  certs.output_cert_path = server_cert_path;
  certs.output_key_path = server_key_path;
  certs.ca_cert_path = root_cert_path;
  certs.ca_key_path = root_key_path;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_OK);

  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "vectis-runtime-wrong-client";
  certs.output_bundle_path = wrong_client_bundle_path;
  certs.ca_cert_path = wrong_root_cert_path;
  certs.ca_key_path = wrong_root_key_path;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_OK);

  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "vectis-runtime-client";
  certs.output_bundle_path = client_bundle_path;
  certs.ca_cert_path = root_cert_path;
  certs.ca_key_path = root_key_path;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_OK);

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_MANUAL;
  config.tls.bind = "127.0.0.1";
  config.tls.port = 28444u;
  config.tls.certificate_path = server_cert_path;
  config.tls.private_key_path = server_key_path;
  config.tls.client_ca_bundle_path = root_cert_path;
  config.tls.require_client_certificate = 1;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/secure", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = app->start(app, &error);
  assert(status == VECTIS_OK);

  assert_https_fails("https://127.0.0.1:28444/secure", root_cert_path, NULL);
  assert_https_fails("https://127.0.0.1:28444/secure", root_cert_path,
                     wrong_client_bundle_path);
  assert_https_ok("https://127.0.0.1:28444/secure", root_cert_path,
                  client_bundle_path);
  assert(read_file(client_bundle_path, &client_bundle_pem,
                   &client_bundle_pem_size));
  assert_https_client_source_ok(
      "https://127.0.0.1:28444/secure", root_cert_path,
      vectis_source_from_memory(client_bundle_pem, client_bundle_pem_size),
      "memory");
  assert(lc_source_from_memory(client_bundle_pem, client_bundle_pem_size,
                               &client_bundle_source, NULL) == LC_OK);
  assert_https_client_source_ok(
      "https://127.0.0.1:28444/secure", root_cert_path,
      vectis_source_from_lc(client_bundle_source), "lc_source");
  lc_source_close(client_bundle_source);
  client_bundle_source = NULL;
  free(client_bundle_pem);
  client_bundle_pem = NULL;

  status = vectis_stop(app, &error);
  assert(status == VECTIS_OK);
  app->close(app);

  (void)remove(root_bundle_path);
  (void)remove(root_cert_path);
  (void)remove(root_key_path);
  (void)remove(wrong_root_bundle_path);
  (void)remove(wrong_root_cert_path);
  (void)remove(wrong_root_key_path);
  (void)remove(server_cert_path);
  (void)remove(server_key_path);
  (void)remove(client_bundle_path);
  (void)remove(wrong_client_bundle_path);
  if (client_bundle_source != NULL) {
    lc_source_close(client_bundle_source);
  }
  free(client_bundle_pem);
  return 0;
}
