#include "vectis_proxy_http_upstream.h"

#include "vectis_internal.h"

#include <stdlib.h>
#include <string.h>

static size_t vectis_proxy_http_upstream_header(char *data, size_t size,
                                                size_t count, void *userdata) {
  vectis_proxy_http_upstream *upstream;
  vectis_proxy_http_event event;
  vectis_proxy_header_status status;
  size_t length;

  upstream = (vectis_proxy_http_upstream *)userdata;
  if (size != 0u && count > ((size_t)-1) / size) {
    upstream->failed = 1;
    return 0u;
  }
  length = size * count;
  status = vectis_proxy_http_response_header(&upstream->response, data, length,
                                             &event, NULL);
  if (status == VECTIS_PROXY_HEADER_OK && event == VECTIS_PROXY_HTTP_FINAL) {
    status = vectis_proxy_headers_sanitize_response(
        &upstream->response.headers, &upstream->outbound_headers);
  }
  if (status != VECTIS_PROXY_HEADER_OK) {
    upstream->failed = 1;
    return 0u;
  }
  if (event != VECTIS_PROXY_HTTP_MORE && upstream->ready != NULL) {
    upstream->ready(upstream, event, upstream->userdata);
  }
  return length;
}

static size_t vectis_proxy_http_upstream_download(char *data, size_t size,
                                                  size_t count,
                                                  void *userdata) {
  vectis_proxy_http_upstream *upstream;
  size_t length;

  upstream = (vectis_proxy_http_upstream *)userdata;
  if (size != 0u && count > ((size_t)-1) / size) {
    upstream->failed = 1;
    return 0u;
  }
  length = size * count;
  if (upstream->body_length != upstream->body_offset) {
    upstream->paused = 1;
    return CURL_WRITEFUNC_PAUSE;
  }
  if (length > upstream->body_capacity ||
      vectis_proxy_http_response_body(&upstream->response, length, NULL) !=
          VECTIS_PROXY_HEADER_OK) {
    upstream->failed = 1;
    return 0u;
  }
  if (length != 0u) {
    memcpy(upstream->body, data, length);
    upstream->body_offset = 0u;
    upstream->body_length = length;
    if (upstream->ready != NULL) {
      upstream->ready(upstream, VECTIS_PROXY_HTTP_MORE, upstream->userdata);
    }
  }
  return length;
}

vectis_status vectis_proxy_http_upstream_init(
    vectis_proxy_http_upstream *upstream, CURL *easy, const char *url,
    const char *request_target, const char *method,
    struct curl_slist *request_headers, size_t buffer_limit,
    long connect_timeout_ms, long total_timeout_ms,
    vectis_proxy_http_upstream_ready_fn ready, void *userdata,
    vectis_error *error) {
  size_t capacity;
  CURLcode code;
  int head;

  if (upstream == NULL || easy == NULL || url == NULL ||
      request_target == NULL || method == NULL || buffer_limit < 8192u ||
      buffer_limit > 1048576u || connect_timeout_ms < 0L ||
      total_timeout_ms < 0L) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "invalid proxy upstream transfer configuration");
    return VECTIS_ERR_INVALID;
  }
  memset(upstream, 0, sizeof(*upstream));
  head = strcmp(method, "HEAD") == 0;
  if (!head && strcmp(method, "GET") != 0) {
    vectis_set_error(error, VECTIS_ERR_NOT_IMPLEMENTED,
                     "proxy upstream upload is not configured");
    return VECTIS_ERR_NOT_IMPLEMENTED;
  }
  capacity =
      buffer_limit > CURL_MAX_WRITE_SIZE ? buffer_limit : CURL_MAX_WRITE_SIZE;
  upstream->body = (unsigned char *)malloc(capacity);
  if (upstream->body == NULL) {
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate bounded proxy download chunk");
    return VECTIS_ERR_NOMEM;
  }
  upstream->body_capacity = capacity;
  upstream->easy = easy;
  upstream->ready = ready;
  upstream->userdata = userdata;
  vectis_proxy_http_response_init(&upstream->response, head);
  vectis_proxy_headers_init(&upstream->outbound_headers);
  code = curl_easy_setopt(easy, CURLOPT_URL, url);
  if (code == CURLE_OK)
    code = curl_easy_setopt(easy, CURLOPT_REQUEST_TARGET, request_target);
  if (code == CURLE_OK)
    code = curl_easy_setopt(easy, CURLOPT_NOBODY, head ? 1L : 0L);
  if (code == CURLE_OK)
    code = curl_easy_setopt(easy, CURLOPT_HTTPHEADER, request_headers);
  if (code == CURLE_OK)
    code = curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION,
                            vectis_proxy_http_upstream_header);
  if (code == CURLE_OK)
    code = curl_easy_setopt(easy, CURLOPT_HEADERDATA, upstream);
  if (code == CURLE_OK)
    code = curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
                            vectis_proxy_http_upstream_download);
  if (code == CURLE_OK)
    code = curl_easy_setopt(easy, CURLOPT_WRITEDATA, upstream);
  if (code == CURLE_OK)
    code = curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, (long)buffer_limit);
  if (code == CURLE_OK)
    code =
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
  if (code == CURLE_OK)
    code = curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, total_timeout_ms);
  if (code == CURLE_OK)
    code = curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  if (code != CURLE_OK) {
    vectis_proxy_http_upstream_cleanup(upstream);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to configure proxy upstream transfer");
    return VECTIS_ERR_STATE;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

const unsigned char *
vectis_proxy_http_upstream_body(const vectis_proxy_http_upstream *upstream,
                                size_t *length) {
  if (length != NULL)
    *length =
        upstream != NULL ? upstream->body_length - upstream->body_offset : 0u;
  if (upstream == NULL || upstream->body_length == upstream->body_offset)
    return NULL;
  return upstream->body + upstream->body_offset;
}

void vectis_proxy_http_upstream_consume(vectis_proxy_http_upstream *upstream,
                                        size_t length) {
  if (upstream == NULL ||
      length > upstream->body_length - upstream->body_offset)
    return;
  upstream->body_offset += length;
  if (upstream->body_offset == upstream->body_length) {
    upstream->body_offset = 0u;
    upstream->body_length = 0u;
  }
}

vectis_status
vectis_proxy_http_upstream_resume(vectis_proxy_http_upstream *upstream,
                                  vectis_error *error) {
  if (upstream == NULL || upstream->easy == NULL ||
      upstream->body_length != upstream->body_offset) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "proxy downstream must drain before curl resumes");
    return VECTIS_ERR_STATE;
  }
  if (upstream->paused) {
    upstream->paused = 0;
    if (curl_easy_pause(upstream->easy, CURLPAUSE_CONT) != CURLE_OK) {
      upstream->failed = 1;
      vectis_set_error(error, VECTIS_ERR_STATE,
                       "failed to resume proxy upstream transfer");
      return VECTIS_ERR_STATE;
    }
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_proxy_header_status
vectis_proxy_http_upstream_finish(vectis_proxy_http_upstream *upstream,
                                  CURLcode result, const char **reason) {
  vectis_proxy_http_event event;
  vectis_proxy_header_status status;

  if (upstream == NULL || upstream->failed || result != CURLE_OK) {
    if (reason != NULL)
      *reason = "upstream transfer or framing failed";
    return VECTIS_PROXY_HEADER_INVALID;
  }
  status = vectis_proxy_http_response_curl_complete(&upstream->response, &event,
                                                    reason);
  if (status == VECTIS_PROXY_HEADER_OK && event == VECTIS_PROXY_HTTP_TRAILERS &&
      upstream->ready != NULL)
    upstream->ready(upstream, event, upstream->userdata);
  return status;
}

void vectis_proxy_http_upstream_cleanup(vectis_proxy_http_upstream *upstream) {
  if (upstream == NULL)
    return;
  vectis_proxy_http_response_cleanup(&upstream->response);
  vectis_proxy_headers_cleanup(&upstream->outbound_headers);
  free(upstream->body);
  memset(upstream, 0, sizeof(*upstream));
}
