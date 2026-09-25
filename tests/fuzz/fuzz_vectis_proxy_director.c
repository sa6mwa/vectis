#include "vectis_proxy_director.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fuzz_director_input {
  const uint8_t *data;
  size_t size;
  int failed_edit;
} fuzz_director_input;

static void fuzz_edit(vectis_proxy_outbound *out, char *line,
                      fuzz_director_input *input, vectis_error *error) {
  vectis_status status;
  char *value;
  char *separator;
  const char *field_value;
  unsigned number;

  value = line + 1;
  status = VECTIS_OK;
  switch (line[0]) {
  case 'T':
    number = (unsigned)(unsigned char)value[0];
    if (number >= (unsigned)'0' && number <= (unsigned)'9')
      number -= (unsigned)'0';
    status = vectis_proxy_outbound_select_target(out, number, error);
    break;
  case 'M':
    number = (unsigned)(unsigned char)value[0];
    if (number >= (unsigned)'0' && number <= (unsigned)'9')
      number -= (unsigned)'0';
    status = vectis_proxy_outbound_set_method(out, (vectis_http_method)number,
                                              error);
    break;
  case 'P':
    status = vectis_proxy_outbound_set_path(out, value, error);
    break;
  case 'Q':
    status = vectis_proxy_outbound_set_query(out, value, error);
    break;
  case 'q':
    status = vectis_proxy_outbound_set_query(out, NULL, error);
    break;
  case 'H':
    status = vectis_proxy_outbound_set_host(out, value, error);
    break;
  case 'h':
    status = vectis_proxy_outbound_set_host(out, NULL, error);
    break;
  case 'A':
  case 'S':
  case 'R':
    separator = strchr(value, ':');
    if (separator != NULL) {
      *separator++ = '\0';
      field_value = separator;
    } else {
      field_value = "";
    }
    if (line[0] == 'A')
      status = vectis_proxy_outbound_add_header(out, value, field_value, error);
    else if (line[0] == 'S')
      status = vectis_proxy_outbound_set_header(out, value, field_value, error);
    else
      status = vectis_proxy_outbound_remove_header(out, value, error);
    break;
  default:
    return;
  }
  if (status != VECTIS_OK)
    input->failed_edit = 1;
}

static vectis_status fuzz_rewrite(const vectis_proxy_inbound *in,
                                  vectis_proxy_outbound *out, void *userdata,
                                  vectis_error *error) {
  fuzz_director_input *input;
  char line[1025];
  size_t start;
  size_t end;
  size_t length;
  size_t edits;

  input = (fuzz_director_input *)userdata;
  assert(strcmp(vectis_proxy_inbound_path(in), "/seed") == 0);
  assert(strcmp(vectis_proxy_inbound_host(in), "client.test") == 0);
  assert(vectis_proxy_inbound_header_count(in) == 2u);
  start = 1u;
  for (edits = 0u; edits < 32u && start < input->size; ++edits) {
    end = start;
    while (end < input->size && input->data[end] != '\n')
      ++end;
    length = end - start;
    if (length != 0u && length < sizeof(line)) {
      memcpy(line, input->data + start, length);
      line[length] = '\0';
      fuzz_edit(out, line, input, error);
    }
    start = end < input->size ? end + 1u : input->size;
  }
  return VECTIS_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char primary[] = "https://primary.test/base";
  char alternate[] = "http://alternate.test/alt";
  char *targets[2];
  vectis_proxy_route_data route;
  vectis_proxy_headers inbound;
  vectis_proxy_headers sanitized;
  vectis_proxy_outbound out;
  fuzz_director_input input;
  vectis_error error;
  vectis_status status;
  char *target;
  char *authority;
  size_t i;

  if (size == 0u || size > 4096u)
    return 0;
  targets[0] = primary;
  targets[1] = alternate;
  memset(&route, 0, sizeof(route));
  route.targets = targets;
  route.target_count = 2u;
  route.rewrite = fuzz_rewrite;
  route.rewrite_userdata = &input;
  vectis_proxy_headers_init(&inbound);
  vectis_proxy_headers_init(&sanitized);
  assert(vectis_proxy_headers_add(&inbound, "Host", "client.test") ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_headers_add(&inbound, "X-Trace", "inbound") ==
         VECTIS_PROXY_HEADER_OK);
  assert(vectis_proxy_headers_add(&sanitized, "X-Trace", "inbound") ==
         VECTIS_PROXY_HEADER_OK);
  input.data = data;
  input.size = size;
  input.failed_edit = 0;
  target = NULL;
  authority = NULL;
  status = vectis_proxy_director_prepare(
      &route, VECTIS_HTTP_GET, "/seed", "a=1&a=2", "client.test",
      (data[0] & 1u) != 0u, NULL, &inbound, &sanitized, &out, &target,
      &authority, &error);
  if (input.failed_edit)
    assert(status != VECTIS_OK);
  if (status == VECTIS_OK) {
    assert(target != NULL && target[0] == '/');
    assert(authority != NULL && authority[0] != '\0');
    assert(out.failure == VECTIS_OK);
    assert(out.target_index < route.target_count);
    assert(out.headers.count <= VECTIS_PROXY_HEADER_COUNT_LIMIT);
    assert(out.headers.bytes <= VECTIS_PROXY_HEADER_BLOCK_LIMIT);
    for (i = 0u; i < out.headers.count; ++i)
      assert(vectis_proxy_request_header_editable(out.headers.fields[i].name));
  }
  assert(sanitized.count == 1u);
  assert(strcmp(sanitized.fields[0].value, "inbound") == 0);
  free(target);
  free(authority);
  vectis_proxy_director_cleanup(&out);
  vectis_proxy_headers_cleanup(&sanitized);
  vectis_proxy_headers_cleanup(&inbound);
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
