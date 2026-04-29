#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vectis/vectis.h>

static vectis_status sample_handler(vectis_app *app,
                                    vectis_request *request,
                                    vectis_response *response,
                                    void *userdata,
                                    vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok", error);
}

int main(void) {
  vectis_cert_bundle_config certs;
  vectis_app_config config;
  vectis_http_client_config http;
  vectis_http_response response;
  vectis_route_config route;
  vectis_error error;
  vectis_status status;
  vectis_app *app;
  int attempt;
  const char root_bundle_path[] = "/tmp/vectis-runtime-root-bundle.pem";
  const char root_cert_path[] = "/tmp/vectis-runtime-root-cert.pem";
  const char root_key_path[] = "/tmp/vectis-runtime-root-key.pem";
  const char intermediate_cert_path[] = "/tmp/vectis-runtime-intermediate-cert.pem";
  const char intermediate_key_path[] = "/tmp/vectis-runtime-intermediate-key.pem";
  const char server_cert_path[] = "/tmp/vectis-runtime-server-cert.pem";
  const char server_key_path[] = "/tmp/vectis-runtime-server-key.pem";

  app = NULL;
  memset(&response, 0, sizeof(response));
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
  certs.subject.common_name = "127.0.0.1";
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
  config.tls.port = 28443u;
  config.tls.certificate_path = server_cert_path;
  config.tls.private_key_path = server_key_path;
  config.tls.ca_bundle_path = intermediate_cert_path;
  app = vectis_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/secure", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = vectis_start(app, &error);
  assert(status == VECTIS_OK);

  vectis_http_client_config_init(&http);
  http.ca_bundle_path = root_cert_path;
  http.timeout_ms = 1000L;
  http.connect_timeout_ms = 200L;
  status = VECTIS_ERR_STATE;
  for (attempt = 0; attempt < 100 && status != VECTIS_OK; ++attempt) {
    vectis_http_response_cleanup(&response);
    status = vectis_http_get(&http, "https://127.0.0.1:28443/secure", &response, &error);
    if (status != VECTIS_OK) {
      usleep(100000u);
    }
  }
  if (status != VECTIS_OK) {
    fprintf(stderr, "https smoke failed: status=%s error=%s detail=%s\n",
            vectis_status_string(status),
            error.message,
            error.detail);
  }
  assert(status == VECTIS_OK);
  assert(response.status_code == 200L);
  assert(response.body_size == 2u);
  assert(memcmp(response.body, "ok", 2u) == 0);
  vectis_http_response_cleanup(&response);

  status = vectis_stop(app, &error);
  assert(status == VECTIS_OK);
  vectis_destroy(app);
  (void)remove(root_bundle_path);
  (void)remove(root_cert_path);
  (void)remove(root_key_path);
  (void)remove(intermediate_cert_path);
  (void)remove(intermediate_key_path);
  (void)remove(server_cert_path);
  (void)remove(server_key_path);
  return 0;
}
