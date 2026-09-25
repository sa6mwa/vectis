#include "vectis_kore_proxy.h"
#include "vectis_kore_proxy_ws.h"

#include "vectis_internal.h"
#include "vectis_proxy_curl.h"
#include "vectis_proxy_director.h"
#include "vectis_proxy_events.h"
#include "vectis_proxy_headers.h"
#include "vectis_proxy_http_upstream.h"
#include "vectis_proxy_http_wire.h"
#include "vectis_proxy_select.h"
#include "vectis_proxy_upload.h"
#include "vectis_proxy_url.h"
#include "vectis_proxy_ws_handshake.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <kore/http.h>
#include <kore/kore.h>
#pragma GCC diagnostic pop

#include <errno.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

typedef struct vectis_kore_proxy_state {
  struct vectis_kore_proxy_state *next;
  struct http_request *request;
  struct connection *connection;
  vectis_app *app;
  vectis_request *route_request;
  vectis_proxy_route_data *route;
  vectis_proxy_http_upstream upstream;
  vectis_proxy_http_wire_plan wire;
  vectis_proxy_upload_buffer upload;
  vectis_proxy_curl_transfer *transfer;
  struct curl_slist *request_headers;
  struct kore_timer *wake_timer;
  char *request_target;
  char *authority;
  char *final_wire;
  char *frame;
  unsigned char *read_chunk;
  const unsigned char *surplus;
  size_t frame_capacity;
  size_t read_capacity;
  size_t read_length;
  size_t read_offset;
  size_t surplus_length;
  size_t surplus_offset;
  size_t final_length;
  int interest;
  int headers_ready;
  int headers_queued;
  int final_queued;
  int done;
  int failed;
  int active;
  int has_upload;
  int read_want_write;
} vectis_kore_proxy_state;

static vectis_kore_proxy_state *vectis_kore_proxy_states;

static void vectis_kore_proxy_event(void *userdata, int error);
static void vectis_kore_proxy_wake(void *userdata, u_int64_t now);
static int vectis_kore_proxy_upload_drive(vectis_kore_proxy_state *state);

static void vectis_kore_proxy_unlink(vectis_kore_proxy_state *state) {
  vectis_kore_proxy_state **slot;

  slot = &vectis_kore_proxy_states;
  while (*slot != NULL && *slot != state)
    slot = &(*slot)->next;
  if (*slot == state)
    *slot = state->next;
  state->next = NULL;
}

static void vectis_kore_proxy_free(vectis_kore_proxy_state *state) {
  if (state == NULL)
    return;
  if (state->wake_timer != NULL) {
    kore_timer_remove(state->wake_timer);
    state->wake_timer = NULL;
  }
  if (state->transfer != NULL) {
    vectis_proxy_curl_cancel(state->transfer);
    state->transfer = NULL;
  }
  vectis_kore_proxy_unlink(state);
  if (state->request != NULL && state->active) {
    state->request->flags |= HTTP_REQUEST_DELETE;
    http_request_wakeup(state->request);
    state->request = NULL;
  }
  if (state->route_request != NULL)
    vectis_internal_request_free(state->route_request);
  vectis_proxy_http_upstream_cleanup(&state->upstream);
  vectis_proxy_http_wire_plan_cleanup(&state->wire);
  vectis_proxy_upload_cleanup(&state->upload);
  curl_slist_free_all(state->request_headers);
  free(state->request_target);
  free(state->authority);
  free(state->final_wire);
  free(state->frame);
  free(state->read_chunk);
  free(state);
}

static void vectis_kore_proxy_disconnect(struct connection *connection) {
  vectis_kore_proxy_state *state;

  state = (vectis_kore_proxy_state *)connection->hdlr_extra;
  connection->hdlr_extra = NULL;
  vectis_kore_proxy_free(state);
}

static void vectis_kore_proxy_schedule(vectis_kore_proxy_state *state) {
  struct connection *connection;
  int desired;

  if (!state->active)
    return;
  connection = state->connection;
  if (connection->state == CONN_STATE_DISCONNECTING)
    return;
  desired = state->has_upload && !state->upload.complete &&
                    vectis_proxy_upload_full(&state->upload)
                ? 0
                : VECTIS_PROXY_EVENT_READ;
  if (state->read_want_write)
    desired |= VECTIS_PROXY_EVENT_WRITE;
  if (!TAILQ_EMPTY(&connection->send_queue) ||
      (state->failed && !state->headers_queued) ||
      (state->headers_ready && !state->headers_queued) ||
      (state->headers_queued &&
       (vectis_proxy_http_upstream_body(&state->upstream, NULL) != NULL ||
        state->done))) {
    if (connection->tls != NULL && SSL_want(connection->tls) == SSL_READING &&
        !TAILQ_EMPTY(&connection->send_queue))
      desired |= VECTIS_PROXY_EVENT_READ;
    else
      desired |= VECTIS_PROXY_EVENT_WRITE;
  }
  vectis_proxy_event_update(connection->fd, &connection->evt, state->interest,
                            desired, 0);
  state->interest = desired;
}

static void vectis_kore_proxy_request_wake(vectis_kore_proxy_state *state) {
  if (!state->active || state->wake_timer != NULL)
    return;
  state->wake_timer =
      kore_timer_add(vectis_kore_proxy_wake, 1u, state, KORE_TIMER_ONESHOT);
}

static void vectis_kore_proxy_wake(void *userdata, u_int64_t now) {
  vectis_kore_proxy_state *state;

  (void)now;
  state = (vectis_kore_proxy_state *)userdata;
  state->wake_timer = NULL;
  if (state->has_upload && !vectis_kore_proxy_upload_drive(state))
    return;
  vectis_kore_proxy_schedule(state);
}

static void vectis_kore_proxy_upload_consumed(void *userdata) {
  vectis_kore_proxy_request_wake((vectis_kore_proxy_state *)userdata);
}

static int vectis_kore_proxy_upload_drive(vectis_kore_proxy_state *state) {
  const unsigned char *data;
  vectis_proxy_frame_result result;
  struct connection *connection;
  size_t length;
  size_t consumed;
  int ssl_error;
  ssize_t got;

  connection = state->connection;
  while (!state->upload.complete && !vectis_proxy_upload_full(&state->upload)) {
    if (state->surplus_offset < state->surplus_length) {
      data = state->surplus + state->surplus_offset;
      length = state->surplus_length - state->surplus_offset;
      result =
          vectis_proxy_upload_feed(&state->upload, data, length, &consumed);
      state->surplus_offset += consumed;
    } else if (state->read_offset < state->read_length) {
      data = state->read_chunk + state->read_offset;
      length = state->read_length - state->read_offset;
      result =
          vectis_proxy_upload_feed(&state->upload, data, length, &consumed);
      state->read_offset += consumed;
      if (state->read_offset == state->read_length) {
        state->read_offset = 0u;
        state->read_length = 0u;
      }
    } else {
      if (connection->tls != NULL) {
        got = SSL_read(connection->tls, state->read_chunk,
                       (int)state->read_capacity);
        if (got <= 0) {
          ssl_error = SSL_get_error(connection->tls, (int)got);
          state->read_want_write = ssl_error == SSL_ERROR_WANT_WRITE;
          if (ssl_error == SSL_ERROR_WANT_READ ||
              ssl_error == SSL_ERROR_WANT_WRITE)
            break;
          kore_connection_disconnect(connection);
          return 0;
        }
      } else {
        got = recv(connection->fd, state->read_chunk, state->read_capacity,
                   MSG_DONTWAIT);
        if (got <= 0) {
          if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
          kore_connection_disconnect(connection);
          return 0;
        }
      }
      state->read_want_write = 0;
      state->read_length = (size_t)got;
      state->read_offset = 0u;
      continue;
    }
    if (result == VECTIS_PROXY_FRAME_INVALID) {
      kore_connection_disconnect(connection);
      return 0;
    }
    if (result == VECTIS_PROXY_FRAME_PAUSED)
      break;
  }
  if (state->transfer != NULL &&
      vectis_proxy_upload_can_resume(&state->upload)) {
    vectis_proxy_upload_resumed(&state->upload);
    if (curl_easy_pause(state->upstream.easy, CURLPAUSE_CONT) != CURLE_OK) {
      kore_connection_disconnect(connection);
      return 0;
    }
  }
  return 1;
}

static void vectis_kore_proxy_ready(vectis_proxy_http_upstream *upstream,
                                    vectis_proxy_http_event event,
                                    void *userdata) {
  vectis_kore_proxy_state *state;
  vectis_error error;

  state = (vectis_kore_proxy_state *)userdata;
  if (event == VECTIS_PROXY_HTTP_FINAL) {
    if (vectis_proxy_http_wire_plan_build(
            &upstream->response, &upstream->outbound_headers,
            state->route->upstream_http_version == VECTIS_PROXY_HTTP_AUTO,
            &state->wire, &error) != VECTIS_OK)
      state->failed = 1;
    else
      state->headers_ready = 1;
  }
  vectis_kore_proxy_request_wake(state);
}

static void vectis_kore_proxy_done(CURL *easy, CURLcode result,
                                   void *userdata) {
  vectis_kore_proxy_state *state;
  const char *reason;

  (void)easy;
  state = (vectis_kore_proxy_state *)userdata;
  state->transfer = NULL;
  reason = NULL;
  if (vectis_proxy_http_upstream_finish(&state->upstream, result, &reason) !=
      VECTIS_PROXY_HEADER_OK)
    state->failed = 1;
  state->done = 1;
  vectis_kore_proxy_request_wake(state);
}

static int vectis_kore_proxy_pump(vectis_kore_proxy_state *state) {
  static const char gateway_error[] =
      "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 11\r\n"
      "Connection: close\r\n\r\nbad gateway";
  struct connection *connection;
  const unsigned char *body;
  vectis_error error;
  size_t body_length;
  size_t written;

  connection = state->connection;
  if (!TAILQ_EMPTY(&connection->send_queue)) {
    connection->evt.flags |= KORE_EVENT_WRITE;
    if (net_send_flush(connection) != KORE_RESULT_OK) {
      kore_connection_disconnect(connection);
      return 0;
    }
    if (connection->state == CONN_STATE_DISCONNECTING)
      return 0;
    if (!TAILQ_EMPTY(&connection->send_queue)) {
      vectis_kore_proxy_schedule(state);
      return 1;
    }
  }
  if (state->failed) {
    if (state->headers_queued) {
      kore_connection_disconnect(connection);
      return 0;
    }
    net_send_queue(connection, gateway_error, sizeof(gateway_error) - 1u);
    state->headers_queued = 1;
    state->final_queued = 1;
    state->done = 1;
    state->failed = 0;
    state->request->status = 502;
    vectis_internal_metrics_note_http_status(state->app, 502);
    connection->http_response_count++;
    vectis_kore_proxy_schedule(state);
    return 1;
  }
  if (state->headers_ready && !state->headers_queued) {
    net_send_queue(connection, state->wire.head, state->wire.head_length);
    state->headers_queued = 1;
    state->request->status = (u_int16_t)state->upstream.response.status;
    vectis_internal_metrics_note_http_status(state->app,
                                             state->upstream.response.status);
    connection->http_response_count++;
    vectis_kore_proxy_schedule(state);
    return 1;
  }
  if (state->headers_queued && !state->final_queued) {
    body = vectis_proxy_http_upstream_body(&state->upstream, &body_length);
    if (body != NULL) {
      if (vectis_proxy_http_wire_chunk(&state->wire, body, body_length,
                                       state->frame, state->frame_capacity,
                                       &written, &error) != VECTIS_OK) {
        kore_connection_disconnect(connection);
        return 0;
      }
      net_send_queue(connection, state->frame, written);
      vectis_proxy_http_upstream_consume(&state->upstream, body_length);
      vectis_kore_proxy_schedule(state);
      return 1;
    }
    if (state->transfer != NULL && state->upstream.paused) {
      if (vectis_proxy_http_upstream_resume(&state->upstream, &error) !=
          VECTIS_OK) {
        kore_connection_disconnect(connection);
        return 0;
      }
      body = vectis_proxy_http_upstream_body(&state->upstream, &body_length);
      if (body != NULL) {
        vectis_kore_proxy_schedule(state);
        return 1;
      }
    }
    if (state->done) {
      if (vectis_proxy_http_wire_finish(
              &state->wire, &state->upstream.response.trailers,
              &state->final_wire, &state->final_length, &error) != VECTIS_OK) {
        kore_connection_disconnect(connection);
        return 0;
      }
      state->final_queued = 1;
      if (state->final_length != 0u)
        net_send_queue(connection, state->final_wire, state->final_length);
      else {
        kore_connection_disconnect(connection);
        return 0;
      }
      vectis_kore_proxy_schedule(state);
      return 1;
    }
  }
  if (state->final_queued && TAILQ_EMPTY(&connection->send_queue)) {
    kore_connection_disconnect(connection);
    return 0;
  }
  vectis_kore_proxy_schedule(state);
  return 1;
}

static void vectis_kore_proxy_event(void *userdata, int error) {
  struct connection *connection;
  vectis_kore_proxy_state *state;
  unsigned char byte;
  ssize_t count;
  int ssl_error;

  connection = (struct connection *)userdata;
  if (connection->state == CONN_STATE_DISCONNECTING)
    return;
  state = (vectis_kore_proxy_state *)connection->hdlr_extra;
  if (state == NULL)
    return;
  if (error) {
    kore_connection_disconnect(connection);
    return;
  }
  if ((connection->evt.flags & KORE_EVENT_READ) != 0 ||
      (state->read_want_write &&
       (connection->evt.flags & KORE_EVENT_WRITE) != 0)) {
    if (state->has_upload && !state->upload.complete) {
      if (!vectis_kore_proxy_upload_drive(state))
        return;
    } else if (connection->tls != NULL) {
      count = SSL_peek(connection->tls, &byte, 1);
      if (count > 0) {
        kore_connection_disconnect(connection);
        return;
      }
      ssl_error = SSL_get_error(connection->tls, (int)count);
      if (ssl_error != SSL_ERROR_WANT_READ) {
        kore_connection_disconnect(connection);
        return;
      }
    } else {
      count = recv(connection->fd, &byte, 1u, MSG_PEEK | MSG_DONTWAIT);
      if (count >= 0) {
        kore_connection_disconnect(connection);
        return;
      }
      if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        kore_connection_disconnect(connection);
        return;
      }
    }
  }
  connection->evt.flags = 0;
  (void)vectis_kore_proxy_pump(state);
}

static vectis_proxy_header_status
vectis_kore_proxy_request_headers(struct http_request *request,
                                  vectis_proxy_headers *headers) {
  struct http_header *header;
  vectis_proxy_header_status status;

  vectis_proxy_headers_init(headers);
  status = vectis_proxy_headers_add(headers, "Host", request->host);
  if (status != VECTIS_PROXY_HEADER_OK)
    return status;
  TAILQ_FOREACH(header, &request->req_headers, list) {
    status = vectis_proxy_headers_add(headers, header->header, header->value);
    if (status != VECTIS_PROXY_HEADER_OK)
      return status;
  }
  return VECTIS_PROXY_HEADER_OK;
}

static int vectis_kore_proxy_add_header(vectis_kore_proxy_state *state,
                                        const char *name, const char *value) {
  struct curl_slist *next;
  size_t length;
  char *field;

  length = strlen(name) + strlen(value) + 3u;
  field = (char *)malloc(length);
  if (field == NULL)
    return 0;
  (void)snprintf(field, length, "%s: %s", name, value);
  next = curl_slist_append(state->request_headers, field);
  free(field);
  if (next == NULL)
    return 0;
  state->request_headers = next;
  return 1;
}

static int vectis_kore_proxy_reject(struct http_request *request, int status,
                                    const char *message) {
  request->owner->flags |= CONN_CLOSE_EMPTY;
  http_response(request, status, message, strlen(message));
  return KORE_RESULT_ERROR;
}

int vectis_kore_proxy_prebody(struct http_request *request, const void *surplus,
                              size_t surplus_length, vectis_app *app,
                              vectis_http_method method) {
  vectis_kore_proxy_state *state;
  vectis_proxy_route_data *route;
  vectis_proxy_headers inbound;
  vectis_proxy_headers outbound;
  vectis_proxy_outbound directed;
  vectis_proxy_request_head head;
  vectis_proxy_header_status header_status;
  vectis_request *route_request;
  vectis_error error;
  vectis_status status;
  const char *reason;
  const char *method_name;
  const char *selected_url;
  char *request_target;
  char *authority;
  vectis_proxy_http_upload upload_config;
  vectis_proxy_frame_result frame_result;
  size_t i;
  int valid_ws;
  int result;
  CURL *easy;

  if (app == NULL || request == NULL || request->path == NULL)
    return KORE_RESULT_OK;
  vectis_error_clear(&error);
  route_request = vectis_internal_request_new(&error);
  if (route_request == NULL)
    return vectis_kore_proxy_reject(request, 500, "proxy admission failed\n");
  vectis_internal_request_set_method(route_request, method);
  route = NULL;
  status = vectis_proxy_select_route(app, method, request->path, route_request,
                                     &route, &error);
  if (status != VECTIS_OK) {
    vectis_internal_request_free(route_request);
    return vectis_kore_proxy_reject(request,
                                    status == VECTIS_ERR_INVALID ? 400 : 500,
                                    "proxy route selection failed\n");
  }
  if (route == NULL) {
    vectis_internal_request_free(route_request);
    return KORE_RESULT_OK;
  }
  vectis_proxy_headers_init(&inbound);
  vectis_proxy_headers_init(&outbound);
  header_status = vectis_kore_proxy_request_headers(request, &inbound);
  reason = NULL;
  if (header_status == VECTIS_PROXY_HEADER_OK)
    header_status = vectis_proxy_request_head_parse(&inbound, &head, &reason);
  if (header_status == VECTIS_PROXY_HEADER_OK)
    header_status = vectis_proxy_headers_sanitize_request(&inbound, &outbound);
  if (header_status != VECTIS_PROXY_HEADER_OK ||
      (request->flags & HTTP_VERSION_1_0) != 0) {
    vectis_proxy_headers_cleanup(&outbound);
    vectis_proxy_headers_cleanup(&inbound);
    vectis_internal_request_free(route_request);
    return vectis_kore_proxy_reject(request, 400,
                                    "invalid proxy request headers\n");
  }
  if (head.websocket_upgrade) {
    valid_ws = vectis_proxy_ws_request_valid(method, &inbound, &reason);
    if (valid_ws && (head.chunked || head.has_content_length))
      valid_ws = 0;
    if (!valid_ws) {
      vectis_proxy_headers_cleanup(&outbound);
      vectis_proxy_headers_cleanup(&inbound);
      vectis_internal_request_free(route_request);
      return vectis_kore_proxy_reject(request, 400,
                                      "invalid proxy WebSocket handshake\n");
    }
  }
  memset(&directed, 0, sizeof(directed));
  request_target = NULL;
  authority = NULL;
  status = vectis_proxy_director_prepare(
      route, method, request->path, request->query_string, head.host,
      head.websocket_upgrade, route_request, &inbound, &outbound, &directed,
      &request_target, &authority, &error);
  if (status != VECTIS_OK) {
    vectis_proxy_director_cleanup(&directed);
    vectis_proxy_headers_cleanup(&outbound);
    vectis_proxy_headers_cleanup(&inbound);
    vectis_internal_request_free(route_request);
    return vectis_kore_proxy_reject(request,
                                    status == VECTIS_ERR_INVALID ? 400 : 500,
                                    "invalid proxy rewrite\n");
  }
  selected_url = route->targets[directed.target_index];
  method_name = vectis_http_method_string(directed.method);
  if (head.websocket_upgrade) {
    result = vectis_kore_proxy_ws_start(
        request, surplus, surplus_length, app, route_request, route, &inbound,
        &directed.headers, selected_url, request_target, authority);
    free(request_target);
    free(authority);
    vectis_proxy_director_cleanup(&directed);
    vectis_proxy_headers_cleanup(&outbound);
    vectis_proxy_headers_cleanup(&inbound);
    if (result != KORE_RESULT_RETRY)
      vectis_internal_request_free(route_request);
    return result;
  }
  state = (vectis_kore_proxy_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    free(request_target);
    free(authority);
    vectis_proxy_director_cleanup(&directed);
    vectis_proxy_headers_cleanup(&outbound);
    vectis_proxy_headers_cleanup(&inbound);
    vectis_internal_request_free(route_request);
    return vectis_kore_proxy_reject(request, 500, "proxy allocation failed\n");
  }
  state->request = request;
  state->connection = request->owner;
  state->app = app;
  state->route_request = route_request;
  state->route = route;
  state->request_target = request_target;
  state->authority = authority;
  state->has_upload = head.chunked || head.has_content_length;
  if (state->has_upload) {
    status = vectis_proxy_upload_init(&state->upload, route->buffer_limit_bytes,
                                      head.chunked, head.content_length,
                                      &inbound, &error);
    if (status == VECTIS_OK) {
      state->upload.on_consume = vectis_kore_proxy_upload_consumed;
      state->upload.consume_userdata = state;
      state->surplus = (const unsigned char *)surplus;
      state->surplus_length = surplus_length;
      state->read_capacity = route->buffer_limit_bytes;
      state->read_chunk = (unsigned char *)malloc(state->read_capacity);
      if (state->read_chunk == NULL)
        status = VECTIS_ERR_NOMEM;
    }
    if (status != VECTIS_OK) {
      vectis_proxy_director_cleanup(&directed);
      vectis_proxy_headers_cleanup(&outbound);
      vectis_proxy_headers_cleanup(&inbound);
      vectis_kore_proxy_free(state);
      return vectis_kore_proxy_reject(request, 500,
                                      "proxy upload allocation failed\n");
    }
    if (surplus_length != 0u) {
      frame_result = vectis_proxy_upload_feed(
          &state->upload, (const unsigned char *)surplus, surplus_length,
          &state->surplus_offset);
      if (frame_result == VECTIS_PROXY_FRAME_INVALID) {
        vectis_proxy_director_cleanup(&directed);
        vectis_proxy_headers_cleanup(&outbound);
        vectis_proxy_headers_cleanup(&inbound);
        vectis_kore_proxy_free(state);
        return vectis_kore_proxy_reject(request, 400,
                                        "invalid proxy request body\n");
      }
    }
  }
  state->frame_capacity = (route->buffer_limit_bytes > CURL_MAX_WRITE_SIZE
                               ? route->buffer_limit_bytes
                               : CURL_MAX_WRITE_SIZE) +
                          32u;
  state->frame = (char *)malloc(state->frame_capacity);
  if (state->frame == NULL) {
    vectis_proxy_director_cleanup(&directed);
    vectis_proxy_headers_cleanup(&outbound);
    vectis_proxy_headers_cleanup(&inbound);
    vectis_kore_proxy_free(state);
    return vectis_kore_proxy_reject(request, 500, "proxy allocation failed\n");
  }
  if (directed.host != NULL &&
      !vectis_kore_proxy_add_header(state, "Host", authority)) {
    vectis_proxy_director_cleanup(&directed);
    vectis_proxy_headers_cleanup(&outbound);
    vectis_proxy_headers_cleanup(&inbound);
    vectis_kore_proxy_free(state);
    return vectis_kore_proxy_reject(request, 500,
                                    "proxy header allocation failed\n");
  }
  for (i = 0u; i < directed.headers.count; ++i) {
    if (!vectis_kore_proxy_add_header(state, directed.headers.fields[i].name,
                                      directed.headers.fields[i].value)) {
      vectis_proxy_director_cleanup(&directed);
      vectis_proxy_headers_cleanup(&outbound);
      vectis_proxy_headers_cleanup(&inbound);
      vectis_kore_proxy_free(state);
      return vectis_kore_proxy_reject(request, 500,
                                      "proxy header allocation failed\n");
    }
  }
  if (state->has_upload) {
    struct curl_slist *next;
    next = curl_slist_append(state->request_headers, "Expect:");
    if (next == NULL) {
      vectis_proxy_director_cleanup(&directed);
      vectis_proxy_headers_cleanup(&outbound);
      vectis_proxy_headers_cleanup(&inbound);
      vectis_kore_proxy_free(state);
      return vectis_kore_proxy_reject(request, 500,
                                      "proxy header allocation failed\n");
    }
    state->request_headers = next;
  }
  if (head.has_trailers) {
    for (i = 0u; i < inbound.count; ++i) {
      if (strcasecmp(inbound.fields[i].name, "trailer") != 0)
        continue;
      if (!vectis_kore_proxy_add_header(state, "Trailer",
                                        inbound.fields[i].value)) {
        vectis_proxy_director_cleanup(&directed);
        vectis_proxy_headers_cleanup(&outbound);
        vectis_proxy_headers_cleanup(&inbound);
        vectis_kore_proxy_free(state);
        return vectis_kore_proxy_reject(request, 500,
                                        "proxy header allocation failed\n");
      }
    }
  }
  vectis_proxy_director_cleanup(&directed);
  vectis_proxy_headers_cleanup(&outbound);
  vectis_proxy_headers_cleanup(&inbound);
  easy = curl_easy_init();
  memset(&upload_config, 0, sizeof(upload_config));
  if (state->has_upload) {
    upload_config.read = vectis_proxy_upload_read;
    upload_config.userdata = &state->upload;
    upload_config.content_length = head.content_length;
    upload_config.known_length = !head.chunked;
    if (head.has_trailers)
      upload_config.trailers = vectis_proxy_upload_curl_trailers;
  }
  if (easy == NULL ||
      vectis_proxy_curl_set_ca(easy, route->tls_ca_pem,
                               route->tls_ca_pem_length, &error) != VECTIS_OK ||
      vectis_proxy_http_upstream_init(
          &state->upstream, easy, selected_url, state->request_target,
          method_name, state->request_headers, route->buffer_limit_bytes,
          route->connect_timeout_ms, route->total_timeout_ms,
          state->has_upload ? &upload_config : NULL, vectis_kore_proxy_ready,
          state, &error) != VECTIS_OK) {
    if (easy != NULL)
      curl_easy_cleanup(easy);
    vectis_kore_proxy_free(state);
    return vectis_kore_proxy_reject(request, 500,
                                    "proxy upstream setup failed\n");
  }
  status = vectis_proxy_curl_submit(
      easy,
      route->upstream_http_version == VECTIS_PROXY_HTTP_1_1 ||
          head.has_trailers,
      vectis_kore_proxy_done, state, &state->transfer, &error);
  if (status != VECTIS_OK) {
    curl_easy_cleanup(easy);
    vectis_kore_proxy_free(state);
    return vectis_kore_proxy_reject(request,
                                    status == VECTIS_ERR_STATE ? 503 : 502,
                                    "proxy upstream unavailable\n");
  }
  state->next = vectis_kore_proxy_states;
  vectis_kore_proxy_states = state;
  state->active = 1;
  request->owner->hdlr_extra = state;
  request->owner->disconnect = vectis_kore_proxy_disconnect;
  request->owner->evt.handle = vectis_kore_proxy_event;
  request->owner->evt.flags &= ~KORE_EVENT_READ;
  request->owner->flags |= CONN_IS_BUSY;
  request->owner->http_timeout = 0u;
  http_request_sleep(request);
  if (head.expect_continue && surplus_length == 0u)
    net_send_queue(request->owner, "HTTP/1.1 100 Continue\r\n\r\n", 25u);
  vectis_kore_proxy_schedule(state);
  return KORE_RESULT_RETRY;
}

void vectis_kore_proxy_worker_cleanup(void) {
  vectis_kore_proxy_state *state;

  vectis_kore_proxy_ws_worker_cleanup();
  for (state = vectis_kore_proxy_states; state != NULL; state = state->next) {
    if (state->wake_timer != NULL) {
      kore_timer_remove(state->wake_timer);
      state->wake_timer = NULL;
    }
    if (state->transfer != NULL) {
      vectis_proxy_curl_cancel(state->transfer);
      state->transfer = NULL;
    }
    state->active = 0;
  }
}
