#include "vectis_proxy_ws_rejection.h"

#include "vectis_internal.h"
#include "vectis_proxy_response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct vectis_proxy_ws_rejection_sink {
  vectis_proxy_ws_rejection *rejection;
  char *output;
  size_t capacity;
  size_t written;
  int failed;
} vectis_proxy_ws_rejection_sink;

void vectis_proxy_ws_rejection_init(vectis_proxy_ws_rejection *rejection) {
  if (rejection == NULL)
    return;
  memset(rejection, 0, sizeof(*rejection));
  vectis_proxy_http_response_init(&rejection->response, 0);
}

void vectis_proxy_ws_rejection_cleanup(vectis_proxy_ws_rejection *rejection) {
  if (rejection == NULL)
    return;
  vectis_proxy_http_response_cleanup(&rejection->response);
  vectis_proxy_http_wire_plan_cleanup(&rejection->wire);
  memset(rejection, 0, sizeof(*rejection));
}

static vectis_status vectis_proxy_ws_rejection_interim(
    unsigned status, const vectis_proxy_headers *source, char **wire,
    size_t *wire_length, vectis_error *error) {
  vectis_proxy_headers headers;
  char status_line[32];
  char *out;
  size_t capacity;
  size_t used;
  size_t i;
  int count;

  vectis_proxy_headers_init(&headers);
  if (vectis_proxy_headers_sanitize_response(source, &headers) !=
      VECTIS_PROXY_HEADER_OK) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "invalid informational response headers");
    return VECTIS_ERR_INVALID;
  }
  count =
      snprintf(status_line, sizeof(status_line), "HTTP/1.1 %u \r\n", status);
  if (count < 0 || (size_t)count >= sizeof(status_line)) {
    vectis_proxy_headers_cleanup(&headers);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "invalid informational response status");
    return VECTIS_ERR_INVALID;
  }
  capacity = (size_t)count + 2u;
  for (i = 0u; i < headers.count; ++i)
    capacity +=
        strlen(headers.fields[i].name) + strlen(headers.fields[i].value) + 4u;
  out = (char *)malloc(capacity + 1u);
  if (out == NULL) {
    vectis_proxy_headers_cleanup(&headers);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate informational response headers");
    return VECTIS_ERR_NOMEM;
  }
  used = (size_t)count;
  memcpy(out, status_line, used);
  for (i = 0u; i < headers.count; ++i) {
    size_t length;

    length = strlen(headers.fields[i].name);
    memcpy(out + used, headers.fields[i].name, length);
    used += length;
    memcpy(out + used, ": ", 2u);
    used += 2u;
    length = strlen(headers.fields[i].value);
    memcpy(out + used, headers.fields[i].value, length);
    used += length;
    memcpy(out + used, "\r\n", 2u);
    used += 2u;
  }
  memcpy(out + used, "\r\n", 2u);
  used += 2u;
  out[used] = '\0';
  *wire = out;
  *wire_length = used;
  vectis_proxy_headers_cleanup(&headers);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status
vectis_proxy_ws_rejection_head(vectis_proxy_ws_rejection *rejection,
                               const unsigned char *head, size_t head_length,
                               int *final, char **wire, size_t *wire_length,
                               vectis_proxy_modify_response_fn modify,
                               void *modify_userdata, vectis_error *error) {
  vectis_proxy_headers sanitized;
  vectis_proxy_http_response downstream;
  vectis_proxy_http_event event;
  vectis_proxy_header_status parsed;
  const char *reason;
  size_t pos;
  size_t end;
  vectis_status status;

  if (final != NULL)
    *final = 0;
  if (wire != NULL)
    *wire = NULL;
  if (wire_length != NULL)
    *wire_length = 0u;
  if (rejection == NULL || head == NULL || final == NULL || wire == NULL ||
      wire_length == NULL ||
      rejection->mode != VECTIS_PROXY_WS_REJECTION_HEAD || head_length < 12u ||
      head_length > VECTIS_PROXY_HEADER_BLOCK_LIMIT ||
      head[head_length - 2u] != '\r' || head[head_length - 1u] != '\n') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "invalid upstream rejection response head");
    return VECTIS_ERR_INVALID;
  }
  pos = 0u;
  while (pos < head_length) {
    for (end = pos; end + 1u < head_length; ++end) {
      if (head[end] == '\r' && head[end + 1u] == '\n')
        break;
    }
    if (end + 1u >= head_length) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "unterminated upstream rejection header");
      return VECTIS_ERR_INVALID;
    }
    parsed = vectis_proxy_http_response_header(&rejection->response,
                                               (const char *)head + pos,
                                               end + 2u - pos, &event, &reason);
    if (parsed != VECTIS_PROXY_HEADER_OK) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       reason != NULL ? reason : "invalid upstream response");
      return VECTIS_ERR_INVALID;
    }
    pos = end + 2u;
  }
  if (event == VECTIS_PROXY_HTTP_INFORMATIONAL) {
    vectis_proxy_headers interim;
    size_t field_start;
    size_t field_end;
    char *copy;
    char *colon;

    vectis_proxy_headers_init(&interim);
    field_start = 0u;
    while (field_start + 1u < head_length &&
           !(head[field_start] == '\r' && head[field_start + 1u] == '\n'))
      ++field_start;
    field_start += 2u;
    while (field_start + 1u < head_length &&
           !(head[field_start] == '\r' && head[field_start + 1u] == '\n')) {
      for (field_end = field_start; field_end + 1u < head_length; ++field_end) {
        if (head[field_end] == '\r' && head[field_end + 1u] == '\n')
          break;
      }
      copy = (char *)malloc(field_end - field_start + 1u);
      if (copy == NULL) {
        vectis_proxy_headers_cleanup(&interim);
        vectis_set_error(error, VECTIS_ERR_NOMEM,
                         "failed to copy informational response field");
        return VECTIS_ERR_NOMEM;
      }
      memcpy(copy, head + field_start, field_end - field_start);
      copy[field_end - field_start] = '\0';
      colon = strchr(copy, ':');
      if (colon == NULL) {
        free(copy);
        vectis_proxy_headers_cleanup(&interim);
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "invalid informational response field");
        return VECTIS_ERR_INVALID;
      }
      *colon++ = '\0';
      while (*colon == ' ' || *colon == '\t')
        ++colon;
      parsed = vectis_proxy_headers_add(&interim, copy, colon);
      free(copy);
      if (parsed != VECTIS_PROXY_HEADER_OK) {
        vectis_proxy_headers_cleanup(&interim);
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "invalid informational response field");
        return VECTIS_ERR_INVALID;
      }
      field_start = field_end + 2u;
    }
    status =
        vectis_proxy_ws_rejection_interim((unsigned)rejection->response.status,
                                          &interim, wire, wire_length, error);
    vectis_proxy_headers_cleanup(&interim);
    return status;
  }
  if (event != VECTIS_PROXY_HTTP_FINAL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upstream rejection response head did not finish");
    return VECTIS_ERR_INVALID;
  }
  vectis_proxy_headers_init(&sanitized);
  parsed = vectis_proxy_headers_sanitize_response(&rejection->response.headers,
                                                  &sanitized);
  if (parsed != VECTIS_PROXY_HEADER_OK) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "invalid upstream rejection hop-by-hop fields");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_proxy_response_apply(&rejection->response, &sanitized, modify,
                                       modify_userdata,
                                       &rejection->downstream_status, error);
  if (status == VECTIS_OK) {
    downstream = rejection->response;
    downstream.status = rejection->downstream_status;
    status = vectis_proxy_http_wire_plan_build(&downstream, &sanitized, 0,
                                               &rejection->wire, error);
  }
  vectis_proxy_headers_cleanup(&sanitized);
  if (status != VECTIS_OK)
    return status;
  *wire = rejection->wire.head;
  *wire_length = rejection->wire.head_length;
  rejection->wire.head = NULL;
  if (!rejection->response.body_allowed) {
    rejection->mode = VECTIS_PROXY_WS_REJECTION_COMPLETE;
  } else if (rejection->response.chunked) {
    rejection->mode = VECTIS_PROXY_WS_REJECTION_CHUNKED;
    vectis_proxy_body_framer_chunked(&rejection->framer);
  } else if (rejection->response.has_content_length) {
    rejection->mode = rejection->response.content_length == 0u
                          ? VECTIS_PROXY_WS_REJECTION_COMPLETE
                          : VECTIS_PROXY_WS_REJECTION_FIXED;
    vectis_proxy_body_framer_fixed(&rejection->framer,
                                   rejection->response.content_length);
  } else {
    rejection->mode = VECTIS_PROXY_WS_REJECTION_CLOSE;
  }
  *final = 1;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static size_t vectis_proxy_ws_rejection_body(void *userdata,
                                             const unsigned char *data,
                                             size_t length) {
  vectis_proxy_ws_rejection_sink *sink;
  vectis_error error;
  const char *reason;
  size_t accepted;

  sink = (vectis_proxy_ws_rejection_sink *)userdata;
  if (sink->written != 0u)
    return 0u;
  accepted = length;
  if (accepted > sink->capacity - 32u)
    accepted = sink->capacity - 32u;
  if (accepted == 0u)
    return 0u;
  if (vectis_proxy_http_response_body(&sink->rejection->response, accepted,
                                      &reason) != VECTIS_PROXY_HEADER_OK ||
      vectis_proxy_http_wire_chunk(&sink->rejection->wire, data, accepted,
                                   sink->output, sink->capacity, &sink->written,
                                   &error) != VECTIS_OK) {
    sink->failed = 1;
    return 0u;
  }
  return accepted;
}

static int vectis_proxy_ws_rejection_trailer(void *userdata, const char *name,
                                             const char *value) {
  vectis_proxy_ws_rejection_sink *sink;

  sink = (vectis_proxy_ws_rejection_sink *)userdata;
  return vectis_proxy_headers_add(&sink->rejection->response.trailers, name,
                                  value) == VECTIS_PROXY_HEADER_OK;
}

vectis_status vectis_proxy_ws_rejection_feed(
    vectis_proxy_ws_rejection *rejection, const unsigned char *input,
    size_t input_length, size_t *consumed, char *output, size_t output_capacity,
    size_t *written, vectis_error *error) {
  vectis_proxy_ws_rejection_sink sink;
  vectis_proxy_frame_result result;

  if (consumed != NULL)
    *consumed = 0u;
  if (written != NULL)
    *written = 0u;
  if (rejection == NULL || (input == NULL && input_length != 0u) ||
      consumed == NULL || output == NULL || output_capacity < 64u ||
      written == NULL || rejection->mode == VECTIS_PROXY_WS_REJECTION_HEAD) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "invalid upstream rejection body input");
    return VECTIS_ERR_INVALID;
  }
  if (rejection->mode == VECTIS_PROXY_WS_REJECTION_COMPLETE) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  memset(&sink, 0, sizeof(sink));
  sink.rejection = rejection;
  sink.output = output;
  sink.capacity = output_capacity;
  if (rejection->mode == VECTIS_PROXY_WS_REJECTION_CLOSE) {
    size_t amount;

    amount = input_length;
    if (amount > output_capacity - 32u)
      amount = output_capacity - 32u;
    if (amount != 0u &&
        vectis_proxy_ws_rejection_body(&sink, input, amount) != amount)
      sink.failed = 1;
    *consumed = amount;
  } else {
    result =
        vectis_proxy_body_framer_feed(&rejection->framer, input, input_length,
                                      consumed, vectis_proxy_ws_rejection_body,
                                      vectis_proxy_ws_rejection_trailer, &sink);
    if (result == VECTIS_PROXY_FRAME_INVALID)
      sink.failed = 1;
    else if (result == VECTIS_PROXY_FRAME_COMPLETE)
      rejection->mode = VECTIS_PROXY_WS_REJECTION_COMPLETE;
  }
  if (sink.failed) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "invalid upstream rejection body framing");
    return VECTIS_ERR_INVALID;
  }
  *written = sink.written;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status
vectis_proxy_ws_rejection_finish(vectis_proxy_ws_rejection *rejection, int eof,
                                 char **wire, size_t *wire_length,
                                 vectis_error *error) {
  const char *reason;

  if (wire != NULL)
    *wire = NULL;
  if (wire_length != NULL)
    *wire_length = 0u;
  if (rejection == NULL || wire == NULL || wire_length == NULL ||
      (rejection->mode != VECTIS_PROXY_WS_REJECTION_COMPLETE &&
       !(eof && rejection->mode == VECTIS_PROXY_WS_REJECTION_CLOSE)) ||
      vectis_proxy_http_response_finish(&rejection->response, &reason) !=
          VECTIS_PROXY_HEADER_OK) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "upstream rejection ended before body framing completed");
    return VECTIS_ERR_INVALID;
  }
  rejection->mode = VECTIS_PROXY_WS_REJECTION_COMPLETE;
  return vectis_proxy_http_wire_finish(&rejection->wire,
                                       &rejection->response.trailers, wire,
                                       wire_length, error);
}
