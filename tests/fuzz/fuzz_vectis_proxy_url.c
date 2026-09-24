#include "vectis_proxy_url.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char path[4096];
  char query[4096];
  char *target;
  char *authority;
  vectis_error error;
  size_t split;
  size_t query_size;
  vectis_status status;

  if (size == 0u || size > sizeof(path) - 1u) {
    return 0;
  }
  for (split = 0u; split < size && data[split] != '\n'; ++split) {
  }
  memcpy(path, data, split);
  path[split] = '\0';
  query_size = split < size ? size - split - 1u : 0u;
  memcpy(query, data + (split < size ? split + 1u : split), query_size);
  query[query_size] = '\0';
  target = NULL;
  authority = NULL;
  status = vectis_proxy_target_build("https://example.test/base", path,
                                     query, &target, &authority, &error);
  if (status == VECTIS_OK) {
    assert(target != NULL && target[0] == '/');
    assert(strcmp(authority, "example.test") == 0);
    assert(strchr(target, '\r') == NULL && strchr(target, '\n') == NULL);
  } else {
    assert(target == NULL && authority == NULL);
  }
  free(target);
  free(authority);
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
