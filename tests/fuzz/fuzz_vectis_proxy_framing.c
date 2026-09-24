#include "vectis_proxy_framing.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct fuzz_sink {
  size_t body_bytes;
  size_t budget;
  size_t trailers;
} fuzz_sink;

static size_t fuzz_body(void *userdata, const unsigned char *data,
                        size_t length) {
  fuzz_sink *sink;

  sink = (fuzz_sink *)userdata;
  (void)data;
  if (length > sink->budget) {
    length = sink->budget;
  }
  sink->budget -= length;
  sink->body_bytes += length;
  return length;
}

static int fuzz_trailer(void *userdata, const char *name, const char *value) {
  fuzz_sink *sink;

  sink = (fuzz_sink *)userdata;
  assert(name[0] != '\0');
  (void)value;
  sink->trailers++;
  return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  vectis_proxy_body_framer framer;
  vectis_proxy_frame_result result;
  fuzz_sink sink;
  size_t offset;
  size_t step;
  size_t consumed;

  if (size < 2u) {
    return 0;
  }
  memset(&sink, 0, sizeof(sink));
  if (data[0] & 1u) {
    vectis_proxy_body_framer_chunked(&framer);
  } else {
    vectis_proxy_body_framer_fixed(&framer, (uint64_t)data[1]);
  }
  offset = 2u;
  result = VECTIS_PROXY_FRAME_MORE;
  while (offset < size && result != VECTIS_PROXY_FRAME_COMPLETE &&
         result != VECTIS_PROXY_FRAME_INVALID) {
    step = (size_t)(data[0] % 17u) + 1u;
    if (step > size - offset) {
      step = size - offset;
    }
    sink.budget = (size_t)(data[1] % 11u);
    result =
        vectis_proxy_body_framer_feed(&framer, data + offset, step, &consumed,
                                      fuzz_body, fuzz_trailer, &sink);
    assert(consumed <= step);
    offset += consumed;
    if (result == VECTIS_PROXY_FRAME_PAUSED && consumed == 0u) {
      sink.budget = step;
      result =
          vectis_proxy_body_framer_feed(&framer, data + offset, step, &consumed,
                                        fuzz_body, fuzz_trailer, &sink);
      assert(consumed <= step);
      offset += consumed;
      if (result == VECTIS_PROXY_FRAME_PAUSED && consumed == 0u) {
        break;
      }
    }
  }
  assert(sink.body_bytes <= size - 2u);
  return 0;
}

#if defined(VECTIS_AFL_FUZZER)
int main(void) {
  uint8_t data[32768];
  size_t size;

  size = fread(data, 1u, sizeof(data), stdin);
  (void)LLVMFuzzerTestOneInput(data, size);
  return 0;
}
#endif
