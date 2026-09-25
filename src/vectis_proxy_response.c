#include "vectis_proxy_response.h"

#include "vectis_internal.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

int vectis_proxy_response_status(const vectis_proxy_response *response) {
  return response != NULL ? response->status : 0;
}

size_t
vectis_proxy_response_header_count(const vectis_proxy_response *response) {
  return response != NULL && response->headers != NULL
             ? response->headers->count
             : 0u;
}

vectis_status
vectis_proxy_response_header_at(const vectis_proxy_response *response,
                                size_t index, const char **name,
                                const char **value) {
  if (name != NULL)
    *name = NULL;
  if (value != NULL)
    *value = NULL;
  if (response == NULL || response->headers == NULL || name == NULL ||
      value == NULL || index >= response->headers->count)
    return VECTIS_ERR_INVALID;
  *name = response->headers->fields[index].name;
  *value = response->headers->fields[index].value;
  return VECTIS_OK;
}

vectis_status vectis_proxy_response_set_status(vectis_proxy_response *response,
                                               int status,
                                               vectis_error *error) {
  int body_allowed;

  if (response == NULL || response->upstream == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy response view is required");
    return VECTIS_ERR_INVALID;
  }
  body_allowed = !response->upstream->head_request && status != 204 &&
                 status != 205 && status != 304;
  if (status < 200 || status > 599 || status == 101 ||
      body_allowed != response->upstream->body_allowed) {
    response->failure = VECTIS_ERR_INVALID;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy response status conflicts with streamed body");
    return VECTIS_ERR_INVALID;
  }
  response->status = status;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status
vectis_proxy_response_header_status(vectis_proxy_header_status status,
                                    vectis_error *error) {
  if (status == VECTIS_PROXY_HEADER_OK) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  if (status == VECTIS_PROXY_HEADER_NOMEM) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate proxy response header");
    return VECTIS_ERR_NOMEM;
  }
  vectis_set_error(error, VECTIS_ERR_INVALID,
                   "proxy response header is invalid or exceeds limits");
  return VECTIS_ERR_INVALID;
}

vectis_status vectis_proxy_response_add_header(vectis_proxy_response *response,
                                               const char *name,
                                               const char *value,
                                               vectis_error *error) {
  vectis_status status;

  if (response == NULL || response->headers == NULL ||
      !vectis_proxy_response_header_editable(name)) {
    if (response != NULL)
      response->failure = VECTIS_ERR_INVALID;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy response cannot edit transport headers");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_proxy_response_header_status(
      vectis_proxy_headers_add(response->headers, name, value), error);
  if (status != VECTIS_OK)
    response->failure = status;
  return status;
}

vectis_status
vectis_proxy_response_remove_header(vectis_proxy_response *response,
                                    const char *name, vectis_error *error) {
  size_t i;
  size_t length;

  if (response == NULL || response->headers == NULL ||
      !vectis_proxy_response_header_editable(name)) {
    if (response != NULL)
      response->failure = VECTIS_ERR_INVALID;
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy response cannot edit transport headers");
    return VECTIS_ERR_INVALID;
  }
  for (i = 0u; i < response->headers->count;) {
    if (strcasecmp(response->headers->fields[i].name, name) != 0) {
      ++i;
      continue;
    }
    length = strlen(response->headers->fields[i].name) +
             strlen(response->headers->fields[i].value);
    free(response->headers->fields[i].name);
    free(response->headers->fields[i].value);
    if (i + 1u < response->headers->count)
      memmove(&response->headers->fields[i], &response->headers->fields[i + 1u],
              (response->headers->count - i - 1u) *
                  sizeof(response->headers->fields[0]));
    --response->headers->count;
    response->headers->bytes -= length;
    memset(&response->headers->fields[response->headers->count], 0,
           sizeof(response->headers->fields[0]));
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_proxy_response_set_header(vectis_proxy_response *response,
                                               const char *name,
                                               const char *value,
                                               vectis_error *error) {
  vectis_status status;

  status = vectis_proxy_response_remove_header(response, name, error);
  if (status != VECTIS_OK)
    return status;
  return vectis_proxy_response_add_header(response, name, value, error);
}

vectis_status vectis_proxy_response_apply(
    const vectis_proxy_http_response *upstream, vectis_proxy_headers *sanitized,
    vectis_proxy_modify_response_fn modify, void *userdata,
    int *downstream_status, vectis_error *error) {
  vectis_proxy_response view;
  vectis_status status;

  if (upstream == NULL || sanitized == NULL || downstream_status == NULL ||
      upstream->status < 200 || upstream->status > 599 ||
      upstream->status == 101) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy response requires final non-upgrade metadata");
    return VECTIS_ERR_INVALID;
  }
  view.upstream = upstream;
  view.headers = sanitized;
  view.status = upstream->status;
  view.failure = VECTIS_OK;
  if (modify != NULL) {
    status = modify(&view, userdata, error);
    if (status != VECTIS_OK)
      return status;
    if (view.failure != VECTIS_OK) {
      vectis_set_error(error, view.failure,
                       "proxy response hook attempted an invalid edit");
      return view.failure;
    }
  }
  *downstream_status = view.status;
  vectis_error_clear(error);
  return VECTIS_OK;
}
