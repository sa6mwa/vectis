#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int fault_armed;
static int fault_remaining;
static size_t blocks;
static void *test_calloc(size_t count, size_t size) {
  void *ptr;
  if (fault_armed && fault_remaining-- == 0)
    return NULL;
  ptr = calloc(count, size);
  if (ptr)
    ++blocks;
  return ptr;
}
static void *test_realloc(void *ptr, size_t size) {
  void *next;
  if (fault_armed && fault_remaining-- == 0)
    return NULL;
  next = realloc(ptr, size);
  if (next && !ptr)
    ++blocks;
  return next;
}
static void test_free(void *ptr) {
  if (ptr)
    --blocks;
  free(ptr);
}
#define calloc test_calloc
#define realloc test_realloc
#define free test_free
#include "../../src/vectis_buffer.c"
#undef calloc
#undef realloc
#undef free

#include <vectis/buffer.h>
int main(void) {
  vectis_buffer *buffer;
  vectis_buffer_span *readonly, *mutable, *empty;
  vectis_buffer_span *child;
  vectis_error error;
  unsigned char bytes[8];
  const void *data;
  void *writable;
  size_t size;
  int fault;
  vectis_buffer_lock *first_lock, *second_lock;
  for (fault = 0; fault < 3; ++fault) {
    fault_armed = 1;
    fault_remaining = fault;
    assert(vectis_buffer_new(4u, &buffer, &error) == VECTIS_ERR_NOMEM &&
           buffer == NULL);
    fault_armed = 0;
    assert(blocks == 0u);
  }
  assert(vectis_buffer_new(4u, &buffer, &error) == VECTIS_OK);
  assert(buffer->size(buffer, &size, &error) == VECTIS_OK && size == 4u);
  assert(buffer->read(buffer, 0u, bytes, 4u, &error) == VECTIS_OK);
  assert(memcmp(bytes, "\0\0\0\0", 4u) == 0);
  assert(buffer->write(buffer, 0u, "a\0bc", 4u, &error) == VECTIS_OK);
  assert(buffer->data(buffer, &data, &size, &error) == VECTIS_OK &&
         size == 4u && memcmp(data, "a\0bc", 4u) == 0);
  assert(buffer->data_mutable(buffer, &writable, &size, &error) == VECTIS_OK &&
         size == 4u && writable == data);
  assert(buffer->read(buffer, 4u, NULL, 0u, &error) == VECTIS_OK);
  assert(buffer->read(buffer, SIZE_MAX, bytes, 1u, &error) ==
         VECTIS_ERR_INVALID);
  assert(buffer->read(buffer, 1u, bytes, SIZE_MAX, &error) ==
         VECTIS_ERR_INVALID);
  assert(buffer->resize(buffer, 8u, &error) == VECTIS_OK);
  assert(buffer->read(buffer, 0u, bytes, 8u, &error) == VECTIS_OK);
  assert(memcmp(bytes, "a\0bc\0\0\0\0", 8u) == 0);
  for (fault = 0; fault < 2; ++fault) {
    fault_armed = 1;
    fault_remaining = fault;
    assert(buffer->borrow(buffer, 0u, 1u, 1, &readonly, &error) ==
               VECTIS_ERR_NOMEM &&
           readonly == NULL);
    fault_armed = 0;
    assert(blocks == 3u);
  }
  fault_armed = 1;
  fault_remaining = 0;
  assert(buffer->resize(buffer, 16u, &error) == VECTIS_ERR_NOMEM);
  fault_armed = 0;
  assert(buffer->size(buffer, &size, &error) == VECTIS_OK && size == 8u);
  assert(buffer->read(buffer, 0u, bytes, 8u, &error) == VECTIS_OK &&
         memcmp(bytes, "a\0bc\0\0\0\0", 8u) == 0);
  ((buffer_control *)buffer->impl)->refs = SIZE_MAX;
  assert(buffer->borrow(buffer, 0u, 1u, 1, &readonly, &error) ==
             VECTIS_ERR_STATE &&
         readonly == NULL);
  ((buffer_control *)buffer->impl)->refs = 1u;
  assert(buffer->borrow(buffer, 1u, 3u, 1, &readonly, &error) == VECTIS_OK);
  assert(buffer->borrow(buffer, 0u, 4u, 0, &mutable, &error) == VECTIS_OK);
  fault_armed = 1;
  fault_remaining = 0;
  assert(vectis_buffer_span_lock_writes(mutable, &first_lock, &error) ==
             VECTIS_ERR_NOMEM &&
         !first_lock);
  fault_armed = 0;
  assert(vectis_buffer_span_lock_writes(mutable, &first_lock, &error) ==
         VECTIS_OK);
  assert(vectis_buffer_span_lock_writes(readonly, &second_lock, &error) ==
         VECTIS_OK);
  assert(mutable->write(mutable, 0, "x", 1, &error) == VECTIS_ERR_STATE);
  first_lock->close(first_lock);
  assert(mutable->data_mutable(mutable, &writable, &size, &error) ==
         VECTIS_ERR_STATE);
  second_lock->close(second_lock);
  assert(mutable->data_mutable(mutable, &writable, &size, &error) == VECTIS_OK);
  assert(buffer->resize(buffer, 8u, &error) == VECTIS_ERR_STATE);
  assert(buffer->data(buffer, &data, &size, &error) == VECTIS_OK && size == 8u);
  assert(buffer->data_mutable(buffer, &writable, &size, &error) ==
             VECTIS_ERR_STATE &&
         writable == NULL && size == 0u);
  assert(buffer->write(buffer, 0u, "x", 1u, &error) == VECTIS_ERR_STATE);
  assert(readonly->data_mutable(readonly, &writable, &size, &error) ==
             VECTIS_ERR_STATE &&
         writable == NULL && size == 0u);
  assert(readonly->write(readonly, 0u, "x", 1u, &error) == VECTIS_ERR_STATE);
  assert(mutable->data_mutable(mutable, &writable, &size, &error) ==
             VECTIS_OK &&
         size == 4u);
  assert(mutable->write(mutable, 2u, "Z", 1u, &error) == VECTIS_OK);
  assert(readonly->data(readonly, &data, &size, &error) == VECTIS_OK &&
         size == 3u);
  assert(memcmp(data, "\0Zc", 3u) == 0);
  assert(readonly->borrow(readonly, 0u, 1u, 0, &child, &error) ==
             VECTIS_ERR_STATE &&
         child == NULL);
  assert(mutable->borrow(mutable, 2u, 1u, 0, &child, &error) == VECTIS_OK);
  buffer->close(buffer);
  assert(readonly->read(readonly, 0u, bytes, 3u, &error) == VECTIS_OK);
  assert(memcmp(bytes, "\0Zc", 3u) == 0);
  mutable->close(mutable);
  assert(child->read(child, 0u, bytes, 1u, &error) == VECTIS_OK &&
         bytes[0] == 'Z');
  child->close(child);
  readonly->close(readonly);
  assert(vectis_buffer_new(0u, &buffer, &error) == VECTIS_OK);
  assert(buffer->data(buffer, &data, &size, &error) == VECTIS_OK &&
         data == NULL && size == 0u);
  assert(buffer->borrow(buffer, 0u, 0u, 1, &empty, &error) == VECTIS_OK);
  assert(empty->data(empty, &data, &size, &error) == VECTIS_OK &&
         data == NULL && size == 0u);
  empty->close(empty);
  assert(buffer->borrow(buffer, 0u, 0u, 2, &empty, &error) ==
             VECTIS_ERR_INVALID &&
         empty == NULL);
  assert(buffer->resize(buffer, 2u, &error) == VECTIS_OK);
  assert(buffer->resize(buffer, 1u, &error) == VECTIS_OK);
  assert(buffer->resize(buffer, 0u, &error) == VECTIS_OK);
  buffer->close(buffer);
  assert(vectis_buffer_span_size(NULL, &size, &error) == VECTIS_ERR_INVALID &&
         size == 0u);
  data = bytes;
  size = 1u;
  assert(vectis_buffer_data(NULL, &data, &size, &error) == VECTIS_ERR_INVALID &&
         data == NULL && size == 0u);
  vectis_buffer_close(NULL);
  vectis_buffer_span_close(NULL);
  assert(blocks == 0u);
  return 0;
}
