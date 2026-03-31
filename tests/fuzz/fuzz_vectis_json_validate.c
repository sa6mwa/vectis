#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vectis/vectis.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *buffer;
  vectis_error error;

  buffer = (char *)malloc(size + 1u);
  if (buffer == NULL) {
    return 0;
  }

  memcpy(buffer, data, size);
  buffer[size] = '\0';
  (void)vectis_json_validate_cstr(buffer, &error);
  free(buffer);
  return 0;
}

