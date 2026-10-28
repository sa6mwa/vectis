#include <stddef.h>
#include <string.h>

#include <cai/cai.h>
#include <curl/curl.h>
#include <kore/kore.h>
#include <lc/lc.h>
#include <lc/postgres.h>
#include <libmdf.h>
#include <libssh2.h>
#include <libxml/parser.h>
#include <lonejson.h>
#include <openssl/ssl.h>
#include <pslog.h>
#include <softline/softline.h>
#include <vectis/digest.h>
#include <vectis/pg.h>
#include <vectis/proxy.h>
#include <vectis/storage.h>
#include <vectis/supervisor.h>
#include <vectis/vectis.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#error "libvectis installed C consumer must compile in C89 mode"
#endif

int main(void) {
  vectis_app_config config;
  vectis_http_client_config http_config;
  vectis_proxy_route_config proxy_config;
  vectis_supervisor_config supervisor_config;
  vectis_error error;
  vectis_app *app;
  vectis_supervisor *scope;
  vectis_supervisor_channel *channel;
  vectis_supervisor_event event;
  char frame[4];
  vectis_http_client *http;
  struct lc_client *client;
  struct kore_server *server;
  LIBSSH2_SESSION *ssh;
  const char *xml_version;
  SSL_CTX *ssl;
  CURLcode curl_code;
  cai_client_config cai_config;
  sl_config_t softline_config;
  pslog_logger logger;
  mdf_options markdown_options;
  mdf *markdown;
  mdf_status markdown_status;
  char *rendered;
  int markdown_ok;
  vectis_pg *pg;
  lc_collection *collection;
  vectis_digest_result digest;

  memset(&error, 0, sizeof(error));
  memset(&logger, 0, sizeof(logger));
  /* Force the new public adapter and participant into both consumer closures,
   * rather than proving only that their headers can be included. */
  pg = NULL;
  collection = NULL;
  if (lc_postgres_driver() == NULL ||
      vectis_pg_from_borrow(NULL, &pg, &error) != VECTIS_ERR_INVALID ||
      pg != NULL ||
      vectis_app_create_collection(NULL, NULL, &collection, &error) !=
          VECTIS_ERR_INVALID ||
      collection != NULL) {
    return 14;
  }
  if (vectis_digest_materialized("sha256", "abc", 3u, &digest, &error) !=
          VECTIS_OK ||
      digest.bytes != 3u || digest.size != 32u || digest.data[0] != 0xbau ||
      digest.data[31] != 0xadu) {
    return 15;
  }
  vectis_error_clear(&error);

  mdf_options_init(&markdown_options);
  markdown = NULL;
  if (mdf_create(MDF_FORMAT_HTML, &markdown_options, &markdown) != MDF_OK) {
    return 1;
  }
  rendered = NULL;
  markdown_status = markdown->render_cstr(
      markdown, "| value |\n| --- |\n| `left\\|right` |\n", &rendered);
  markdown_ok = markdown_status == MDF_OK && rendered != NULL &&
                strstr(rendered, ">left|right</") != NULL;
  if (rendered != NULL) {
    markdown->string_free(markdown, rendered);
  }
  markdown->destroy(markdown);
  if (!markdown_ok) {
    return 1;
  }

  vectis_app_config_init(&config);
  vectis_proxy_route_config_init(&proxy_config);
  vectis_supervisor_config_init(&supervisor_config);
  if (supervisor_config.capacity == 0u) {
    return 12;
  }
  if (proxy_config.path != NULL ||
      proxy_config.upstream_http_version != VECTIS_PROXY_HTTP_AUTO ||
      proxy_config.tls_ca_pem != NULL ||
      proxy_config.tls_client_cert_pem != NULL ||
      proxy_config.tls_client_key_pem != NULL || proxy_config.rewrite != NULL ||
      proxy_config.preflight != NULL || proxy_config.modify_response != NULL ||
      proxy_config.on_error != NULL ||
      vectis_proxy_inbound_header_count(NULL) != 0u ||
      vectis_proxy_response_header_count(NULL) != 0u ||
      vectis_proxy_local_respond(NULL, 200, NULL, 0u, &error) !=
          VECTIS_ERR_INVALID ||
      vectis_proxy_local_add_header(NULL, "X-Test", "yes", &error) !=
          VECTIS_ERR_INVALID ||
      vectis_proxy_outbound_select_target(NULL, 0u, &error) !=
          VECTIS_ERR_INVALID) {
    return 11;
  }
  vectis_error_clear(&error);
  if (config.app_name == NULL || strcmp(config.app_name, "vectis") != 0) {
    return 1;
  }
  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    return 2;
  }
  if (app->route_count == NULL || app->route_count(app) != 0u ||
      app->logger == NULL || app->logger(app) == NULL ||
      app->proxy_route == NULL || app->close == NULL) {
    app->close(app);
    return 3;
  }
  supervisor_config.name = "installed";
  if (vectis_supervisor_new(app, &supervisor_config, &scope, &error) !=
          VECTIS_OK ||
      vectis_supervisor_channel_get(scope, &channel, &error) != VECTIS_OK ||
      vectis_supervisor_channel_send(channel, 42u, "sdk", 3u, &error) !=
          VECTIS_OK ||
      vectis_supervisor_channel_receive(channel, &event, frame, sizeof(frame),
                                        &error) != VECTIS_OK ||
      event.kind != 42u || event.payload_size != 3u ||
      memcmp(frame, "sdk", 3u) != 0) {
    app->close(app);
    return 13;
  }
  app->close(app);

  http = NULL;
  vectis_http_client_config_init(&http_config);
  if (vectis_http_client_new(&http_config, &http, &error) != VECTIS_OK) {
    return 4;
  }
  if (http == NULL || http->execute == NULL || http->get == NULL ||
      http->del == NULL || http->post_json == NULL || http->close == NULL) {
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

  cai_client_config_init(&cai_config);
  if (cai_config.base_url != NULL ||
      strcmp(CAI_MODEL_DEFAULT_RESPONSES, "") == 0) {
    return 7;
  }
  sl_config_init(&softline_config);
  if (softline_config.line_max_len == 0u) {
    return 8;
  }

  client = NULL;
  server = NULL;
  ssh = NULL;
  xml_version = xmlParserVersion;
  ssl = NULL;
  curl_code = CURLE_OK;

  if (client != NULL || server != NULL || ssh != NULL || ssl != NULL ||
      curl_code != CURLE_OK || logger.impl != NULL || xml_version == NULL) {
    return 9;
  }
  return error.code == VECTIS_OK ? 0 : 10;
}
