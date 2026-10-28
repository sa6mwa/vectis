#include <cstring>

#include <vectis/auth.h>
#include <vectis/buffer.h>
#include <vectis/compression.h>
#include <vectis/crypto.h>
#include <vectis/curl.h>
#include <vectis/digest.h>
#include <vectis/gz.h>
#include <vectis/pdf.h>
#include <vectis/pg.h>
#include <vectis/png.h>
#include <vectis/proxy.h>
#include <vectis/sqlite.h>
#include <vectis/storage.h>
#include <vectis/supervisor.h>
#include <vectis/totp_qr.h>
#include <vectis/vectis.h>
#include <vectis/webdav.h>
#include <vectis/xml.h>
#include <vectis/zlib.h>
#include <vectis/zlib_raw.h>

int main() {
  vectis_crypto_gcm_result crypto_result = {};
  vectis_crypto_gcm_result_cleanup(&crypto_result);
  vectis_pg_config pg_config;
  vectis_pg_config_init(&pg_config);
  if (pg_config.connect_timeout_ms != 5000 || pg_config.io_timeout_ms != 30000)
    return 1;
  vectis_png_limits png_limits;
  vectis_png_limits_init(&png_limits);
  if (png_limits.max_width != 16384 || png_limits.max_row_bytes != 65536)
    return 1;
  vectis_compression_config compression_config;
  vectis_compression_config_init(&compression_config);
  if (compression_config.max_window_log != 26 ||
      compression_config.memory_limit != 67108864)
    return 1;
  vectis_pdf_page_config pdf_page;
  vectis_pdf_page_config_init(&pdf_page);
  if (pdf_page.width < 595 || pdf_page.height < 841)
    return 1;
  vectis_sqlite_config sqlite_config;
  vectis_sqlite_config_init(&sqlite_config);
  if (std::strcmp(sqlite_config.mode, "readwrite_create") != 0 ||
      sqlite_config.busy_timeout_ms != 0)
    return 1;
  vectis_app_config app_config;
  vectis_http_client_config http_config;
  vectis_proxy_route_config proxy_config;
  vectis_supervisor_config supervisor_config;
  vectis_auth_store_config auth_store_config;
  vectis_auth_issue_config auth_issue_config;
  vectis_webdav_auth_provider_config webdav_auth_provider_config;
  vectis_totp totp;
  vectis_error error;
  vectis_app *app;
  vectis_http_client *client;

  std::memset(&error, 0, sizeof(error));
  vectis_app_config_init(&app_config);
  vectis_http_client_config_init(&http_config);
  vectis_proxy_route_config_init(&proxy_config);
  vectis_supervisor_config_init(&supervisor_config);
  if (supervisor_config.capacity == 0u) {
    return 7;
  }
  vectis_auth_store_config_init(&auth_store_config);
  vectis_auth_issue_config_init(&auth_issue_config);
  vectis_webdav_auth_provider_config_init(&webdav_auth_provider_config);
  std::memset(&totp, 0, sizeof(totp));
  if (vectis_totp_init(&totp, "JBSWY3DPEHPK3PXP") != VECTIS_TOTP_QR_OK) {
    return 5;
  }
  if (std::strcmp(vectis_totp_qr_status_string(VECTIS_TOTP_QR_OK), "ok") != 0) {
    return 6;
  }

  app = vectis_app_new(&app_config, &error);
  if (app == 0) {
    return 1;
  }
  if (app->route_count == 0 || app->logger == 0 || app->close == 0 ||
      app->proxy_route == 0 || proxy_config.path != 0) {
    app->close(app);
    return 2;
  }
  app->close(app);

  client = 0;
  if (vectis_http_client_new(&http_config, &client, &error) != VECTIS_OK) {
    return 3;
  }
  if (client == 0 || client->get == 0 || client->del == 0 ||
      client->close == 0) {
    if (client != 0) {
      client->close(client);
    }
    return 4;
  }
  client->close(client);
  return 0;
}
