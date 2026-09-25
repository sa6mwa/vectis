#include "vectis_proxy_local.h"

#include "vectis_internal.h"

#include <stdlib.h>
#include <string.h>

void vectis_proxy_local_init(vectis_proxy_local_response *response) {
  if (response != NULL)
    memset(response, 0, sizeof(*response));
}

void vectis_proxy_local_cleanup(vectis_proxy_local_response *response) {
  if (response == NULL)
    return;
  vectis_proxy_headers_cleanup(&response->headers);
  free(response->body);
  memset(response, 0, sizeof(*response));
}

vectis_status vectis_proxy_local_respond(vectis_proxy_local_response *response,
                                         int status, const void *body,
                                         size_t body_length,
                                         vectis_error *error) {
  unsigned char *copy;

  if (response == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy local response is required");
    return VECTIS_ERR_INVALID;
  }
  if (status < 200 || status > 599 || status == 101 ||
      body_length > VECTIS_PROXY_LOCAL_BODY_LIMIT ||
      (body_length != 0u && body == NULL) ||
      ((status == 204 || status == 205 || status == 304) &&
       body_length != 0u)) {
    response->failure = VECTIS_ERR_INVALID;
    vectis_set_error(
        error, VECTIS_ERR_INVALID,
        "proxy local status or body is invalid or exceeds 65536 bytes");
    return VECTIS_ERR_INVALID;
  }
  copy = NULL;
  if (body_length != 0u) {
    copy = (unsigned char *)malloc(body_length);
    if (copy == NULL) {
      response->failure = VECTIS_ERR_NOMEM;
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy proxy local response body");
      return VECTIS_ERR_NOMEM;
    }
    memcpy(copy, body, body_length);
  }
  free(response->body);
  response->body = copy;
  response->body_length = body_length;
  response->status = status;
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status
vectis_proxy_local_add_header(vectis_proxy_local_response *response,
                              const char *name, const char *value,
                              vectis_error *error) {
  vectis_proxy_header_status result;

  if (response == NULL || response->status == 0 ||
      !vectis_proxy_response_header_editable(name)) {
    if (response != NULL)
      response->failure = VECTIS_ERR_INVALID;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy local response cannot edit transport headers");
    return VECTIS_ERR_INVALID;
  }
  result = vectis_proxy_headers_add(&response->headers, name, value);
  if (result != VECTIS_PROXY_HEADER_OK) {
    response->failure = result == VECTIS_PROXY_HEADER_NOMEM
                            ? VECTIS_ERR_NOMEM
                            : VECTIS_ERR_INVALID;
    vectis_set_error(
        error, response->failure,
        "proxy local response header is invalid or exceeds limits");
    return response->failure;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

void vectis_proxy_local_error(const vectis_proxy_route_data *route,
                              const vectis_error *cause, int default_status,
                              vectis_proxy_local_response *response) {
  static const char bad_gateway[] = "bad gateway";
  static const char timeout[] = "gateway timeout";
  const char *body;
  vectis_error error;
  vectis_status status;

  vectis_proxy_local_init(response);
  if (route != NULL && route->on_error != NULL) {
    vectis_error_clear(&error);
    status = route->on_error(cause, default_status, response,
                             route->on_error_userdata, &error);
    if (status == VECTIS_OK && response->failure == VECTIS_OK &&
        response->status != 0)
      return;
    vectis_proxy_local_cleanup(response);
  }
  body = default_status == 504 ? timeout : bad_gateway;
  (void)vectis_proxy_local_respond(response, default_status, body, strlen(body),
                                   NULL);
}
