#include "vectis_kore_proxy_ws.h"

#include "vectis_kore_proxy_local.h"

#include "vectis_internal.h"
#include "vectis_proxy_curl.h"
#include "vectis_proxy_events.h"
#include "vectis_proxy_url.h"
#include "vectis_proxy_ws_handshake.h"
#include "vectis_proxy_ws_rejection.h"
#include "vectis_proxy_ws_wire.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <kore/http.h>
#include <kore/kore.h>
#pragma GCC diagnostic pop

#include <errno.h>
#include <openssl/ssl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define VECTIS_KORE_WS_HEAD_CAPACITY (VECTIS_PROXY_HEADER_BLOCK_LIMIT + 16384u)
#define VECTIS_KORE_WS_WORK_BUDGET 64u

typedef enum vectis_kore_ws_phase {
  VECTIS_KORE_WS_CONNECTING = 0,
  VECTIS_KORE_WS_SEND_HEAD = 1,
  VECTIS_KORE_WS_READ_HEAD = 2,
  VECTIS_KORE_WS_RELAY = 3,
  VECTIS_KORE_WS_REJECTION = 4,
  VECTIS_KORE_WS_ERROR = 5
} vectis_kore_ws_phase;

typedef struct vectis_kore_ws_state {
  struct vectis_kore_ws_state *next;
  struct kore_event upstream_event;
  struct connection *downstream;
  struct http_request *request;
  vectis_app *app;
  vectis_request *route_request;
  vectis_proxy_route_data *route;
  vectis_proxy_headers inbound;
  vectis_proxy_ws_rejection rejection;
  vectis_proxy_curl_transfer *transfer;
  struct kore_timer *wake_timer;
  struct kore_timer *idle_timer;
  struct kore_timer *total_timer;
  CURL *easy;
  curl_socket_t upstream_fd;
  char *request_wire;
  unsigned char *response_head;
  unsigned char *to_upstream;
  unsigned char *to_downstream;
  const unsigned char *initial;
  size_t initial_length;
  size_t initial_offset;
  size_t buffer_capacity;
  size_t request_length;
  size_t request_offset;
  size_t response_length;
  size_t up_length;
  size_t up_offset;
  size_t down_length;
  int upstream_interest;
  int downstream_interest;
  int upstream_ready;
  int down_read_want_write;
  int upstream_again;
  int upstream_error;
  int upstream_eof;
  int upstream_tls;
  unsigned long retry_delay_ms;
  int active;
  int closing;
  int rejection_finished;
  int error_status;
  vectis_kore_ws_phase phase;
} vectis_kore_ws_state;

static vectis_kore_ws_state *vectis_kore_ws_states;

static void vectis_kore_ws_pump(vectis_kore_ws_state *state);
static void vectis_kore_ws_schedule(vectis_kore_ws_state *state);

static void vectis_kore_ws_unlink(vectis_kore_ws_state *state) {
  vectis_kore_ws_state **slot;

  slot = &vectis_kore_ws_states;
  while (*slot != NULL && *slot != state)
    slot = &(*slot)->next;
  if (*slot == state)
    *slot = state->next;
  state->next = NULL;
}

static void vectis_kore_ws_free(vectis_kore_ws_state *state) {
  if (state == NULL)
    return;
  if (state->wake_timer != NULL)
    kore_timer_remove(state->wake_timer);
  if (state->idle_timer != NULL)
    kore_timer_remove(state->idle_timer);
  if (state->total_timer != NULL)
    kore_timer_remove(state->total_timer);
  if (state->upstream_interest != 0) {
    vectis_proxy_event_update((int)state->upstream_fd, &state->upstream_event,
                              state->upstream_interest, 0, 1);
    state->upstream_interest = 0;
  }
  if (state->transfer != NULL)
    vectis_proxy_curl_cancel(state->transfer);
  vectis_kore_ws_unlink(state);
  if (state->request != NULL && state->active) {
    state->request->flags |= HTTP_REQUEST_DELETE;
    http_request_wakeup(state->request);
  }
  if (state->route_request != NULL)
    vectis_internal_request_free(state->route_request);
  vectis_proxy_headers_cleanup(&state->inbound);
  vectis_proxy_ws_rejection_cleanup(&state->rejection);
  free(state->request_wire);
  free(state->response_head);
  free(state->to_upstream);
  free(state->to_downstream);
  free(state);
}

static void vectis_kore_ws_disconnect(struct connection *connection) {
  vectis_kore_ws_state *state;

  state = (vectis_kore_ws_state *)connection->hdlr_extra;
  connection->hdlr_extra = NULL;
  vectis_kore_ws_free(state);
}

static void vectis_kore_ws_wake(void *userdata, u_int64_t now) {
  vectis_kore_ws_state *state;

  (void)now;
  state = (vectis_kore_ws_state *)userdata;
  state->wake_timer = NULL;
  vectis_kore_ws_pump(state);
}

static void vectis_kore_ws_request_wake(vectis_kore_ws_state *state,
                                        u_int64_t delay) {
  if (!state->active || state->wake_timer != NULL)
    return;
  state->wake_timer =
      kore_timer_add(vectis_kore_ws_wake, delay, state, KORE_TIMER_ONESHOT);
}

static void vectis_kore_ws_idle(void *userdata, u_int64_t now) {
  vectis_kore_ws_state *state;

  (void)now;
  state = (vectis_kore_ws_state *)userdata;
  state->idle_timer = NULL;
  if (state->phase == VECTIS_KORE_WS_RELAY ||
      state->phase == VECTIS_KORE_WS_REJECTION)
    state->closing = 1;
  else {
    state->error_status = 504;
    state->phase = VECTIS_KORE_WS_ERROR;
  }
  vectis_kore_ws_pump(state);
}

static void vectis_kore_ws_total(void *userdata, u_int64_t now) {
  vectis_kore_ws_state *state;

  (void)now;
  state = (vectis_kore_ws_state *)userdata;
  state->total_timer = NULL;
  if (state->phase == VECTIS_KORE_WS_RELAY ||
      state->phase == VECTIS_KORE_WS_REJECTION)
    state->closing = 1;
  else {
    state->error_status = 504;
    state->phase = VECTIS_KORE_WS_ERROR;
  }
  vectis_kore_ws_pump(state);
}

static void vectis_kore_ws_progress(vectis_kore_ws_state *state) {
  if (state->idle_timer != NULL)
    kore_timer_remove(state->idle_timer);
  state->idle_timer = kore_timer_add(vectis_kore_ws_idle,
                                     (u_int64_t)state->route->idle_timeout_ms,
                                     state, KORE_TIMER_ONESHOT);
}

static void vectis_kore_ws_connected(CURL *easy, CURLcode result,
                                     void *userdata) {
  vectis_kore_ws_state *state;
  vectis_error error;

  (void)easy;
  state = (vectis_kore_ws_state *)userdata;
  if (result != CURLE_OK) {
    state->transfer = NULL;
    state->error_status = result == CURLE_OPERATION_TIMEDOUT ? 504 : 502;
    state->phase = VECTIS_KORE_WS_ERROR;
  } else if (vectis_proxy_curl_handoff_socket(
                 state->transfer, &state->upstream_fd, &error) != VECTIS_OK) {
    state->error_status = 502;
    state->phase = VECTIS_KORE_WS_ERROR;
  } else {
    state->phase = VECTIS_KORE_WS_SEND_HEAD;
  }
  vectis_kore_ws_request_wake(state, 1u);
}

static void vectis_kore_ws_schedule(vectis_kore_ws_state *state) {
  struct connection *connection;
  int upstream;
  int downstream;

  if (!state->active)
    return;
  connection = state->downstream;
  if (connection->state == CONN_STATE_DISCONNECTING)
    return;
  upstream = 0;
  downstream = 0;
  if (state->upstream_fd != CURL_SOCKET_BAD && !state->closing) {
    if (state->phase == VECTIS_KORE_WS_SEND_HEAD ||
        (state->phase == VECTIS_KORE_WS_RELAY &&
         state->up_offset < state->up_length))
      upstream |= VECTIS_PROXY_EVENT_WRITE;
    if ((state->phase == VECTIS_KORE_WS_READ_HEAD &&
         state->response_length == 0u) ||
        (state->phase == VECTIS_KORE_WS_REJECTION &&
         !state->rejection_finished && state->response_length == 0u &&
         state->down_length == 0u && TAILQ_EMPTY(&connection->send_queue)) ||
        (state->phase == VECTIS_KORE_WS_RELAY && state->down_length == 0u &&
         TAILQ_EMPTY(&connection->send_queue)))
      upstream |= VECTIS_PROXY_EVENT_READ;
    if (state->upstream_again)
      upstream |= VECTIS_PROXY_EVENT_READ | VECTIS_PROXY_EVENT_WRITE;
  }
  if (!TAILQ_EMPTY(&connection->send_queue) ||
      state->phase == VECTIS_KORE_WS_ERROR || state->down_length != 0u) {
    if (connection->tls != NULL && SSL_want(connection->tls) == SSL_READING)
      downstream |= VECTIS_PROXY_EVENT_READ;
    else
      downstream |= VECTIS_PROXY_EVENT_WRITE;
  }
  if (state->phase == VECTIS_KORE_WS_RELAY &&
      state->up_offset == state->up_length &&
      state->initial_offset == state->initial_length && !state->closing)
    downstream |= state->down_read_want_write ? VECTIS_PROXY_EVENT_WRITE
                                              : VECTIS_PROXY_EVENT_READ;
  if (state->upstream_fd != CURL_SOCKET_BAD) {
    vectis_proxy_event_update((int)state->upstream_fd, &state->upstream_event,
                              state->upstream_interest, upstream, 1);
    state->upstream_interest = upstream;
  }
  vectis_proxy_event_update(connection->fd, &connection->evt,
                            state->downstream_interest, downstream, 1);
  state->downstream_interest = downstream;
}

static int vectis_kore_ws_read_down(vectis_kore_ws_state *state) {
  struct connection *connection;
  ssize_t got;
  int ssl_error;

  connection = state->downstream;
  if (state->initial_offset < state->initial_length) {
    size_t length;

    length = state->initial_length - state->initial_offset;
    if (length > state->buffer_capacity)
      length = state->buffer_capacity;
    memcpy(state->to_upstream, state->initial + state->initial_offset, length);
    state->initial_offset += length;
    state->up_length = length;
    return 1;
  }
  if (connection->tls != NULL) {
    got = SSL_read(connection->tls, state->to_upstream,
                   (int)state->buffer_capacity);
    if (got <= 0) {
      ssl_error = SSL_get_error(connection->tls, (int)got);
      state->down_read_want_write = ssl_error == SSL_ERROR_WANT_WRITE;
      if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
        return 0;
      state->closing = 1;
      return 0;
    }
  } else {
    got = recv(connection->fd, state->to_upstream, state->buffer_capacity,
               MSG_DONTWAIT);
    if (got <= 0) {
      if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
      state->closing = 1;
      return 0;
    }
  }
  state->down_read_want_write = 0;
  state->up_length = (size_t)got;
  return 1;
}

static int vectis_kore_ws_response(vectis_kore_ws_state *state) {
  vectis_proxy_headers headers;
  vectis_proxy_ws_head_result result;
  vectis_error error;
  const char *reason;
  char *wire;
  size_t head_length;
  size_t wire_length;
  size_t surplus;
  int final;
  unsigned status;

  vectis_proxy_headers_init(&headers);
  result = vectis_proxy_ws_wire_response_head(
      state->response_head, state->response_length, &head_length, &status,
      &headers, &reason);
  if (result == VECTIS_PROXY_WS_HEAD_MORE)
    return 0;
  if (result != VECTIS_PROXY_WS_HEAD_COMPLETE) {
    state->error_status = 502;
    state->phase = VECTIS_KORE_WS_ERROR;
    vectis_proxy_headers_cleanup(&headers);
    return 0;
  }
  if (status != 101u) {
    wire = NULL;
    if (vectis_proxy_ws_rejection_head(
            &state->rejection, state->response_head, head_length, &final, &wire,
            &wire_length, state->route->modify_response,
            state->route->modify_response_userdata, &error) != VECTIS_OK) {
      state->error_status = 502;
      state->phase = VECTIS_KORE_WS_ERROR;
      vectis_proxy_headers_cleanup(&headers);
      return 0;
    }
    vectis_proxy_headers_cleanup(&headers);
    surplus = state->response_length - head_length;
    memmove(state->response_head, state->response_head + head_length, surplus);
    state->response_length = surplus;
    net_send_queue(state->downstream, wire, wire_length);
    free(wire);
    if (final) {
      state->request->status = (u_int16_t)state->rejection.downstream_status;
      vectis_internal_metrics_note_http_status(
          state->app, state->rejection.downstream_status);
      state->downstream->http_response_count++;
      state->phase = VECTIS_KORE_WS_REJECTION;
    }
    return 1;
  }
  if (!vectis_proxy_ws_response_valid(&state->inbound, status, &headers,
                                      &reason)) {
    state->error_status = 502;
    state->phase = VECTIS_KORE_WS_ERROR;
    vectis_proxy_headers_cleanup(&headers);
    return 0;
  }
  wire = NULL;
  if (vectis_proxy_ws_wire_upgrade_response(&headers, &wire, &wire_length,
                                            &error) != VECTIS_OK) {
    state->error_status = 502;
    state->phase = VECTIS_KORE_WS_ERROR;
    vectis_proxy_headers_cleanup(&headers);
    return 0;
  }
  vectis_proxy_headers_cleanup(&headers);
  if (state->response_length - head_length > state->buffer_capacity) {
    free(wire);
    state->error_status = 502;
    state->phase = VECTIS_KORE_WS_ERROR;
    return 0;
  }
  state->down_length = state->response_length - head_length;
  if (state->down_length != 0u)
    memcpy(state->to_downstream, state->response_head + head_length,
           state->down_length);
  net_send_queue(state->downstream, wire, wire_length);
  free(wire);
  state->request->status = 101;
  vectis_internal_metrics_note_http_status(state->app, 101);
  state->downstream->http_response_count++;
  state->phase = VECTIS_KORE_WS_RELAY;
  return 1;
}

static int vectis_kore_ws_rejection_step(vectis_kore_ws_state *state) {
  struct connection *connection;
  vectis_error error;
  size_t consumed;
  size_t written;
  size_t amount;
  char *wire;
  size_t wire_length;
  CURLcode code;

  connection = state->downstream;
  if (state->down_length != 0u) {
    net_send_queue(connection, state->to_downstream, state->down_length);
    state->down_length = 0u;
    return 1;
  }
  if (state->response_length != 0u) {
    if (vectis_proxy_ws_rejection_feed(
            &state->rejection, state->response_head, state->response_length,
            &consumed, (char *)state->to_downstream, state->buffer_capacity,
            &written, &error) != VECTIS_OK) {
      state->closing = 1;
      return 1;
    }
    state->response_length -= consumed;
    memmove(state->response_head, state->response_head + consumed,
            state->response_length);
    state->down_length = written;
    if (state->rejection.mode == VECTIS_PROXY_WS_REJECTION_COMPLETE)
      state->response_length = 0u;
    if (consumed != 0u || written != 0u)
      return 1;
  }
  if (state->rejection.mode == VECTIS_PROXY_WS_REJECTION_COMPLETE ||
      state->upstream_eof || state->upstream_error) {
    if (state->rejection.mode == VECTIS_PROXY_WS_REJECTION_COMPLETE ||
        (state->upstream_eof &&
         state->rejection.mode == VECTIS_PROXY_WS_REJECTION_CLOSE)) {
      wire = NULL;
      if (vectis_proxy_ws_rejection_finish(&state->rejection,
                                           state->upstream_eof, &wire,
                                           &wire_length, &error) == VECTIS_OK &&
          wire_length != 0u)
        net_send_queue(connection, wire, wire_length);
      free(wire);
    }
    state->rejection_finished = 1;
    state->closing = 1;
    return 1;
  }
  amount = 0u;
  code = curl_easy_recv(state->easy, state->response_head,
                        state->buffer_capacity < VECTIS_KORE_WS_HEAD_CAPACITY
                            ? state->buffer_capacity
                            : VECTIS_KORE_WS_HEAD_CAPACITY,
                        &amount);
  if (code == CURLE_OK && amount != 0u) {
    state->response_length = amount;
    state->upstream_again = 0;
    return 1;
  }
  if (code == CURLE_OK)
    state->upstream_eof = 1;
  else if (code != CURLE_AGAIN) {
    state->closing = 1;
    return 1;
  } else {
    state->upstream_again = 1;
  }
  return state->upstream_eof;
}

static int vectis_kore_ws_write_error(vectis_kore_ws_state *state) {
  vectis_proxy_local_response local;
  vectis_error cause;

  if (state->transfer != NULL) {
    if (state->upstream_interest != 0) {
      vectis_proxy_event_update((int)state->upstream_fd, &state->upstream_event,
                                state->upstream_interest, 0, 1);
      state->upstream_interest = 0;
    }
    vectis_proxy_curl_cancel(state->transfer);
    state->transfer = NULL;
    state->easy = NULL;
    state->upstream_fd = CURL_SOCKET_BAD;
  }
  vectis_set_error(&cause,
                   state->error_status == 504 ? VECTIS_ERR_TIMEOUT
                                              : VECTIS_ERR_STATE,
                   state->error_status == 504 ? "proxy upstream timed out"
                                              : "proxy upstream failed");
  vectis_proxy_local_error(state->route, &cause, state->error_status, &local);
  if (!vectis_kore_proxy_local_send(state->request, state->app, &local)) {
    vectis_proxy_local_cleanup(&local);
    state->closing = 1;
    return 1;
  }
  vectis_proxy_local_cleanup(&local);
  state->closing = 1;
  return 1;
}

static void vectis_kore_ws_pump(vectis_kore_ws_state *state) {
  struct connection *connection;
  size_t amount;
  size_t step;
  int made_progress;
  int progress;
  int upstream_ready;
  CURLcode code;

  if (!state->active)
    return;
  connection = state->downstream;
  if (connection->state == CONN_STATE_DISCONNECTING)
    return;
  upstream_ready = state->upstream_ready;
  state->upstream_ready = 0;
  made_progress = 0;
  for (step = 0u; step < VECTIS_KORE_WS_WORK_BUDGET; ++step) {
    progress = 0;
    if (!TAILQ_EMPTY(&connection->send_queue)) {
      connection->evt.flags |= KORE_EVENT_WRITE;
      if (net_send_flush(connection) != KORE_RESULT_OK ||
          connection->state == CONN_STATE_DISCONNECTING) {
        kore_connection_disconnect(connection);
        return;
      }
      if (!TAILQ_EMPTY(&connection->send_queue))
        break;
      progress = 1;
    }
    if (state->closing) {
      if (state->down_length != 0u && TAILQ_EMPTY(&connection->send_queue)) {
        net_send_queue(connection, state->to_downstream, state->down_length);
        state->down_length = 0u;
        progress = 1;
      } else if (state->down_length == 0u) {
        kore_connection_disconnect(connection);
        return;
      }
    } else if (state->phase == VECTIS_KORE_WS_ERROR) {
      progress = vectis_kore_ws_write_error(state);
    } else if (state->phase == VECTIS_KORE_WS_SEND_HEAD) {
      amount = 0u;
      code = curl_easy_send(
          state->easy, state->request_wire + state->request_offset,
          state->request_length - state->request_offset, &amount);
      if (code != CURLE_OK && code != CURLE_AGAIN) {
        state->error_status = 502;
        state->phase = VECTIS_KORE_WS_ERROR;
        progress = 1;
      } else if (code == CURLE_OK && amount != 0u) {
        state->request_offset += amount;
        state->upstream_again = 0;
        progress = 1;
        if (state->request_offset == state->request_length)
          state->phase = VECTIS_KORE_WS_READ_HEAD;
      } else {
        state->upstream_again = 1;
      }
    } else if (state->phase == VECTIS_KORE_WS_READ_HEAD) {
      if (state->response_length != 0u) {
        progress = vectis_kore_ws_response(state);
        if (progress || state->phase != VECTIS_KORE_WS_READ_HEAD)
          goto next_step;
      }
      amount = 0u;
      code = curl_easy_recv(
          state->easy, state->response_head + state->response_length,
          VECTIS_KORE_WS_HEAD_CAPACITY - state->response_length < 16384u
              ? VECTIS_KORE_WS_HEAD_CAPACITY - state->response_length
              : 16384u,
          &amount);
      if (code != CURLE_OK && code != CURLE_AGAIN) {
        state->error_status = 502;
        state->phase = VECTIS_KORE_WS_ERROR;
        progress = 1;
      } else if (code == CURLE_OK && amount != 0u) {
        state->response_length += amount;
        state->upstream_again = 0;
        progress = 1;
        if (vectis_kore_ws_response(state))
          progress = 1;
      } else if (code == CURLE_OK) {
        state->error_status = 502;
        state->phase = VECTIS_KORE_WS_ERROR;
        progress = 1;
      } else {
        state->upstream_again =
            state->upstream_again || (state->upstream_tls && !made_progress &&
                                      (upstream_ready & KORE_EVENT_READ) != 0);
      }
    } else if (state->phase == VECTIS_KORE_WS_REJECTION) {
      progress = vectis_kore_ws_rejection_step(state);
    } else if (state->phase == VECTIS_KORE_WS_RELAY) {
      if (state->up_offset < state->up_length) {
        amount = 0u;
        code =
            curl_easy_send(state->easy, state->to_upstream + state->up_offset,
                           state->up_length - state->up_offset, &amount);
        if (code != CURLE_OK && code != CURLE_AGAIN) {
          kore_connection_disconnect(connection);
          return;
        }
        if (code == CURLE_OK && amount != 0u) {
          state->up_offset += amount;
          state->upstream_again = 0;
          progress = 1;
          if (state->up_offset == state->up_length) {
            state->up_offset = 0u;
            state->up_length = 0u;
          }
        } else {
          state->upstream_again = 1;
        }
      }
      if (state->down_length != 0u && TAILQ_EMPTY(&connection->send_queue)) {
        net_send_queue(connection, state->to_downstream, state->down_length);
        state->down_length = 0u;
        progress = 1;
      }
      if (state->up_length == 0u && !state->closing &&
          vectis_kore_ws_read_down(state))
        progress = 1;
      if (state->down_length == 0u && TAILQ_EMPTY(&connection->send_queue) &&
          !state->closing) {
        amount = 0u;
        code = curl_easy_recv(state->easy, state->to_downstream,
                              state->buffer_capacity, &amount);
        if (code != CURLE_OK && code != CURLE_AGAIN) {
          kore_connection_disconnect(connection);
          return;
        }
        if (code == CURLE_OK && amount != 0u) {
          state->down_length = amount;
          state->upstream_again = 0;
          progress = 1;
        } else if (code == CURLE_OK) {
          state->closing = 1;
          progress = 1;
        } else if (state->upstream_error) {
          state->closing = 1;
          progress = 1;
        } else if (code == CURLE_AGAIN) {
          state->upstream_again =
              state->upstream_again ||
              (state->upstream_tls && !made_progress && !progress &&
               (upstream_ready & KORE_EVENT_READ) != 0);
        }
      }
    }
  next_step:
    if (progress)
      made_progress = 1;
    if (!progress)
      break;
  }
  if (made_progress) {
    vectis_kore_ws_progress(state);
    state->retry_delay_ms = 25u;
  }
  if (state->closing && TAILQ_EMPTY(&connection->send_queue) &&
      state->down_length == 0u) {
    kore_connection_disconnect(connection);
    return;
  }
  if (step == VECTIS_KORE_WS_WORK_BUDGET)
    vectis_kore_ws_request_wake(state, 1u);
  else if (state->upstream_again && state->phase != VECTIS_KORE_WS_CONNECTING) {
    if (state->retry_delay_ms == 0u)
      state->retry_delay_ms = 25u;
    vectis_kore_ws_request_wake(state, (u_int64_t)state->retry_delay_ms);
    if (state->retry_delay_ms < 1000u)
      state->retry_delay_ms *= 2u;
  }
  vectis_kore_ws_schedule(state);
}

static void vectis_kore_ws_upstream_event(void *userdata, int error) {
  vectis_kore_ws_state *state;

  state =
      (vectis_kore_ws_state *)((char *)userdata -
                               offsetof(vectis_kore_ws_state, upstream_event));
  if (!state->active)
    return;
  state->upstream_ready = state->upstream_event.flags;
  state->upstream_event.flags = 0;
  if (error) {
    if (state->phase == VECTIS_KORE_WS_READ_HEAD) {
      vectis_kore_ws_pump(state);
      return;
    }
    if (state->phase == VECTIS_KORE_WS_REJECTION) {
      /* A readable hangup can still contain the last body bytes. Let
       * curl_easy_recv distinguish clean EOF from a transport failure. */
      vectis_kore_ws_pump(state);
      return;
    }
    if (state->phase != VECTIS_KORE_WS_RELAY) {
      state->error_status = 502;
      state->phase = VECTIS_KORE_WS_ERROR;
      vectis_kore_ws_pump(state);
      return;
    }
    state->upstream_error = 1;
    vectis_kore_ws_pump(state);
    return;
  }
  vectis_kore_ws_pump(state);
}

static void vectis_kore_ws_downstream_event(void *userdata, int error) {
  struct connection *connection;
  vectis_kore_ws_state *state;

  connection = (struct connection *)userdata;
  if (connection->state == CONN_STATE_DISCONNECTING)
    return;
  state = (vectis_kore_ws_state *)connection->hdlr_extra;
  if (state == NULL)
    return;
  connection->evt.flags = 0;
  if (error) {
    kore_connection_disconnect(connection);
    return;
  }
  vectis_kore_ws_pump(state);
}

static int vectis_kore_ws_reject(struct http_request *request, int status) {
  static const char message[] = "proxy WebSocket setup failed\n";

  request->owner->flags |= CONN_CLOSE_EMPTY;
  http_response(request, status, message, sizeof(message) - 1u);
  return KORE_RESULT_ERROR;
}

int vectis_kore_proxy_ws_start(struct http_request *request,
                               const void *surplus, size_t surplus_length,
                               vectis_app *app, vectis_request *route_request,
                               vectis_proxy_route_data *route,
                               vectis_proxy_headers *inbound,
                               const vectis_proxy_headers *outbound,
                               const char *url, const char *request_target,
                               const char *authority) {
  vectis_kore_ws_state *state;
  vectis_error error;
  vectis_status status;
  CURL *easy;

  state = (vectis_kore_ws_state *)calloc(1u, sizeof(*state));
  if (state == NULL)
    return vectis_kore_ws_reject(request, 500);
  state->upstream_fd = CURL_SOCKET_BAD;
  vectis_proxy_ws_rejection_init(&state->rejection);
  state->downstream = request->owner;
  state->request = request;
  state->app = app;
  state->route = route;
  state->upstream_tls = strncmp(url, "https://", 8u) == 0;
  state->buffer_capacity = route->buffer_limit_bytes;
  state->initial = (const unsigned char *)surplus;
  state->initial_length = surplus_length;
  state->upstream_event.type = KORE_TYPE_CONNECTION;
  state->upstream_event.handle = vectis_kore_ws_upstream_event;
  state->response_head = (unsigned char *)malloc(VECTIS_KORE_WS_HEAD_CAPACITY);
  state->to_upstream = (unsigned char *)malloc(state->buffer_capacity);
  state->to_downstream = (unsigned char *)malloc(state->buffer_capacity);
  if (state->response_head == NULL || state->to_upstream == NULL ||
      state->to_downstream == NULL) {
    vectis_kore_ws_free(state);
    return vectis_kore_ws_reject(request, 500);
  }
  status = vectis_proxy_ws_wire_request(request_target, authority, outbound,
                                        &state->request_wire,
                                        &state->request_length, &error);
  if (status != VECTIS_OK) {
    vectis_kore_ws_free(state);
    return vectis_kore_ws_reject(request,
                                 status == VECTIS_ERR_INVALID ? 400 : 500);
  }
  easy = curl_easy_init();
  if (easy == NULL) {
    vectis_kore_ws_free(state);
    return vectis_kore_ws_reject(request, 500);
  }
  if (curl_easy_setopt(easy, CURLOPT_URL, url) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                       route->connect_timeout_ms) != CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2) !=
          CURLE_OK ||
      curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
      vectis_proxy_curl_set_ca(easy, route->tls_ca_pem,
                               route->tls_ca_pem_length, &error) != VECTIS_OK ||
      vectis_proxy_curl_set_client_identity(
          easy, route->tls_client_cert_pem, route->tls_client_cert_pem_length,
          route->tls_client_key_pem, route->tls_client_key_pem_length,
          &error) != VECTIS_OK) {
    curl_easy_cleanup(easy);
    vectis_kore_ws_free(state);
    return vectis_kore_ws_reject(request, 500);
  }
  state->easy = easy;
  status = vectis_proxy_curl_submit_connect(easy, vectis_kore_ws_connected,
                                            state, &state->transfer, &error);
  if (status != VECTIS_OK) {
    curl_easy_cleanup(easy);
    state->easy = NULL;
    vectis_kore_ws_free(state);
    return vectis_kore_ws_reject(request,
                                 status == VECTIS_ERR_STATE ? 503 : 502);
  }
  state->inbound = *inbound;
  memset(inbound, 0, sizeof(*inbound));
  state->route_request = route_request;
  state->next = vectis_kore_ws_states;
  vectis_kore_ws_states = state;
  state->active = 1;
  request->owner->hdlr_extra = state;
  request->owner->disconnect = vectis_kore_ws_disconnect;
  vectis_proxy_event_takeover(request->owner->fd);
  request->owner->evt.handle = vectis_kore_ws_downstream_event;
  request->owner->evt.flags &= ~KORE_EVENT_READ;
  request->owner->flags |= CONN_IS_BUSY;
  request->owner->http_timeout = 0u;
  http_request_sleep(request);
  vectis_kore_ws_progress(state);
  if (route->total_timeout_ms > 0L)
    state->total_timer =
        kore_timer_add(vectis_kore_ws_total, (u_int64_t)route->total_timeout_ms,
                       state, KORE_TIMER_ONESHOT);
  vectis_kore_ws_schedule(state);
  return KORE_RESULT_RETRY;
}

void vectis_kore_proxy_ws_worker_cleanup(void) {
  vectis_kore_ws_state *state;

  for (state = vectis_kore_ws_states; state != NULL; state = state->next) {
    if (state->wake_timer != NULL) {
      kore_timer_remove(state->wake_timer);
      state->wake_timer = NULL;
    }
    if (state->idle_timer != NULL) {
      kore_timer_remove(state->idle_timer);
      state->idle_timer = NULL;
    }
    if (state->total_timer != NULL) {
      kore_timer_remove(state->total_timer);
      state->total_timer = NULL;
    }
    if (state->upstream_interest != 0) {
      vectis_proxy_event_update((int)state->upstream_fd, &state->upstream_event,
                                state->upstream_interest, 0, 1);
      state->upstream_interest = 0;
    }
    if (state->transfer != NULL) {
      vectis_proxy_curl_cancel(state->transfer);
      state->transfer = NULL;
    }
    state->active = 0;
  }
}
