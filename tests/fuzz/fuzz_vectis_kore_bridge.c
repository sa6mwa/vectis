#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <kore/http.h>
#include <lc/lc.h>
#include <vectis/vectis.h>

#include "vectis_internal.h"

int vectis_kore_body_chunk(struct http_request *req, const void *data,
                           size_t len);
int vectis_kore_route(struct http_request *req);
void vectis_kore_request_free(struct http_request *req);

#define VECTIS_FUZZ_MAX_QUERY 256u
#define VECTIS_FUZZ_MAX_HEADER_VALUE 128u
#define VECTIS_FUZZ_MAX_HEADERS 8u
#define VECTIS_FUZZ_MAX_BODY 4096u

typedef struct vectis_fuzz_input {
  const uint8_t *data;
  size_t size;
  size_t offset;
} vectis_fuzz_input;

typedef struct vectis_fuzz_stream_state {
  size_t bytes;
  size_t chunks;
} vectis_fuzz_stream_state;

static uint8_t vectis_fuzz_byte(vectis_fuzz_input *input) {
  if (input->offset >= input->size) {
    return 0u;
  }
  return input->data[input->offset++];
}

static size_t vectis_fuzz_range(vectis_fuzz_input *input, size_t max) {
  size_t value;

  if (max == 0u) {
    return 0u;
  }
  value = (size_t)vectis_fuzz_byte(input);
  value <<= 8;
  value |= (size_t)vectis_fuzz_byte(input);
  return value % (max + 1u);
}

static char vectis_fuzz_query_char(uint8_t byte) {
  static const char alphabet[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789=&+%.;_-";

  return alphabet[byte % (sizeof(alphabet) - 1u)];
}

static char vectis_fuzz_token_char(uint8_t byte) {
  static const char alphabet[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";

  return alphabet[byte % (sizeof(alphabet) - 1u)];
}

static char vectis_fuzz_header_value_char(uint8_t byte) {
  static const char alphabet[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
      " -_.,;:/?@[]{}()=+*%";

  return alphabet[byte % (sizeof(alphabet) - 1u)];
}

static char *vectis_fuzz_string(vectis_fuzz_input *input, size_t max,
                                char (*map)(uint8_t), int allow_empty) {
  char *out;
  size_t len;
  size_t i;

  len = vectis_fuzz_range(input, max);
  if (!allow_empty && len == 0u) {
    len = 1u;
  }
  out = (char *)malloc(len + 1u);
  if (out == NULL) {
    return NULL;
  }
  for (i = 0u; i < len; ++i) {
    out[i] = map(vectis_fuzz_byte(input));
  }
  out[len] = '\0';
  return out;
}

static const char *vectis_fuzz_path(uint8_t selector) {
  switch (selector % 7u) {
  case 0u:
    return "/plain";
  case 1u:
    return "/buffer";
  case 2u:
    return "/stream";
  case 3u:
    return "/items/fuzzed";
  case 4u:
    return "/items";
  default:
    return "/../bad";
  }
}

static u_int8_t vectis_fuzz_method(uint8_t selector) {
  switch (selector % 7u) {
  case 0u:
    return HTTP_METHOD_GET;
  case 1u:
    return HTTP_METHOD_POST;
  case 2u:
    return HTTP_METHOD_PUT;
  case 3u:
    return HTTP_METHOD_PATCH;
  case 4u:
    return HTTP_METHOD_DELETE;
  case 5u:
    return HTTP_METHOD_HEAD;
  default:
    return HTTP_METHOD_OPTIONS;
  }
}

static void vectis_fuzz_request_init(struct http_request *req, const char *path,
                                     char *query, u_int8_t method,
                                     size_t body_size) {
  memset(req, 0, sizeof(*req));
  req->method = method;
  req->path = path;
  req->query_string = query;
  req->http_body_length = (u_int64_t)body_size;
  TAILQ_INIT(&req->req_cookies);
  TAILQ_INIT(&req->resp_cookies);
  TAILQ_INIT(&req->req_headers);
  TAILQ_INIT(&req->resp_headers);
  TAILQ_INIT(&req->arguments);
  TAILQ_INIT(&req->files);
}

static int vectis_fuzz_add_header(struct http_request *req, char *name,
                                  char *value) {
  struct http_header *header;

  header = (struct http_header *)calloc(1u, sizeof(*header));
  if (header == NULL) {
    return 0;
  }
  header->header = name;
  header->value = value;
  TAILQ_INSERT_TAIL(&req->req_headers, header, list);
  return 1;
}

static void vectis_fuzz_headers_cleanup(struct http_request *req) {
  struct http_header *header;
  struct http_header *next;

  header = TAILQ_FIRST(&req->req_headers);
  while (header != NULL) {
    next = TAILQ_NEXT(header, list);
    TAILQ_REMOVE(&req->req_headers, header, list);
    free(header->header);
    free(header->value);
    free(header);
    header = next;
  }
}

static void vectis_fuzz_read_source(struct lc_source *source) {
  lc_error error;
  unsigned char buffer[127];

  if (source == NULL) {
    return;
  }
  lc_error_init(&error);
  while (source->read(source, buffer, sizeof(buffer), &error) > 0u) {
  }
  lc_error_cleanup(&error);
}

static vectis_status vectis_fuzz_handler(vectis_app *app,
                                         vectis_request *request,
                                         vectis_response *response,
                                         void *userdata, vectis_error *error) {
  (void)app;
  (void)userdata;

  (void)vectis_request_query(request, "a");
  (void)vectis_request_query(request, "empty");
  (void)vectis_request_header(request, "x-fuzz-0");
  (void)vectis_request_path_param(request, "id");
  vectis_fuzz_read_source(vectis_request_body_reader(request));
  return vectis_response_status(response, 204, error);
}

static vectis_status vectis_fuzz_stream_open(vectis_app *app,
                                             vectis_request *request,
                                             void *userdata, void **state,
                                             vectis_error *error) {
  vectis_fuzz_stream_state *stream;

  (void)app;
  (void)request;
  (void)userdata;
  (void)error;

  stream = (vectis_fuzz_stream_state *)calloc(1u, sizeof(*stream));
  if (stream == NULL) {
    return VECTIS_ERR_NOMEM;
  }
  *state = stream;
  return VECTIS_OK;
}

static vectis_status vectis_fuzz_stream_write(vectis_app *app,
                                              vectis_request *request,
                                              const void *data, size_t size,
                                              void *state, void *userdata,
                                              vectis_error *error) {
  vectis_fuzz_stream_state *stream;

  (void)app;
  (void)request;
  (void)data;
  (void)userdata;
  (void)error;

  stream = (vectis_fuzz_stream_state *)state;
  if (stream == NULL && size > 0u) {
    return VECTIS_ERR_INVALID;
  }
  if (stream != NULL) {
    stream->bytes += size;
    stream->chunks += 1u;
  }
  return VECTIS_OK;
}

static vectis_status vectis_fuzz_stream_finish(vectis_app *app,
                                               vectis_request *request,
                                               vectis_response *response,
                                               void *state, void *userdata,
                                               vectis_error *error) {
  (void)app;
  (void)request;
  (void)state;
  (void)userdata;
  return vectis_response_status(response, 204, error);
}

static void vectis_fuzz_stream_close(vectis_app *app, vectis_request *request,
                                     void *state, void *userdata) {
  (void)app;
  (void)request;
  (void)userdata;
  free(state);
}

static vectis_app *vectis_fuzz_app_new(void) {
  vectis_app_config config;
  vectis_route_config route;
  vectis_upload_route_config stream_route;
  vectis_error error;
  vectis_app *app;

  vectis_error_clear(&error);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app = vectis_app_new(&config, &error);
  if (app == NULL) {
    return NULL;
  }

  route = vectis_route_methods(VECTIS_HTTP_METHODS_ALL, "/plain",
                               vectis_fuzz_handler, NULL);
  if (vectis_register_route(app, &route, &error) != VECTIS_OK) {
    vectis_destroy(app);
    return NULL;
  }
  route = vectis_upload_route_max_methods(VECTIS_HTTP_METHODS_ALL, "/buffer",
                                          VECTIS_FUZZ_MAX_BODY,
                                          vectis_fuzz_handler, NULL);
  route.body.memory_buffer_limit_bytes = 97u;
  if (vectis_register_route(app, &route, &error) != VECTIS_OK) {
    vectis_destroy(app);
    return NULL;
  }
  route = vectis_route_methods(VECTIS_HTTP_METHODS_ALL, "/items/:id?",
                               vectis_fuzz_handler, NULL);
  if (vectis_register_route(app, &route, &error) != VECTIS_OK) {
    vectis_destroy(app);
    return NULL;
  }

  stream_route = vectis_stream_upload_route_methods(
      VECTIS_HTTP_METHODS_ALL, "/stream", vectis_fuzz_stream_open,
      vectis_fuzz_stream_write, vectis_fuzz_stream_finish,
      vectis_fuzz_stream_close, NULL);
  stream_route.body.max_bytes = VECTIS_FUZZ_MAX_BODY;
  stream_route.body.memory_buffer_limit_bytes = 17u;
  if (vectis_register_upload_stream(app, &stream_route, &error) != VECTIS_OK) {
    vectis_destroy(app);
    return NULL;
  }

  return app;
}

static void vectis_fuzz_feed_body(struct http_request *req,
                                  vectis_fuzz_input *input, const uint8_t *body,
                                  size_t body_size) {
  size_t offset;
  size_t chunk;
  size_t remaining;

  offset = 0u;
  while (offset < body_size) {
    remaining = body_size - offset;
    chunk = vectis_fuzz_range(input, remaining);
    if (chunk == 0u) {
      chunk = 1u;
    }
    (void)vectis_kore_body_chunk(req, body + offset, chunk);
    offset += chunk;
  }
  if (body_size == 0u && (vectis_fuzz_byte(input) & 1u) != 0u) {
    (void)vectis_kore_body_chunk(req, body, 0u);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  vectis_fuzz_input input;
  struct http_request req;
  static vectis_app *app;
  const char *path;
  const uint8_t *body;
  char *query;
  char *header_name;
  char *header_value;
  size_t header_count;
  size_t body_size;
  size_t i;
  uint8_t selector;

  if (data == NULL || size == 0u) {
    return 0;
  }
  if (app == NULL) {
    app = vectis_fuzz_app_new();
    if (app == NULL) {
      return 0;
    }
  }
  input.data = data;
  input.size = size;
  input.offset = 0u;

  selector = vectis_fuzz_byte(&input);
  path = vectis_fuzz_path(selector);
  query = vectis_fuzz_string(&input, VECTIS_FUZZ_MAX_QUERY,
                             vectis_fuzz_query_char, 1);
  if (query == NULL) {
    return 0;
  }
  body_size = vectis_fuzz_range(&input, VECTIS_FUZZ_MAX_BODY);
  if (body_size > size - input.offset) {
    body_size = size - input.offset;
  }
  body = data + input.offset;
  input.offset += body_size;

  vectis_fuzz_request_init(&req, path, query,
                           vectis_fuzz_method(vectis_fuzz_byte(&input)),
                           body_size);
  header_count = vectis_fuzz_range(&input, VECTIS_FUZZ_MAX_HEADERS);
  for (i = 0u; i < header_count; ++i) {
    header_name = vectis_fuzz_string(&input, 24u, vectis_fuzz_token_char, 0);
    header_value = vectis_fuzz_string(&input, VECTIS_FUZZ_MAX_HEADER_VALUE,
                                      vectis_fuzz_header_value_char, 1);
    if (header_name == NULL || header_value == NULL ||
        !vectis_fuzz_add_header(&req, header_name, header_value)) {
      free(header_name);
      free(header_value);
      break;
    }
  }

  vectis_internal_kore_fuzzer_set_app(app);
  vectis_fuzz_feed_body(&req, &input, body, body_size);
  (void)vectis_kore_route(&req);

  vectis_kore_request_free(&req);
  vectis_fuzz_headers_cleanup(&req);
  free(query);
  return 0;
}
