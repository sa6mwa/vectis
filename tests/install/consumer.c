#include <stddef.h>
#include <string.h>

#include <curl/curl.h>
#include <kore/kore.h>
#include <lc/lc.h>
#include <libssh2.h>
#include <lonejson.h>
#include <openssl/ssl.h>
#include <pslog.h>
#include <vectis/vectis.h>

int main(void) {
  vectis_app_config config;
  vectis_error error;
  lonejson_error json_error;
  struct lc_client *client;
  struct kore_server *server;
  LIBSSH2_SESSION *ssh;
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
  if (vectis_status_string(VECTIS_OK) == NULL ||
      vectis_http_method_string(VECTIS_HTTP_GET) == NULL ||
      vectis_body_mode_string(VECTIS_BODY_JSON) == NULL) {
    return 2;
  }
  if (lonejson_validate_cstr("{}", &json_error) != LONEJSON_STATUS_OK) {
    return 3;
  }

  client = NULL;
  server = NULL;
  ssh = NULL;
  ssl = NULL;
  curl_code = CURLE_OK;

  if (client != NULL || server != NULL || ssh != NULL || ssl != NULL ||
      curl_code != CURLE_OK || logger.impl != NULL) {
    return 4;
  }
  return error.code == VECTIS_OK ? 0 : 5;
}
