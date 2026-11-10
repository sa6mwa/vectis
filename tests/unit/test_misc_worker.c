#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <cpkt/lz4.h>
#include <open62541/types_generated.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void diagnostics(void) {
  UA_DiagnosticInfo *root, *node, *cursor, copy;
  size_t i;
  root = UA_DiagnosticInfo_new();
  assert(root != NULL);
  node = root;
  for (i = 0u; i < 8192u; ++i) {
    node->hasAdditionalInfo = true;
    node->additionalInfo = UA_STRING_ALLOC("diagnostic");
    assert(node->additionalInfo.data != NULL);
    if (i + 1u < 8192u) {
      node->hasInnerDiagnosticInfo = true;
      node->innerDiagnosticInfo = UA_DiagnosticInfo_new();
      assert(node->innerDiagnosticInfo != NULL);
      node = node->innerDiagnosticInfo;
    }
  }
  assert(UA_DiagnosticInfo_copy(root, &copy) == UA_STATUSCODE_GOOD);
  node = root;
  cursor = &copy;
  for (i = 0u; i < 8192u; ++i) {
    assert(cursor != NULL && cursor != node);
    assert(cursor->additionalInfo.data != node->additionalInfo.data);
    assert(cursor->additionalInfo.length == 10u);
    assert(memcmp(cursor->additionalInfo.data, "diagnostic", 10u) == 0);
    node = node->innerDiagnosticInfo;
    cursor = cursor->innerDiagnosticInfo;
  }
  assert(cursor == NULL && node == NULL);
  UA_DiagnosticInfo_clear(&copy);
  UA_DiagnosticInfo_delete(root);
}

static void lz4(void) {
  const int size = 65536;
  char *input, *encoded, *decoded;
  int capacity, length, i;
  size_t frame, source_size, output_size, remaining;
  cpkt_lz4_LZ4F_dctx *context = NULL;
  input = malloc((size_t)size);
  decoded = malloc((size_t)size);
  capacity = cpkt_lz4_LZ4_compressBound(size);
  encoded = malloc((size_t)capacity + 128u);
  assert(input != NULL && decoded != NULL && encoded != NULL);
  for (i = 0; i < size; ++i)
    input[i] = (char)((i / 3 + i / 17) % 251);
  length = cpkt_lz4_LZ4_compress_default(input, encoded, size, capacity);
  assert(length > 0);
  assert(cpkt_lz4_LZ4_decompress_safe(encoded, decoded, length, size) == size);
  assert(memcmp(input, decoded, (size_t)size) == 0);
  assert(cpkt_lz4_LZ4_decompress_safe(encoded, decoded, length, 1) < 0);
  length = cpkt_lz4_LZ4_compress_HC(input, encoded, size, capacity,
                                    CPKT_LZ4_LZ4HC_CLEVEL_MAX);
  assert(length > 0);
  assert(cpkt_lz4_LZ4_decompress_safe(encoded, decoded, length, size) == size);
  assert(memcmp(input, decoded, (size_t)size) == 0);
  frame = cpkt_lz4_LZ4F_compressFrame(encoded, (size_t)capacity + 128u, input,
                                      (size_t)size, NULL);
  assert(!cpkt_lz4_LZ4F_isError(frame));
  assert(!cpkt_lz4_LZ4F_isError(cpkt_lz4_LZ4F_createDecompressionContext(
      &context, CPKT_LZ4_LZ4F_VERSION)));
  source_size = frame;
  output_size = (size_t)size;
  remaining = cpkt_lz4_LZ4F_decompress(context, decoded, &output_size, encoded,
                                       &source_size, NULL);
  assert(remaining == 0u && source_size == frame &&
         output_size == (size_t)size);
  assert(memcmp(input, decoded, (size_t)size) == 0);
  assert(
      !cpkt_lz4_LZ4F_isError(cpkt_lz4_LZ4F_freeDecompressionContext(context)));
  free(encoded);
  free(decoded);
  free(input);
}

static void *worker(void *unused) {
#if defined(__linux__)
  pthread_attr_t attributes;
  size_t size, guard;
  assert(pthread_getattr_np(pthread_self(), &attributes) == 0);
  assert(pthread_attr_getstacksize(&attributes, &size) == 0);
  assert(pthread_attr_getguardsize(&attributes, &guard) == 0 && guard > 0u);
  assert(pthread_attr_destroy(&attributes) == 0);
  printf("misc worker: %lu stack bytes, %lu guard bytes\n", (unsigned long)size,
         (unsigned long)guard);
#endif
  (void)unused;
  lz4();
  diagnostics();
  return NULL;
}

int main(void) {
  pthread_t thread;
  assert(pthread_create(&thread, NULL, worker, NULL) == 0);
  assert(pthread_join(thread, NULL) == 0);
  return 0;
}
