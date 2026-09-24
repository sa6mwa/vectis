#include "vectis_proxy_headers.h"

#include "vectis_proxy_framing.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int vectis_proxy_ascii_equal(const char *a, size_t length,
                                    const char *b) {
  size_t i;
  unsigned char left;
  unsigned char right;

  if (strlen(b) != length) {
    return 0;
  }
  for (i = 0u; i < length; ++i) {
    left = (unsigned char)a[i];
    right = (unsigned char)b[i];
    if (left >= 'A' && left <= 'Z') {
      left = (unsigned char)(left + ('a' - 'A'));
    }
    if (right >= 'A' && right <= 'Z') {
      right = (unsigned char)(right + ('a' - 'A'));
    }
    if (left != right) {
      return 0;
    }
  }
  return 1;
}

static int vectis_proxy_ascii_prefix(const char *value, const char *prefix) {
  size_t length;

  length = strlen(prefix);
  return strlen(value) >= length &&
         vectis_proxy_ascii_equal(value, length, prefix);
}

static int vectis_proxy_token_byte(unsigned char value) {
  if (value == 0u) {
    return 0;
  }
  if ((value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
      (value >= 'a' && value <= 'z')) {
    return 1;
  }
  return strchr("!#$%&'*+-.^_`|~", value) != NULL;
}

static void vectis_proxy_trim_ows(const char **value, size_t *length) {
  while (*length != 0u && (**value == ' ' || **value == '\t')) {
    (*value)++;
    (*length)--;
  }
  while (*length != 0u &&
         ((*value)[*length - 1u] == ' ' || (*value)[*length - 1u] == '\t')) {
    (*length)--;
  }
}

/* Iterate a comma-separated token field. A NULL wanted token validates only.
 * Every token is checked even after a match is found. */
static int vectis_proxy_token_list(const char *value, const char *wanted,
                                   int trailers, size_t *count, int *found) {
  const char *cursor;
  const char *start;
  size_t length;
  size_t i;

  cursor = value;
  if (*cursor == '\0') {
    return 0;
  }
  for (;;) {
    start = cursor;
    while (*cursor != '\0' && *cursor != ',') {
      cursor++;
    }
    length = (size_t)(cursor - start);
    vectis_proxy_trim_ows(&start, &length);
    if (length == 0u || ++(*count) > VECTIS_PROXY_HEADER_COUNT_LIMIT) {
      return 0;
    }
    for (i = 0u; i < length; ++i) {
      if (!vectis_proxy_token_byte((unsigned char)start[i])) {
        return 0;
      }
    }
    if (trailers && !vectis_proxy_trailer_field_allowed(start, length)) {
      return 0;
    }
    if (wanted != NULL && vectis_proxy_ascii_equal(start, length, wanted)) {
      *found = 1;
    }
    if (*cursor == '\0') {
      return 1;
    }
    cursor++;
    if (*cursor == '\0') {
      return 0;
    }
  }
}

static int vectis_proxy_has_list_token(const vectis_proxy_headers *headers,
                                       const char *field, const char *wanted,
                                       int trailers, int *found) {
  size_t i;
  size_t count;

  *found = 0;
  count = 0u;
  for (i = 0u; i < headers->count; ++i) {
    if (vectis_proxy_ascii_equal(headers->fields[i].name,
                                 strlen(headers->fields[i].name), field) &&
        !vectis_proxy_token_list(headers->fields[i].value, wanted, trailers,
                                 &count, found)) {
      return 0;
    }
  }
  return 1;
}

static int vectis_proxy_value_equals(const char *value, const char *expected) {
  size_t length;

  length = strlen(value);
  vectis_proxy_trim_ows(&value, &length);
  return vectis_proxy_ascii_equal(value, length, expected);
}

static int vectis_proxy_parse_length(const char *value, uint64_t *parsed) {
  size_t length;
  size_t i;
  uint64_t n;
  unsigned digit;

  length = strlen(value);
  vectis_proxy_trim_ows(&value, &length);
  if (length == 0u) {
    return 0;
  }
  n = 0u;
  for (i = 0u; i < length; ++i) {
    if (value[i] < '0' || value[i] > '9') {
      return 0;
    }
    digit = (unsigned)(value[i] - '0');
    if (n > (UINT64_MAX - digit) / 10u) {
      return 0;
    }
    n = n * 10u + digit;
  }
  *parsed = n;
  return 1;
}

static int vectis_proxy_valid_host(const char *value) {
  const char *cursor;
  const char *host_end;
  const char *port;
  size_t label_length;
  size_t host_length;
  unsigned number;
  unsigned digit;
  char address[INET6_ADDRSTRLEN];
  unsigned char ipv6[16];

  if (value == NULL || *value == '\0') {
    return 0;
  }
  port = NULL;
  if (*value == '[') {
    host_end = strchr(value, ']');
    if (host_end == NULL || host_end == value + 1 ||
        (size_t)(host_end - value - 1) >= sizeof(address)) {
      return 0;
    }
    memcpy(address, value + 1, (size_t)(host_end - value - 1));
    address[host_end - value - 1] = '\0';
    if (inet_pton(AF_INET6, address, ipv6) != 1) {
      return 0;
    }
    if (host_end[1] == ':') {
      port = host_end + 2;
    } else if (host_end[1] != '\0') {
      return 0;
    }
  } else {
    host_end = strchr(value, ':');
    if (host_end == NULL) {
      host_end = value + strlen(value);
    } else {
      port = host_end + 1;
    }
    host_length = (size_t)(host_end - value);
    if (host_length == 0u || host_length > 253u) {
      return 0;
    }
    label_length = 0u;
    for (cursor = value; cursor != host_end; ++cursor) {
      unsigned char c = (unsigned char)*cursor;
      if (c == '.') {
        if (label_length == 0u || cursor[-1] == '-') {
          return 0;
        }
        label_length = 0u;
      } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '~') {
        if (label_length == 0u && c == '-') {
          return 0;
        }
        if (++label_length > 63u) {
          return 0;
        }
      } else {
        return 0;
      }
    }
    if (host_end[-1] == '-' || (host_end[-1] == '.' && host_length == 1u)) {
      return 0;
    }
  }
  if (port != NULL) {
    if (*port == '\0') {
      return 0;
    }
    number = 0u;
    for (; *port != '\0'; ++port) {
      if (*port < '0' || *port > '9') {
        return 0;
      }
      digit = (unsigned)(*port - '0');
      if (number > (65535u - digit) / 10u) {
        return 0;
      }
      number = number * 10u + digit;
    }
    if (number == 0u) {
      return 0;
    }
  }
  return 1;
}

void vectis_proxy_headers_init(vectis_proxy_headers *headers) {
  if (headers != NULL) {
    memset(headers, 0, sizeof(*headers));
  }
}

void vectis_proxy_headers_cleanup(vectis_proxy_headers *headers) {
  size_t i;

  if (headers == NULL) {
    return;
  }
  for (i = 0u; i < headers->count; ++i) {
    free(headers->fields[i].name);
    free(headers->fields[i].value);
  }
  vectis_proxy_headers_init(headers);
}

vectis_proxy_header_status
vectis_proxy_headers_add(vectis_proxy_headers *headers, const char *name,
                         const char *value) {
  size_t name_length;
  size_t value_length;
  size_t i;
  char *name_copy;
  char *value_copy;

  if (headers == NULL || name == NULL || value == NULL) {
    return VECTIS_PROXY_HEADER_INVALID;
  }
  name_length = strlen(name);
  value_length = strlen(value);
  if (name_length == 0u) {
    return VECTIS_PROXY_HEADER_INVALID;
  }
  if (headers->count == VECTIS_PROXY_HEADER_COUNT_LIMIT ||
      name_length > VECTIS_PROXY_HEADER_BLOCK_LIMIT - headers->bytes ||
      value_length >
          VECTIS_PROXY_HEADER_BLOCK_LIMIT - headers->bytes - name_length) {
    return VECTIS_PROXY_HEADER_LIMIT;
  }
  for (i = 0u; i < name_length; ++i) {
    if (!vectis_proxy_token_byte((unsigned char)name[i])) {
      return VECTIS_PROXY_HEADER_INVALID;
    }
  }
  for (i = 0u; i < value_length; ++i) {
    unsigned char c = (unsigned char)value[i];
    if ((c < 0x20u && c != '\t') || c == 0x7fu) {
      return VECTIS_PROXY_HEADER_INVALID;
    }
  }
  name_copy = (char *)malloc(name_length + 1u);
  value_copy = (char *)malloc(value_length + 1u);
  if (name_copy == NULL || value_copy == NULL) {
    free(name_copy);
    free(value_copy);
    return VECTIS_PROXY_HEADER_NOMEM;
  }
  memcpy(name_copy, name, name_length + 1u);
  memcpy(value_copy, value, value_length + 1u);
  headers->fields[headers->count].name = name_copy;
  headers->fields[headers->count].value = value_copy;
  headers->count++;
  headers->bytes += name_length + value_length;
  return VECTIS_PROXY_HEADER_OK;
}

vectis_proxy_header_status
vectis_proxy_request_head_parse(const vectis_proxy_headers *headers,
                                vectis_proxy_request_head *head,
                                const char **reason) {
  size_t i;
  uint64_t length;
  int connection_upgrade;
  int upgrade_websocket;
  int found;
  int upgrade_seen;
  int expect_seen;
  int transfer_seen;
  int host_seen;

  if (reason != NULL) {
    *reason = NULL;
  }
  if (headers == NULL || head == NULL) {
    if (reason != NULL)
      *reason = "request headers are required";
    return VECTIS_PROXY_HEADER_INVALID;
  }
  memset(head, 0, sizeof(*head));
  transfer_seen = 0;
  expect_seen = 0;
  upgrade_seen = 0;
  host_seen = 0;
  for (i = 0u; i < headers->count; ++i) {
    const char *name = headers->fields[i].name;
    const char *value = headers->fields[i].value;
    size_t name_length = strlen(name);
    if (vectis_proxy_ascii_equal(name, name_length, "host")) {
      if (++host_seen != 1 || !vectis_proxy_valid_host(value)) {
        if (reason != NULL)
          *reason = "invalid or repeated Host";
        return VECTIS_PROXY_HEADER_INVALID;
      }
      head->host = value;
    } else if (vectis_proxy_ascii_equal(name, name_length, "content-length")) {
      if (!vectis_proxy_parse_length(value, &length) ||
          (head->has_content_length && length != head->content_length)) {
        if (reason != NULL)
          *reason = "invalid or conflicting Content-Length";
        return VECTIS_PROXY_HEADER_INVALID;
      }
      head->has_content_length = 1;
      head->content_length = length;
    } else if (vectis_proxy_ascii_equal(name, name_length,
                                        "transfer-encoding")) {
      if (++transfer_seen != 1 ||
          !vectis_proxy_value_equals(value, "chunked")) {
        if (reason != NULL)
          *reason = "unsupported Transfer-Encoding";
        return VECTIS_PROXY_HEADER_INVALID;
      }
      head->chunked = 1;
    } else if (vectis_proxy_ascii_equal(name, name_length, "expect")) {
      if (++expect_seen != 1 ||
          !vectis_proxy_value_equals(value, "100-continue")) {
        if (reason != NULL)
          *reason = "unsupported Expect header";
        return VECTIS_PROXY_HEADER_INVALID;
      }
      head->expect_continue = 1;
    } else if (vectis_proxy_ascii_equal(name, name_length, "upgrade")) {
      if (++upgrade_seen != 1 ||
          !vectis_proxy_value_equals(value, "websocket")) {
        if (reason != NULL)
          *reason = "unsupported Upgrade protocol";
        return VECTIS_PROXY_HEADER_UNSUPPORTED_UPGRADE;
      }
    } else if (vectis_proxy_ascii_equal(name, name_length, "trailer")) {
      head->has_trailers = 1;
    }
  }
  if (host_seen != 1 || (head->chunked && head->has_content_length)) {
    if (reason != NULL)
      *reason = "missing Host or conflicting body framing";
    return VECTIS_PROXY_HEADER_INVALID;
  }
  if (head->has_trailers && !head->chunked) {
    if (reason != NULL)
      *reason = "Trailer requires chunked body framing";
    return VECTIS_PROXY_HEADER_INVALID;
  }
  if (head->expect_continue &&
      !(head->chunked ||
        (head->has_content_length && head->content_length != 0u))) {
    if (reason != NULL)
      *reason = "Expect requires a request body";
    return VECTIS_PROXY_HEADER_INVALID;
  }
  if (!vectis_proxy_has_list_token(headers, "connection", "upgrade", 0,
                                   &connection_upgrade) ||
      !vectis_proxy_has_list_token(headers, "upgrade", "websocket", 0,
                                   &upgrade_websocket) ||
      !vectis_proxy_has_list_token(headers, "trailer", NULL, 1, &found)) {
    if (reason != NULL)
      *reason = "invalid connection, upgrade, or Trailer tokens";
    return VECTIS_PROXY_HEADER_INVALID;
  }
  if (head->has_trailers &&
      vectis_proxy_has_list_token(headers, "connection", "trailer", 0,
                                  &found) &&
      found) {
    if (reason != NULL)
      *reason = "Connection may not nominate Trailer";
    return VECTIS_PROXY_HEADER_INVALID;
  }
  if (upgrade_seen != 0 || connection_upgrade) {
    if (upgrade_seen != 1 || !connection_upgrade || !upgrade_websocket) {
      if (reason != NULL)
        *reason = "unsupported or incomplete Upgrade";
      return VECTIS_PROXY_HEADER_UNSUPPORTED_UPGRADE;
    }
    head->websocket_upgrade = 1;
    if (head->chunked || head->has_content_length || head->expect_continue) {
      if (reason != NULL)
        *reason = "WebSocket upgrade may not have a framed body";
      return VECTIS_PROXY_HEADER_INVALID;
    }
  }
  if (!vectis_proxy_has_list_token(headers, "connection", "host", 0, &found) ||
      found ||
      !vectis_proxy_has_list_token(headers, "connection", "content-length", 0,
                                   &found) ||
      found ||
      !vectis_proxy_has_list_token(headers, "connection", "transfer-encoding",
                                   0, &found) ||
      found) {
    if (reason != NULL)
      *reason = "Connection nominates request framing";
    return VECTIS_PROXY_HEADER_INVALID;
  }
  return VECTIS_PROXY_HEADER_OK;
}

static int vectis_proxy_hop_field(const char *name) {
  static const char *const hop[] = {"connection",
                                    "keep-alive",
                                    "proxy-connection",
                                    "te",
                                    "trailer",
                                    "transfer-encoding",
                                    "upgrade",
                                    "proxy-authenticate",
                                    "proxy-authorization",
                                    "host",
                                    "content-length",
                                    "expect",
                                    "forwarded",
                                    "x-real-ip"};
  size_t i;
  size_t length;

  length = strlen(name);
  if (vectis_proxy_ascii_prefix(name, "x-forwarded-")) {
    return 1;
  }
  for (i = 0u; i < sizeof(hop) / sizeof(hop[0]); ++i) {
    if (vectis_proxy_ascii_equal(name, length, hop[i])) {
      return 1;
    }
  }
  return 0;
}

vectis_proxy_header_status
vectis_proxy_headers_sanitize_request(const vectis_proxy_headers *source,
                                      vectis_proxy_headers *destination) {
  vectis_proxy_request_head head;
  size_t i;
  int nominated;
  vectis_proxy_header_status status;

  if (source == NULL || destination == NULL || source == destination ||
      destination->count != 0u) {
    return VECTIS_PROXY_HEADER_INVALID;
  }
  if (vectis_proxy_request_head_parse(source, &head, NULL) !=
      VECTIS_PROXY_HEADER_OK) {
    return VECTIS_PROXY_HEADER_INVALID;
  }
  for (i = 0u; i < source->count; ++i) {
    const char *name = source->fields[i].name;
    if (vectis_proxy_hop_field(name)) {
      continue;
    }
    if (!vectis_proxy_has_list_token(source, "connection", name, 0,
                                     &nominated)) {
      status = VECTIS_PROXY_HEADER_INVALID;
      goto failed;
    }
    if (nominated) {
      continue;
    }
    status =
        vectis_proxy_headers_add(destination, name, source->fields[i].value);
    if (status != VECTIS_PROXY_HEADER_OK) {
      goto failed;
    }
  }
  return VECTIS_PROXY_HEADER_OK;
failed:
  vectis_proxy_headers_cleanup(destination);
  return status;
}

int vectis_proxy_request_trailer_declared(const vectis_proxy_headers *headers,
                                          const char *name) {
  int found;

  if (headers == NULL || name == NULL ||
      !vectis_proxy_trailer_field_allowed(name, strlen(name))) {
    return 0;
  }
  return vectis_proxy_has_list_token(headers, "trailer", name, 1, &found) &&
         found;
}
