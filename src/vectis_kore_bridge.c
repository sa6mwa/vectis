#include "vectis_internal.h"

#include <kore/kore.h>
#include <kore/http.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

int vectis_kore_main(int argc, char **argv);
int vectis_kore_route(struct http_request *req);
void kore_parent_configure(int argc, char **argv);
void kore_parent_teardown(void);

extern int skip_chroot;
extern int skip_runas;

static pthread_mutex_t vectis_kore_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t vectis_kore_thread;
static int vectis_kore_thread_active = 0;
static vectis_kore_runtime_config vectis_kore_current;

static size_t vectis_kore_environment_size(void) {
  size_t total;
  int i;

  total = 0u;
  if (environ == NULL) {
    return total;
  }
  for (i = 0; environ[i] != NULL; ++i) {
    total += strlen(environ[i]) + 1u;
  }
  return total;
}

static char *vectis_kore_argv_arena(char **argv, const char **args, int argc) {
  char *arena;
  char *cursor;
  size_t total;
  size_t len;
  int i;

  total = vectis_kore_environment_size();
  for (i = 0; i < argc; ++i) {
    total += strlen(args[i]) + 1u;
  }
  total += 1u;

  arena = (char *)calloc(1u, total);
  if (arena == NULL) {
    return NULL;
  }
  cursor = arena;
  for (i = 0; i < argc; ++i) {
    len = strlen(args[i]) + 1u;
    argv[i] = cursor;
    memcpy(cursor, args[i], len);
    cursor += len;
  }
  argv[argc] = NULL;
  return arena;
}

static vectis_http_method vectis_kore_method(u_int8_t method) {
  switch (method) {
  case HTTP_METHOD_GET:
    return VECTIS_HTTP_GET;
  case HTTP_METHOD_POST:
    return VECTIS_HTTP_POST;
  case HTTP_METHOD_PUT:
    return VECTIS_HTTP_PUT;
  case HTTP_METHOD_PATCH:
    return VECTIS_HTTP_PATCH;
  case HTTP_METHOD_DELETE:
    return VECTIS_HTTP_DELETE;
  case HTTP_METHOD_HEAD:
    return VECTIS_HTTP_HEAD;
  case HTTP_METHOD_OPTIONS:
    return VECTIS_HTTP_OPTIONS;
  default:
    return VECTIS_HTTP_ANY;
  }
}

static char *vectis_kore_memdup_cstr(const char *data, size_t len) {
  char *copy;

  copy = (char *)malloc(len + 1u);
  if (copy == NULL) {
    return NULL;
  }
  if (len > 0u) {
    memcpy(copy, data, len);
  }
  copy[len] = '\0';
  return copy;
}

static vectis_status vectis_kore_copy_headers(struct http_request *req,
                                              vectis_request *request,
                                              vectis_error *error) {
  struct http_header *header;
  vectis_status status;

  TAILQ_FOREACH(header, &req->req_headers, list) {
    if (header->header == NULL || header->header[0] == '\0') {
      continue;
    }
    status = vectis_internal_request_add_header(request,
                                                header->header,
                                                header->value != NULL ? header->value : "",
                                                error);
    if (status != VECTIS_OK) {
      return status;
    }
  }
  return VECTIS_OK;
}

static vectis_status vectis_kore_add_query_pair(vectis_request *request,
                                                const char *name,
                                                size_t name_len,
                                                const char *value,
                                                size_t value_len,
                                                vectis_error *error) {
  char *owned_name;
  char *owned_value;
  vectis_status status;

  if (name_len == 0u) {
    return VECTIS_OK;
  }
  owned_name = vectis_kore_memdup_cstr(name, name_len);
  owned_value = vectis_kore_memdup_cstr(value, value_len);
  if (owned_name == NULL || owned_value == NULL) {
    free(owned_name);
    free(owned_value);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate query parameter");
    return VECTIS_ERR_NOMEM;
  }
  if (!http_argument_urldecode(owned_name, 1) ||
      !http_argument_urldecode(owned_value, 1)) {
    free(owned_name);
    free(owned_value);
    vectis_set_error(error, VECTIS_ERR_INVALID, "failed to decode query parameter");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_internal_request_add_query(request, owned_name, owned_value, error);
  free(owned_name);
  free(owned_value);
  return status;
}

static vectis_status vectis_kore_copy_query(struct http_request *req,
                                            vectis_request *request,
                                            vectis_error *error) {
  const char *cursor;
  const char *pair;
  const char *amp;
  const char *eq;
  size_t pair_len;
  size_t name_len;
  size_t value_len;
  vectis_status status;

  if (req->query_string == NULL || req->query_string[0] == '\0') {
    return VECTIS_OK;
  }
  cursor = req->query_string;
  while (*cursor != '\0') {
    pair = cursor;
    amp = strchr(pair, '&');
    pair_len = amp != NULL ? (size_t)(amp - pair) : strlen(pair);
    if (pair_len > 0u) {
      eq = memchr(pair, '=', pair_len);
      if (eq != NULL) {
        name_len = (size_t)(eq - pair);
        value_len = pair_len - name_len - 1u;
        status = vectis_kore_add_query_pair(request,
                                            pair,
                                            name_len,
                                            eq + 1,
                                            value_len,
                                            error);
      } else {
        status = vectis_kore_add_query_pair(request,
                                            pair,
                                            pair_len,
                                            "",
                                            0u,
                                            error);
      }
      if (status != VECTIS_OK) {
        return status;
      }
    }
    if (amp == NULL) {
      break;
    }
    cursor = amp + 1;
  }
  return VECTIS_OK;
}

static vectis_status vectis_kore_copy_request_metadata(struct http_request *req,
                                                       vectis_request *request,
                                                       vectis_error *error) {
  vectis_status status;

  status = vectis_kore_copy_headers(req, request, error);
  if (status != VECTIS_OK) {
    return status;
  }
  return vectis_kore_copy_query(req, request, error);
}

static void *vectis_kore_thread_main(void *userdata) {
  char *argv[5];
  const char *args[4];
  char *arena;
  int argc;

  (void)userdata;
  argc = 0;
  args[argc++] = "vectis-kore";
  args[argc++] = "-f";
  args[argc++] = "-n";
  args[argc++] = "-r";
  arena = vectis_kore_argv_arena(argv, args, argc);
  if (arena == NULL) {
    return NULL;
  }
  skip_chroot = 1;
  skip_runas = 1;
  (void)vectis_kore_main(argc, argv);
  free(arena);
  return NULL;
}

static vectis_status vectis_kore_read_body(struct http_request *req,
                                           vectis_request *request,
                                           void **owned_body,
                                           vectis_error *error) {
  void *body;
  size_t body_size;
  size_t offset;
  ssize_t nread;

  if (req->content_length == 0u) {
    return vectis_internal_request_set_body(request, NULL, 0u, error);
  }
  if (req->content_length > (u_int64_t)((size_t)-1)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body is too large");
    return VECTIS_ERR_INVALID;
  }
  body_size = (size_t)req->content_length;
  body = malloc(body_size);
  if (body == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate request body");
    return VECTIS_ERR_NOMEM;
  }
  if (!http_body_rewind(req)) {
    free(body);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to rewind request body");
    return VECTIS_ERR_STATE;
  }
  offset = 0u;
  while (offset < body_size) {
    nread = http_body_read(req,
                           ((unsigned char *)body) + offset,
                           body_size - offset);
    if (nread <= 0) {
      free(body);
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to read request body");
      return VECTIS_ERR_STATE;
    }
    offset += (size_t)nread;
  }
  if (vectis_internal_request_set_body(request, body, body_size, error) != VECTIS_OK) {
    free(body);
    return error != NULL ? error->code : VECTIS_ERR_INVALID;
  }
  if (owned_body != NULL) {
    *owned_body = body;
  }
  return VECTIS_OK;
}

static void vectis_kore_send_response(struct http_request *req,
                                      vectis_response *response) {
  vectis_bytes body;
  const char *content_type;
  int status;

  status = vectis_internal_response_status_code(response);
  if (status == 0) {
    status = 204;
  }
  content_type = vectis_internal_response_content_type(response);
  if (content_type != NULL) {
    http_response_header(req, "content-type", content_type);
  }
  body = vectis_internal_response_body(response);
  http_response(req, status, body.data, body.size);
}

int vectis_kore_route(struct http_request *req) {
  vectis_request *request;
  vectis_response *response;
  vectis_error error;
  vectis_app *app;
  vectis_http_method method;
  vectis_status status;
  void *owned_body;

  vectis_error_clear(&error);
  owned_body = NULL;
  request = vectis_internal_request_new(&error);
  response = vectis_internal_response_new(&error);

  (void)pthread_mutex_lock(&vectis_kore_mutex);
  app = vectis_kore_current.app;
  (void)pthread_mutex_unlock(&vectis_kore_mutex);

  if (request == NULL || response == NULL) {
    if (request != NULL) {
      vectis_internal_request_free(request);
    }
    if (response != NULL) {
      vectis_internal_response_free(response);
    }
    http_response(req, 500, error.message, strlen(error.message));
    return KORE_RESULT_OK;
  }

  if (app == NULL || req == NULL || req->path == NULL) {
    http_response(req, 503, NULL, 0);
    vectis_internal_request_free(request);
    vectis_internal_response_free(response);
    return KORE_RESULT_OK;
  }

  status = vectis_kore_copy_request_metadata(req, request, &error);
  if (status == VECTIS_OK) {
    status = vectis_kore_read_body(req, request, &owned_body, &error);
  }
  if (status == VECTIS_OK) {
    method = vectis_kore_method(req->method);
    status = vectis_internal_dispatch_route(app, method, req->path, request, response, &error);
  }

  if (status == VECTIS_OK) {
    vectis_kore_send_response(req, response);
  } else if (status == VECTIS_ERR_INVALID) {
    http_response(req, 400, error.message, strlen(error.message));
  } else if (status == VECTIS_ERR_STATE) {
    http_response(req, 404, NULL, 0);
  } else {
    http_response(req, 500, error.message, strlen(error.message));
  }

  free(owned_body);
  vectis_internal_response_free(response);
  vectis_internal_request_free(request);
  return KORE_RESULT_OK;
}

void kore_parent_configure(int argc, char **argv) {
  struct kore_server *server;
  struct kore_domain *domain;
  struct kore_route *route;
  char port[16];

  (void)argc;
  (void)argv;

  (void)pthread_mutex_lock(&vectis_kore_mutex);
  if (vectis_kore_current.logger != NULL) {
    kore_log_set_logger(vectis_kore_current.logger);
  }
  server = kore_server_create(vectis_kore_current.app_name != NULL ?
                              vectis_kore_current.app_name : "vectis");
  if (vectis_kore_current.tls_mode == VECTIS_TLS_MODE_DISABLED) {
    server->tls = 0;
  }
  (void)snprintf(port, sizeof(port), "%u", (unsigned)vectis_kore_current.port);
  if (!kore_server_bind(server,
                        vectis_kore_current.bind != NULL ? vectis_kore_current.bind : "0.0.0.0",
                        port,
                        NULL)) {
    fatal("failed to bind Vectis Kore listener");
  }
  domain = kore_domain_new("*");
  if (!kore_domain_attach(domain, server)) {
    fatal("failed to attach Vectis Kore domain");
  }
  route = kore_route_create(domain, "^/.*$", HANDLER_TYPE_DYNAMIC);
  if (route == NULL) {
    fatal("failed to create Vectis Kore route");
  }
  kore_route_callback(route, "vectis_kore_route");
  kore_server_finalize(server);
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
}

void kore_parent_teardown(void) {
  (void)pthread_mutex_lock(&vectis_kore_mutex);
  memset(&vectis_kore_current, 0, sizeof(vectis_kore_current));
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
}

vectis_status vectis_internal_kore_start(const vectis_kore_runtime_config *config,
                                         vectis_error *error) {
  int rc;

  if (config == NULL || config->app == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "Kore runtime config is required");
    return VECTIS_ERR_INVALID;
  }
  (void)pthread_mutex_lock(&vectis_kore_mutex);
  if (vectis_kore_thread_active) {
    (void)pthread_mutex_unlock(&vectis_kore_mutex);
    vectis_set_error(error, VECTIS_ERR_STATE, "Kore runtime is already running");
    return VECTIS_ERR_STATE;
  }
  vectis_kore_current = *config;
  rc = pthread_create(&vectis_kore_thread, NULL, vectis_kore_thread_main, NULL);
  if (rc != 0) {
    memset(&vectis_kore_current, 0, sizeof(vectis_kore_current));
    (void)pthread_mutex_unlock(&vectis_kore_mutex);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to start Kore runtime thread");
    return VECTIS_ERR_STATE;
  }
  vectis_kore_thread_active = 1;
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_internal_kore_stop(vectis_app *app, vectis_error *error) {
  int active;

  (void)pthread_mutex_lock(&vectis_kore_mutex);
  active = vectis_kore_thread_active && vectis_kore_current.app == app;
  if (active) {
    kore_signal(SIGTERM);
  }
  (void)pthread_mutex_unlock(&vectis_kore_mutex);

  if (!active) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }

  (void)pthread_join(vectis_kore_thread, NULL);
  (void)pthread_mutex_lock(&vectis_kore_mutex);
  vectis_kore_thread_active = 0;
  memset(&vectis_kore_current, 0, sizeof(vectis_kore_current));
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
  vectis_error_clear(error);
  return VECTIS_OK;
}
