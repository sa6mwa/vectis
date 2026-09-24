#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <kore/http.h>
#include <kore/kore.h>
#include <vectis/vectis.h>
#include "vectis_internal.h"

struct probe_state {
  struct http_request *req;
  char body[4];
  size_t have;
  char pipeline[256];
  size_t pipeline_len;
};

#define CHUNK_BODY_SIZE (1024 * 1024)
#define CHUNK_WINDOW_SIZE 8192

enum chunk_phase {
  CHUNK_SIZE_LINE,
  CHUNK_DATA,
  CHUNK_DATA_CR,
  CHUNK_DATA_LF,
  CHUNK_TRAILERS,
  CHUNK_DONE
};

struct chunk_state {
  struct http_request *req;
  enum chunk_phase phase;
  size_t remaining;
  size_t total;
  size_t expected_total;
  int expect_initial_pipeline;
  char line[128];
  size_t line_length;
  int line_cr;
  int trailer_seen;
  char pipeline[256];
  size_t pipeline_length;
};

extern void vectis_kore_set_prebody_probe(
    int (*probe)(struct http_request *, const void *, size_t));

static const char raw_path[] = "/raw-target/a%2Fb/%2e/c";
static const char raw_query[] = "q=1&q=2&plus=%2B&empty=";
static const char ws_upgrade_headers[] =
    "Connection: Upgrade\r\nUpgrade: websocket\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
static vectis_app *probe_app;
static int probe_proxy_first;

static vectis_status proxy_marker_reply(vectis_app *app,
    vectis_request *request, vectis_response *response, void *userdata,
    vectis_error *error);

static int
probe_connection_handle(struct connection *c)
{
  if ((c->evt.flags & KORE_EVENT_READ) && !net_recv_flush(c))
    return KORE_RESULT_ERROR;
  if ((c->evt.flags & KORE_EVENT_WRITE) && !net_send_flush(c))
    return KORE_RESULT_ERROR;
  return KORE_RESULT_OK;
}

static void
probe_finish(struct connection *c)
{
  struct probe_state *state;

  state = (struct probe_state *)c->hdlr_extra;
  assert(state != NULL && state->have == sizeof(state->body));
  http_response(state->req, 200, state->body, sizeof(state->body));
  state->req->flags |= HTTP_REQUEST_DELETE;
  http_request_wakeup(state->req);
  c->handle = kore_connection_handle;
  c->flags &= ~CONN_IS_BUSY;
  if (state->pipeline_len > 0) {
    assert(c->http_pipeline == NULL);
    c->http_pipeline = kore_malloc(state->pipeline_len);
    memcpy(c->http_pipeline, state->pipeline, state->pipeline_len);
    c->http_pipeline_len = state->pipeline_len;
  }
  c->hdlr_extra = NULL;
  kore_free(state);
  http_start_recv(c);
}

static int
probe_recv(struct netbuf *nb)
{
  struct connection *c;
  struct probe_state *state;

  c = nb->owner;
  state = (struct probe_state *)c->hdlr_extra;
  assert(state != NULL);
  assert(nb->s_off <= sizeof(state->body) - state->have);
  memcpy(state->body + state->have, nb->buf, nb->s_off);
  state->have += nb->s_off;
  if (state->have == sizeof(state->body))
    probe_finish(c);
  return KORE_RESULT_OK;
}

static int
raw_recv(struct netbuf *nb)
{
  struct connection *c;

  c = nb->owner;
  net_send_queue(c, nb->buf, nb->s_off);
  net_recv_reset(c, 4096, raw_recv);
  return KORE_RESULT_OK;
}

static unsigned char
chunk_byte(size_t offset)
{
  return (unsigned char)((offset * 73u + 19u) & 0xffu);
}

static void
chunk_finish(struct connection *c, struct chunk_state *state)
{
  assert(state->total == state->expected_total);
  assert(state->trailer_seen == 1);
  if (state->expect_initial_pipeline)
    assert(state->pipeline_length > 0);
  http_response(state->req, 200, "done", 4);
  state->req->flags |= HTTP_REQUEST_DELETE;
  http_request_wakeup(state->req);
  if (state->pipeline_length > 0) {
    assert(c->http_pipeline == NULL);
    c->http_pipeline = kore_malloc(state->pipeline_length);
    memcpy(c->http_pipeline, state->pipeline, state->pipeline_length);
    c->http_pipeline_len = state->pipeline_length;
  }
  c->handle = kore_connection_handle;
  c->flags &= ~CONN_IS_BUSY;
  c->hdlr_extra = NULL;
  kore_free(state);
  http_start_recv(c);
}

static size_t
chunk_next_window(struct chunk_state *state)
{
  if (state->phase == CHUNK_DATA &&
      state->remaining < CHUNK_WINDOW_SIZE)
    return state->remaining;
  if (state->phase == CHUNK_DATA)
    return CHUNK_WINDOW_SIZE;
  return 1;
}

static int
chunk_feed(struct connection *c, struct chunk_state *state,
    const unsigned char *data, size_t length)
{
  unsigned long parsed;
  size_t amount;
  size_t index;
  size_t n;
  char *end;

  assert(length <= http_header_max);
  index = 0;
  while (index < length && state->phase != CHUNK_DONE) {
    if (state->phase == CHUNK_DATA) {
      amount = length - index;
      if (amount > state->remaining)
        amount = state->remaining;
      for (n = 0; n < amount; n++)
        assert(data[index + n] == chunk_byte(state->total + n));
      state->total += amount;
      assert(state->total <= state->expected_total);
      state->remaining -= amount;
      index += amount;
      if (state->remaining == 0)
        state->phase = CHUNK_DATA_CR;
      continue;
    }
    if (state->phase == CHUNK_DATA_CR) {
      assert(data[index++] == '\r');
      state->phase = CHUNK_DATA_LF;
      continue;
    }
    if (state->phase == CHUNK_DATA_LF) {
      assert(data[index++] == '\n');
      state->phase = CHUNK_SIZE_LINE;
      continue;
    }
    if (state->line_cr) {
      assert(data[index++] == '\n');
      state->line_cr = 0;
      state->line[state->line_length] = '\0';
      if (state->phase == CHUNK_SIZE_LINE) {
        assert(state->line_length > 0);
        errno = 0;
        parsed = strtoul(state->line, &end, 16);
        assert(errno == 0 && *end == '\0');
        assert(parsed <= state->expected_total - state->total);
        state->remaining = (size_t)parsed;
        state->phase = parsed == 0 ? CHUNK_TRAILERS : CHUNK_DATA;
      } else {
        assert(state->phase == CHUNK_TRAILERS);
        if (state->line_length == 0) {
          state->phase = CHUNK_DONE;
        } else {
          assert(strcmp(state->line, "X-Trace: done") == 0);
          state->trailer_seen++;
        }
      }
      state->line_length = 0;
    } else if (data[index] == '\r') {
      state->line_cr = 1;
      index++;
    } else {
      assert(state->line_length < sizeof(state->line) - 1);
      state->line[state->line_length++] = (char)data[index++];
    }
  }
  if (state->phase == CHUNK_DONE) {
    assert(length - index <= sizeof(state->pipeline));
    state->pipeline_length = length - index;
    if (state->pipeline_length > 0)
      memcpy(state->pipeline, data + index, state->pipeline_length);
    chunk_finish(c, state);
    return 1;
  }
  return 0;
}

static int
chunk_recv(struct netbuf *nb)
{
  struct connection *c;
  struct chunk_state *state;

  c = nb->owner;
  state = (struct chunk_state *)c->hdlr_extra;
  assert(state != NULL);
  if (!chunk_feed(c, state, nb->buf, nb->s_off))
    net_recv_reset(c, chunk_next_window(state), chunk_recv);
  return KORE_RESULT_OK;
}

static int
probe_prebody(struct http_request *req, const void *data, size_t len)
{
  struct connection *c;
  struct http_header *header;
  struct probe_state *state;
  struct chunk_state *chunk;
  const char *first_length;
  const char *second_length;
  const char *transfer_encoding;
  const char *second_host;
  const char *expect;
  u_int64_t declared;
  unsigned length_count;
  unsigned encoding_count;
  unsigned host_count;
  int framing_seen;
  size_t first;

  if (strncmp(req->path, "/proxy-select/", 14) == 0) {
    vectis_route_handler_fn selected;
    vectis_body_policy policy;
    vectis_request *probe_request;
    vectis_internal_websocket_match ws_match;
    vectis_error probe_error;
    vectis_status status;
    vectis_status path_status;
    vectis_http_method route_method;
    const char *allow;
    char *decoded;
    int denied;
    int raw_fallback_eligible;
    int live_upload;

    if (req->method != HTTP_METHOD_GET && req->method != HTTP_METHOD_POST)
      return KORE_RESULT_OK;
    assert(probe_app != NULL);
    probe_request = vectis_internal_request_new(&probe_error);
    assert(probe_request != NULL);
    selected = NULL;
    decoded = NULL;
    denied = 0;
    live_upload = 0;
    raw_fallback_eligible = 0;
    allow = NULL;
    route_method = req->method == HTTP_METHOD_POST ? VECTIS_HTTP_POST :
        VECTIS_HTTP_GET;
    status = vectis_internal_kore_decode_request_path(
        req->path, &decoded, &probe_error);
    if (status == VECTIS_OK) {
      path_status = vectis_internal_validate_request_path(
          decoded, &probe_error);
      if (path_status != VECTIS_OK && path_status != VECTIS_ERR_INVALID) {
        free(decoded);
        vectis_internal_request_free(probe_request);
        return KORE_RESULT_ERROR;
      }
      raw_fallback_eligible = path_status == VECTIS_ERR_INVALID;
      if (path_status == VECTIS_OK && route_method == VECTIS_HTTP_GET) {
        status = vectis_internal_match_websocket(
            probe_app, route_method, decoded, probe_request, &ws_match,
            &probe_error);
        if (status == VECTIS_OK) {
          free(decoded);
          vectis_internal_request_free(probe_request);
          return KORE_RESULT_OK;
        }
        if (status != VECTIS_ERR_STATE) {
          free(decoded);
          vectis_internal_request_free(probe_request);
          return KORE_RESULT_ERROR;
        }
      }
      vectis_error_clear(&probe_error);
      status = vectis_internal_static_route_method_denied(
          probe_app, route_method, decoded, &denied, &allow,
          &probe_error);
      if (status == VECTIS_OK && !denied)
        status = vectis_internal_route_body_policy(
            probe_app, route_method, decoded, &policy, &live_upload, &selected,
            NULL, probe_request, &probe_error);
    } else if (status == VECTIS_ERR_INVALID) {
      raw_fallback_eligible = 1;
    }
    if (raw_fallback_eligible && status == VECTIS_ERR_INVALID && !denied) {
      status = vectis_internal_proxy_raw_path_match(
          probe_app, route_method, req->path, proxy_marker_reply,
          probe_request, NULL, &probe_error);
      if (status == VECTIS_OK)
        selected = proxy_marker_reply;
    }
    if (route_method == VECTIS_HTTP_POST &&
        strcmp(req->path, "/proxy-select/upload/a") == 0 &&
        status == VECTIS_OK)
      assert(live_upload == !probe_proxy_first);
    if (route_method == VECTIS_HTTP_POST &&
        strcmp(req->path, "/proxy-select/upload-reverse/a") == 0 &&
        status == VECTIS_OK)
      assert(live_upload == probe_proxy_first);
    if (status == VECTIS_OK && !denied &&
        selected == proxy_marker_reply) {
      static const char marker[] = "proxy-prebody-selected";

      if (strcmp(req->path, "/proxy-select/a") == 0 ||
          strcmp(req->path, "/proxy-select/%41") == 0)
        assert(strcmp(vectis_request_path_param(probe_request, "id"),
            strcmp(req->path, "/proxy-select/a") == 0 ? "a" : "A") == 0);
      if (strcmp(req->path, "/proxy-select/a%2Fb") == 0)
        assert(strcmp(vectis_request_path_param(probe_request, "id"),
            "a%2Fb") == 0);
      req->owner->flags |= CONN_CLOSE_EMPTY;
      http_response(req, 200, marker, sizeof(marker) - 1);
      free(decoded);
      vectis_internal_request_free(probe_request);
      return KORE_RESULT_ERROR;
    }
    if (status == VECTIS_ERR_INVALID) {
      static const char marker[] = "proxy-raw-rejected";

      req->owner->flags |= CONN_CLOSE_EMPTY;
      http_response(req, 400, marker, sizeof(marker) - 1);
      free(decoded);
      vectis_internal_request_free(probe_request);
      return KORE_RESULT_ERROR;
    }
    assert(status == VECTIS_OK || status == VECTIS_ERR_STATE);
    free(decoded);
    vectis_internal_request_free(probe_request);
    return KORE_RESULT_OK;
  }
  if (strncmp(req->path, "/raw-target", 11) == 0) {
    static const char marker[] = "raw-target-preserved";
    int preserved;

    preserved = strcmp(req->path, raw_path) == 0 &&
        req->query_string != NULL &&
        strcmp(req->query_string, raw_query) == 0;
    req->owner->flags |= CONN_CLOSE_EMPTY;
    http_response(req, preserved ? 200 : 400,
        preserved ? marker : "raw-target-changed",
        preserved ? sizeof(marker) - 1 : sizeof("raw-target-changed") - 1);
    return KORE_RESULT_ERROR;
  }
  if (strcmp(req->path, "/framing-header-limit") == 0) {
    static const char marker[] = "prebody-header-limit-accepted";
    unsigned padding_count;

    padding_count = 0;
    TAILQ_FOREACH(header, &req->req_headers, list) {
      if (strcmp(header->header, "x-pad") == 0)
        padding_count++;
    }
    assert(padding_count == HTTP_REQ_HEADER_MAX - 3u);
    req->owner->flags |= CONN_CLOSE_EMPTY;
    http_response(req, 200, marker, sizeof(marker) - 1);
    return KORE_RESULT_ERROR;
  }
  if (strcmp(req->path, "/framing-header-nul") == 0) {
    static const char marker[] = "prebody-header-nul-hidden";

    req->owner->flags |= CONN_CLOSE_EMPTY;
    http_response(req, 200, marker, sizeof(marker) - 1);
    return KORE_RESULT_ERROR;
  }
  if (strcmp(req->path, "/framing-bare-cr") == 0) {
    static const char marker[] = "prebody-bare-cr-accepted";

    req->owner->flags |= CONN_CLOSE_EMPTY;
    http_response(req, 200, marker, sizeof(marker) - 1);
    return KORE_RESULT_ERROR;
  }
  if (strncmp(req->path, "/framing-", 9) == 0) {
    first_length = NULL;
    second_length = NULL;
    transfer_encoding = NULL;
    second_host = NULL;
    length_count = 0;
    encoding_count = 0;
    host_count = 0;
    TAILQ_FOREACH(header, &req->req_headers, list) {
      if (strcmp(header->header, "content-length") == 0) {
        if (length_count == 0)
          first_length = header->value;
        else if (length_count == 1)
          second_length = header->value;
        length_count++;
      } else if (strcmp(header->header, "transfer-encoding") == 0) {
        transfer_encoding = header->value;
        encoding_count++;
      } else if (strcmp(header->header, "host") == 0) {
        second_host = header->value;
        host_count++;
      }
    }
    framing_seen = 0;
    if (strcmp(req->path, "/framing-cl-te") == 0)
      framing_seen = length_count == 1 && encoding_count == 1 &&
          strcmp(first_length, "4") == 0 &&
          strcmp(transfer_encoding, "chunked") == 0;
    else if (strcmp(req->path, "/framing-duplicate-cl") == 0)
      framing_seen = length_count == 2 && encoding_count == 0 &&
          strcmp(first_length, "4") == 0 &&
          strcmp(second_length, "5") == 0;
    else if (strcmp(req->path, "/framing-duplicate-host") == 0)
      framing_seen = length_count == 0 && host_count == 1 &&
          strcmp(req->host, "localhost") == 0 &&
          strcmp(second_host, "attacker.invalid") == 0;
    else if (strcmp(req->path, "/framing-http10") == 0)
      framing_seen = (req->flags & HTTP_VERSION_1_0) != 0;
    if (framing_seen) {
      static const char marker[] = "prebody-framing-reject";
      req->owner->flags |= CONN_CLOSE_EMPTY;
      http_response(req, 400, marker, sizeof(marker) - 1);
      return KORE_RESULT_ERROR;
    }
    return KORE_RESULT_OK;
  }
  if (strcmp(req->path, "/reject") == 0)
    return KORE_RESULT_ERROR;
  if (strcmp(req->path, "/forbidden") == 0) {
    req->owner->flags |= CONN_CLOSE_EMPTY;
    http_response(req, 403, "forbidden", 9);
    return KORE_RESULT_ERROR;
  }
  if (strcmp(req->path, "/raw") == 0) {
    static const char upgrade[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    c = req->owner;
    c->flags |= CONN_IS_BUSY;
    c->handle = probe_connection_handle;
    http_request_sleep(req);
    net_send_queue(c, upgrade, sizeof(upgrade) - 1);
    if (len > 0)
      net_send_queue(c, data, len);
    net_recv_reset(c, 4096, raw_recv);
    return KORE_RESULT_RETRY;
  }
  if (strcmp(req->path, "/chunked") == 0 ||
      strcmp(req->path, "/chunked-small") == 0) {
    assert(req->method == HTTP_METHOD_POST);
    c = req->owner;
    c->http_timeout = 0;
    chunk = kore_calloc(1, sizeof(*chunk));
    chunk->req = req;
    chunk->expect_initial_pipeline =
        strcmp(req->path, "/chunked-small") == 0;
    chunk->expected_total = chunk->expect_initial_pipeline
        ? 4 : CHUNK_BODY_SIZE;
    c->hdlr_extra = chunk;
    c->flags |= CONN_IS_BUSY;
    c->handle = probe_connection_handle;
    http_request_sleep(req);
    if (!chunk_feed(c, chunk, data, len))
      net_recv_reset(c, chunk_next_window(chunk), chunk_recv);
    return KORE_RESULT_RETRY;
  }
  if (strcmp(req->path, "/probe") != 0 &&
      strcmp(req->path, "/probe-continue") != 0)
    return KORE_RESULT_OK;
  assert(req->method == HTTP_METHOD_POST ||
      req->method == HTTP_METHOD_GET ||
      req->method == HTTP_METHOD_OPTIONS);
  assert(http_request_header_uint64(req, "content-length", &declared));
  assert(declared == 4);
  if (strcmp(req->path, "/probe-continue") == 0) {
    assert(http_request_header(req, "expect", &expect));
    assert(strcmp(expect, "100-continue") == 0);
    assert(len == 0);
  }
  c = req->owner;
  assert(c->hdlr_extra == NULL);
  state = kore_calloc(1, sizeof(*state));
  state->req = req;
  first = len < sizeof(state->body) ? len : sizeof(state->body);
  memcpy(state->body, data, first);
  state->have = first;
  assert(len - first <= sizeof(state->pipeline));
  if (len > first) {
    state->pipeline_len = len - first;
    memcpy(state->pipeline, (const char *)data + first, state->pipeline_len);
  }
  c->hdlr_extra = state;
  c->flags |= CONN_IS_BUSY;
  c->handle = probe_connection_handle;
  http_request_sleep(req);
  if (strcmp(req->path, "/probe-continue") == 0) {
    static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";

    net_send_queue(c, interim, sizeof(interim) - 1);
  }
  if (state->have == sizeof(state->body)) {
    probe_finish(c);
  } else {
    net_recv_reset(c, sizeof(state->body) - state->have, probe_recv);
  }
  return KORE_RESULT_RETRY;
}

static vectis_status
reply(vectis_app *app, vectis_request *request, vectis_response *response,
    void *userdata, vectis_error *error)
{
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok", error);
}

static vectis_status
proxy_marker_reply(vectis_app *app, vectis_request *request,
    vectis_response *response, void *userdata, vectis_error *error)
{
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 500, "text/plain",
      "proxy marker reached ordinary dispatch", error);
}

static void
selection_ws_message(vectis_app *app, vectis_websocket *websocket,
    vectis_websocket_opcode opcode, const void *data, size_t size,
    void *userdata)
{
  (void)app;
  (void)websocket;
  (void)opcode;
  (void)data;
  (void)size;
  (void)userdata;
}

struct selection_upload_state {
  size_t size;
};

static vectis_status
selection_upload_open(vectis_app *app, vectis_request *request,
    void *userdata, void **state, vectis_error *error)
{
  struct selection_upload_state *upload;

  (void)app;
  (void)request;
  (void)userdata;
  upload = calloc(1, sizeof(*upload));
  if (upload == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM, "upload state allocation failed");
    return VECTIS_ERR_NOMEM;
  }
  *state = upload;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status
selection_upload_write(vectis_app *app, vectis_request *request,
    const void *data, size_t size, void *state, void *userdata,
    vectis_error *error)
{
  struct selection_upload_state *upload;
  vectis_bytes body;

  (void)app;
  (void)userdata;
  upload = (struct selection_upload_state *)state;
  if (upload == NULL || data == NULL ||
      size > (sizeof("data") - 1u) - upload->size ||
      memcmp(data, "data" + upload->size, size) != 0) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "upload chunk is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (vectis_request_body_bytes(request, &body, error) != VECTIS_ERR_INVALID) {
    vectis_set_error(error, VECTIS_ERR_STATE, "upload was materialized");
    return VECTIS_ERR_STATE;
  }
  upload->size += size;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status
selection_upload_finish(vectis_app *app, vectis_request *request,
    vectis_response *response, void *state, void *userdata,
    vectis_error *error)
{
  struct selection_upload_state *upload;

  (void)app;
  (void)request;
  (void)userdata;
  upload = (struct selection_upload_state *)state;
  if (upload == NULL || upload->size != sizeof("data") - 1u)
    return vectis_response_text(response, 422, "text/plain",
        "wrong live upload body", error);
  return vectis_response_text(response, 200, "text/plain",
      "live-upload-winner", error);
}

static void
selection_upload_close(vectis_app *app, vectis_request *request,
    void *state, void *userdata)
{
  (void)app;
  (void)request;
  (void)userdata;
  free(state);
}

static void
register_proxy_selection_routes(vectis_app *app, int proxy_first,
    vectis_error *error)
{
  vectis_route_config ordinary;
  vectis_route_config proxy;
  vectis_route_config static_overlap_proxy;
  vectis_route_config upload_proxy;
  vectis_route_config reverse_upload_proxy;
  vectis_upload_route_config upload;
  vectis_upload_route_config reverse_upload;
  vectis_websocket_route_config websocket;

  probe_proxy_first = proxy_first;
  ordinary = vectis_route(VECTIS_HTTP_GET, "^/proxy-select/.*$", reply, NULL);
  ordinary.path_kind = VECTIS_ROUTE_PATH_REGEX;
  proxy = vectis_route(VECTIS_HTTP_GET, "/proxy-select/:id",
      proxy_marker_reply, NULL);
  proxy.path_kind = VECTIS_ROUTE_PATH_PARAMS;
  if (proxy_first) {
    assert(vectis_register_route(app, &proxy, error) == VECTIS_OK);
    assert(vectis_register_route(app, &ordinary, error) == VECTIS_OK);
  } else {
    assert(vectis_register_route(app, &ordinary, error) == VECTIS_OK);
    assert(vectis_register_route(app, &proxy, error) == VECTIS_OK);
  }
  static_overlap_proxy = vectis_route(
      VECTIS_HTTP_GET, "^/proxy-select/static/.*$", proxy_marker_reply,
      NULL);
  static_overlap_proxy.path_kind = VECTIS_ROUTE_PATH_REGEX;
  assert(vectis_register_route(app, &static_overlap_proxy, error) == VECTIS_OK);
  upload_proxy = vectis_route(VECTIS_HTTP_POST,
      "^/proxy-select/upload/.*$", proxy_marker_reply, NULL);
  upload_proxy.path_kind = VECTIS_ROUTE_PATH_REGEX;
  upload = vectis_stream_upload_route(VECTIS_HTTP_POST,
      "/proxy-select/upload/a", selection_upload_open,
      selection_upload_write, selection_upload_finish,
      selection_upload_close, NULL);
  upload.body.max_bytes = 4u;
  if (proxy_first) {
    assert(vectis_register_route(app, &upload_proxy, error) == VECTIS_OK);
    assert(app->upload_stream(app, &upload, error) == VECTIS_OK);
  } else {
    assert(app->upload_stream(app, &upload, error) == VECTIS_OK);
    assert(vectis_register_route(app, &upload_proxy, error) == VECTIS_OK);
  }
  reverse_upload_proxy = vectis_route(VECTIS_HTTP_POST,
      "^/proxy-select/upload-reverse/.*$", proxy_marker_reply, NULL);
  reverse_upload_proxy.path_kind = VECTIS_ROUTE_PATH_REGEX;
  reverse_upload = vectis_stream_upload_route(VECTIS_HTTP_POST,
      "/proxy-select/upload-reverse/a", selection_upload_open,
      selection_upload_write, selection_upload_finish,
      selection_upload_close, NULL);
  reverse_upload.body.max_bytes = 4u;
  if (proxy_first) {
    assert(app->upload_stream(app, &reverse_upload, error) == VECTIS_OK);
    assert(vectis_register_route(app, &reverse_upload_proxy, error) ==
        VECTIS_OK);
  } else {
    assert(vectis_register_route(app, &reverse_upload_proxy, error) ==
        VECTIS_OK);
    assert(app->upload_stream(app, &reverse_upload, error) == VECTIS_OK);
  }
  websocket = vectis_websocket_route(
      "/proxy-select/ws", selection_ws_message, NULL);
  assert(app->websocket(app, &websocket, error) == VECTIS_OK);
}

static void
register_proxy_static_overlap(vectis_app *app, const char *root,
    vectis_error *error)
{
  vectis_static_directory_config mount;

  vectis_static_directory_config_init(&mount);
  mount.path_prefix = "/proxy-select/static/";
  mount.root_dir = root;
  assert(app->static_directory(app, &mount, error) == VECTIS_OK);
}

static unsigned short
available_port(void)
{
  struct sockaddr_in addr;
  socklen_t size;
  int fd;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
  size = sizeof(addr);
  assert(getsockname(fd, (struct sockaddr *)&addr, &size) == 0);
  assert(close(fd) == 0);
  return ntohs(addr.sin_port);
}

static int
connect_local(unsigned short port)
{
  struct sockaddr_in addr;
  int attempt;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  for (attempt = 0; attempt < 100; attempt++) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
      return fd;
    assert(close(fd) == 0);
    usleep(10000u);
  }
  assert(0 && "Vectis listener did not start");
  return -1;
}

static unsigned
response_count(const char *data)
{
  unsigned count;
  const char *p;

  count = 0;
  p = data;
  while ((p = strstr(p, "HTTP/1.1 200")) != NULL) {
    count++;
    p++;
  }
  return count;
}

static int
check_request_halfclose_at_headers(unsigned short port)
{
  static const char request[] =
      "GET /one HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  struct pollfd watch;
  char response[1024];
  size_t used;
  ssize_t got;
  int fd;

  fd = connect_local(port);
  assert(send(fd, request, sizeof(request) - 1, 0) ==
      (ssize_t)(sizeof(request) - 1));
  assert(shutdown(fd, SHUT_WR) == 0);
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  response[0] = '\0';
  while (used < sizeof(response) - 1 &&
      poll(&watch, 1, 1000) > 0) {
    got = recv(fd, response + used, sizeof(response) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    response[used] = '\0';
  }
  assert(close(fd) == 0);
  return strstr(response, "HTTP/1.1 200") != NULL &&
      strstr(response, "ok") != NULL;
}

static void
send_exact(int fd, const void *data, size_t length)
{
  const unsigned char *bytes;
  size_t sent;
  ssize_t amount;

  bytes = (const unsigned char *)data;
  sent = 0;
  while (sent < length) {
    amount = send(fd, bytes + sent, length - sent, 0);
    assert(amount > 0);
    sent += (size_t)amount;
  }
}

static int
check_chunked_ingress(unsigned short port)
{
  static const char request[] =
      "POST /chunked HTTP/1.1\r\nHost: localhost\r\n"
      "Transfer-Encoding: chunked\r\nTrailer: X-Trace\r\n\r\n"
      "2000\r\n";
  static const char ending[] =
      "0\r\nX-Trace: done\r\n\r\n"
      "GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct timeval timeout;
  struct pollfd watch;
  unsigned char body[CHUNK_WINDOW_SIZE];
  char output[4096];
  size_t offset;
  size_t used;
  size_t index;
  ssize_t got;
  int fd;
  int ok;

  fd = connect_local(port);
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
      &timeout, sizeof(timeout)) == 0);
  assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
      &timeout, sizeof(timeout)) == 0);
  send_exact(fd, request, sizeof(request) - 1);
  for (index = 0; index < 4; index++)
    body[index] = chunk_byte(index);
  send_exact(fd, body, 4);
  usleep(10000u);
  for (offset = 0; offset < CHUNK_BODY_SIZE;
      offset += CHUNK_WINDOW_SIZE) {
    if (offset > 0)
      send_exact(fd, "2000\r\n", 6);
    for (index = offset == 0 ? 4 : 0; index < sizeof(body); index++)
      body[index] = chunk_byte(offset + index);
    send_exact(fd, body + (offset == 0 ? 4 : 0),
        sizeof(body) - (offset == 0 ? 4 : 0));
    send_exact(fd, "\r\n", 2);
  }
  send_exact(fd, ending, sizeof(ending) - 1);
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1 && response_count(output) < 2) {
    if (poll(&watch, 1, 10000) <= 0)
      break;
    got = recv(fd, output + used, sizeof(output) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  ok = response_count(output) == 2 && strstr(output, "done") != NULL;
  fprintf(stderr, "chunked 1 MiB ingress + pipelined GET: %s\n",
      ok ? "passed" : "failed");
  assert(close(fd) == 0);
  return ok;
}

static int
check_small_chunked_ingress(unsigned short port, int tls)
{
  static const char prefix[] =
      "POST /chunked-small HTTP/1.1\r\nHost: localhost\r\n"
      "Transfer-Encoding: chunked\r\nTrailer: X-Trace\r\n\r\n"
      "4\r\n";
  static const char suffix[] =
      "\r\n0\r\nX-Trace: done\r\n\r\n"
      "GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n";
  struct pollfd watch;
  SSL_CTX *ctx;
  SSL *ssl;
  char wire[512];
  char output[4096];
  size_t wire_length;
  size_t used;
  size_t index;
  ssize_t got;
  int fd;
  int ok;

  wire_length = 0;
  memcpy(wire + wire_length, prefix, sizeof(prefix) - 1);
  wire_length += sizeof(prefix) - 1;
  for (index = 0; index < 4; index++)
    wire[wire_length++] = (char)chunk_byte(index);
  memcpy(wire + wire_length, suffix, sizeof(suffix) - 1);
  wire_length += sizeof(suffix) - 1;
  assert(wire_length <= sizeof(wire));
  fd = connect_local(port);
  ctx = NULL;
  ssl = NULL;
  if (tls) {
    ctx = SSL_CTX_new(TLS_client_method());
    assert(ctx != NULL);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    ssl = SSL_new(ctx);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, fd) == 1);
    assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
    assert(SSL_connect(ssl) == 1);
    assert(SSL_write(ssl, wire, (int)wire_length) == (int)wire_length);
  } else {
    assert(send(fd, wire, wire_length, 0) == (ssize_t)wire_length);
  }
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1 && response_count(output) < 2) {
    if ((!tls || SSL_pending(ssl) == 0) && poll(&watch, 1, 1000) <= 0)
      break;
    if (tls)
      got = SSL_read(ssl, output + used,
          (int)(sizeof(output) - 1 - used));
    else
      got = recv(fd, output + used, sizeof(output) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  ok = response_count(output) == 2 && strstr(output, "done") != NULL;
  fprintf(stderr, "%s borrowed chunked body + pipelined GET: %s\n",
      tls ? "TLS" : "cleartext", ok ? "passed" : "failed");
  if (ssl != NULL)
    SSL_free(ssl);
  if (ctx != NULL)
    SSL_CTX_free(ctx);
  assert(close(fd) == 0);
  return ok;
}

static int
check_local_rejection(unsigned short port, const char *path, int status)
{
  struct pollfd watch;
  char wire[256];
  char output[1024];
  char marker[16];
  ssize_t got;
  int fd;

  fd = connect_local(port);
  assert(snprintf(wire, sizeof(wire),
      "POST %s HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 4\r\n\r\n", path) > 0);
  assert(send(fd, wire, strlen(wire), 0) == (ssize_t)strlen(wire));
  watch.fd = fd;
  watch.events = POLLIN;
  got = 0;
  if (poll(&watch, 1, 1000) > 0)
    got = recv(fd, output, sizeof(output) - 1, 0);
  if (got > 0)
    output[got] = '\0';
  else
    output[0] = '\0';
  assert(snprintf(marker, sizeof(marker), " %d ", status) > 0);
  assert(close(fd) == 0);
  return strstr(output, marker) != NULL;
}

static int
check_raw_handoff(unsigned short port, int tls)
{
  static const char frame[] = "\x81\x85\x01\x02\x03\x04igohn";
  static const char wire[] =
      "GET /raw HTTP/1.1\r\nHost: localhost\r\n"
      "Connection: Upgrade\r\nUpgrade: websocket\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"
      "\x81\x85\x01\x02\x03\x04igohn";
  SSL_CTX *ctx;
  SSL *ssl;
  struct pollfd watch;
  char output[4096];
  size_t used;
  ssize_t got;
  int fd;
  int ok;

  fd = connect_local(port);
  ctx = NULL;
  ssl = NULL;
  if (tls) {
    ctx = SSL_CTX_new(TLS_client_method());
    assert(ctx != NULL);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    ssl = SSL_new(ctx);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, fd) == 1);
    assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
    assert(SSL_connect(ssl) == 1);
    assert(SSL_write(ssl, wire, (int)(sizeof(wire) - 1)) ==
        (int)(sizeof(wire) - 1));
  } else {
    assert(send(fd, wire, sizeof(wire) - 1, 0) ==
        (ssize_t)(sizeof(wire) - 1));
  }
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1 && strstr(output, frame) == NULL) {
    if ((!tls || SSL_pending(ssl) == 0) && poll(&watch, 1, 1000) <= 0)
      break;
    if (tls)
      got = SSL_read(ssl, output + used, (int)(sizeof(output) - 1 - used));
    else
      got = recv(fd, output + used, sizeof(output) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  ok = strstr(output, " 101 ") != NULL && strstr(output, frame) != NULL;
  fprintf(stderr, "%s early WebSocket frame: %s\n",
      tls ? "TLS" : "cleartext", ok ? "preserved" : "missing");
  if (ssl != NULL)
    SSL_free(ssl);
  if (ctx != NULL)
    SSL_CTX_free(ctx);
  assert(close(fd) == 0);
  return ok;
}

static int
check_tls_handoff(unsigned short port, const char *wire)
{
  SSL_CTX *ctx;
  SSL *ssl;
  struct pollfd watch;
  char output[4096];
  size_t used;
  int fd;
  int got;
  unsigned count;

  fd = connect_local(port);
  ctx = SSL_CTX_new(TLS_client_method());
  assert(ctx != NULL);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
  ssl = SSL_new(ctx);
  assert(ssl != NULL);
  assert(SSL_set_fd(ssl, fd) == 1);
  assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
  assert(SSL_connect(ssl) == 1);
  assert(SSL_write(ssl, wire, (int)strlen(wire)) == (int)strlen(wire));
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1 && response_count(output) < 3) {
    if (SSL_pending(ssl) == 0 && poll(&watch, 1, 1000) <= 0)
      break;
    got = SSL_read(ssl, output + used, (int)(sizeof(output) - 1 - used));
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  count = response_count(output);
  fprintf(stderr, "TLS ordinary + takeover + ordinary responses: %u\n",
      count);
  SSL_free(ssl);
  SSL_CTX_free(ctx);
  assert(close(fd) == 0);
  return count == 3 && strstr(output, "data") != NULL;
}

static int
check_framed_method_handoff(unsigned short port, const char *method, int tls)
{
  SSL_CTX *ctx;
  SSL *ssl;
  struct pollfd watch;
  char wire[256];
  char output[4096];
  size_t used;
  ssize_t got;
  int fd;
  int length;
  int ok;

  length = snprintf(wire, sizeof(wire),
      "%s /probe HTTP/1.1\r\nHostile: attacker.invalid\r\n"
      "Host: localhost\r\n"
      "Content-Length: 4\r\n\r\ndata"
      "GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n", method);
  assert(length > 0 && (size_t)length < sizeof(wire));
  fd = connect_local(port);
  ctx = NULL;
  ssl = NULL;
  if (tls) {
    ctx = SSL_CTX_new(TLS_client_method());
    assert(ctx != NULL);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    ssl = SSL_new(ctx);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, fd) == 1);
    assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
    assert(SSL_connect(ssl) == 1);
    assert(SSL_write(ssl, wire, length) == length);
  } else {
    send_exact(fd, wire, (size_t)length);
  }

  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1 && response_count(output) < 2) {
    if ((!tls || SSL_pending(ssl) == 0) && poll(&watch, 1, 1000) <= 0)
      break;
    if (tls)
      got = SSL_read(ssl, output + used,
          (int)(sizeof(output) - 1 - used));
    else
      got = recv(fd, output + used, sizeof(output) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  ok = response_count(output) == 2 &&
      strstr(output, "data") != NULL && strstr(output, "ok") != NULL;
  fprintf(stderr, "%s framed %s + pipelined GET: %s\n",
      tls ? "TLS" : "cleartext", method, ok ? "passed" : "failed");
  if (ssl != NULL)
    SSL_free(ssl);
  if (ctx != NULL)
    SSL_CTX_free(ctx);
  assert(close(fd) == 0);
  return ok;
}

static int
check_expect_continue_handoff(unsigned short port, int tls)
{
  static const char headers[] =
      "POST /probe-continue HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 4\r\nExpect: 100-continue\r\n\r\n";
  static const char body_and_next[] =
      "dataGET /two HTTP/1.1\r\nHost: localhost\r\n\r\n";
  SSL_CTX *ctx;
  SSL *ssl;
  struct pollfd watch;
  char output[4096];
  size_t used;
  ssize_t got;
  int fd;
  int ok;

  fd = connect_local(port);
  ctx = NULL;
  ssl = NULL;
  if (tls) {
    ctx = SSL_CTX_new(TLS_client_method());
    assert(ctx != NULL);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    ssl = SSL_new(ctx);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, fd) == 1);
    assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
    assert(SSL_connect(ssl) == 1);
    assert(SSL_write(ssl, headers, (int)(sizeof(headers) - 1u)) ==
        (int)(sizeof(headers) - 1u));
  } else {
    send_exact(fd, headers, sizeof(headers) - 1u);
  }
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1u &&
      strstr(output, "\r\n\r\n") == NULL) {
    if ((!tls || SSL_pending(ssl) == 0) && poll(&watch, 1, 1000) <= 0)
      break;
    if (tls)
      got = SSL_read(ssl, output + used,
          (int)(sizeof(output) - 1u - used));
    else
      got = recv(fd, output + used, sizeof(output) - 1u - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  ok = strcmp(output, "HTTP/1.1 100 Continue\r\n\r\n") == 0;
  if (ok) {
    if (tls) {
      assert(SSL_write(ssl, body_and_next,
          (int)(sizeof(body_and_next) - 1u)) ==
          (int)(sizeof(body_and_next) - 1u));
    } else {
      send_exact(fd, body_and_next, sizeof(body_and_next) - 1u);
    }
    while (used < sizeof(output) - 1u && response_count(output) < 2) {
      if ((!tls || SSL_pending(ssl) == 0) && poll(&watch, 1, 1000) <= 0)
        break;
      if (tls)
        got = SSL_read(ssl, output + used,
            (int)(sizeof(output) - 1u - used));
      else
        got = recv(fd, output + used, sizeof(output) - 1u - used, 0);
      if (got <= 0)
        break;
      used += (size_t)got;
      output[used] = '\0';
    }
    ok = response_count(output) == 2 && strstr(output, "data") != NULL &&
        strstr(output, "ok") != NULL &&
        strstr(output + sizeof("HTTP/1.1 100 Continue\r\n\r\n") - 1u,
            "HTTP/1.1 100 Continue") == NULL;
  }
  fprintf(stderr, "%s 100-continue + pipelined GET: %s\n",
      tls ? "TLS" : "cleartext", ok ? "passed" : "failed");
  if (!ok)
    fprintf(stderr, "received: %s\n", output);
  if (ssl != NULL)
    SSL_free(ssl);
  if (ctx != NULL)
    SSL_CTX_free(ctx);
  assert(close(fd) == 0);
  return ok;
}

static int
check_expect_rejection_close(unsigned short port, int tls)
{
  static const char wire[] =
      "POST /forbidden HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 4\r\nExpect: 100-continue\r\n\r\n"
      "dataGET /two HTTP/1.1\r\nHost: localhost\r\n\r\n";
  SSL_CTX *ctx;
  SSL *ssl;
  struct pollfd watch;
  char output[2048];
  size_t used;
  ssize_t got;
  const char *first403;
  int fd;
  int closed;
  int ok;
  int tls_error;

  fd = connect_local(port);
  ctx = NULL;
  ssl = NULL;
  if (tls) {
    ctx = SSL_CTX_new(TLS_client_method());
    assert(ctx != NULL);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    ssl = SSL_new(ctx);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, fd) == 1);
    assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
    assert(SSL_connect(ssl) == 1);
    assert(SSL_write(ssl, wire, (int)(sizeof(wire) - 1u)) ==
        (int)(sizeof(wire) - 1u));
  } else {
    send_exact(fd, wire, sizeof(wire) - 1u);
  }
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  closed = 0;
  tls_error = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1u) {
    if ((!tls || SSL_pending(ssl) == 0) && poll(&watch, 1, 1000) <= 0)
      break;
    if (tls)
      got = SSL_read(ssl, output + used,
          (int)(sizeof(output) - 1u - used));
    else
      got = recv(fd, output + used, sizeof(output) - 1u - used, 0);
    if (got <= 0) {
      if (tls) {
        tls_error = SSL_get_error(ssl, (int)got);
        closed = tls_error == SSL_ERROR_ZERO_RETURN;
      } else {
        closed = got == 0;
      }
      break;
    }
    used += (size_t)got;
    output[used] = '\0';
  }
  first403 = strstr(output, "HTTP/1.1 403");
  ok = closed && first403 != NULL &&
      strstr(output, "forbidden") != NULL &&
      strstr(output, "connection: close") != NULL &&
      strstr(output, "HTTP/1.1 100") == NULL &&
      strstr(first403 + 1, "HTTP/1.1 ") == NULL;
  fprintf(stderr, "%s Expect rejection closes unread body: %s\n",
      tls ? "TLS" : "cleartext", ok ? "passed" : "failed");
  if (!ok)
    fprintf(stderr, "received: %s\nTLS error=%d\n", output, tls_error);
  if (ssl != NULL)
    SSL_free(ssl);
  if (ctx != NULL)
    SSL_CTX_free(ctx);
  assert(close(fd) == 0);
  return ok;
}

static int
check_prebody_wire_result(unsigned short port, const void *wire,
    size_t wire_length, const char *path, int tls, int status,
    const char *marker)
{
  SSL_CTX *ctx;
  SSL *ssl;
  struct pollfd watch;
  char output[1024];
  size_t used;
  ssize_t got;
  int fd;
  int ok;
  char status_text[16];

  assert(wire_length > 0 && wire_length <= INT_MAX);
  fd = connect_local(port);
  ctx = NULL;
  ssl = NULL;
  if (tls) {
    ctx = SSL_CTX_new(TLS_client_method());
    assert(ctx != NULL);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    ssl = SSL_new(ctx);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, fd) == 1);
    assert(SSL_set_tlsext_host_name(ssl, "localhost") == 1);
    assert(SSL_connect(ssl) == 1);
    assert(SSL_write(ssl, wire, (int)wire_length) == (int)wire_length);
  } else {
    send_exact(fd, wire, wire_length);
  }
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  output[0] = '\0';
  assert(snprintf(status_text, sizeof(status_text), " %d ", status) > 0);
  while (used < sizeof(output) - 1 &&
      strstr(output, marker) == NULL) {
    if ((!tls || SSL_pending(ssl) == 0) && poll(&watch, 1, 1000) <= 0)
      break;
    if (tls)
      got = SSL_read(ssl, output + used,
          (int)(sizeof(output) - 1 - used));
    else
      got = recv(fd, output + used, sizeof(output) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  ok = strstr(output, status_text) != NULL &&
      strstr(output, marker) != NULL;
  if (!ok)
    fprintf(stderr, "expected status %d and %s, received: %s\n",
        status, marker, output);
  fprintf(stderr, "%s %s admission visibility: %s\n",
      tls ? "TLS" : "cleartext", path, ok ? "passed" : "failed");
  assert(ok);
  if (ssl != NULL)
    SSL_free(ssl);
  if (ctx != NULL)
    SSL_CTX_free(ctx);
  assert(close(fd) == 0);
  return ok;
}

static int
check_prebody_method_body_result(unsigned short port, const char *method,
    const char *path, const char *version, const char *headers, int tls,
    const char *body, int status, const char *marker)
{
  char wire[512];
  int length;

  length = snprintf(wire, sizeof(wire),
      "%s %s HTTP/%s\r\nHost: localhost\r\n%s\r\n%s",
      method, path, version, headers, body == NULL ? "" : body);
  assert(length > 0 && (size_t)length < sizeof(wire));
  return check_prebody_wire_result(port, wire, (size_t)length, path, tls,
      status, marker);
}

static int
check_prebody_method_result(unsigned short port, const char *method,
    const char *path, const char *version, const char *headers, int tls,
    int status, const char *marker)
{
  return check_prebody_method_body_result(port, method, path, version,
      headers, tls, NULL, status, marker);
}

static int
check_prebody_header_result(unsigned short port, const char *path,
    const char *version, const char *headers, int tls, int status,
    const char *marker)
{
  return check_prebody_method_result(port, "GET", path, version, headers,
      tls, status, marker);
}

static int
check_framing_header_visibility(unsigned short port, const char *path,
    const char *version, const char *headers, int tls)
{
  return check_prebody_header_result(port, path, version, headers, tls,
      400, "prebody-framing-reject");
}

static int
check_header_count_boundary(unsigned short port, int tls)
{
  char headers[512];
  size_t used;
  int length;
  int index;

  used = 0;
  for (index = 0; index < HTTP_REQ_HEADER_MAX - 3; index++) {
    length = snprintf(headers + used, sizeof(headers) - used,
        "X-Pad: a\r\n");
    assert(length > 0 && (size_t)length < sizeof(headers) - used);
    used += (size_t)length;
  }
  assert(check_prebody_header_result(port, "/framing-header-limit",
      "1.1", headers, tls, 200, "prebody-header-limit-accepted"));
  length = snprintf(headers + used, sizeof(headers) - used,
      "Transfer-Encoding: chunked\r\n");
  assert(length > 0 && (size_t)length < sizeof(headers) - used);
  assert(check_prebody_header_result(port, "/framing-header-limit",
      "1.1", headers, tls, 400, "Bad Request"));
  return 1;
}

static int
check_embedded_header_nul(unsigned short port, int tls)
{
  static const char wire[] =
      "GET /framing-header-nul HTTP/1.1\r\n"
      "Host: localhost\r\nX-Pad: x\0\r\n"
      "Transfer-Encoding: chunked\r\n\r\n";

  return check_prebody_wire_result(port, wire, sizeof(wire) - 1u,
      "/framing-header-nul", tls, 400, "Bad Request");
}

static int
check_bare_header_cr(unsigned short port, int tls)
{
  static const char wire[] =
      "GET /framing-bare-cr HTTP/1.1\r\n"
      "Host: localhost\r\nX-Pad: x\r"
      "Transfer-Encoding: chunked\r\n\r\n";

  return check_prebody_wire_result(port, wire, sizeof(wire) - 1u,
      "/framing-bare-cr", tls, 400, "Bad Request");
}

int
main(void)
{
  static const char wire[] =
      "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n"
      "POST /probe HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 4\r\n\r\ndata"
      "GET /two HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char split_start[] =
      "POST /probe HTTP/1.1\r\nHost: localhost\r\n"
      "Content-Length: 4\r\n\r\nda";
  static const char split_end[] =
      "taGET /two HTTP/1.1\r\nHost: localhost\r\n\r\n";
  static const char static_content[] = "static-prebody-winner";
  vectis_app_config config;
  vectis_cert_bundle_config certs;
  vectis_route_config route;
  vectis_route_config raw_route;
  vectis_error error;
  vectis_app *app;
  struct pollfd watch;
  char output[4096];
  unsigned short port;
  size_t used;
  ssize_t got;
  int fd;
  unsigned count;
  unsigned split_count;
  int tls_passed;
  int raw_passed;
  int tls_raw_passed;
  int reject_passed;
  int chunked_passed;
  int small_chunked_passed;
  int tls_chunked_passed;
  int framed_methods_passed;
  int framing_headers_passed;
  int raw_target_passed;
  char cert_path[128];
  char key_path[128];
  char static_root[] = "proxy-static.XXXXXX";
  char static_root_abs[PATH_MAX];
  char static_file_path[PATH_MAX];
  char working_dir[PATH_MAX];
  FILE *static_file;
  int path_length;

  assert(getcwd(working_dir, sizeof(working_dir)) != NULL);
  assert(mkdtemp(static_root) != NULL);
  path_length = snprintf(static_root_abs, sizeof(static_root_abs), "%s/%s",
      working_dir, static_root);
  assert(path_length > 0 && (size_t)path_length < sizeof(static_root_abs));
  path_length = snprintf(static_file_path, sizeof(static_file_path),
      "%s/file.txt", static_root);
  assert(path_length > 0 && (size_t)path_length < sizeof(static_file_path));
  static_file = fopen(static_file_path, "wb");
  assert(static_file != NULL);
  assert(fwrite(static_content, 1, sizeof(static_content) - 1, static_file) ==
      sizeof(static_content) - 1);
  assert(fclose(static_file) == 0);
  port = available_port();
  vectis_kore_set_prebody_probe(probe_prebody);
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/one", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/two", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  raw_route = vectis_route(VECTIS_HTTP_GET,
      "^/ordinary-raw-target/.*$", reply, NULL);
  raw_route.path_kind = VECTIS_ROUTE_PATH_REGEX;
  assert(vectis_register_route(app, &raw_route, &error) == VECTIS_OK);
  register_proxy_static_overlap(app, static_root_abs, &error);
  register_proxy_selection_routes(app, 0, &error);
  probe_app = app;
  assert(app->start(app, &error) == VECTIS_OK);
  if (getenv("VECTIS_KORE_PREBODY_EXIT_AFTER_START") != NULL)
    _exit(71);

  fd = connect_local(port);
  assert(send(fd, wire, sizeof(wire) - 1, 0) == (ssize_t)(sizeof(wire) - 1));
  watch.fd = fd;
  watch.events = POLLIN;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1 && response_count(output) < 3) {
    if (poll(&watch, 1, 1000) <= 0)
      break;
    got = recv(fd, output + used, sizeof(output) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  count = response_count(output);
  fprintf(stderr, "ordinary + takeover + ordinary responses: %u\n", count);
  assert(close(fd) == 0);

  fd = connect_local(port);
  assert(send(fd, split_start, sizeof(split_start) - 1, 0) ==
      (ssize_t)(sizeof(split_start) - 1));
  usleep(20000u);
  assert(send(fd, split_end, sizeof(split_end) - 1, 0) ==
      (ssize_t)(sizeof(split_end) - 1));
  watch.fd = fd;
  used = 0;
  output[0] = '\0';
  while (used < sizeof(output) - 1 && response_count(output) < 2) {
    if (poll(&watch, 1, 1000) <= 0)
      break;
    got = recv(fd, output + used, sizeof(output) - 1 - used, 0);
    if (got <= 0)
      break;
    used += (size_t)got;
    output[used] = '\0';
  }
  split_count = response_count(output);
  fprintf(stderr, "split takeover + ordinary responses: %u\n", split_count);
  assert(close(fd) == 0);
  raw_passed = check_raw_handoff(port, 0);
  assert(check_request_halfclose_at_headers(port));
  chunked_passed = check_chunked_ingress(port);
  small_chunked_passed = check_small_chunked_ingress(port, 0);
  framed_methods_passed = check_framed_method_handoff(port, "GET", 0);
  framed_methods_passed &= check_framed_method_handoff(port, "OPTIONS", 0);
  framed_methods_passed &= check_expect_continue_handoff(port, 0);
  framed_methods_passed &= check_expect_rejection_close(port, 0);
  framing_headers_passed = check_framing_header_visibility(port,
      "/framing-cl-te", "1.1",
      "Content-Length: 4\r\nTransfer-Encoding: chunked\r\n", 0);
  framing_headers_passed &= check_framing_header_visibility(port,
      "/framing-duplicate-cl", "1.1",
      "Content-Length: 4\r\nContent-Length: 5\r\n", 0);
  framing_headers_passed &= check_framing_header_visibility(port,
      "/framing-duplicate-host", "1.1",
      "Host: attacker.invalid\r\n", 0);
  framing_headers_passed &= check_framing_header_visibility(port,
      "/framing-http10", "1.0", "", 0);
  framing_headers_passed &= check_header_count_boundary(port, 0);
  framing_headers_passed &= check_embedded_header_nul(port, 0);
  framing_headers_passed &= check_bare_header_cr(port, 0);
  raw_target_passed = check_prebody_header_result(port,
      "/raw-target/a%2Fb/%2e/c?q=1&q=2&plus=%2B&empty=",
      "1.1", "", 0, 200, "raw-target-preserved");
  raw_target_passed &= check_prebody_header_result(port,
      "/ordinary-raw-target/a%2Fb", "1.1", "", 0,
      400, "unsafe percent escape");
  raw_target_passed &= check_prebody_header_result(port,
      "/ordinary-raw-target/a%25b", "1.1", "", 0,
      400, "percent escapes or backslashes");
  raw_target_passed &= check_prebody_header_result(port,
      "/ordinary-raw-target/a%3Ab", "1.1", "", 0,
      400, "request path must not contain ':'");
  raw_target_passed &= check_prebody_header_result(port,
      "/ordinary-raw-target/%2e%2e/child", "1.1", "", 0,
      400, "dot segments");
  assert(check_prebody_header_result(port, "/proxy-select/a", "1.1", "",
      0, 200, "ok"));
  assert(check_prebody_header_result(port, "/proxy-select/%41", "1.1",
      "", 0, 200, "ok"));
  assert(check_prebody_header_result(port, "/proxy-select/a%2Fb", "1.1",
      "", 0, 200, "proxy-prebody-selected"));
  assert(check_prebody_header_result(port, "/proxy-select/a%25b", "1.1",
      "", 0, 200, "proxy-prebody-selected"));
  assert(check_prebody_header_result(port, "/proxy-select/a%3Ab", "1.1",
      "", 0, 200, "proxy-prebody-selected"));
  assert(check_prebody_header_result(port, "/proxy-select/%2e%2e", "1.1",
      "", 0, 400, "proxy-raw-rejected"));
  assert(check_prebody_header_result(port, "/proxy-select/%ZZ", "1.1",
      "", 0, 400, "proxy-raw-rejected"));
  assert(check_prebody_header_result(port, "/proxy-select/ws", "1.1",
      ws_upgrade_headers, 0, 101, "sec-websocket-accept:"));
  assert(check_prebody_header_result(port, "/proxy-select/ws", "1.1",
      "", 0, 400, " 400 "));
  assert(check_prebody_header_result(port, "/proxy-select/ws", "1.1",
      "Connection: Upgrade\r\nUpgrade: h2c\r\n", 0, 400, " 400 "));
  assert(check_prebody_header_result(port,
      "/proxy-select/static/file.txt", "1.1", "", 0, 200,
      "static-prebody-winner"));
  assert(check_prebody_method_result(port, "POST",
      "/proxy-select/static/file.txt", "1.1", "Content-Length: 0\r\n",
      0, 405, "allow: GET, HEAD"));
  assert(check_prebody_header_result(port,
      "/proxy-select/static/a%2Fb", "1.1", "", 0, 200,
      "proxy-prebody-selected"));
  assert(check_prebody_method_body_result(port, "POST",
      "/proxy-select/upload/a", "1.1", "Content-Length: 4\r\n",
      0, "data", 200, "live-upload-winner"));
  assert(check_prebody_method_body_result(port, "POST",
      "/proxy-select/upload/a%2Fb", "1.1", "Content-Length: 4\r\n",
      0, "data", 200, "proxy-prebody-selected"));
  assert(check_prebody_method_body_result(port, "POST",
      "/proxy-select/upload-reverse/a", "1.1", "Content-Length: 4\r\n",
      0, "data", 200, "proxy-prebody-selected"));
  reject_passed = check_local_rejection(port, "/reject", 400);
  reject_passed &= check_local_rejection(port, "/forbidden", 403);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  probe_app = NULL;

  assert(snprintf(cert_path, sizeof(cert_path),
      "/tmp/vectis-kore-prebody-%ld-cert.pem", (long)getpid()) > 0);
  assert(snprintf(key_path, sizeof(key_path),
      "/tmp/vectis-kore-prebody-%ld-key.pem", (long)getpid()) > 0);
  vectis_cert_bundle_config_init(&certs);
  certs.subject.common_name = "localhost";
  certs.dns_names = "localhost";
  certs.output_cert_path = cert_path;
  certs.output_key_path = key_path;
  certs.key_bits = 2048u;
  certs.valid_days = 1L;
  assert(vectis_cert_generate_bundle(&certs, &error) == VECTIS_OK);
  port = available_port();
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_MANUAL;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  config.tls.domain = "localhost";
  config.tls.certificate_path = cert_path;
  config.tls.private_key_path = key_path;
  config.tls.ca_bundle_path = cert_path;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/one", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  route = vectis_route(VECTIS_HTTP_GET, "/two", reply, NULL);
  assert(vectis_register_route(app, &route, &error) == VECTIS_OK);
  raw_route = vectis_route(VECTIS_HTTP_GET,
      "^/ordinary-raw-target/.*$", reply, NULL);
  raw_route.path_kind = VECTIS_ROUTE_PATH_REGEX;
  assert(vectis_register_route(app, &raw_route, &error) == VECTIS_OK);
  register_proxy_static_overlap(app, static_root_abs, &error);
  register_proxy_selection_routes(app, 1, &error);
  probe_app = app;
  assert(app->start(app, &error) == VECTIS_OK);
  tls_passed = check_tls_handoff(port, wire);
  tls_chunked_passed = check_small_chunked_ingress(port, 1);
  framed_methods_passed &= check_framed_method_handoff(port, "GET", 1);
  framed_methods_passed &= check_framed_method_handoff(port, "OPTIONS", 1);
  framed_methods_passed &= check_expect_continue_handoff(port, 1);
  framed_methods_passed &= check_expect_rejection_close(port, 1);
  framing_headers_passed &= check_framing_header_visibility(port,
      "/framing-cl-te", "1.1",
      "Content-Length: 4\r\nTransfer-Encoding: chunked\r\n", 1);
  framing_headers_passed &= check_framing_header_visibility(port,
      "/framing-duplicate-cl", "1.1",
      "Content-Length: 4\r\nContent-Length: 5\r\n", 1);
  framing_headers_passed &= check_framing_header_visibility(port,
      "/framing-duplicate-host", "1.1",
      "Host: attacker.invalid\r\n", 1);
  framing_headers_passed &= check_framing_header_visibility(port,
      "/framing-http10", "1.0", "", 1);
  framing_headers_passed &= check_header_count_boundary(port, 1);
  framing_headers_passed &= check_embedded_header_nul(port, 1);
  framing_headers_passed &= check_bare_header_cr(port, 1);
  raw_target_passed &= check_prebody_header_result(port,
      "/raw-target/a%2Fb/%2e/c?q=1&q=2&plus=%2B&empty=",
      "1.1", "", 1, 200, "raw-target-preserved");
  raw_target_passed &= check_prebody_header_result(port,
      "/ordinary-raw-target/a%2Fb", "1.1", "", 1,
      400, "unsafe percent escape");
  raw_target_passed &= check_prebody_header_result(port,
      "/ordinary-raw-target/a%25b", "1.1", "", 1,
      400, "percent escapes or backslashes");
  raw_target_passed &= check_prebody_header_result(port,
      "/ordinary-raw-target/a%3Ab", "1.1", "", 1,
      400, "request path must not contain ':'");
  raw_target_passed &= check_prebody_header_result(port,
      "/ordinary-raw-target/%2e%2e/child", "1.1", "", 1,
      400, "dot segments");
  assert(check_prebody_header_result(port, "/proxy-select/a", "1.1", "",
      1, 200, "proxy-prebody-selected"));
  assert(check_prebody_header_result(port, "/proxy-select/%41", "1.1",
      "", 1, 200, "proxy-prebody-selected"));
  assert(check_prebody_header_result(port, "/proxy-select/a%2Fb", "1.1",
      "", 1, 200, "proxy-prebody-selected"));
  assert(check_prebody_header_result(port, "/proxy-select/%2e%2e", "1.1",
      "", 1, 400, "proxy-raw-rejected"));
  assert(check_prebody_header_result(port, "/proxy-select/ws", "1.1",
      ws_upgrade_headers, 1, 101, "sec-websocket-accept:"));
  assert(check_prebody_header_result(port, "/proxy-select/ws", "1.1",
      "", 1, 400, " 400 "));
  assert(check_prebody_header_result(port, "/proxy-select/ws", "1.1",
      "Connection: Upgrade\r\nUpgrade: h2c\r\n", 1, 400, " 400 "));
  assert(check_prebody_header_result(port,
      "/proxy-select/static/file.txt", "1.1", "", 1, 200,
      "static-prebody-winner"));
  assert(check_prebody_method_result(port, "POST",
      "/proxy-select/static/file.txt", "1.1", "Content-Length: 0\r\n",
      1, 405, "allow: GET, HEAD"));
  assert(check_prebody_header_result(port,
      "/proxy-select/static/a%2Fb", "1.1", "", 1, 200,
      "proxy-prebody-selected"));
  assert(check_prebody_method_body_result(port, "POST",
      "/proxy-select/upload/a", "1.1", "Content-Length: 4\r\n",
      1, "data", 200, "proxy-prebody-selected"));
  assert(check_prebody_method_body_result(port, "POST",
      "/proxy-select/upload/a%2Fb", "1.1", "Content-Length: 4\r\n",
      1, "data", 200, "proxy-prebody-selected"));
  assert(check_prebody_method_body_result(port, "POST",
      "/proxy-select/upload-reverse/a", "1.1", "Content-Length: 4\r\n",
      1, "data", 200, "live-upload-winner"));
  tls_raw_passed = check_raw_handoff(port, 1);
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
  probe_app = NULL;
  assert(remove(cert_path) == 0);
  assert(remove(key_path) == 0);
  assert(remove(static_file_path) == 0);
  assert(rmdir(static_root) == 0);
  vectis_kore_set_prebody_probe(NULL);
  return count == 3 && split_count == 2 && tls_passed &&
      raw_passed && tls_raw_passed && reject_passed && chunked_passed &&
      small_chunked_passed && tls_chunked_passed && framed_methods_passed &&
      framing_headers_passed && raw_target_passed &&
      strstr(output, "data") != NULL ? 0 : 1;
}
