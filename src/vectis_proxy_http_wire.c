#include "vectis_proxy_http_wire.h"

#include "vectis_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define VECTIS_PROXY_WIRE_MAX_HEAD_BYTES 131072u
#define VECTIS_PROXY_WIRE_MAX_TRAILER_BYTES 65536u

static int vectis_proxy_http_wire_append(char *buffer, size_t capacity,
                                         size_t *used, const char *data,
                                         size_t length) {
  if (length > capacity - *used)
    return 0;
  memcpy(buffer + *used, data, length);
  *used += length;
  return 1;
}

vectis_status vectis_proxy_http_wire_plan_build(
    const vectis_proxy_http_response *response,
    const vectis_proxy_headers *outbound_headers, int auto_version,
    vectis_proxy_http_wire_plan *plan, vectis_error *error) {
  char status_line[48];
  char length_line[64];
  size_t capacity;
  size_t used;
  size_t i;
  int count;
  int chunked;
  int body_allowed;
  int declared_trailers;

  if (response == NULL || outbound_headers == NULL || plan == NULL ||
      response->status < 200 || response->status > 599 ||
      (auto_version != 0 && auto_version != 1)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy response writer requires final metadata");
    return VECTIS_ERR_INVALID;
  }
  memset(plan, 0, sizeof(*plan));
  body_allowed = response->body_allowed;
  declared_trailers = 0;
  for (i = 0u; i < response->headers.count; ++i) {
    if (strcasecmp(response->headers.fields[i].name, "trailer") == 0)
      declared_trailers = 1;
  }
  chunked =
      body_allowed && (auto_version || response->chunked ||
                       !response->has_content_length || declared_trailers);
  capacity = 128u;
  for (i = 0u; i < outbound_headers->count; ++i) {
    size_t name_length = strlen(outbound_headers->fields[i].name);
    size_t value_length = strlen(outbound_headers->fields[i].value);
    if (capacity > VECTIS_PROXY_WIRE_MAX_HEAD_BYTES - 4u ||
        name_length > VECTIS_PROXY_WIRE_MAX_HEAD_BYTES - capacity - 4u ||
        value_length >
            VECTIS_PROXY_WIRE_MAX_HEAD_BYTES - capacity - 4u - name_length) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "proxy response headers exceed wire limit");
      return VECTIS_ERR_INVALID;
    }
    capacity += name_length + value_length + 4u;
  }
  if (chunked) {
    for (i = 0u; i < response->headers.count; ++i) {
      size_t value_length;
      if (strcasecmp(response->headers.fields[i].name, "trailer") != 0)
        continue;
      value_length = strlen(response->headers.fields[i].value);
      if (capacity > VECTIS_PROXY_WIRE_MAX_HEAD_BYTES - 11u ||
          value_length > VECTIS_PROXY_WIRE_MAX_HEAD_BYTES - capacity - 11u) {
        vectis_set_error(error, VECTIS_ERR_INVALID,
                         "proxy Trailer declaration exceeds wire limit");
        return VECTIS_ERR_INVALID;
      }
      capacity += value_length + 11u;
    }
  }
  plan->head = (char *)malloc(capacity);
  if (plan->head == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate proxy response headers");
    return VECTIS_ERR_NOMEM;
  }
  used = 0u;
  count = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %d \r\n",
                   response->status);
  if (count < 0 || (size_t)count >= sizeof(status_line) ||
      !vectis_proxy_http_wire_append(plan->head, capacity, &used, status_line,
                                     (size_t)count))
    goto invalid;
  for (i = 0u; i < outbound_headers->count; ++i) {
    const char *name = outbound_headers->fields[i].name;
    const char *value = outbound_headers->fields[i].value;
    if (!vectis_proxy_http_wire_append(plan->head, capacity, &used, name,
                                       strlen(name)) ||
        !vectis_proxy_http_wire_append(plan->head, capacity, &used, ": ", 2u) ||
        !vectis_proxy_http_wire_append(plan->head, capacity, &used, value,
                                       strlen(value)) ||
        !vectis_proxy_http_wire_append(plan->head, capacity, &used, "\r\n", 2u))
      goto invalid;
  }
  if (chunked) {
    for (i = 0u; i < response->headers.count; ++i) {
      const char *value;
      if (strcasecmp(response->headers.fields[i].name, "trailer") != 0)
        continue;
      value = response->headers.fields[i].value;
      if (!vectis_proxy_http_wire_append(plan->head, capacity, &used,
                                         "Trailer: ", 9u) ||
          !vectis_proxy_http_wire_append(plan->head, capacity, &used, value,
                                         strlen(value)) ||
          !vectis_proxy_http_wire_append(plan->head, capacity, &used, "\r\n",
                                         2u))
        goto invalid;
    }
  }
  if (chunked) {
    if (!vectis_proxy_http_wire_append(plan->head, capacity, &used,
                                       "Transfer-Encoding: chunked\r\n", 28u))
      goto invalid;
  } else if (response->has_content_length &&
             (body_allowed || response->head_request ||
              response->status == 304)) {
    count =
        snprintf(length_line, sizeof(length_line), "Content-Length: %llu\r\n",
                 (unsigned long long)response->content_length);
    if (count < 0 || (size_t)count >= sizeof(length_line) ||
        !vectis_proxy_http_wire_append(plan->head, capacity, &used, length_line,
                                       (size_t)count))
      goto invalid;
  }
  if (!vectis_proxy_http_wire_append(plan->head, capacity, &used,
                                     "Connection: close\r\n\r\n", 21u))
    goto invalid;
  if (used == capacity)
    goto invalid;
  plan->head[used] = '\0';
  plan->head_length = used;
  plan->chunked = chunked;
  plan->body_allowed = body_allowed;
  vectis_error_clear(error);
  return VECTIS_OK;

invalid:
  vectis_proxy_http_wire_plan_cleanup(plan);
  vectis_set_error(error, VECTIS_ERR_INVALID,
                   "failed to format bounded proxy response headers");
  return VECTIS_ERR_INVALID;
}

void vectis_proxy_http_wire_plan_cleanup(vectis_proxy_http_wire_plan *plan) {
  if (plan == NULL)
    return;
  free(plan->head);
  memset(plan, 0, sizeof(*plan));
}

vectis_status
vectis_proxy_http_wire_chunk(const vectis_proxy_http_wire_plan *plan,
                             const unsigned char *body, size_t body_length,
                             char *buffer, size_t capacity, size_t *written,
                             vectis_error *error) {
  char prefix[32];
  size_t used;
  int count;

  if (written != NULL)
    *written = 0u;
  if (plan == NULL || !plan->body_allowed || body == NULL ||
      body_length == 0u || buffer == NULL || written == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "invalid proxy response body chunk");
    return VECTIS_ERR_INVALID;
  }
  used = 0u;
  if (plan->chunked) {
    count =
        snprintf(prefix, sizeof(prefix), "%lx\r\n", (unsigned long)body_length);
    if (count < 0 || (size_t)count >= sizeof(prefix) ||
        !vectis_proxy_http_wire_append(buffer, capacity, &used, prefix,
                                       (size_t)count))
      goto invalid_chunk;
  }
  if (!vectis_proxy_http_wire_append(buffer, capacity, &used,
                                     (const char *)body, body_length))
    goto invalid_chunk;
  if (plan->chunked &&
      !vectis_proxy_http_wire_append(buffer, capacity, &used, "\r\n", 2u))
    goto invalid_chunk;
  *written = used;
  vectis_error_clear(error);
  return VECTIS_OK;

invalid_chunk:
  vectis_set_error(error, VECTIS_ERR_INVALID,
                   "proxy response chunk exceeds bounded output buffer");
  return VECTIS_ERR_INVALID;
}

vectis_status vectis_proxy_http_wire_chunk_in_place(
    const vectis_proxy_http_wire_plan *plan, unsigned char *body,
    size_t body_length, size_t headroom, size_t tailroom,
    const unsigned char **wire, size_t *written, vectis_error *error) {
  char prefix[32];
  size_t prefix_length;
  int count;

  if (wire != NULL)
    *wire = NULL;
  if (written != NULL)
    *written = 0u;
  if (plan == NULL || !plan->body_allowed || body == NULL ||
      body_length == 0u || wire == NULL || written == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "invalid in-place proxy response chunk");
    return VECTIS_ERR_INVALID;
  }
  if (!plan->chunked) {
    *wire = body;
    *written = body_length;
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  count =
      snprintf(prefix, sizeof(prefix), "%lx\r\n", (unsigned long)body_length);
  if (count < 0 || (size_t)count >= sizeof(prefix)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy response chunk length is invalid");
    return VECTIS_ERR_INVALID;
  }
  prefix_length = (size_t)count;
  if (headroom < prefix_length || tailroom < 2u ||
      body_length > (size_t)-1 - prefix_length - 2u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy response chunk exceeds framing space");
    return VECTIS_ERR_INVALID;
  }
  memcpy(body - prefix_length, prefix, prefix_length);
  memcpy(body + body_length, "\r\n", 2u);
  *wire = body - prefix_length;
  *written = body_length + prefix_length + 2u;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status
vectis_proxy_http_wire_finish(const vectis_proxy_http_wire_plan *plan,
                              const vectis_proxy_headers *trailers, char **out,
                              size_t *out_length, vectis_error *error) {
  size_t capacity;
  size_t used;
  size_t i;
  char *buffer;

  if (out != NULL)
    *out = NULL;
  if (out_length != NULL)
    *out_length = 0u;
  if (plan == NULL || trailers == NULL || out == NULL || out_length == NULL ||
      (!plan->chunked && trailers->count != 0u)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy trailers require chunked downstream framing");
    return VECTIS_ERR_INVALID;
  }
  if (!plan->chunked) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  capacity = 7u;
  for (i = 0u; i < trailers->count; ++i) {
    size_t name_length = strlen(trailers->fields[i].name);
    size_t value_length = strlen(trailers->fields[i].value);
    if (capacity > VECTIS_PROXY_WIRE_MAX_TRAILER_BYTES - 4u ||
        name_length > VECTIS_PROXY_WIRE_MAX_TRAILER_BYTES - capacity - 4u ||
        value_length >
            VECTIS_PROXY_WIRE_MAX_TRAILER_BYTES - capacity - 4u - name_length) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "proxy response trailers exceed wire limit");
      return VECTIS_ERR_INVALID;
    }
    capacity += name_length + value_length + 4u;
  }
  buffer = (char *)malloc(capacity);
  if (buffer == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate proxy response trailers");
    return VECTIS_ERR_NOMEM;
  }
  used = 0u;
  if (!vectis_proxy_http_wire_append(buffer, capacity, &used, "0\r\n", 3u))
    goto invalid_final;
  for (i = 0u; i < trailers->count; ++i) {
    const char *name = trailers->fields[i].name;
    const char *value = trailers->fields[i].value;
    if (!vectis_proxy_http_wire_append(buffer, capacity, &used, name,
                                       strlen(name)) ||
        !vectis_proxy_http_wire_append(buffer, capacity, &used, ": ", 2u) ||
        !vectis_proxy_http_wire_append(buffer, capacity, &used, value,
                                       strlen(value)) ||
        !vectis_proxy_http_wire_append(buffer, capacity, &used, "\r\n", 2u))
      goto invalid_final;
  }
  if (!vectis_proxy_http_wire_append(buffer, capacity, &used, "\r\n", 2u))
    goto invalid_final;
  *out = buffer;
  *out_length = used;
  vectis_error_clear(error);
  return VECTIS_OK;

invalid_final:
  free(buffer);
  vectis_set_error(error, VECTIS_ERR_INVALID,
                   "failed to format bounded proxy response trailers");
  return VECTIS_ERR_INVALID;
}
