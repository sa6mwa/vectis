#include "vectis_proxy_upload.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUZZ_UPLOAD_CAPACITY 8192u
#define FUZZ_INPUT_LIMIT 65536u

static void fuzz_drain(vectis_proxy_upload_buffer *upload, const uint8_t *body,
                       size_t *read_total, size_t requested, int fixed) {
  char output[1024];
  size_t pending;
  size_t amount;

  pending = upload->end - upload->begin;
  if (requested > sizeof(output))
    requested = sizeof(output);
  amount = vectis_proxy_upload_read(output, 1u, requested, upload);
  if (pending == 0u) {
    assert(amount == (upload->complete ? 0u : CURL_READFUNC_PAUSE));
    return;
  }
  assert(amount != CURL_READFUNC_ABORT && amount != CURL_READFUNC_PAUSE);
  assert(amount > 0u && amount <= pending && amount <= requested);
  if (fixed)
    assert(memcmp(output, body + *read_total, amount) == 0);
  *read_total += amount;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  vectis_proxy_upload_buffer upload;
  vectis_proxy_headers headers;
  vectis_proxy_frame_result result;
  vectis_error error;
  struct curl_slist *trailers;
  unsigned char *space;
  const uint8_t *body;
  size_t body_length;
  size_t offset;
  size_t step;
  size_t capacity;
  size_t consumed;
  size_t read_total;
  size_t iterations;
  size_t drain_size;
  char byte;
  int chunked;
  int direct;

  if (size < 4u || size > FUZZ_INPUT_LIMIT)
    return 0;
  chunked = (data[0] & 1u) != 0u;
  direct = !chunked && (data[0] & 8u) != 0u;
  body = data + 4u;
  body_length = ((size_t)data[2] << 8u) | (size_t)data[3];
  step = (size_t)(data[1] % 97u) + 1u;
  drain_size = (size_t)(data[2] % 251u) + 1u;
  vectis_proxy_headers_init(&headers);
  if ((data[0] & 4u) != 0u)
    assert(vectis_proxy_headers_add(&headers, "Trailer", "X-Trace") ==
           VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_upload_init(&upload, FUZZ_UPLOAD_CAPACITY, chunked,
                                  (uint64_t)body_length, &headers,
                                  &error) == VECTIS_OK);
  assert(vectis_proxy_upload_read(&byte, 1u, 1u, &upload) ==
         (upload.complete ? 0u : CURL_READFUNC_PAUSE));
  offset = 0u;
  read_total = 0u;
  result =
      upload.complete ? VECTIS_PROXY_FRAME_COMPLETE : VECTIS_PROXY_FRAME_MORE;
  for (iterations = 0u;
       offset < size - 4u && iterations < FUZZ_INPUT_LIMIT * 2u &&
       result != VECTIS_PROXY_FRAME_COMPLETE &&
       result != VECTIS_PROXY_FRAME_INVALID;
       ++iterations) {
    consumed = 0u;
    if (direct) {
      space = vectis_proxy_upload_reserve_fixed(&upload, &capacity);
      if (space != NULL) {
        consumed = step;
        if (consumed > capacity)
          consumed = capacity;
        if (consumed > size - 4u - offset)
          consumed = size - 4u - offset;
        memcpy(space, body + offset, consumed);
        result = vectis_proxy_upload_commit_fixed(&upload, consumed);
        assert(result != VECTIS_PROXY_FRAME_INVALID);
      } else {
        result = VECTIS_PROXY_FRAME_PAUSED;
      }
    } else {
      size_t offered;

      offered = step;
      if (offered > size - 4u - offset)
        offered = size - 4u - offset;
      result =
          vectis_proxy_upload_feed(&upload, body + offset, offered, &consumed);
      assert(consumed <= offered);
    }
    offset += consumed;
    assert(upload.begin <= upload.end);
    assert(upload.end <= upload.capacity);
    if (vectis_proxy_upload_can_resume(&upload))
      vectis_proxy_upload_resumed(&upload);
    if (result == VECTIS_PROXY_FRAME_PAUSED || (data[0] & 2u) != 0u) {
      fuzz_drain(&upload, body, &read_total, drain_size, !chunked);
    }
    if (consumed == 0u && upload.begin == upload.end)
      break;
  }
  while (upload.begin != upload.end)
    fuzz_drain(&upload, body, &read_total, 1024u, !chunked);
  if (upload.complete) {
    assert(vectis_proxy_upload_read(&byte, 1u, 1u, &upload) == 0u);
    trailers = NULL;
    assert(vectis_proxy_upload_curl_trailers(&trailers, &upload) ==
           CURL_TRAILERFUNC_OK);
    curl_slist_free_all(trailers);
  }
  if (!chunked)
    assert(read_total <= body_length);
  vectis_proxy_upload_cleanup(&upload);
  vectis_proxy_headers_cleanup(&headers);
  return 0;
}

#if defined(VECTIS_AFL_FUZZER)
int main(void) {
  uint8_t data[FUZZ_INPUT_LIMIT];
  size_t size;

  size = fread(data, 1u, sizeof(data), stdin);
  (void)LLVMFuzzerTestOneInput(data, size);
  return 0;
}
#endif
