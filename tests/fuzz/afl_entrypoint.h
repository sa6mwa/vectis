#ifndef VECTIS_AFL_ENTRYPOINT_H
#define VECTIS_AFL_ENTRYPOINT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

#ifdef VECTIS_AFL_FUZZER
int main(void) {
  unsigned char stack_buffer[4096];
  unsigned char *heap_buffer;
  unsigned char *buffer;
  size_t capacity;
  size_t size;
  size_t nread;
  int rc;

  buffer = stack_buffer;
  heap_buffer = NULL;
  capacity = sizeof(stack_buffer);
  size = 0u;
  for (;;) {
    if (size == capacity) {
      unsigned char *grown;
      size_t next_capacity;

      next_capacity = capacity * 2u;
      if (next_capacity <= capacity) {
        free(heap_buffer);
        return 1;
      }
      grown = (unsigned char *)malloc(next_capacity);
      if (grown == NULL) {
        free(heap_buffer);
        return 1;
      }
      memcpy(grown, buffer, size);
      free(heap_buffer);
      heap_buffer = grown;
      buffer = grown;
      capacity = next_capacity;
    }
    nread = fread(buffer + size, 1u, capacity - size, stdin);
    size += nread;
    if (nread == 0u) {
      if (ferror(stdin)) {
        free(heap_buffer);
        return 1;
      }
      break;
    }
  }
  rc = LLVMFuzzerTestOneInput(buffer, size);
  free(heap_buffer);
  return rc == 0 ? 0 : 1;
}
#endif

#endif
