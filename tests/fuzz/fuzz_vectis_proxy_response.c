#include "vectis_proxy_response.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct fuzz_response_input {
  const uint8_t *data;
  size_t size;
  int failed;
} fuzz_response_input;

static vectis_status fuzz_modify(vectis_proxy_response *response,
                                 void *userdata, vectis_error *error) {
  fuzz_response_input *input;
  vectis_status result;
  const char *name;
  const char *value;
  char raw_name[81];
  char raw_value[193];
  size_t pos;
  size_t name_length;
  size_t value_length;
  size_t step;
  unsigned operation;
  int status;

  input = (fuzz_response_input *)userdata;
  pos = 1u;
  for (step = 0u; step < 32u && pos < input->size; ++step) {
    operation = (unsigned)(input->data[pos++] % 7u);
    if (operation == 0u) {
      if (input->size - pos < 2u)
        break;
      status = (int)(((unsigned)input->data[pos] << 8u) |
                     (unsigned)input->data[pos + 1u]);
      pos += 2u;
      result = vectis_proxy_response_set_status(response, status, error);
    } else {
      if (input->size - pos < 2u)
        break;
      name_length = (size_t)(input->data[pos++] % sizeof(raw_name));
      value_length = (size_t)(input->data[pos++] % sizeof(raw_value));
      if (input->size - pos < name_length + value_length)
        break;
      memcpy(raw_name, input->data + pos, name_length);
      raw_name[name_length] = '\0';
      pos += name_length;
      memcpy(raw_value, input->data + pos, value_length);
      raw_value[value_length] = '\0';
      pos += value_length;
      name = operation == 4u   ? "Content-Length"
             : operation == 5u ? "X-Forwarded-For"
             : operation == 6u ? "X-Fuzz"
                               : raw_name;
      value = raw_value;
      if (operation == 2u)
        result = vectis_proxy_response_set_header(response, name, value, error);
      else if (operation == 3u)
        result = vectis_proxy_response_remove_header(response, name, error);
      else
        result = vectis_proxy_response_add_header(response, name, value, error);
    }
    if (result != VECTIS_OK)
      input->failed = 1;
  }
  return VECTIS_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static const int statuses[] = {200, 204, 205, 304};
  vectis_proxy_http_response upstream;
  vectis_proxy_headers sanitized;
  fuzz_response_input input;
  vectis_error error;
  vectis_status result;
  const char *name;
  const char *value;
  size_t i;
  int downstream_status;
  int body_allowed;

  if (size == 0u || size > 4096u)
    return 0;
  memset(&upstream, 0, sizeof(upstream));
  upstream.status = statuses[data[0] % 4u];
  upstream.head_request = (data[0] & 4u) != 0u;
  upstream.body_allowed = !upstream.head_request && upstream.status == 200;
  vectis_proxy_headers_init(&sanitized);
  assert(vectis_proxy_headers_add(&sanitized, "X-Seed", "present") ==
         VECTIS_PROXY_HEADER_OK);
  input.data = data;
  input.size = size;
  input.failed = 0;
  downstream_status = 0;
  result = vectis_proxy_response_apply(&upstream, &sanitized, fuzz_modify,
                                       &input, &downstream_status, &error);
  if (input.failed)
    assert(result != VECTIS_OK);
  else
    assert(result == VECTIS_OK);
  if (result == VECTIS_OK) {
    assert(downstream_status >= 200 && downstream_status <= 599);
    body_allowed = !upstream.head_request && downstream_status != 204 &&
                   downstream_status != 205 && downstream_status != 304;
    assert(body_allowed == upstream.body_allowed);
  }
  assert(sanitized.count <= VECTIS_PROXY_HEADER_COUNT_LIMIT);
  assert(sanitized.bytes <= VECTIS_PROXY_HEADER_BLOCK_LIMIT);
  for (i = 0u; i < sanitized.count; ++i) {
    name = sanitized.fields[i].name;
    value = sanitized.fields[i].value;
    assert(vectis_proxy_response_header_editable(name));
    assert(strchr(name, '\r') == NULL && strchr(name, '\n') == NULL);
    assert(strchr(value, '\r') == NULL && strchr(value, '\n') == NULL);
  }
  vectis_proxy_headers_cleanup(&sanitized);
  return 0;
}

#if defined(VECTIS_AFL_FUZZER)
int main(void) {
  uint8_t data[4096];
  size_t size;

  size = fread(data, 1u, sizeof(data), stdin);
  (void)LLVMFuzzerTestOneInput(data, size);
  return 0;
}
#endif
