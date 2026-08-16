#include <stdio.h>
#include <stdlib.h>

#include <vectis/vectis.h>

static int print_error(const char *operation, const vectis_error *error) {
  fprintf(stderr, "%s failed", operation);
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, ": %s", error->message);
  }
  if (error != NULL && error->detail[0] != '\0') {
    fprintf(stderr, " (%s)", error->detail);
  }
  fprintf(stderr, "\n");
  return 1;
}

static vectis_status health(vectis_app *app, vectis_request *request,
                            vectis_response *response, void *userdata,
                            vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok\n", error);
}

int main(int argc, char **argv) {
  vectis_app_config config;
  vectis_route_config route;
  vectis_error error;
  vectis_app *app;
  long port;

  if (argc != 2) {
    fprintf(stderr, "usage: vectis_wait_signal_harness PORT\n");
    return 2;
  }
  port = strtol(argv[1], NULL, 10);
  if (port <= 0L || port > 65535L) {
    fprintf(stderr, "invalid port: %s\n", argv[1]);
    return 2;
  }

  vectis_app_config_init(&config);
  config.app_name = "vectis-wait-signal-harness";
  config.tls.bind = "127.0.0.1";
  config.tls.port = (unsigned short)port;
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    return print_error("vectis_app_new", &error);
  }
  route =
      vectis_route_methods(VECTIS_HTTP_METHODS_GET | VECTIS_HTTP_METHODS_HEAD,
                           "/health", health, NULL);
  if (app->route(app, &route, &error) != VECTIS_OK) {
    (void)print_error("app->route", &error);
    app->close(app);
    return 1;
  }
  printf("READY\n");
  fflush(stdout);
  if (app->run(app, &error) != VECTIS_OK) {
    (void)print_error("app->run", &error);
    app->close(app);
    return 1;
  }
  app->close(app);
  printf("STOPPED\n");
  fflush(stdout);
  return 0;
}
