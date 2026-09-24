#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vectis/proxy.h>

int main(void) {
  vectis_app_config app_config;
  vectis_proxy_route_config proxy_config;
  vectis_app *app;
  vectis_error error;

  vectis_app_config_init(&app_config);
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  vectis_proxy_route_config_init(&proxy_config);
  proxy_config.path = "/proxy";
  proxy_config.target = "http://upstream.test";
  assert(app->proxy_route(app, &proxy_config, &error) ==
         VECTIS_ERR_NOT_IMPLEMENTED);
  assert(strstr(error.message, "Kore runtime") != NULL);
  assert(app->route_count(app) == 0u);
  app->close(app);
  puts("proxy no-kore registration ok");
  return 0;
}
