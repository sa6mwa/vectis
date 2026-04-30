#include <stddef.h>
#include <string.h>

#include <curl/curl.h>
#include <kore/kore.h>
#include <lc/lc.h>
#include <libssh2.h>
#include <libxml/parser.h>
#include <lonejson.h>
#include <openssl/ssl.h>
#include <pslog.h>
#include <vectis/vectis.h>

int main(void) {
  vectis_app_config config;
  vectis_http_client_config http_config;
  vectis_error error;
  vectis_app *app;
  vectis_http_client *http;
  lonejson_error json_error;
  struct lc_client *client;
  struct kore_server *server;
  LIBSSH2_SESSION *ssh;
  const char *xml_version;
  SSL_CTX *ssl;
  CURLcode curl_code;
  pslog_logger logger;

  memset(&error, 0, sizeof(error));
  memset(&json_error, 0, sizeof(json_error));
  memset(&logger, 0, sizeof(logger));

  vectis_app_config_init(&config);
  if (config.app_name == NULL || strcmp(config.app_name, "vectis") != 0) {
    return 1;
  }
  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    return 2;
  }
  if (app->route_count == NULL ||
      app->route_count(app) != 0u ||
      app->logger == NULL ||
      app->logger(app) == NULL ||
      app->close == NULL) {
    app->close(app);
    return 3;
  }
  app->close(app);

  http = NULL;
  vectis_http_client_config_init(&http_config);
  if (vectis_http_client_new(&http_config, &http, &error) != VECTIS_OK) {
    return 4;
  }
  if (http == NULL ||
      http->execute == NULL ||
      http->get == NULL ||
      http->del == NULL ||
      http->post_json == NULL ||
      http->close == NULL) {
    if (http != NULL) {
      http->close(http);
    }
    return 5;
  }
  http->close(http);

  if (vectis_status_string(VECTIS_OK) == NULL ||
      vectis_http_method_string(VECTIS_HTTP_GET) == NULL ||
      vectis_body_mode_string(VECTIS_BODY_JSON) == NULL) {
    return 6;
  }
  if (lonejson_validate_cstr("{}", &json_error) != LONEJSON_STATUS_OK) {
    return 7;
  }

  client = NULL;
  server = NULL;
  ssh = NULL;
  xml_version = xmlParserVersion;
  ssl = NULL;
  curl_code = CURLE_OK;

  if (client != NULL || server != NULL || ssh != NULL || ssl != NULL ||
      curl_code != CURLE_OK || logger.impl != NULL || xml_version == NULL) {
    return 8;
  }
  return error.code == VECTIS_OK ? 0 : 9;
}
