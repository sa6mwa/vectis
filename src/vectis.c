#include "vectis_internal.h"

#include <lc/lc.h>
#include <lonejson.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct vectis_route_entry {
  vectis_http_method method;
  char *path;
  vectis_route_handler_fn handler;
  void *userdata;
} vectis_route_entry;

typedef struct vectis_app_impl {
  pthread_mutex_t mutex;
  int started;
  int owns_logger;
  char *app_name;
  char *bind;
  char *cert_key_bundle_path;
  char *certificate_path;
  char *private_key_path;
  char *acme_email;
  char *acme_directory_url;
  char *unix_socket_path;
  char *client_bundle_path;
  char *default_namespace;
  char **endpoints;
  size_t endpoint_count;
  long timeout_ms;
  unsigned short port;
  vectis_tls_mode tls_mode;
  pslog_logger *logger;
  vectis_route_entry *routes;
  size_t route_count;
  size_t route_capacity;
  struct lc_client *lockd_client;
} vectis_app_impl;

static vectis_status vectis_app_start_impl(vectis_app *app, vectis_error *error);
static vectis_status vectis_app_stop_impl(vectis_app *app, vectis_error *error);
static vectis_status vectis_app_register_route_impl(vectis_app *app,
                                                    const vectis_route_config *route,
                                                    vectis_error *error);
static size_t vectis_app_route_count_impl(const vectis_app *app);
static pslog_logger *vectis_app_logger_impl(vectis_app *app);
static vectis_status vectis_open_lockd_client(vectis_app_impl *impl, vectis_error *error);

static const vectis_methods vectis_default_methods = {
    vectis_destroy,
    vectis_app_start_impl,
    vectis_app_stop_impl,
    vectis_app_register_route_impl,
    vectis_app_route_count_impl,
    vectis_app_logger_impl};

void vectis_error_clear(vectis_error *error) {
  if (error == NULL) {
    return;
  }
  error->code = VECTIS_OK;
  error->message[0] = '\0';
}

void vectis_set_error(vectis_error *error, vectis_status code, const char *message) {
  vectis_error_clear(error);
  if (error == NULL) {
    return;
  }
  error->code = code;
  if (message == NULL) {
    return;
  }
  (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

static void vectis_set_errorf(vectis_error *error,
                              vectis_status code,
                              const char *fmt,
                              ...) {
  va_list ap;

  vectis_error_clear(error);
  if (error == NULL) {
    return;
  }
  error->code = code;
  if (fmt == NULL) {
    return;
  }
  va_start(ap, fmt);
  (void)vsnprintf(error->message, sizeof(error->message), fmt, ap);
  va_end(ap);
}

static vectis_status vectis_status_from_lc_code(int code) {
  switch (code) {
  case LC_OK:
    return VECTIS_OK;
  case LC_ERR_INVALID:
    return VECTIS_ERR_INVALID;
  case LC_ERR_NOMEM:
    return VECTIS_ERR_NOMEM;
  default:
    return VECTIS_ERR_STATE;
  }
}

static int vectis_has_lockd_transport(const vectis_app_impl *impl) {
  return (impl->endpoint_count > 0u) || (impl->unix_socket_path != NULL);
}

static int vectis_has_complete_lockd_config(const vectis_app_impl *impl) {
  if (!vectis_has_lockd_transport(impl)) {
    return 0;
  }

  if (impl->unix_socket_path != NULL) {
    return 1;
  }

  return impl->client_bundle_path != NULL && impl->client_bundle_path[0] != '\0';
}

static vectis_status vectis_set_lc_error(vectis_error *error,
                                         vectis_status fallback,
                                         const char *prefix,
                                         lc_error *lcerr) {
  vectis_status status;

  status = fallback;
  if (lcerr != NULL) {
    vectis_status mapped;

    mapped = vectis_status_from_lc_code(lcerr->code);
    if (mapped != VECTIS_OK) {
      status = mapped;
    }
    if (lcerr->message != NULL && lcerr->detail != NULL) {
      vectis_set_errorf(error, status, "%s: %s (%s)", prefix, lcerr->message, lcerr->detail);
    } else if (lcerr->message != NULL) {
      vectis_set_errorf(error, status, "%s: %s", prefix, lcerr->message);
    } else {
      vectis_set_error(error, status, prefix);
    }
    lc_error_cleanup(lcerr);
  } else {
    vectis_set_error(error, status, prefix);
  }

  return status;
}

const char *vectis_status_string(vectis_status status) {
  switch (status) {
  case VECTIS_OK:
    return "ok";
  case VECTIS_ERR_INVALID:
    return "invalid";
  case VECTIS_ERR_NOMEM:
    return "nomem";
  case VECTIS_ERR_STATE:
    return "state";
  case VECTIS_ERR_CONFLICT:
    return "conflict";
  case VECTIS_ERR_NOT_IMPLEMENTED:
    return "not_implemented";
  default:
    return "unknown";
  }
}

void vectis_tls_config_init(vectis_tls_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->mode = VECTIS_TLS_MODE_MANUAL;
  config->bind = "0.0.0.0";
  config->port = 8443u;
}

void vectis_lockd_config_init(vectis_lockd_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->timeout_ms = 30000L;
}

void vectis_app_config_init(vectis_app_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->app_name = "vectis";
  config->log_mode = PSLOG_MODE_JSON;
  config->min_log_level = PSLOG_LEVEL_INFO;
  vectis_tls_config_init(&config->tls);
  vectis_lockd_config_init(&config->lockd);
}

static char *vectis_strdup(const char *value) {
  size_t len;
  char *copy;

  if (value == NULL) {
    return NULL;
  }
  len = strlen(value) + 1u;
  copy = (char *)malloc(len);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, len);
  return copy;
}

static void vectis_free_endpoints(vectis_app_impl *impl) {
  size_t i;

  if (impl->endpoints == NULL) {
    return;
  }
  for (i = 0u; i < impl->endpoint_count; ++i) {
    free(impl->endpoints[i]);
  }
  free(impl->endpoints);
  impl->endpoints = NULL;
  impl->endpoint_count = 0u;
}

static vectis_status vectis_copy_endpoints(vectis_app_impl *impl,
                                           const vectis_lockd_config *lockd,
                                           vectis_error *error) {
  size_t i;

  if (lockd->endpoint_count == 0u) {
    return VECTIS_OK;
  }
  impl->endpoints = (char **)calloc(lockd->endpoint_count, sizeof(*impl->endpoints));
  if (impl->endpoints == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate lockd endpoints");
    return VECTIS_ERR_NOMEM;
  }
  impl->endpoint_count = lockd->endpoint_count;
  for (i = 0u; i < lockd->endpoint_count; ++i) {
    impl->endpoints[i] = vectis_strdup(lockd->endpoints[i]);
    if (impl->endpoints[i] == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy lockd endpoint");
      return VECTIS_ERR_NOMEM;
    }
  }
  return VECTIS_OK;
}

static pslog_logger *vectis_make_owned_logger(const vectis_app_config *config,
                                              vectis_error *error) {
  pslog_config psconf;
  pslog_logger *root;
  pslog_logger *scoped;
  const char *service_name;

  pslog_default_config(&psconf);
  psconf.mode = config->log_mode;
  psconf.min_level = config->min_log_level;
  psconf.output = pslog_output_from_fp(stderr, 0);

  root = pslog_new(&psconf);
  if (root == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to create vectis logger");
    return NULL;
  }

  service_name = config->app_name != NULL ? config->app_name : "vectis";
  scoped = pslog_withf(root, "service=%s component=%s", service_name, "vectis");
  root->destroy(root);
  if (scoped == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to derive vectis logger fields");
    return NULL;
  }

  return scoped;
}

static vectis_status vectis_open_lockd_client(vectis_app_impl *impl, vectis_error *error) {
  lc_client_config lcconf;
  lc_error lcerr;
  int rc;

  if (impl->lockd_client != NULL) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }

  if (!vectis_has_complete_lockd_config(impl)) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }

  memset(&lcerr, 0, sizeof(lcerr));
  lc_client_config_init(&lcconf);
  lcconf.endpoints = (const char *const *)impl->endpoints;
  lcconf.endpoint_count = impl->endpoint_count;
  lcconf.unix_socket_path = impl->unix_socket_path;
  lcconf.client_bundle_path = impl->client_bundle_path;
  lcconf.default_namespace = impl->default_namespace;
  lcconf.timeout_ms = impl->timeout_ms;
  lcconf.logger = impl->logger;

  rc = lc_client_open(&lcconf, &impl->lockd_client, &lcerr);
  if (rc != LC_OK) {
    return vectis_set_lc_error(error, VECTIS_ERR_STATE, "failed to open lockd client", &lcerr);
  }

  vectis_error_clear(error);
  return VECTIS_OK;
}

static void vectis_free_routes(vectis_app_impl *impl) {
  size_t i;

  for (i = 0u; i < impl->route_count; ++i) {
    free(impl->routes[i].path);
  }
  free(impl->routes);
  impl->routes = NULL;
  impl->route_count = 0u;
  impl->route_capacity = 0u;
}

static void vectis_destroy_impl(vectis_app_impl *impl) {
  if (impl == NULL) {
    return;
  }

  if (impl->lockd_client != NULL) {
    lc_client_close(impl->lockd_client);
    impl->lockd_client = NULL;
  }

  vectis_free_routes(impl);
  vectis_free_endpoints(impl);

  free(impl->app_name);
  free(impl->bind);
  free(impl->cert_key_bundle_path);
  free(impl->certificate_path);
  free(impl->private_key_path);
  free(impl->acme_email);
  free(impl->acme_directory_url);
  free(impl->unix_socket_path);
  free(impl->client_bundle_path);
  free(impl->default_namespace);

  if (impl->owns_logger && impl->logger != NULL) {
    impl->logger->destroy(impl->logger);
  }

  (void)pthread_mutex_destroy(&impl->mutex);
  free(impl);
}

static vectis_status vectis_validate_route(const vectis_route_config *route,
                                           vectis_error *error) {
  if (route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route is required");
    return VECTIS_ERR_INVALID;
  }
  if (route->path == NULL || route->path[0] != '/') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route path must start with '/'");
    return VECTIS_ERR_INVALID;
  }
  if (route->handler == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "route handler is required");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static vectis_status vectis_validate_startable(const vectis_app_impl *impl,
                                               vectis_error *error) {
  int has_lockd_transport;

  if (impl->tls_mode == VECTIS_TLS_MODE_MANUAL) {
    if (impl->cert_key_bundle_path == NULL &&
        (impl->certificate_path == NULL || impl->private_key_path == NULL)) {
      vectis_set_error(error,
                       VECTIS_ERR_INVALID,
                       "manual TLS requires cert_key_bundle_path or certificate_path + private_key_path");
      return VECTIS_ERR_INVALID;
    }
  } else if (impl->tls_mode == VECTIS_TLS_MODE_ACME) {
    if (impl->acme_email == NULL || impl->acme_email[0] == '\0') {
      vectis_set_error(error, VECTIS_ERR_INVALID, "ACME mode requires acme_email");
      return VECTIS_ERR_INVALID;
    }
  }

  has_lockd_transport = (impl->endpoint_count > 0u) || (impl->unix_socket_path != NULL);
  if (!has_lockd_transport) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "lockd requires either endpoints or unix_socket_path");
    return VECTIS_ERR_INVALID;
  }

  if (impl->client_bundle_path == NULL && impl->unix_socket_path == NULL) {
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "tcp lockd transport requires client_bundle_path");
    return VECTIS_ERR_INVALID;
  }

  return VECTIS_OK;
}

vectis_app *vectis_new(const vectis_app_config *config, vectis_error *error) {
  vectis_app_config defaults;
  const vectis_app_config *effective;
  vectis_app *app;
  vectis_app_impl *impl;
  vectis_status status;

  vectis_error_clear(error);
  vectis_app_config_init(&defaults);
  effective = config != NULL ? config : &defaults;

  app = (vectis_app *)calloc(1u, sizeof(*app));
  impl = (vectis_app_impl *)calloc(1u, sizeof(*impl));
  if (app == NULL || impl == NULL) {
    free(app);
    free(impl);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate vectis app");
    return NULL;
  }

  if (pthread_mutex_init(&impl->mutex, NULL) != 0) {
    free(app);
    free(impl);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to initialize app mutex");
    return NULL;
  }

  impl->app_name = vectis_strdup(effective->app_name != NULL ? effective->app_name : "vectis");
  impl->bind = vectis_strdup(effective->tls.bind != NULL ? effective->tls.bind : "0.0.0.0");
  impl->cert_key_bundle_path = vectis_strdup(effective->tls.cert_key_bundle_path);
  impl->certificate_path = vectis_strdup(effective->tls.certificate_path);
  impl->private_key_path = vectis_strdup(effective->tls.private_key_path);
  impl->acme_email = vectis_strdup(effective->tls.acme_email);
  impl->acme_directory_url = vectis_strdup(effective->tls.acme_directory_url);
  impl->unix_socket_path = vectis_strdup(effective->lockd.unix_socket_path);
  impl->client_bundle_path = vectis_strdup(effective->lockd.client_bundle_path);
  impl->default_namespace = vectis_strdup(effective->lockd.default_namespace);
  impl->timeout_ms = effective->lockd.timeout_ms;
  impl->port = effective->tls.port;
  impl->tls_mode = effective->tls.mode;

  status = vectis_copy_endpoints(impl, &effective->lockd, error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }

  if (effective->logger != NULL) {
    impl->logger = effective->logger;
    impl->owns_logger = 0;
  } else {
    impl->logger = vectis_make_owned_logger(effective, error);
    if (impl->logger == NULL) {
      vectis_destroy_impl(impl);
      free(app);
      return NULL;
    }
    impl->owns_logger = 1;
  }

  status = vectis_open_lockd_client(impl, error);
  if (status != VECTIS_OK) {
    vectis_destroy_impl(impl);
    free(app);
    return NULL;
  }

  app->vt = &vectis_default_methods;
  app->impl = impl;
  return app;
}

void vectis_destroy(vectis_app *app) {
  vectis_app_impl *impl;

  if (app == NULL) {
    return;
  }
  impl = (vectis_app_impl *)app->impl;
  vectis_destroy_impl(impl);
  free(app);
}

static vectis_status vectis_app_start_impl(vectis_app *app, vectis_error *error) {
  vectis_app_impl *impl;
  vectis_status status;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  impl = (vectis_app_impl *)app->impl;

  status = vectis_validate_startable(impl, error);
  if (status != VECTIS_OK) {
    return status;
  }

  status = vectis_open_lockd_client(impl, error);
  if (status != VECTIS_OK) {
    return status;
  }

  (void)pthread_mutex_lock(&impl->mutex);
  if (impl->started) {
    (void)pthread_mutex_unlock(&impl->mutex);
    vectis_set_error(error, VECTIS_ERR_STATE, "app is already started");
    return VECTIS_ERR_STATE;
  }
  (void)pthread_mutex_unlock(&impl->mutex);

  vectis_set_error(error,
                   VECTIS_ERR_NOT_IMPLEMENTED,
                   "Kore runtime bootstrap and lockd client startup are not implemented yet");
  return VECTIS_ERR_NOT_IMPLEMENTED;
}

vectis_status vectis_start(vectis_app *app, vectis_error *error) {
  if (app == NULL || app->vt == NULL || app->vt->start == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  return app->vt->start(app, error);
}

static vectis_status vectis_app_stop_impl(vectis_app *app, vectis_error *error) {
  vectis_app_impl *impl;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  impl = (vectis_app_impl *)app->impl;

  (void)pthread_mutex_lock(&impl->mutex);
  if (!impl->started) {
    (void)pthread_mutex_unlock(&impl->mutex);
    vectis_set_error(error, VECTIS_ERR_STATE, "app is not started");
    return VECTIS_ERR_STATE;
  }
  (void)pthread_mutex_unlock(&impl->mutex);

  vectis_set_error(error,
                   VECTIS_ERR_NOT_IMPLEMENTED,
                   "Kore runtime shutdown and consumer teardown are not implemented yet");
  return VECTIS_ERR_NOT_IMPLEMENTED;
}

vectis_status vectis_stop(vectis_app *app, vectis_error *error) {
  if (app == NULL || app->vt == NULL || app->vt->stop == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  return app->vt->stop(app, error);
}

static int vectis_route_conflicts(const vectis_route_entry *existing,
                                  const vectis_route_config *candidate) {
  return existing->method == candidate->method &&
         strcmp(existing->path, candidate->path) == 0;
}

static vectis_status vectis_app_register_route_impl(vectis_app *app,
                                                    const vectis_route_config *route,
                                                    vectis_error *error) {
  vectis_app_impl *impl;
  vectis_route_entry *grown;
  size_t i;
  size_t next_capacity;
  vectis_status status;

  if (app == NULL || app->impl == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }

  status = vectis_validate_route(route, error);
  if (status != VECTIS_OK) {
    return status;
  }

  impl = (vectis_app_impl *)app->impl;
  (void)pthread_mutex_lock(&impl->mutex);
  for (i = 0u; i < impl->route_count; ++i) {
    if (vectis_route_conflicts(&impl->routes[i], route)) {
      (void)pthread_mutex_unlock(&impl->mutex);
      vectis_set_errorf(error,
                        VECTIS_ERR_CONFLICT,
                        "duplicate route registration for %s",
                        route->path);
      return VECTIS_ERR_CONFLICT;
    }
  }

  if (impl->route_count == impl->route_capacity) {
    next_capacity = impl->route_capacity == 0u ? 4u : impl->route_capacity * 2u;
    grown = (vectis_route_entry *)realloc(impl->routes, next_capacity * sizeof(*grown));
    if (grown == NULL) {
      (void)pthread_mutex_unlock(&impl->mutex);
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to grow route registry");
      return VECTIS_ERR_NOMEM;
    }
    impl->routes = grown;
    impl->route_capacity = next_capacity;
  }

  impl->routes[impl->route_count].method = route->method;
  impl->routes[impl->route_count].path = vectis_strdup(route->path);
  impl->routes[impl->route_count].handler = route->handler;
  impl->routes[impl->route_count].userdata = route->userdata;
  if (impl->routes[impl->route_count].path == NULL) {
    (void)pthread_mutex_unlock(&impl->mutex);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy route path");
    return VECTIS_ERR_NOMEM;
  }
  impl->route_count++;
  (void)pthread_mutex_unlock(&impl->mutex);

  return VECTIS_OK;
}

vectis_status vectis_register_route(vectis_app *app,
                                    const vectis_route_config *route,
                                    vectis_error *error) {
  if (app == NULL || app->vt == NULL || app->vt->register_route == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "app is required");
    return VECTIS_ERR_INVALID;
  }
  return app->vt->register_route(app, route, error);
}

static size_t vectis_app_route_count_impl(const vectis_app *app) {
  vectis_app_impl *impl;
  size_t count;

  if (app == NULL || app->impl == NULL) {
    return 0u;
  }
  impl = (vectis_app_impl *)app->impl;
  (void)pthread_mutex_lock(&impl->mutex);
  count = impl->route_count;
  (void)pthread_mutex_unlock(&impl->mutex);
  return count;
}

size_t vectis_route_count(const vectis_app *app) {
  if (app == NULL || app->vt == NULL || app->vt->route_count == NULL) {
    return 0u;
  }
  return app->vt->route_count(app);
}

static pslog_logger *vectis_app_logger_impl(vectis_app *app) {
  vectis_app_impl *impl;

  if (app == NULL || app->impl == NULL) {
    return NULL;
  }
  impl = (vectis_app_impl *)app->impl;
  return impl->logger;
}

pslog_logger *vectis_logger(vectis_app *app) {
  if (app == NULL || app->vt == NULL || app->vt->logger == NULL) {
    return NULL;
  }
  return app->vt->logger(app);
}

vectis_status vectis_json_validate_cstr(const char *json, vectis_error *error) {
  lonejson_error json_error;
  lonejson_status status;

  if (json == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "json is required");
    return VECTIS_ERR_INVALID;
  }

  status = lonejson_validate_cstr(json, &json_error);
  if (status != LONEJSON_STATUS_OK) {
    vectis_set_errorf(error,
                      VECTIS_ERR_INVALID,
                      "invalid json at line %lu column %lu: %s",
                      (unsigned long)json_error.line,
                      (unsigned long)json_error.column,
                      json_error.message);
    return VECTIS_ERR_INVALID;
  }

  vectis_error_clear(error);
  return VECTIS_OK;
}

struct lc_client *vectis_internal_lockd_client(vectis_app *app) {
  vectis_app_impl *impl;

  if (app == NULL || app->impl == NULL) {
    return NULL;
  }

  impl = (vectis_app_impl *)app->impl;
  return impl->lockd_client;
}
