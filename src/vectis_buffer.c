#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vectis/buffer.h>
typedef struct buffer_control {
  unsigned char *data;
  size_t size;
  size_t refs;
  size_t write_locks;
} buffer_control;
typedef struct buffer_span_impl {
  buffer_control *owner;
  size_t offset;
  size_t size;
  int readonly;
} buffer_span_impl;
static vectis_status buffer_error(vectis_error *error, vectis_status code,
                                  const char *message) {
  if (error) {
    memset(error, 0, sizeof(*error));
    error->code = code;
    error->source = VECTIS_ERROR_SOURCE_VECTIS;
    strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return code;
}
static void release(buffer_control *owner) {
  if (--owner->refs == 0u) {
    free(owner->data);
    free(owner);
  }
}
static vectis_status range(size_t size, size_t offset, size_t count,
                           const void *bytes, vectis_error *error) {
  if (offset > size || count > size - offset || (count && !bytes))
    return buffer_error(error, VECTIS_ERR_INVALID, "invalid buffer byte range");
  return VECTIS_OK;
}
vectis_status vectis_buffer_new(size_t size, vectis_buffer **out,
                                vectis_error *error) {
  vectis_buffer *buffer;
  buffer_control *owner;
  if (out)
    *out = NULL;
  if (!out)
    return buffer_error(error, VECTIS_ERR_INVALID, "buffer output is required");
  buffer = (vectis_buffer *)calloc(1u, sizeof(*buffer));
  owner = (buffer_control *)calloc(1u, sizeof(*owner));
  if (!buffer || !owner) {
    free(buffer);
    free(owner);
    return buffer_error(error, VECTIS_ERR_NOMEM, "buffer allocation failed");
  }
  if (size)
    owner->data = (unsigned char *)calloc(1u, size);
  if (size && !owner->data) {
    free(owner);
    free(buffer);
    return buffer_error(error, VECTIS_ERR_NOMEM,
                        "buffer bytes allocation failed");
  }
  owner->size = size;
  owner->refs = 1u;
  buffer->impl = owner;
  buffer->size = vectis_buffer_size;
  buffer->read = vectis_buffer_read;
  buffer->write = vectis_buffer_write;
  buffer->resize = vectis_buffer_resize;
  buffer->data = vectis_buffer_data;
  buffer->data_mutable = vectis_buffer_data_mutable;
  buffer->borrow = vectis_buffer_borrow;
  buffer->close = vectis_buffer_close;
  *out = buffer;
  return VECTIS_OK;
}
vectis_status vectis_buffer_size(vectis_buffer *buffer, size_t *out,
                                 vectis_error *error) {
  if (out)
    *out = 0u;
  if (!buffer || !buffer->impl || !out)
    return buffer_error(error, VECTIS_ERR_INVALID,
                        "buffer and size output are required");
  *out = ((buffer_control *)buffer->impl)->size;
  return VECTIS_OK;
}
vectis_status vectis_buffer_read(vectis_buffer *buffer, size_t offset,
                                 void *bytes, size_t count,
                                 vectis_error *error) {
  buffer_control *owner;
  if (!buffer || !buffer->impl)
    return buffer_error(error, VECTIS_ERR_INVALID, "buffer is required");
  owner = (buffer_control *)buffer->impl;
  if (range(owner->size, offset, count, bytes, error) != VECTIS_OK)
    return VECTIS_ERR_INVALID;
  if (count)
    memmove(bytes, owner->data + offset, count);
  return VECTIS_OK;
}
vectis_status vectis_buffer_write(vectis_buffer *buffer, size_t offset,
                                  const void *bytes, size_t count,
                                  vectis_error *error) {
  buffer_control *owner;
  if (!buffer || !buffer->impl)
    return buffer_error(error, VECTIS_ERR_INVALID, "buffer is required");
  owner = (buffer_control *)buffer->impl;
  if (owner->refs != 1u)
    return buffer_error(error, VECTIS_ERR_STATE, "buffer is borrowed");
  if (range(owner->size, offset, count, bytes, error) != VECTIS_OK)
    return VECTIS_ERR_INVALID;
  if (count)
    memmove(owner->data + offset, bytes, count);
  return VECTIS_OK;
}
vectis_status vectis_buffer_resize(vectis_buffer *buffer, size_t size,
                                   vectis_error *error) {
  buffer_control *owner;
  unsigned char *bytes;
  if (!buffer || !buffer->impl)
    return buffer_error(error, VECTIS_ERR_INVALID, "buffer is required");
  owner = (buffer_control *)buffer->impl;
  if (owner->refs != 1u)
    return buffer_error(error, VECTIS_ERR_STATE, "buffer is borrowed");
  if (size == owner->size)
    return VECTIS_OK;
  if (!size) {
    free(owner->data);
    owner->data = NULL;
    owner->size = 0u;
    return VECTIS_OK;
  }
  bytes = (unsigned char *)realloc(owner->data, size);
  if (!bytes)
    return buffer_error(error, VECTIS_ERR_NOMEM, "buffer resize failed");
  if (size > owner->size)
    memset(bytes + owner->size, 0, size - owner->size);
  owner->data = bytes;
  owner->size = size;
  return VECTIS_OK;
}
static vectis_status control_borrow(buffer_control *owner, size_t offset,
                                    size_t size, int readonly,
                                    vectis_buffer_span **out,
                                    vectis_error *error) {
  vectis_buffer_span *span;
  buffer_span_impl *impl;
  if (out)
    *out = NULL;
  if (!owner || !out || (readonly != 0 && readonly != 1))
    return buffer_error(error, VECTIS_ERR_INVALID, "invalid buffer borrow");
  if (offset > owner->size || size > owner->size - offset)
    return buffer_error(error, VECTIS_ERR_INVALID,
                        "invalid buffer borrow range");
  if (owner->refs == SIZE_MAX)
    return buffer_error(error, VECTIS_ERR_STATE, "buffer owner count overflow");
  span = (vectis_buffer_span *)calloc(1u, sizeof(*span));
  impl = (buffer_span_impl *)calloc(1u, sizeof(*impl));
  if (!span || !impl) {
    free(span);
    free(impl);
    return buffer_error(error, VECTIS_ERR_NOMEM,
                        "buffer span allocation failed");
  }
  ++owner->refs;
  impl->owner = owner;
  impl->offset = offset;
  impl->size = size;
  impl->readonly = readonly;
  span->impl = impl;
  span->borrow = vectis_buffer_span_borrow;
  span->size = vectis_buffer_span_size;
  span->read = vectis_buffer_span_read;
  span->write = vectis_buffer_span_write;
  span->data = vectis_buffer_span_data;
  span->data_mutable = vectis_buffer_span_data_mutable;
  span->close = vectis_buffer_span_close;
  *out = span;
  return VECTIS_OK;
}
vectis_status vectis_buffer_borrow(vectis_buffer *buffer, size_t offset,
                                   size_t size, int readonly,
                                   vectis_buffer_span **out,
                                   vectis_error *error) {
  if (out)
    *out = NULL;
  if (!buffer || !buffer->impl)
    return buffer_error(error, VECTIS_ERR_INVALID, "buffer is required");
  return control_borrow((buffer_control *)buffer->impl, offset, size, readonly,
                        out, error);
}
vectis_status vectis_buffer_span_borrow(vectis_buffer_span *source,
                                        size_t offset, size_t size,
                                        int readonly, vectis_buffer_span **out,
                                        vectis_error *error) {
  buffer_span_impl *impl;
  if (out)
    *out = NULL;
  if (!source || !source->impl || !out || (readonly != 0 && readonly != 1))
    return buffer_error(error, VECTIS_ERR_INVALID, "invalid span borrow");
  impl = (buffer_span_impl *)source->impl;
  if (offset > impl->size || size > impl->size - offset)
    return buffer_error(error, VECTIS_ERR_INVALID, "invalid span borrow range");
  if (impl->readonly && !readonly)
    return buffer_error(error, VECTIS_ERR_STATE, "span is readonly");
  return control_borrow(impl->owner, impl->offset + offset, size, readonly, out,
                        error);
}
void vectis_buffer_close(vectis_buffer *buffer) {
  if (!buffer)
    return;
  if (buffer->impl)
    release((buffer_control *)buffer->impl);
  free(buffer);
}

vectis_status vectis_buffer_data(vectis_buffer *buffer, const void **out,
                                 size_t *size, vectis_error *error) {
  buffer_control *owner;
  if (out)
    *out = NULL;
  if (size)
    *size = 0u;
  if (!buffer || !buffer->impl || !out || !size)
    return buffer_error(error, VECTIS_ERR_INVALID,
                        "buffer and data outputs are required");
  owner = (buffer_control *)buffer->impl;
  *out = owner->data;
  *size = owner->size;
  return VECTIS_OK;
}
vectis_status vectis_buffer_data_mutable(vectis_buffer *buffer, void **out,
                                         size_t *size, vectis_error *error) {
  buffer_control *owner;
  if (out)
    *out = NULL;
  if (size)
    *size = 0u;
  if (!buffer || !buffer->impl || !out || !size)
    return buffer_error(error, VECTIS_ERR_INVALID,
                        "buffer and data outputs are required");
  owner = (buffer_control *)buffer->impl;
  if (owner->refs != 1u)
    return buffer_error(error, VECTIS_ERR_STATE, "buffer is borrowed");
  *out = owner->data;
  *size = owner->size;
  return VECTIS_OK;
}
vectis_status vectis_buffer_span_size(vectis_buffer_span *span, size_t *out,
                                      vectis_error *error) {
  if (out)
    *out = 0u;
  if (!span || !span->impl || !out)
    return buffer_error(error, VECTIS_ERR_INVALID,
                        "span and size output are required");
  *out = ((buffer_span_impl *)span->impl)->size;
  return VECTIS_OK;
}
vectis_status vectis_buffer_span_read(vectis_buffer_span *span, size_t offset,
                                      void *bytes, size_t size,
                                      vectis_error *error) {
  buffer_span_impl *impl;
  if (!span || !span->impl)
    return buffer_error(error, VECTIS_ERR_INVALID, "span is required");
  impl = (buffer_span_impl *)span->impl;
  if (range(impl->size, offset, size, bytes, error) != VECTIS_OK)
    return VECTIS_ERR_INVALID;
  if (size)
    memmove(bytes, impl->owner->data + impl->offset + offset, size);
  return VECTIS_OK;
}
vectis_status vectis_buffer_span_write(vectis_buffer_span *span, size_t offset,
                                       const void *bytes, size_t size,
                                       vectis_error *error) {
  buffer_span_impl *impl;
  if (!span || !span->impl)
    return buffer_error(error, VECTIS_ERR_INVALID, "span is required");
  impl = (buffer_span_impl *)span->impl;
  if (impl->owner->write_locks)
    return buffer_error(error, VECTIS_ERR_STATE, "buffer mutation is locked");
  if (impl->readonly)
    return buffer_error(error, VECTIS_ERR_STATE, "buffer span is readonly");
  if (range(impl->size, offset, size, bytes, error) != VECTIS_OK)
    return VECTIS_ERR_INVALID;
  if (size)
    memmove(impl->owner->data + impl->offset + offset, bytes, size);
  return VECTIS_OK;
}
vectis_status vectis_buffer_span_data(vectis_buffer_span *span,
                                      const void **out, size_t *size,
                                      vectis_error *error) {
  buffer_span_impl *impl;
  if (out)
    *out = NULL;
  if (size)
    *size = 0u;
  if (!span || !span->impl || !out || !size)
    return buffer_error(error, VECTIS_ERR_INVALID,
                        "span and data outputs are required");
  impl = (buffer_span_impl *)span->impl;
  *out = impl->owner->data ? impl->owner->data + impl->offset : NULL;
  *size = impl->size;
  return VECTIS_OK;
}
vectis_status vectis_buffer_span_data_mutable(vectis_buffer_span *span,
                                              void **out, size_t *size,
                                              vectis_error *error) {
  buffer_span_impl *impl;
  if (out)
    *out = NULL;
  if (size)
    *size = 0u;
  if (!span || !span->impl || !out || !size)
    return buffer_error(error, VECTIS_ERR_INVALID,
                        "span and data outputs are required");
  impl = (buffer_span_impl *)span->impl;
  if (impl->owner->write_locks)
    return buffer_error(error, VECTIS_ERR_STATE, "buffer mutation is locked");
  if (impl->readonly)
    return buffer_error(error, VECTIS_ERR_STATE, "buffer span is readonly");
  *out = impl->owner->data ? impl->owner->data + impl->offset : NULL;
  *size = impl->size;
  return VECTIS_OK;
}
vectis_status vectis_buffer_span_lock_writes(vectis_buffer_span *span,
                                             vectis_buffer_lock **out,
                                             vectis_error *error) {
  buffer_control *control;
  vectis_buffer_lock *lock;
  if (out)
    *out = NULL;
  if (!span || !span->impl || !out)
    return buffer_error(error, VECTIS_ERR_INVALID, "span/lock output required");
  control = ((buffer_span_impl *)span->impl)->owner;
  if (control->refs == SIZE_MAX || control->write_locks == SIZE_MAX)
    return buffer_error(error, VECTIS_ERR_STATE, "buffer lock count exhausted");
  lock = (vectis_buffer_lock *)calloc(1, sizeof(*lock));
  if (!lock)
    return buffer_error(error, VECTIS_ERR_NOMEM,
                        "buffer lock allocation failed");
  ++control->refs;
  ++control->write_locks;
  lock->impl = control;
  lock->close = vectis_buffer_lock_close;
  *out = lock;
  return VECTIS_OK;
}
void vectis_buffer_lock_close(vectis_buffer_lock *lock) {
  buffer_control *control;
  if (!lock)
    return;
  control = (buffer_control *)lock->impl;
  if (control) {
    --control->write_locks;
    release(control);
  }
  free(lock);
}

void vectis_buffer_span_close(vectis_buffer_span *span) {
  buffer_span_impl *impl;
  if (!span)
    return;
  impl = (buffer_span_impl *)span->impl;
  if (impl) {
    release(impl->owner);
    free(impl);
  }
  free(span);
}
