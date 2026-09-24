#include "vectis_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vectis/vectis.h>

static char ordinary_config;
static char proxy_config;
static char second_proxy_config;

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

static vectis_status proxy_marker(vectis_app *app, vectis_request *request,
                                  vectis_response *response, void *userdata,
                                  vectis_error *error) {
  (void)app;
  (void)request;
  (void)response;
  (void)userdata;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_app *make_app(int proxy_first, vectis_error *error) {
  vectis_app_config config;
  vectis_route_config ordinary;
  vectis_route_config proxy;
  vectis_app *app;

  vectis_app_config_init(&config);
  app = vectis_app_new(&config, error);
  assert(app != NULL);
  ordinary = vectis_route(VECTIS_HTTP_GET, "^/proxy/.*$", ordinary_handler,
                          &ordinary_config);
  ordinary.path_kind = VECTIS_ROUTE_PATH_REGEX;
  proxy =
      vectis_route(VECTIS_HTTP_GET, "/proxy/:id", proxy_marker, &proxy_config);
  proxy.path_kind = VECTIS_ROUTE_PATH_PARAMS;
  if (proxy_first) {
    assert(vectis_register_route(app, &proxy, error) == VECTIS_OK);
    assert(vectis_register_route(app, &ordinary, error) == VECTIS_OK);
  } else {
    assert(vectis_register_route(app, &ordinary, error) == VECTIS_OK);
    assert(vectis_register_route(app, &proxy, error) == VECTIS_OK);
  }
  assert(vectis_register_route(app, &proxy, error) == VECTIS_ERR_CONFLICT);
  proxy = vectis_route(VECTIS_HTTP_GET, "/proxy/other/:id", proxy_marker,
                       &second_proxy_config);
  proxy.path_kind = VECTIS_ROUTE_PATH_PARAMS;
  assert(vectis_register_route(app, &proxy, error) == VECTIS_OK);
  return app;
}

static void check_order(int proxy_first) {
  static const char *invalid[] = {
      "/proxy/%",     "/proxy/%ZZ",       "/proxy/%0a",    "/proxy/%5C",
      "/proxy/..",    "/proxy/%2e%2e",    "/proxy/a%2F..", "/proxy/%252e%252e",
      "/proxy/a?x=1", "/proxy/a#fragment"};
  vectis_route_handler_fn selected;
  vectis_body_policy policy;
  vectis_request *request;
  vectis_app *app;
  vectis_error error;
  vectis_status status;
  void *selected_userdata;
  size_t i;

  app = make_app(proxy_first, &error);
  request = vectis_internal_request_new(&error);
  assert(request != NULL);
  selected = NULL;
  selected_userdata = NULL;
  status = vectis_internal_route_body_policy(app, VECTIS_HTTP_GET, "/proxy/a",
                                             &policy, NULL, &selected,
                                             &selected_userdata, &error);
  assert(status == VECTIS_OK);
  assert(selected == (proxy_first ? proxy_marker : ordinary_handler));
  assert(selected_userdata == (proxy_first ? &proxy_config : &ordinary_config));

  selected = proxy_marker;
  selected_userdata = &proxy_config;
  status = vectis_internal_route_body_policy(
      app, VECTIS_HTTP_GET, "/proxy/a%2Fb", &policy, NULL, &selected,
      &selected_userdata, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(selected == NULL);
  assert(selected_userdata == NULL);
  status = vectis_internal_proxy_raw_path_match(
      app, VECTIS_HTTP_GET, "/proxy/a%2Fb", proxy_marker, request,
      &selected_userdata, &error);
  assert(status == VECTIS_OK);
  assert(selected_userdata == &proxy_config);
  assert(strcmp(vectis_request_path_param(request, "id"), "a%2Fb") == 0);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_proxy_raw_path_match(
      app, VECTIS_HTTP_GET, "/proxy/a%25b", proxy_marker, request,
      &selected_userdata, &error);
  assert(status == VECTIS_OK);
  assert(selected_userdata == &proxy_config);
  assert(strcmp(vectis_request_path_param(request, "id"), "a%25b") == 0);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_proxy_raw_path_match(
      app, VECTIS_HTTP_GET, "/proxy/a%3Ab", proxy_marker, request,
      &selected_userdata, &error);
  assert(status == VECTIS_OK);
  assert(strcmp(vectis_request_path_param(request, "id"), "a%3Ab") == 0);
  vectis_internal_request_cleanup(request);

  status = vectis_internal_proxy_raw_path_match(
      app, VECTIS_HTTP_GET, "/proxy/other/a%2Fb", proxy_marker, request,
      &selected_userdata, &error);
  assert(status == VECTIS_OK);
  assert(selected_userdata == &second_proxy_config);
  assert(strcmp(vectis_request_path_param(request, "id"), "a%2Fb") == 0);
  vectis_internal_request_cleanup(request);

  for (i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    status = vectis_internal_proxy_raw_path_match(
        app, VECTIS_HTTP_GET, invalid[i], proxy_marker, request,
        &selected_userdata, &error);
    assert(status == VECTIS_ERR_INVALID);
    assert(selected_userdata == NULL);
  }
  status = vectis_internal_proxy_raw_path_match(
      app, VECTIS_HTTP_POST, "/proxy/a%2Fb", proxy_marker, request,
      &selected_userdata, &error);
  assert(status == VECTIS_ERR_STATE);
  status = vectis_internal_proxy_raw_path_match(
      app, VECTIS_HTTP_GET, "/other/a%2Fb", proxy_marker, request,
      &selected_userdata, &error);
  assert(status == VECTIS_ERR_STATE);
  vectis_internal_request_free(request);
  app->close(app);
}

static void check_static_overlap(void) {
  char root[] = "proxy-static.XXXXXX";
  vectis_static_directory_config mount;
  vectis_route_handler_fn selected;
  vectis_route_config proxy;
  vectis_body_policy policy;
  vectis_request *request;
  vectis_app_config config;
  vectis_app *app;
  vectis_error error;
  const char *allow;
  vectis_status status;
  int denied;

  assert(mkdtemp(root) != NULL);
  vectis_app_config_init(&config);
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  vectis_static_directory_config_init(&mount);
  mount.path_prefix = "/assets/";
  mount.root_dir = root;
  assert(app->static_directory(app, &mount, &error) == VECTIS_OK);
  proxy = vectis_route(VECTIS_HTTP_GET, "^/assets/.*$", proxy_marker, NULL);
  proxy.path_kind = VECTIS_ROUTE_PATH_REGEX;
  assert(vectis_register_route(app, &proxy, &error) == VECTIS_OK);

  selected = NULL;
  status =
      vectis_internal_route_body_policy(app, VECTIS_HTTP_GET, "/assets/a:b/",
                                        &policy, NULL, &selected, NULL, &error);
  assert(status == VECTIS_OK);
  assert(selected != NULL && selected != proxy_marker);
  allow = NULL;
  denied = 0;
  status = vectis_internal_static_route_method_denied(
      app, VECTIS_HTTP_POST, "/assets/file", &denied, &allow, &error);
  assert(status == VECTIS_OK);
  assert(denied == 1);
  assert(allow != NULL && strstr(allow, "GET") != NULL);

  request = vectis_internal_request_new(&error);
  assert(request != NULL);
  status = vectis_internal_proxy_raw_path_match(app, VECTIS_HTTP_GET,
                                                "/assets/a%2Fb", proxy_marker,
                                                request, NULL, &error);
  assert(status == VECTIS_OK);
  vectis_internal_request_free(request);
  app->close(app);
  assert(rmdir(root) == 0);
}

int main(void) {
  check_order(0);
  check_order(1);
  check_static_overlap();
  return 0;
}
