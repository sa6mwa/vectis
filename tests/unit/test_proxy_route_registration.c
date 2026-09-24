#include "vectis_internal.h"
#include "vectis_proxy_route_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vectis/proxy.h>

static vectis_app *new_app(vectis_error *error) {
  vectis_app_config config;

  vectis_app_config_init(&config);
  return vectis_app_new(&config, error);
}

static void test_route_ownership_and_selection(void) {
  char primary[] = "https://upstream.test/base";
  char alternate[] = "http://backup.test/other";
  const char *alternates[1];
  vectis_proxy_route_config config;
  vectis_proxy_route_data *data;
  vectis_route_handler_fn selected;
  vectis_body_policy body;
  vectis_request *request;
  vectis_response *response;
  vectis_app *app;
  vectis_error error;
  void *userdata;

  app = new_app(&error);
  assert(app != NULL && app->proxy_route != NULL);
  alternates[0] = alternate;
  vectis_proxy_route_config_init(&config);
  config.path = "/proxy/:id";
  config.path_kind = VECTIS_ROUTE_PATH_PARAMS;
  config.methods = VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_POST;
  config.target = primary;
  config.alternate_targets = alternates;
  config.alternate_target_count = 1u;
  assert(app->proxy_route(app, &config, &error) == VECTIS_OK);
  assert(app->route_count(app) == 1u);
  primary[8] = 'X';
  alternate[7] = 'X';

  request = vectis_internal_request_new(&error);
  response = vectis_internal_response_new(&error);
  assert(request != NULL && response != NULL);
  selected = NULL;
  userdata = NULL;
  assert(vectis_internal_route_body_policy(app, VECTIS_HTTP_GET, "/proxy/a",
                                           &body, NULL, &selected, &userdata,
                                           request, &error) == VECTIS_OK);
  assert(selected == vectis_proxy_route_marker);
  assert(body.mode == VECTIS_BODY_NONE);
  data = (vectis_proxy_route_data *)userdata;
  assert(data != NULL && data->target_count == 2u);
  assert(strcmp(data->targets[0], "https://upstream.test/base") == 0);
  assert(strcmp(data->targets[1], "http://backup.test/other") == 0);
  assert(data->upstream_http_version == VECTIS_PROXY_HTTP_AUTO);
  assert(data->connect_timeout_ms == 10000L);
  assert(data->idle_timeout_ms == 60000L);
  assert(data->total_timeout_ms == 0L);
  assert(data->buffer_limit_bytes == 16384u);
  assert(strcmp(vectis_request_path_param(request, "id"), "a") == 0);

  vectis_internal_request_cleanup(request);
  assert(vectis_internal_proxy_raw_path_match(
             app, VECTIS_HTTP_GET, "/proxy/a%2Fb", vectis_proxy_route_marker,
             request, &userdata, &error) == VECTIS_OK);
  assert(userdata == data);
  assert(strcmp(vectis_request_path_param(request, "id"), "a%2Fb") == 0);

  assert(vectis_proxy_route_marker(app, request, response, data, &error) ==
         VECTIS_ERR_STATE);
  assert(strstr(error.message, "request headers") != NULL);
  assert(app->proxy_route(app, &config, &error) == VECTIS_ERR_CONFLICT);

  vectis_internal_response_free(response);
  vectis_internal_request_free(request);
  app->close(app);
}

static void test_invalid_targets(void) {
  static const char *const invalid[] = {"http://user@host.test/",
                                        "https://host.test/path?query=1",
                                        "https://host.test/path#fragment",
                                        "ftp://host.test/",
                                        "https://host.test:bad/",
                                        "https://host.test/../base",
                                        "http:///missing-host",
                                        "host.test"};
  vectis_proxy_route_config config;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  size_t i;

  app = new_app(&error);
  assert(app != NULL);
  vectis_proxy_route_config_init(&config);
  config.path = "/proxy";
  for (i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    config.target = invalid[i];
    status = vectis_register_proxy_route(app, &config, &error);
    if (status != VECTIS_ERR_INVALID) {
      fprintf(stderr, "accepted invalid target %s: %d (%s)\n", invalid[i],
              (int)status, error.message);
    }
    assert(status == VECTIS_ERR_INVALID);
    assert(app->route_count(app) == 0u);
  }
  config.target = "https://upstream.test";
  config.connect_timeout_ms = -1L;
  assert(app->proxy_route(app, &config, &error) == VECTIS_ERR_INVALID);
  config.connect_timeout_ms = 0L;
  config.buffer_limit_bytes = 4096u;
  assert(app->proxy_route(app, &config, &error) == VECTIS_ERR_INVALID);
  config.buffer_limit_bytes = 0u;
  assert(app->proxy_route(app, &config, &error) == VECTIS_OK);
  assert(app->proxy_route(app, &config, &error) == VECTIS_ERR_CONFLICT);
  app->close(app);
}

int main(void) {
  test_route_ownership_and_selection();
  test_invalid_targets();
  puts("proxy route registration ok");
  return 0;
}
