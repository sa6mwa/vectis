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

static void *read_file_alloc(const char *path, size_t *out_size) {
  FILE *fp;
  void *buffer;
  long len;

  assert(out_size != NULL);
  *out_size = 0u;
  fp = fopen(path, "rb");
  assert(fp != NULL);
  assert(fseek(fp, 0L, SEEK_END) == 0);
  len = ftell(fp);
  assert(len > 0L);
  assert(fseek(fp, 0L, SEEK_SET) == 0);
  buffer = malloc((size_t)len);
  assert(buffer != NULL);
  assert(fread(buffer, 1u, (size_t)len, fp) == (size_t)len);
  assert(fclose(fp) == 0);
  *out_size = (size_t)len;
  return buffer;
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
  void *bundle;
  size_t bundle_size;
  int attempt;
  const char bundle_path[] = "/tmp/vectis-runtime-bundle.pem";
  const char cert_path[] = "/tmp/vectis-runtime-cert.pem";
  const char key_path[] = "/tmp/vectis-runtime-key.pem";

  app = NULL;
  bundle = NULL;
  bundle_size = 0u;
  memset(&response, 0, sizeof(response));
  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "127.0.0.1";
  certs.ip_addresses = "127.0.0.1";
  certs.output_bundle_path = bundle_path;
  certs.output_cert_path = cert_path;
  certs.output_key_path = key_path;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  status = vectis_cert_generate_bundle(&certs, &error);
  assert(status == VECTIS_OK);

  bundle = read_file_alloc(bundle_path, &bundle_size);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_MANUAL;
  config.tls.bind = "127.0.0.1";
  config.tls.port = 28443u;
  config.tls.cert_key_bundle = vectis_source_from_memory(bundle, bundle_size);
  app = vectis_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/secure", sample_handler, NULL);
  status = vectis_register_route(app, &route, &error);
  assert(status == VECTIS_OK);
  status = vectis_start(app, &error);
  assert(status == VECTIS_OK);

  vectis_http_client_config_init(&http);
  http.ca_bundle_path = cert_path;
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
  free(bundle);
  (void)remove(bundle_path);
  (void)remove(cert_path);
  (void)remove(key_path);
  return 0;
}
