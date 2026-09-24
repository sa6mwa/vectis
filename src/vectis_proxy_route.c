#include "vectis_proxy_route_internal.h"

#include "vectis_internal.h"

#include <curl/curl.h>
#include <curl/urlapi.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if VECTIS_WITH_KORE_RUNTIME
#define VECTIS_PROXY_DEFAULT_CONNECT_TIMEOUT_MS 10000L
#define VECTIS_PROXY_DEFAULT_IDLE_TIMEOUT_MS 60000L
#define VECTIS_PROXY_DEFAULT_BUFFER_LIMIT_BYTES 16384u
#define VECTIS_PROXY_MIN_BUFFER_LIMIT_BYTES 8192u
#define VECTIS_PROXY_MAX_BUFFER_LIMIT_BYTES 1048576u
#define VECTIS_PROXY_MAX_TARGETS 16u

static char *vectis_proxy_copy_string(const char *value) {
  char *copy;
  size_t length;

  length = strlen(value);
  copy = (char *)malloc(length + 1u);
  if (copy != NULL) {
    memcpy(copy, value, length + 1u);
  }
  return copy;
}

static void vectis_proxy_route_data_free(void *userdata) {
  vectis_proxy_route_data *data;
  size_t i;

  data = (vectis_proxy_route_data *)userdata;
  if (data == NULL) {
    return;
  }
  for (i = 0u; i < data->target_count; ++i) {
    free(data->targets[i]);
  }
  free(data->targets);
  free(data);
}

static int vectis_proxy_target_valid(const char *target) {
  CURLU *url;
  CURLUcode code;
  char *scheme;
  char *host;
  char *part;
  int valid;
  size_t authority_offset;
  const unsigned char *cursor;

  if (target == NULL || target[0] == '\0' || strchr(target, '\\') != NULL ||
      strchr(target, '?') != NULL || strchr(target, '#') != NULL) {
    return 0;
  }
  for (cursor = (const unsigned char *)target; *cursor != '\0'; ++cursor) {
    if (*cursor <= 0x20u || *cursor == 0x7fu) {
      return 0;
    }
  }
  if (strncasecmp(target, "http://", 7u) == 0) {
    authority_offset = 7u;
  } else if (strncasecmp(target, "https://", 8u) == 0) {
    authority_offset = 8u;
  } else {
    return 0;
  }
  if (target[authority_offset] == '\0' || target[authority_offset] == '/' ||
      target[authority_offset] == '?' || target[authority_offset] == '#') {
    return 0;
  }
  url = curl_url();
  if (url == NULL) {
    return 0;
  }
  valid = 0;
  scheme = NULL;
  host = NULL;
  code = curl_url_set(url, CURLUPART_URL, target, 0u);
  if (code != CURLUE_OK ||
      curl_url_get(url, CURLUPART_SCHEME, &scheme, 0u) != CURLUE_OK ||
      curl_url_get(url, CURLUPART_HOST, &host, 0u) != CURLUE_OK ||
      (strcasecmp(scheme, "http") != 0 && strcasecmp(scheme, "https") != 0) ||
      host[0] == '\0') {
    goto done;
  }
  code = curl_url_get(url, CURLUPART_USER, &part, 0u);
  if (code != CURLUE_NO_USER) {
    if (code == CURLUE_OK)
      curl_free(part);
    goto done;
  }
  code = curl_url_get(url, CURLUPART_PASSWORD, &part, 0u);
  if (code != CURLUE_NO_PASSWORD) {
    if (code == CURLUE_OK)
      curl_free(part);
    goto done;
  }
  code = curl_url_get(url, CURLUPART_OPTIONS, &part, 0u);
  if (code != CURLUE_NO_OPTIONS) {
    if (code == CURLUE_OK)
      curl_free(part);
    goto done;
  }
  code = curl_url_get(url, CURLUPART_QUERY, &part, 0u);
  if (code != CURLUE_NO_QUERY) {
    if (code == CURLUE_OK)
      curl_free(part);
    goto done;
  }
  code = curl_url_get(url, CURLUPART_FRAGMENT, &part, 0u);
  if (code != CURLUE_NO_FRAGMENT) {
    if (code == CURLUE_OK)
      curl_free(part);
    goto done;
  }
  valid = 1;
done:
  curl_free(scheme);
  curl_free(host);
  curl_url_cleanup(url);
  return valid;
}
#endif

void vectis_proxy_route_config_init(vectis_proxy_route_config *config) {
  if (config != NULL) {
    memset(config, 0, sizeof(*config));
  }
}

vectis_status vectis_proxy_route_marker(vectis_app *app,
                                        vectis_request *request,
                                        vectis_response *response,
                                        void *userdata, vectis_error *error) {
  (void)app;
  (void)request;
  (void)response;
  (void)userdata;
  vectis_set_error(error, VECTIS_ERR_STATE,
                   "proxy route must be handled at request headers");
  return VECTIS_ERR_STATE;
}

vectis_status
vectis_register_proxy_route(vectis_app *app,
                            const vectis_proxy_route_config *config,
                            vectis_error *error) {
#if !VECTIS_WITH_KORE_RUNTIME
  (void)app;
  (void)config;
  vectis_set_error(error, VECTIS_ERR_NOT_IMPLEMENTED,
                   "proxy routes require the Kore runtime");
  return VECTIS_ERR_NOT_IMPLEMENTED;
#else
  vectis_proxy_route_data *data;
  vectis_route_config route;
  vectis_status status;
  size_t i;
  size_t count;
  const char *target;

  if (app == NULL || app->impl == NULL || config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy route requires an app and config");
    return VECTIS_ERR_INVALID;
  }
  if (config->path == NULL || config->target == NULL ||
      config->alternate_target_count >= VECTIS_PROXY_MAX_TARGETS ||
      (config->alternate_target_count != 0u &&
       config->alternate_targets == NULL)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy route requires a path and configured target URLs");
    return VECTIS_ERR_INVALID;
  }
  if (config->upstream_http_version != VECTIS_PROXY_HTTP_AUTO &&
      config->upstream_http_version != VECTIS_PROXY_HTTP_1_1) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy upstream_http_version is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (config->connect_timeout_ms < 0L || config->idle_timeout_ms < 0L ||
      config->total_timeout_ms < 0L ||
      (config->buffer_limit_bytes != 0u &&
       (config->buffer_limit_bytes < VECTIS_PROXY_MIN_BUFFER_LIMIT_BYTES ||
        config->buffer_limit_bytes > VECTIS_PROXY_MAX_BUFFER_LIMIT_BYTES))) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "proxy timeouts or buffer_limit_bytes are invalid");
    return VECTIS_ERR_INVALID;
  }
  count = config->alternate_target_count + 1u;
  data = (vectis_proxy_route_data *)calloc(1u, sizeof(*data));
  if (data == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate proxy route configuration");
    return VECTIS_ERR_NOMEM;
  }
  data->targets = (char **)calloc(count, sizeof(*data->targets));
  if (data->targets == NULL) {
    vectis_proxy_route_data_free(data);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate proxy route targets");
    return VECTIS_ERR_NOMEM;
  }
  data->target_count = count;
  for (i = 0u; i < count; ++i) {
    target = i == 0u ? config->target : config->alternate_targets[i - 1u];
    if (!vectis_proxy_target_valid(target)) {
      vectis_proxy_route_data_free(data);
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "proxy target must be an HTTP or HTTPS base URL without "
                       "credentials, query, or fragment");
      return VECTIS_ERR_INVALID;
    }
    data->targets[i] = vectis_proxy_copy_string(target);
    if (data->targets[i] == NULL) {
      vectis_proxy_route_data_free(data);
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy proxy target URL");
      return VECTIS_ERR_NOMEM;
    }
  }
  data->upstream_http_version = config->upstream_http_version;
  data->connect_timeout_ms = config->connect_timeout_ms != 0L
                                 ? config->connect_timeout_ms
                                 : VECTIS_PROXY_DEFAULT_CONNECT_TIMEOUT_MS;
  data->idle_timeout_ms = config->idle_timeout_ms != 0L
                              ? config->idle_timeout_ms
                              : VECTIS_PROXY_DEFAULT_IDLE_TIMEOUT_MS;
  data->total_timeout_ms = config->total_timeout_ms;
  data->buffer_limit_bytes = config->buffer_limit_bytes != 0u
                                 ? config->buffer_limit_bytes
                                 : VECTIS_PROXY_DEFAULT_BUFFER_LIMIT_BYTES;
  route = vectis_route(VECTIS_HTTP_ANY, config->path, vectis_proxy_route_marker,
                       data);
  route.methods = config->methods != VECTIS_HTTP_METHODS_NONE
                      ? config->methods
                      : VECTIS_HTTP_METHODS_ALL;
  route.path_kind = config->path_kind;
  status = vectis_internal_register_owned_route_with_cleanup(
      app, &route, vectis_proxy_route_data_free, error);
  if (status != VECTIS_OK) {
    vectis_proxy_route_data_free(data);
  } else {
    vectis_error_clear(error);
  }
  return status;
#endif
}
