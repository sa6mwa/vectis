#include "vectis_proxy_local.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static const int statuses[] = {200, 204, 205, 304, 401, 502, 503, 504};
  vectis_proxy_local_response response;
  vectis_error error;
  vectis_status result;
  char name[81];
  char value[193];
  size_t body_length;
  size_t offset;
  size_t name_length;
  size_t value_length;
  size_t step;
  size_t i;
  int status;

  if (size < 2u || size > 70000u)
    return 0;
  status = statuses[data[0] % (sizeof(statuses) / sizeof(statuses[0]))];
  if ((data[0] & 0x80u) != 0u)
    status = (int)((unsigned)data[0] * 256u + (unsigned)data[1]);
  body_length = (data[1] & 1u) != 0u ? size - 2u : (size_t)data[1];
  if (body_length > size - 2u)
    body_length = size - 2u;
  vectis_proxy_local_init(&response);
  result = vectis_proxy_local_respond(&response, status, data + 2u, body_length,
                                      &error);
  if (result == VECTIS_OK) {
    assert(response.status == status);
    assert(response.body_length == body_length);
    assert(response.body_length <= VECTIS_PROXY_LOCAL_BODY_LIMIT);
    if (body_length != 0u)
      assert(memcmp(response.body, data + 2u, body_length) == 0);
  } else {
    assert(response.failure != VECTIS_OK);
  }
  offset = 2u + body_length;
  for (step = 0u; step < 16u && size - offset >= 2u; ++step) {
    name_length = (size_t)(data[offset++] % sizeof(name));
    value_length = (size_t)(data[offset++] % sizeof(value));
    if (size - offset < name_length + value_length)
      break;
    memcpy(name, data + offset, name_length);
    name[name_length] = '\0';
    offset += name_length;
    memcpy(value, data + offset, value_length);
    value[value_length] = '\0';
    offset += value_length;
    (void)vectis_proxy_local_add_header(&response, name, value, &error);
  }
  assert(response.headers.count <= VECTIS_PROXY_HEADER_COUNT_LIMIT);
  assert(response.headers.bytes <= VECTIS_PROXY_HEADER_BLOCK_LIMIT);
  for (i = 0u; i < response.headers.count; ++i) {
    assert(
        vectis_proxy_response_header_editable(response.headers.fields[i].name));
    assert(strchr(response.headers.fields[i].name, '\r') == NULL);
    assert(strchr(response.headers.fields[i].name, '\n') == NULL);
    assert(strchr(response.headers.fields[i].value, '\r') == NULL);
    assert(strchr(response.headers.fields[i].value, '\n') == NULL);
  }
  vectis_proxy_local_cleanup(&response);
  return 0;
}

#if defined(VECTIS_AFL_FUZZER)
int main(void) {
  uint8_t data[70000];
  size_t size;

  size = fread(data, 1u, sizeof(data), stdin);
  (void)LLVMFuzzerTestOneInput(data, size);
  return 0;
}
#endif
