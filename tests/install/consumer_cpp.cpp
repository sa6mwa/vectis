#include <cstring>

#include <vectis/vectis.h>

int main() {
  vectis_app_config app_config;
  vectis_http_client_config http_config;
  vectis_error error;
  vectis_app *app;
  vectis_http_client *http;

  std::memset(&error, 0, sizeof(error));
  vectis_app_config_init(&app_config);
  app = vectis_app_new(&app_config, &error);
  if (app == 0) {
    return 1;
  }
  if (app->route_count == 0 ||
      app->route_count(app) != 0u ||
      app->logger == 0 ||
      app->logger(app) == 0 ||
      app->close == 0) {
    app->close(app);
    return 2;
  }
  app->close(app);

  http = 0;
  vectis_http_client_config_init(&http_config);
  if (vectis_http_client_new(&http_config, &http, &error) != VECTIS_OK) {
    return 3;
  }
  if (http == 0 ||
      http->execute == 0 ||
      http->get == 0 ||
      http->del == 0 ||
      http->post_json == 0 ||
      http->close == 0) {
    if (http != 0) {
      http->close(http);
    }
    return 4;
  }
  http->close(http);
  return error.code == VECTIS_OK ? 0 : 5;
}
