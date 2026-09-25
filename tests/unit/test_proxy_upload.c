#include "vectis_proxy_upload.h"

#include <assert.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

static void test_fixed_backpressure(void) {
  vectis_proxy_upload_buffer upload;
  vectis_proxy_headers headers;
  vectis_proxy_frame_result result;
  vectis_error error;
  unsigned char *body;
  char chunk[8192];
  size_t consumed;
  size_t amount;

  body = (unsigned char *)malloc(8197u);
  assert(body != NULL);
  memset(body, 'a', 8192u);
  memset(body + 8192u, 'b', 5u);
  vectis_proxy_headers_init(&headers);
  assert(vectis_proxy_upload_init(&upload, 8192u, 0, 8197u, &headers, &error) ==
         VECTIS_OK);
  assert(vectis_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) ==
         CURL_READFUNC_PAUSE);
  assert(!vectis_proxy_upload_can_resume(&upload));
  result = vectis_proxy_upload_feed(&upload, body, 8197u, &consumed);
  assert(result == VECTIS_PROXY_FRAME_PAUSED && consumed == 8192u);
  assert(vectis_proxy_upload_full(&upload));
  assert(vectis_proxy_upload_can_resume(&upload));
  vectis_proxy_upload_resumed(&upload);
  amount = vectis_proxy_upload_read(chunk, 1u, 4096u, &upload);
  assert(amount == 4096u);
  assert(memcmp(chunk, body, amount) == 0);
  result = vectis_proxy_upload_feed(&upload, body + consumed, 5u, &consumed);
  assert(result == VECTIS_PROXY_FRAME_COMPLETE && consumed == 5u);
  assert(upload.complete && !vectis_proxy_upload_full(&upload));
  amount = vectis_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload);
  assert(amount == 4101u);
  assert(memcmp(chunk, body + 4096u, amount) == 0);
  assert(vectis_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) == 0u);
  vectis_proxy_upload_cleanup(&upload);
  vectis_proxy_headers_cleanup(&headers);
  free(body);
}

static void test_chunked_trailers(void) {
  static const char wire[] = "3\r\nabc\r\n0\r\nX-Trace: done\r\n\r\nGET /next";
  vectis_proxy_upload_buffer upload;
  vectis_proxy_headers headers;
  vectis_proxy_frame_result result;
  vectis_error error;
  struct curl_slist *sent_trailers;
  char chunk[8];
  size_t consumed;

  vectis_proxy_headers_init(&headers);
  assert(vectis_proxy_headers_add(&headers, "Trailer", "X-Trace") ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_upload_init(&upload, 8192u, 1, 0u, &headers, &error) ==
         VECTIS_OK);
  result = vectis_proxy_upload_feed(&upload, (const unsigned char *)wire,
                                    sizeof(wire) - 1u, &consumed);
  assert(result == VECTIS_PROXY_FRAME_COMPLETE);
  assert(consumed == sizeof(wire) - sizeof("GET /next"));
  assert(vectis_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) == 3u);
  assert(memcmp(chunk, "abc", 3u) == 0);
  assert(vectis_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) == 0u);
  assert(upload.trailers.count == 1u);
  assert(strcmp(upload.trailers.fields[0].name, "X-Trace") == 0);
  assert(strcmp(upload.trailers.fields[0].value, "done") == 0);
  sent_trailers = NULL;
  assert(vectis_proxy_upload_curl_trailers(&sent_trailers, &upload) ==
         CURL_TRAILERFUNC_OK);
  assert(sent_trailers != NULL &&
         strcmp(sent_trailers->data, "X-Trace: done") == 0);
  curl_slist_free_all(sent_trailers);
  vectis_proxy_upload_cleanup(&upload);

  assert(vectis_proxy_upload_init(&upload, 8192u, 1, 0u, &headers, &error) ==
         VECTIS_OK);
  result = vectis_proxy_upload_feed(
      &upload, (const unsigned char *)"0\r\nX-Other: bad\r\n\r\n", 19u,
      &consumed);
  assert(result == VECTIS_PROXY_FRAME_INVALID);
  vectis_proxy_upload_cleanup(&upload);
  vectis_proxy_headers_cleanup(&headers);
}

static void test_fixed_direct_receive(void) {
  vectis_proxy_upload_buffer upload;
  vectis_proxy_headers headers;
  vectis_error error;
  unsigned char *space;
  char chunk[8192];
  size_t capacity;

  vectis_proxy_headers_init(&headers);
  assert(vectis_proxy_upload_init(&upload, 8192u, 0, 8197u, &headers, &error) ==
         VECTIS_OK);
  space = vectis_proxy_upload_reserve_fixed(&upload, &capacity);
  assert(space != NULL && capacity == 8192u);
  memset(space, 'a', capacity);
  assert(vectis_proxy_upload_commit_fixed(&upload, capacity) ==
         VECTIS_PROXY_FRAME_MORE);
  assert(vectis_proxy_upload_full(&upload));
  assert(vectis_proxy_upload_reserve_fixed(&upload, &capacity) == NULL);
  assert(capacity == 0u);
  assert(vectis_proxy_upload_read(chunk, 1u, 4096u, &upload) == 4096u);
  space = vectis_proxy_upload_reserve_fixed(&upload, &capacity);
  assert(space != NULL && capacity == 5u);
  memset(space, 'b', capacity);
  assert(vectis_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) == 4096u);
  assert(memcmp(chunk, "aaaa", 4u) == 0);
  assert(vectis_proxy_upload_reserve_fixed(&upload, &capacity) == space);
  assert(capacity == 5u);
  assert(vectis_proxy_upload_commit_fixed(&upload, capacity) ==
         VECTIS_PROXY_FRAME_COMPLETE);
  assert(upload.complete);
  assert(vectis_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) == 5u);
  assert(memcmp(chunk, "bbbbb", 5u) == 0);
  assert(vectis_proxy_upload_read(chunk, 1u, sizeof(chunk), &upload) == 0u);
  vectis_proxy_upload_cleanup(&upload);
  vectis_proxy_headers_cleanup(&headers);
}

int main(void) {
  test_fixed_backpressure();
  test_fixed_direct_receive();
  test_chunked_trailers();
  return 0;
}
