#include <curl/curl.h>
#include <lc/lc.h>
#include <libssh2.h>
#include <libxml/parser.h>
#include <lonejson.h>
#include <openssl/ssl.h>
#include <pslog.h>
#include <vectis/vectis.h>

int main(void) {
  lc_client_config lockd;
  pslog_config logs;
  CURL *curl;
  SSL_CTX *ssl_ctx;

  lc_client_config_init(&lockd);
  pslog_default_config(&logs);

  curl = curl_easy_init();
  if (curl != NULL) {
    curl_easy_cleanup(curl);
  }

  ssl_ctx = SSL_CTX_new(TLS_client_method());
  if (ssl_ctx != NULL) {
    SSL_CTX_free(ssl_ctx);
  }

  (void)libssh2_init(0);
  libssh2_exit();
  (void)LONEJSON_VERSION_MAJOR;
  (void)xmlParserVersion;
  return 0;
}
