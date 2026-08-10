#include <cstring>

#include <vectis/auth.h>
#include <vectis/vectis.h>

int main() {
  vectis_app_config app_config;
  vectis_http_client_config http_config;
  vectis_auth_store_config auth_store_config;
  vectis_auth_issue_config auth_issue_config;
  vectis_error error;
  vectis_app *app;
  vectis_http_client *client;

  std::memset(&error, 0, sizeof(error));
  vectis_app_config_init(&app_config);
  vectis_http_client_config_init(&http_config);
  vectis_auth_store_config_init(&auth_store_config);
  vectis_auth_issue_config_init(&auth_issue_config);

  app = vectis_app_new(&app_config, &error);
  if (app == 0) {
    return 1;
  }
  if (app->route_count == 0 || app->logger == 0 || app->close == 0) {
    app->close(app);
    return 2;
  }
  app->close(app);

  client = 0;
  if (vectis_http_client_new(&http_config, &client, &error) != VECTIS_OK) {
    return 3;
  }
  if (client == 0 || client->get == 0 || client->del == 0 || client->close == 0) {
    if (client != 0) {
      client->close(client);
    }
    return 4;
  }
  client->close(client);
  return 0;
}
