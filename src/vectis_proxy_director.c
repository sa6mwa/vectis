#include "vectis_proxy_director.h"

#include "vectis_internal.h"
#include "vectis_proxy_url.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define VECTIS_PROXY_DIRECTOR_STRING_LIMIT 65536u

static vectis_status vectis_proxy_director_copy(char **slot, const char *value,
                                                vectis_error *error) {
  char *copy;
  size_t length;

  if (value == NULL) {
    free(*slot);
    *slot = NULL;
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  length = strlen(value);
  if (length > VECTIS_PROXY_DIRECTOR_STRING_LIMIT) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy rewrite string exceeds 65536 bytes");
    return VECTIS_ERR_INVALID;
  }
  copy = (char *)malloc(length + 1u);
  if (copy == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy proxy rewrite string");
    return VECTIS_ERR_NOMEM;
  }
  memcpy(copy, value, length + 1u);
  free(*slot);
  *slot = copy;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static int vectis_proxy_director_host_valid(const char *host) {
  const unsigned char *cursor;

  if (host == NULL || host[0] == '\0')
    return 0;
  for (cursor = (const unsigned char *)host; *cursor != '\0'; ++cursor) {
    if (*cursor <= 0x20u || *cursor >= 0x7fu || *cursor == '/' ||
        *cursor == '\\' || *cursor == '@' || *cursor == '?' || *cursor == '#')
      return 0;
  }
  return 1;
}

vectis_http_method vectis_proxy_inbound_method(const vectis_proxy_inbound *in) {
  return in != NULL ? in->method : VECTIS_HTTP_GET;
}

const char *vectis_proxy_inbound_path(const vectis_proxy_inbound *in) {
  return in != NULL ? in->path : NULL;
}

const char *vectis_proxy_inbound_query(const vectis_proxy_inbound *in) {
  return in != NULL ? in->query : NULL;
}

const char *vectis_proxy_inbound_host(const vectis_proxy_inbound *in) {
  return in != NULL ? in->host : NULL;
}

int vectis_proxy_inbound_websocket(const vectis_proxy_inbound *in) {
  return in != NULL && in->websocket;
}

const char *vectis_proxy_inbound_path_param(const vectis_proxy_inbound *in,
                                            const char *name) {
  if (in == NULL || in->matched_request == NULL || name == NULL)
    return NULL;
  return vectis_request_path_param(in->matched_request, name);
}

size_t vectis_proxy_inbound_header_count(const vectis_proxy_inbound *in) {
  return in != NULL && in->headers != NULL ? in->headers->count : 0u;
}

vectis_status vectis_proxy_inbound_header_at(const vectis_proxy_inbound *in,
                                             size_t index, const char **name,
                                             const char **value) {
  if (name != NULL)
    *name = NULL;
  if (value != NULL)
    *value = NULL;
  if (in == NULL || in->headers == NULL || name == NULL || value == NULL ||
      index >= in->headers->count)
    return VECTIS_ERR_INVALID;
  *name = in->headers->fields[index].name;
  *value = in->headers->fields[index].value;
  return VECTIS_OK;
}

vectis_status vectis_proxy_outbound_select_target(vectis_proxy_outbound *out,
                                                  size_t index,
                                                  vectis_error *error) {
  if (out == NULL || out->route == NULL || index >= out->route->target_count) {
    if (out != NULL)
      out->failure = VECTIS_ERR_INVALID;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy rewrite target is not configured");
    return VECTIS_ERR_INVALID;
  }
  out->target_index = index;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_proxy_outbound_set_method(vectis_proxy_outbound *out,
                                               vectis_http_method method,
                                               vectis_error *error) {
  if (out == NULL || vectis_http_method_string(method) == NULL ||
      (out->websocket && method != VECTIS_HTTP_GET)) {
    if (out != NULL)
      out->failure = VECTIS_ERR_INVALID;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy rewrite method is invalid for this request");
    return VECTIS_ERR_INVALID;
  }
  out->method = method;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_proxy_outbound_set_path(vectis_proxy_outbound *out,
                                             const char *raw_path,
                                             vectis_error *error) {
  vectis_status status;

  if (out == NULL || raw_path == NULL ||
      vectis_internal_proxy_validate_raw_path(raw_path, error) != VECTIS_OK) {
    if (out != NULL)
      out->failure = VECTIS_ERR_INVALID;
    if (out == NULL || raw_path == NULL)
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "proxy rewrite path is required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_proxy_director_copy(&out->path, raw_path, error);
  if (status != VECTIS_OK)
    out->failure = status;
  return status;
}

vectis_status vectis_proxy_outbound_set_query(vectis_proxy_outbound *out,
                                              const char *raw_query,
                                              vectis_error *error) {
  vectis_status status;

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy rewrite output is required");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_proxy_director_copy(&out->query, raw_query, error);
  if (status != VECTIS_OK)
    out->failure = status;
  return status;
}

vectis_status vectis_proxy_outbound_set_host(vectis_proxy_outbound *out,
                                             const char *host,
                                             vectis_error *error) {
  vectis_status status;

  if (out == NULL ||
      (host != NULL && !vectis_proxy_director_host_valid(host))) {
    if (out != NULL)
      out->failure = VECTIS_ERR_INVALID;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy rewrite Host is invalid");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_proxy_director_copy(&out->host, host, error);
  if (status != VECTIS_OK)
    out->failure = status;
  return status;
}

static vectis_status
vectis_proxy_director_header_status(vectis_proxy_header_status status,
                                    vectis_error *error) {
  if (status == VECTIS_PROXY_HEADER_OK) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  if (status == VECTIS_PROXY_HEADER_NOMEM) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate proxy rewrite header");
    return VECTIS_ERR_NOMEM;
  }
  vectis_set_error(error, VECTIS_ERR_INVALID,
                   "proxy rewrite header is invalid or exceeds limits");
  return VECTIS_ERR_INVALID;
}

vectis_status vectis_proxy_outbound_add_header(vectis_proxy_outbound *out,
                                               const char *name,
                                               const char *value,
                                               vectis_error *error) {
  vectis_status status;

  if (out == NULL || !vectis_proxy_request_header_editable(name)) {
    if (out != NULL)
      out->failure = VECTIS_ERR_INVALID;
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "proxy rewrite cannot edit transport or handshake headers");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_proxy_director_header_status(
      vectis_proxy_headers_add(&out->headers, name, value), error);
  if (status != VECTIS_OK)
    out->failure = status;
  return status;
}

vectis_status vectis_proxy_outbound_remove_header(vectis_proxy_outbound *out,
                                                  const char *name,
                                                  vectis_error *error) {
  size_t i;
  size_t length;

  if (out == NULL || !vectis_proxy_request_header_editable(name)) {
    if (out != NULL)
      out->failure = VECTIS_ERR_INVALID;
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "proxy rewrite cannot edit transport or handshake headers");
    return VECTIS_ERR_INVALID;
  }
  for (i = 0u; i < out->headers.count;) {
    if (strcasecmp(out->headers.fields[i].name, name) != 0) {
      ++i;
      continue;
    }
    length = strlen(out->headers.fields[i].name) +
             strlen(out->headers.fields[i].value);
    free(out->headers.fields[i].name);
    free(out->headers.fields[i].value);
    if (i + 1u < out->headers.count)
      memmove(&out->headers.fields[i], &out->headers.fields[i + 1u],
              (out->headers.count - i - 1u) * sizeof(out->headers.fields[0]));
    --out->headers.count;
    out->headers.bytes -= length;
    memset(&out->headers.fields[out->headers.count], 0,
           sizeof(out->headers.fields[0]));
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_proxy_outbound_set_header(vectis_proxy_outbound *out,
                                               const char *name,
                                               const char *value,
                                               vectis_error *error) {
  vectis_status status;

  status = vectis_proxy_outbound_remove_header(out, name, error);
  if (status != VECTIS_OK)
    return status;
  return vectis_proxy_outbound_add_header(out, name, value, error);
}

void vectis_proxy_director_cleanup(vectis_proxy_outbound *out) {
  if (out == NULL)
    return;
  vectis_proxy_headers_cleanup(&out->headers);
  free(out->path);
  free(out->query);
  free(out->host);
  memset(out, 0, sizeof(*out));
}

vectis_status vectis_proxy_director_prepare(
    const vectis_proxy_route_data *route, vectis_http_method method,
    const char *path, const char *query, const char *host, int websocket,
    vectis_request *matched_request, const vectis_proxy_headers *inbound,
    const vectis_proxy_headers *sanitized, vectis_proxy_outbound *out,
    char **target, char **authority, vectis_error *error) {
  vectis_proxy_inbound input;
  vectis_status status;
  char *initial_target;
  char *initial_authority;
  char *default_authority;
  size_t i;

  if (out == NULL || target == NULL || authority == NULL || route == NULL ||
      route->target_count == 0u || sanitized == NULL || inbound == NULL ||
      path == NULL || host == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy director requires request metadata and route");
    return VECTIS_ERR_INVALID;
  }
  *target = NULL;
  *authority = NULL;
  memset(out, 0, sizeof(*out));
  initial_target = NULL;
  initial_authority = NULL;
  status =
      vectis_proxy_target_build(route->targets[0], path, query, &initial_target,
                                &initial_authority, error);
  free(initial_target);
  free(initial_authority);
  if (status != VECTIS_OK)
    return status;
  out->route = route;
  out->method = method;
  out->websocket = websocket;
  for (i = 0u; i < sanitized->count; ++i) {
    if (vectis_proxy_headers_add(&out->headers, sanitized->fields[i].name,
                                 sanitized->fields[i].value) !=
        VECTIS_PROXY_HEADER_OK) {
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy proxy outbound headers");
      return VECTIS_ERR_NOMEM;
    }
  }
  status = vectis_proxy_director_copy(&out->path, path, error);
  if (status == VECTIS_OK)
    status = vectis_proxy_director_copy(&out->query, query, error);
  if (status != VECTIS_OK)
    return status;
  input.method = method;
  input.path = path;
  input.query = query;
  input.host = host;
  input.headers = inbound;
  input.matched_request = matched_request;
  input.websocket = websocket;
  if (route->rewrite != NULL) {
    status = route->rewrite(&input, out, route->rewrite_userdata, error);
    if (status != VECTIS_OK)
      return status;
    if (out->failure != VECTIS_OK) {
      vectis_set_error(error, out->failure,
                       "proxy rewrite attempted an invalid edit");
      return out->failure;
    }
  }
  status =
      vectis_proxy_target_build(route->targets[out->target_index], out->path,
                                out->query, target, &default_authority, error);
  if (status != VECTIS_OK)
    return status;
  if (out->websocket && out->method != VECTIS_HTTP_GET) {
    free(*target);
    *target = NULL;
    free(default_authority);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy WebSocket rewrite must retain GET");
    return VECTIS_ERR_INVALID;
  }
  if (out->host != NULL) {
    free(default_authority);
    status = vectis_proxy_director_copy(authority, out->host, error);
    if (status != VECTIS_OK) {
      free(*target);
      *target = NULL;
      return status;
    }
  } else {
    *authority = default_authority;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}
