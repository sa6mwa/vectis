#include "vectis_internal.h"
#include "vectis_proxy_select.h"

#include <assert.h>
#include <string.h>
#include <vectis/proxy.h>

static vectis_status ordinary_handler(vectis_app *app, vectis_request *request,
                                      vectis_response *response, void *userdata,
                                      vectis_error *error) {
  (void)app;
  (void)request;
  (void)response;
  (void)userdata;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static void websocket_message(vectis_app *app, vectis_websocket *websocket,
                              vectis_websocket_opcode opcode, const void *data,
                              size_t len, void *userdata) {
  (void)app;
  (void)websocket;
  (void)opcode;
  (void)data;
  (void)len;
  (void)userdata;
}

static void test_selection(int proxy_first) {
  vectis_proxy_route_config proxy;
  vectis_proxy_route_data *selected;
  vectis_websocket_route_config websocket;
  vectis_route_config ordinary;
  vectis_app_config app_config;
  vectis_request *request;
  vectis_app *app;
  vectis_error error;

  vectis_app_config_init(&app_config);
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  vectis_proxy_route_config_init(&proxy);
  proxy.path = "/proxy/:id";
  proxy.path_kind = VECTIS_ROUTE_PATH_PARAMS;
  proxy.target = "https://upstream.test/base";
  ordinary =
      vectis_route(VECTIS_HTTP_GET, "^/proxy/.*$", ordinary_handler, NULL);
  ordinary.path_kind = VECTIS_ROUTE_PATH_REGEX;
  if (proxy_first) {
    assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
    assert(app->route(app, &ordinary, &error) == VECTIS_OK);
  } else {
    assert(app->route(app, &ordinary, &error) == VECTIS_OK);
    assert(app->proxy_route(app, &proxy, &error) == VECTIS_OK);
  }
  websocket = vectis_websocket_route("/proxy/ws", websocket_message, NULL);
  assert(vectis_register_websocket(app, &websocket, &error) == VECTIS_OK);

  request = vectis_internal_request_new(&error);
  assert(request != NULL);
  selected = NULL;
  assert(vectis_proxy_select_route(app, VECTIS_HTTP_GET, "/proxy/a", request,
                                   &selected, &error) == VECTIS_OK);
  assert((selected != NULL) == proxy_first);
  if (proxy_first) {
    assert(strcmp(selected->targets[0], "https://upstream.test/base") == 0);
    assert(strcmp(vectis_request_path_param(request, "id"), "a") == 0);
  }
  vectis_internal_request_cleanup(request);

  selected = NULL;
  assert(vectis_proxy_select_route(app, VECTIS_HTTP_GET, "/proxy/a%2Fb",
                                   request, &selected, &error) == VECTIS_OK);
  assert(selected != NULL);
  assert(strcmp(vectis_request_path_param(request, "id"), "a%2Fb") == 0);
  vectis_internal_request_cleanup(request);

  selected = NULL;
  assert(vectis_proxy_select_route(app, VECTIS_HTTP_GET, "/proxy/ws", request,
                                   &selected, &error) == VECTIS_OK);
  assert(selected == NULL);
  vectis_internal_request_cleanup(request);

  selected = NULL;
  assert(vectis_proxy_select_route(app, VECTIS_HTTP_GET, "/proxy/%2e%2e",
                                   request, &selected, &error) == VECTIS_OK);
  assert(selected == NULL);
  assert(vectis_proxy_select_route(app, VECTIS_HTTP_GET, "/proxy/%ZZ", request,
                                   &selected, &error) == VECTIS_OK);
  assert(selected == NULL);
  vectis_internal_request_free(request);
  app->close(app);
}

int main(void) {
  test_selection(0);
  test_selection(1);
  return 0;
}
