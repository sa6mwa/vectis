#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <vectis/zlib.h>
#include <vectis/zlib_raw.h>

typedef struct gzip_control {
  gz_header fields;
  vectis_buffer_span *buffers[3];
  size_t references;
  int read_mode;
} gzip_control;

typedef struct raw_impl {
  z_stream stream;
  int compress;
  int initialized;
  int last_end;
  int active;
  vectis_buffer_span *input;
  vectis_buffer_span *output;
  gzip_control *header;
  int back_mode;
  vectis_buffer_span *window;
} raw_impl;

static vectis_status raw_error(vectis_error *error, vectis_status status,
                               int native, const char *message) {
  if (error) {
    memset(error, 0, sizeof(*error));
    error->code = status;
    error->source = VECTIS_ERROR_SOURCE_VECTIS;
    error->dependency_code = native;
    strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return status;
}

static void gzip_release(gzip_control *control) {
  int index;
  if (!control || --control->references)
    return;
  for (index = 0; index < 3; ++index)
    vectis_buffer_span_close(control->buffers[index]);
  free(control);
}

void vectis_zlib_gzip_header_options_init(
    vectis_zlib_gzip_header_options *options) {
  if (options)
    memset(options, 0, sizeof(*options));
}

vectis_status vectis_zlib_gzip_header_native(vectis_zlib_gzip_header *header,
                                             gz_headerp *out,
                                             vectis_error *error) {
  if (out)
    *out = NULL;
  if (!header || !header->impl || !out)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "gzip header/output required");
  *out = &((gzip_control *)header->impl)->fields;
  return VECTIS_OK;
}

void vectis_zlib_gzip_header_close(vectis_zlib_gzip_header *header) {
  if (!header)
    return;
  gzip_release((gzip_control *)header->impl);
  free(header);
}

vectis_status
vectis_zlib_gzip_header_new(const vectis_zlib_gzip_header_options *options,
                            vectis_zlib_gzip_header **out,
                            vectis_error *error) {
  vectis_zlib_gzip_header *header;
  gzip_control *control;
  vectis_buffer_span *supplied[3];
  int index;
  size_t size, capacity;
  const void *bytes;
  void *writable;
  Bytef **pointer;
  vectis_status status;
  if (out)
    *out = NULL;
  if (!out || !options ||
      (options->read_mode != 0 && options->read_mode != 1) ||
      options->fields.extra || options->fields.name || options->fields.comment)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "gzip header requires typed byte fields");
  header = (vectis_zlib_gzip_header *)calloc(1, sizeof(*header));
  control = (gzip_control *)calloc(1, sizeof(*control));
  if (!header || !control) {
    free(header);
    free(control);
    return raw_error(error, VECTIS_ERR_NOMEM, Z_MEM_ERROR,
                     "gzip header allocation failed");
  }
  header->impl = control;
  header->native = vectis_zlib_gzip_header_native;
  header->io = vectis_zlib_gzip_header_io;
  header->close = vectis_zlib_gzip_header_close;
  control->references = 1;
  control->read_mode = options->read_mode;
  control->fields = options->fields;
  supplied[0] = options->extra;
  supplied[1] = options->name;
  supplied[2] = options->comment;
  for (index = 0; index < 3; ++index) {
    if (!supplied[index])
      continue;
    status = vectis_buffer_span_size(supplied[index], &size, error);
    if (status == VECTIS_OK)
      status = vectis_buffer_span_borrow(supplied[index], 0, size,
                                         !options->read_mode,
                                         &control->buffers[index], error);
    if (status != VECTIS_OK) {
      vectis_zlib_gzip_header_close(header);
      return status;
    }
    pointer = index == 0   ? &control->fields.extra
              : index == 1 ? &control->fields.name
                           : &control->fields.comment;
    if (options->read_mode) {
      status = vectis_buffer_span_data_mutable(control->buffers[index],
                                               &writable, &size, error);
      if (status == VECTIS_OK)
        *pointer = (Bytef *)writable;
      capacity = index == 0   ? control->fields.extra_max
                 : index == 1 ? control->fields.name_max
                              : control->fields.comm_max;
      if (status == VECTIS_OK && capacity > size)
        status = raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                           "gzip header capacity exceeds storage");
    } else {
      status = vectis_buffer_span_data(control->buffers[index], &bytes, &size,
                                       error);
      if (status == VECTIS_OK)
        memcpy(pointer, &bytes, sizeof(bytes));
      if (status == VECTIS_OK && index == 0 && control->fields.extra_len > size)
        status = raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                           "gzip extra length exceeds storage");
      if (status == VECTIS_OK && index != 0 &&
          (!size || !memchr(bytes, 0, size)))
        status = raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                           "gzip name/comment require a terminating zero");
    }
    if (status != VECTIS_OK) {
      vectis_zlib_gzip_header_close(header);
      return status;
    }
  }
  *out = header;
  return VECTIS_OK;
}

vectis_status vectis_zlib_gzip_header_io(vectis_zlib_gzip_header *header,
                                         int field, vectis_buffer_span **out,
                                         vectis_error *error) {
  gzip_control *control;
  const void *bytes;
  Bytef *pointer;
  size_t size, capacity;
  vectis_status status;
  if (out)
    *out = NULL;
  if (!header || !header->impl || !out || field < 0 || field > 2)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "invalid gzip byte field query");
  control = (gzip_control *)header->impl;
  pointer = field == 0   ? control->fields.extra
            : field == 1 ? control->fields.name
                         : control->fields.comment;
  if (!pointer)
    return VECTIS_OK;
  if (!control->buffers[field])
    return raw_error(error, VECTIS_ERR_STATE, Z_STREAM_ERROR,
                     "gzip byte field has no managed storage");
  status =
      vectis_buffer_span_data(control->buffers[field], &bytes, &size, error);
  if (status != VECTIS_OK)
    return status;
  if ((const void *)pointer != bytes)
    return raw_error(error, VECTIS_ERR_STATE, Z_STREAM_ERROR,
                     "gzip pointer left managed storage");
  if (field == 0) {
    capacity = control->fields.extra_len;
    if (control->read_mode && capacity > control->fields.extra_max)
      capacity = control->fields.extra_max;
  } else
    capacity = control->read_mode ? (field == 1 ? control->fields.name_max
                                                : control->fields.comm_max)
                                  : size;
  if (capacity > size)
    return raw_error(error, VECTIS_ERR_STATE, Z_STREAM_ERROR,
                     "gzip byte field exceeds retained storage");
  return vectis_buffer_span_borrow(control->buffers[field], 0, capacity,
                                   !control->read_mode, out, error);
}

static vectis_zlib_raw *raw_shell(void) {
  vectis_zlib_raw *receiver;
  raw_impl *impl;
  receiver = (vectis_zlib_raw *)calloc(1u, sizeof(*receiver));
  impl = (raw_impl *)calloc(1u, sizeof(*impl));
  if (!receiver || !impl) {
    free(receiver);
    free(impl);
    return NULL;
  }
  receiver->impl = impl;
  receiver->native = vectis_zlib_raw_native;
  receiver->process = vectis_zlib_raw_process;
  receiver->copy = vectis_zlib_raw_copy;
  receiver->bind = vectis_zlib_raw_bind;
  receiver->io = vectis_zlib_raw_io;
  receiver->availability = vectis_zlib_raw_availability;
  receiver->run = vectis_zlib_raw_run;
  receiver->params = vectis_zlib_raw_params;
  receiver->bound = vectis_zlib_raw_bound;
  receiver->header = vectis_zlib_raw_header;
  receiver->reset = vectis_zlib_raw_reset;
  receiver->back = vectis_zlib_raw_back;
  receiver->end = vectis_zlib_raw_end;
  receiver->close = vectis_zlib_raw_close;
  return receiver;
}

void vectis_zlib_raw_options_init(vectis_zlib_raw_options *options) {
  if (!options)
    return;
  memset(options, 0, sizeof(*options));
  options->compress = 1;
  options->extended = 1;
  options->level = Z_DEFAULT_COMPRESSION;
  options->method = Z_DEFLATED;
  options->window_bits = MAX_WBITS;
  options->memory_level = 8;
  options->strategy = Z_DEFAULT_STRATEGY;
  options->version = ZLIB_VERSION;
  options->stream_size = (int)sizeof(z_stream);
}

vectis_status vectis_zlib_raw_new(const vectis_zlib_raw_options *options,
                                  vectis_zlib_raw **out, vectis_error *error) {
  vectis_zlib_raw *receiver;
  raw_impl *impl;
  int code;
  if (out)
    *out = NULL;
  if (!out || !options || (options->compress != 0 && options->compress != 1) ||
      (options->extended != 0 && options->extended != 1) ||
      ((options->alloc == NULL) != (options->free == NULL)))
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "invalid raw zlib options");
  receiver = raw_shell();
  if (!receiver)
    return raw_error(error, VECTIS_ERR_NOMEM, Z_MEM_ERROR,
                     "raw zlib owner allocation failed");
  impl = (raw_impl *)receiver->impl;
  impl->compress = options->compress;
  impl->stream.zalloc = options->alloc;
  impl->stream.zfree = options->free;
  impl->stream.opaque = options->opaque;
  if (impl->compress)
    code = options->extended
               ? deflateInit2_(&impl->stream, options->level, options->method,
                               options->window_bits, options->memory_level,
                               options->strategy, options->version,
                               options->stream_size)
               : deflateInit_(&impl->stream, options->level, options->version,
                              options->stream_size);
  else
    code = options->extended
               ? inflateInit2_(&impl->stream, options->window_bits,
                               options->version, options->stream_size)
               : inflateInit_(&impl->stream, options->version,
                              options->stream_size);
  if (code != Z_OK) {
    free(impl);
    free(receiver);
    return raw_error(
        error, code == Z_MEM_ERROR ? VECTIS_ERR_NOMEM : VECTIS_ERR_INVALID,
        code, "native raw zlib initialization failed");
  }
  impl->initialized = 1;
  *out = receiver;
  return VECTIS_OK;
}

vectis_status vectis_zlib_raw_back_new(const vectis_zlib_raw_options *options,
                                       vectis_buffer_span *window,
                                       vectis_zlib_raw **out,
                                       vectis_error *error) {
  vectis_zlib_raw *receiver;
  raw_impl *impl;
  size_t bytes, size;
  void *data;
  int code;
  vectis_status status;
  if (out)
    *out = NULL;
  if (!out || !options || !window || options->window_bits < 8 ||
      options->window_bits > 15 ||
      ((options->alloc == NULL) != (options->free == NULL)))
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "invalid back-inflate options/window");
  bytes = (size_t)1u << options->window_bits;
  receiver = raw_shell();
  if (!receiver)
    return raw_error(error, VECTIS_ERR_NOMEM, Z_MEM_ERROR,
                     "back-inflate owner allocation failed");
  impl = (raw_impl *)receiver->impl;
  impl->back_mode = 1;
  status = vectis_buffer_span_borrow(window, 0, bytes, 0, &impl->window, error);
  if (status == VECTIS_OK)
    status = vectis_buffer_span_data_mutable(impl->window, &data, &size, error);
  if (status != VECTIS_OK) {
    vectis_buffer_span_close(impl->window);
    free(impl);
    free(receiver);
    return status;
  }
  impl->stream.zalloc = options->alloc;
  impl->stream.zfree = options->free;
  impl->stream.opaque = options->opaque;
  code = inflateBackInit_(&impl->stream, options->window_bits,
                          (unsigned char *)data, options->version,
                          options->stream_size);
  if (code != Z_OK) {
    vectis_buffer_span_close(impl->window);
    free(impl);
    free(receiver);
    return raw_error(
        error, code == Z_MEM_ERROR ? VECTIS_ERR_NOMEM : VECTIS_ERR_INVALID,
        code, "native back-inflate initialization failed");
  }
  impl->initialized = 1;
  *out = receiver;
  return VECTIS_OK;
}

typedef struct back_callbacks {
  in_func input;
  out_func output;
  void *input_user;
  void *output_user;
  vectis_zlib_back_result *result;
  int failed;
  const void *window;
  size_t window_size;
} back_callbacks;
static unsigned back_input(void *user, z_const unsigned char **out) {
  back_callbacks *callbacks = (back_callbacks *)user;
  unsigned size;
  size = callbacks->input(callbacks->input_user, out);
  if ((size && !*out) || size > UINT64_MAX - callbacks->result->supplied) {
    callbacks->failed = 1;
    *out = NULL;
    return 0;
  }
  if (size) {
    uintptr_t source = (uintptr_t)*out, window = (uintptr_t)callbacks->window;
    if ((source <= window && window - source < size) ||
        (window < source && source - window < callbacks->window_size)) {
      callbacks->failed = 1;
      *out = NULL;
      return 0;
    }
  }
  callbacks->result->supplied += size;
  return size;
}
static int back_output(void *user, unsigned char *data, unsigned size) {
  back_callbacks *callbacks = (back_callbacks *)user;
  int code;
  if (size > UINT64_MAX - callbacks->result->output) {
    callbacks->failed = 1;
    return 1;
  }
  code = callbacks->output(callbacks->output_user, data, size);
  if (!code)
    callbacks->result->output += size;
  return code;
}
vectis_status vectis_zlib_raw_back(vectis_zlib_raw *receiver, in_func input,
                                   void *input_user, out_func output,
                                   void *output_user,
                                   vectis_zlib_back_result *result,
                                   vectis_error *error) {
  z_streamp stream;
  raw_impl *impl;
  vectis_buffer_lock *lock = NULL;
  vectis_status status;
  back_callbacks callbacks;
  const void *window;
  size_t window_size;
  if (result)
    memset(result, 0, sizeof(*result));
  if (!result || !input || !output)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "back callbacks/result required");
  status = vectis_zlib_raw_native(receiver, &stream, error);
  if (status != VECTIS_OK)
    return status;
  impl = (raw_impl *)receiver->impl;
  if (!impl->back_mode)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "back-inflate state required");
  status = vectis_buffer_span_data(impl->window, &window, &window_size, error);
  if (status != VECTIS_OK)
    return status;
  if (stream->avail_in) {
    uintptr_t source = (uintptr_t)stream->next_in, base = (uintptr_t)window;
    if ((source <= base && base - source < stream->avail_in) ||
        (base < source && source - base < window_size))
      return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                       "back-inflate input overlaps window");
  }
  status = vectis_buffer_span_lock_writes(impl->window, &lock, error);
  if (status != VECTIS_OK)
    return status;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.input = input;
  callbacks.output = output;
  callbacks.input_user = input_user;
  callbacks.output_user = output_user;
  callbacks.result = result;
  callbacks.window = window;
  callbacks.window_size = window_size;
  result->supplied = stream->avail_in;
  impl->active = 1;
  result->zlib_status =
      inflateBack(stream, back_input, &callbacks, back_output, &callbacks);
  impl->active = 0;
  vectis_buffer_lock_close(lock);
  result->input_failed =
      result->zlib_status == Z_BUF_ERROR && stream->next_in == NULL;
  result->consumed = result->supplied - stream->avail_in;
  if (callbacks.failed)
    return raw_error(error, VECTIS_ERR_INVALID, result->zlib_status,
                     "invalid back-inflate callback bytes or count overflow");
  return VECTIS_OK;
}

vectis_status vectis_zlib_raw_native(vectis_zlib_raw *receiver, z_streamp *out,
                                     vectis_error *error) {
  raw_impl *impl;
  if (out)
    *out = NULL;
  if (!receiver || !receiver->impl || !out)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "raw zlib owner/output required");
  impl = (raw_impl *)receiver->impl;
  if (!impl->initialized)
    return raw_error(error, VECTIS_ERR_STATE, Z_STREAM_ERROR,
                     "raw zlib state ended");
  if (impl->active)
    return raw_error(error, VECTIS_ERR_STATE, Z_STREAM_ERROR,
                     "raw zlib state is active");
  *out = &impl->stream;
  return VECTIS_OK;
}

vectis_status vectis_zlib_raw_process(vectis_zlib_raw *receiver,
                                      const void *input, size_t input_size,
                                      void *output, size_t output_size,
                                      int flush, vectis_zlib_raw_step *result,
                                      vectis_error *error) {
  z_streamp stream;
  raw_impl *impl;
  vectis_status status;
  if (result)
    memset(result, 0, sizeof(*result));
  if (!result || (input_size && !input) || (output_size && !output) ||
      input_size > VECTIS_ZLIB_CHUNK_BYTES ||
      output_size > VECTIS_ZLIB_CHUNK_BYTES || input_size > UINT_MAX ||
      output_size > UINT_MAX)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "invalid raw zlib chunk buffers");
  if (input_size && output_size) {
    uintptr_t input_address = (uintptr_t)input,
              output_address = (uintptr_t)output;
    if ((input_address <= output_address &&
         output_address - input_address < input_size) ||
        (output_address < input_address &&
         input_address - output_address < output_size))
      return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                       "raw zlib buffers overlap");
  }
  status = vectis_zlib_raw_native(receiver, &stream, error);
  if (status != VECTIS_OK)
    return status;
  impl = (raw_impl *)receiver->impl;
  if (impl->back_mode)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "use back-inflate for this state");
  if (impl->input || impl->output)
    return raw_error(error, VECTIS_ERR_STATE, Z_STREAM_ERROR,
                     "unbind retained IO before borrowed processing");
  /* Legacy zlib headers omit const on next_in. Native IO borrows readonly
   * bytes; void/character pointer representations are compatible in C89. */
  memcpy(&stream->next_in, &input, sizeof(input));
  stream->avail_in = (uInt)input_size;
  stream->next_out = (Bytef *)output;
  stream->avail_out = (uInt)output_size;
  impl->active = 1;
  result->zlib_status =
      impl->compress ? deflate(stream, flush) : inflate(stream, flush);
  impl->active = 0;
  result->consumed = input_size - stream->avail_in;
  result->produced = output_size - stream->avail_out;
  stream->next_in = Z_NULL;
  stream->avail_in = 0;
  stream->next_out = Z_NULL;
  stream->avail_out = 0;
  return VECTIS_OK;
}

static vectis_status raw_cursor(raw_impl *impl, int input, size_t *offset,
                                size_t *remaining, vectis_error *error) {
  vectis_buffer_span *span = input ? impl->input : impl->output;
  const void *base;
  const Bytef *pointer = input ? impl->stream.next_in : impl->stream.next_out;
  uintptr_t address, begin;
  size_t capacity;
  vectis_status status;
  *offset = 0;
  *remaining = 0;
  if (!span) {
    if (pointer || (input ? impl->stream.avail_in : impl->stream.avail_out))
      return raw_error(error, VECTIS_ERR_STATE, Z_STREAM_ERROR,
                       "native IO has no retained typed lease");
    return VECTIS_OK;
  }
  status = vectis_buffer_span_data(span, &base, &capacity, error);
  if (status != VECTIS_OK)
    return status;
  address = (uintptr_t)pointer;
  begin = (uintptr_t)base;
  if (address < begin || address - begin > capacity)
    return raw_error(error, VECTIS_ERR_STATE, Z_STREAM_ERROR,
                     "native IO cursor left typed storage");
  *offset = (size_t)(address - begin);
  *remaining = capacity - *offset;
  if ((size_t)(input ? impl->stream.avail_in : impl->stream.avail_out) >
      *remaining)
    return raw_error(error, VECTIS_ERR_STATE, Z_STREAM_ERROR,
                     "native availability exceeds typed storage");
  return VECTIS_OK;
}

vectis_status vectis_zlib_raw_bind(vectis_zlib_raw *receiver, int input,
                                   vectis_buffer_span *span, size_t offset,
                                   size_t capacity, vectis_error *error) {
  raw_impl *impl;
  z_streamp stream;
  vectis_buffer_span *lease = NULL, **slot;
  const void *bytes = NULL;
  void *writable = NULL;
  size_t size;
  vectis_status status;
  if ((input != 0 && input != 1) || capacity > UINT_MAX ||
      (!span && (offset || capacity)))
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "invalid raw IO binding");
  status = vectis_zlib_raw_native(receiver, &stream, error);
  if (status != VECTIS_OK)
    return status;
  impl = (raw_impl *)receiver->impl;
  if (span) {
    status =
        vectis_buffer_span_borrow(span, offset, capacity, input, &lease, error);
    if (status == VECTIS_OK) {
      status = input ? vectis_buffer_span_data(lease, &bytes, &size, error)
                     : vectis_buffer_span_data_mutable(lease, &writable, &size,
                                                       error);
    }
    if (status != VECTIS_OK) {
      vectis_buffer_span_close(lease);
      return status;
    }
  }
  slot = input ? &impl->input : &impl->output;
  if (input) {
    memcpy(&stream->next_in, &bytes, sizeof(bytes));
    stream->avail_in = (uInt)capacity;
  } else {
    stream->next_out = (Bytef *)writable;
    stream->avail_out = (uInt)capacity;
  }
  vectis_buffer_span_close(*slot);
  *slot = lease;
  return VECTIS_OK;
}

vectis_status vectis_zlib_raw_io(vectis_zlib_raw *receiver, int input,
                                 vectis_buffer_span **out,
                                 vectis_error *error) {
  raw_impl *impl;
  z_streamp stream;
  size_t offset, remaining;
  vectis_status status;
  if (out)
    *out = NULL;
  if (!out || (input != 0 && input != 1))
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "invalid raw IO query");
  status = vectis_zlib_raw_native(receiver, &stream, error);
  if (status != VECTIS_OK)
    return status;
  impl = (raw_impl *)receiver->impl;
  status = raw_cursor(impl, input, &offset, &remaining, error);
  if (status != VECTIS_OK)
    return status;
  if (!(input ? impl->input : impl->output))
    return VECTIS_OK;
  return vectis_buffer_span_borrow(
      input ? impl->input : impl->output, offset,
      (size_t)(input ? stream->avail_in : stream->avail_out), input, out,
      error);
}

vectis_status vectis_zlib_raw_availability(vectis_zlib_raw *receiver, int input,
                                           size_t size, vectis_error *error) {
  raw_impl *impl;
  z_streamp stream;
  size_t offset, remaining;
  vectis_status status;
  if (size > UINT_MAX || (input != 0 && input != 1))
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "availability outside uInt");
  status = vectis_zlib_raw_native(receiver, &stream, error);
  if (status != VECTIS_OK)
    return status;
  impl = (raw_impl *)receiver->impl;
  status = raw_cursor(impl, input, &offset, &remaining, error);
  if (status != VECTIS_OK)
    return status;
  if (size > remaining)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "availability exceeds remaining typed storage");
  if (input)
    stream->avail_in = (uInt)size;
  else
    stream->avail_out = (uInt)size;
  return VECTIS_OK;
}

static vectis_status raw_bound_operation(vectis_zlib_raw *receiver,
                                         int parameters, int first, int second,
                                         vectis_zlib_raw_step *result,
                                         vectis_error *error) {
  raw_impl *impl;
  z_streamp stream;
  size_t offset, remaining;
  uInt input_before, output_before;
  vectis_status status;
  uintptr_t input_address, output_address;
  if (result)
    memset(result, 0, sizeof(*result));
  if (!result)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "raw result required");
  status = vectis_zlib_raw_native(receiver, &stream, error);
  if (status != VECTIS_OK)
    return status;
  impl = (raw_impl *)receiver->impl;
  if (impl->back_mode)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "bound inflate cannot use back state");
  status = raw_cursor(impl, 1, &offset, &remaining, error);
  if (status == VECTIS_OK)
    status = raw_cursor(impl, 0, &offset, &remaining, error);
  if (status != VECTIS_OK)
    return status;
  input_before = stream->avail_in;
  output_before = stream->avail_out;
  input_address = (uintptr_t)stream->next_in;
  output_address = (uintptr_t)stream->next_out;
  if (input_before && output_before &&
      ((input_address <= output_address &&
        output_address - input_address < input_before) ||
       (output_address < input_address &&
        input_address - output_address < output_before)))
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "raw bound IO overlaps");
  impl->active = 1;
  result->zlib_status =
      parameters
          ? (impl->compress ? deflateParams(stream, first, second)
                            : Z_STREAM_ERROR)
          : (impl->compress ? deflate(stream, first) : inflate(stream, first));
  impl->active = 0;
  result->consumed = (size_t)(input_before - stream->avail_in);
  result->produced = (size_t)(output_before - stream->avail_out);
  return VECTIS_OK;
}

vectis_status vectis_zlib_raw_run(vectis_zlib_raw *receiver, int flush,
                                  vectis_zlib_raw_step *result,
                                  vectis_error *error) {
  return raw_bound_operation(receiver, 0, flush, 0, result, error);
}

vectis_status vectis_zlib_raw_params(vectis_zlib_raw *receiver, int level,
                                     int strategy, vectis_zlib_raw_step *result,
                                     vectis_error *error) {
  return raw_bound_operation(receiver, 1, level, strategy, result, error);
}

vectis_status vectis_zlib_raw_bound(vectis_zlib_raw *receiver, uint64_t size,
                                    int sized, uint64_t *out,
                                    vectis_error *error) {
  z_streamp stream;
  raw_impl *impl;
  vectis_status status;
  if (out)
    *out = 0;
  if (!out || (sized != 0 && sized != 1) ||
      (sized ? size > (uint64_t)SIZE_MAX : size > (uint64_t)ULONG_MAX))
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "bound input outside native length domain");
  status = vectis_zlib_raw_native(receiver, &stream, error);
  if (status != VECTIS_OK)
    return status;
  impl = (raw_impl *)receiver->impl;
  if (!impl->compress)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "bound requires deflate state");
  *out = sized ? (uint64_t)deflateBound_z(stream, (z_size_t)size)
               : (uint64_t)deflateBound(stream, (uLong)size);
  return VECTIS_OK;
}

vectis_status vectis_zlib_raw_header(vectis_zlib_raw *receiver,
                                     vectis_zlib_gzip_header *header, int *code,
                                     vectis_error *error) {
  raw_impl *impl;
  gzip_control *control = NULL;
  z_streamp stream;
  vectis_status status;
  if (!code)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "gzip attachment result required");
  status = vectis_zlib_raw_native(receiver, &stream, error);
  if (status != VECTIS_OK)
    return status;
  impl = (raw_impl *)receiver->impl;
  if (impl->back_mode)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "back state has no gzip header");
  if (header) {
    control = (gzip_control *)header->impl;
    if (!control || control->read_mode == impl->compress)
      return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                       "gzip header read/write mode mismatch");
    if (control->references == SIZE_MAX)
      return raw_error(error, VECTIS_ERR_STATE, Z_STREAM_ERROR,
                       "gzip header reference count exhausted");
  } else if (!impl->compress)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "inflate header capture requires a header");
  *code = impl->compress
              ? deflateSetHeader(stream, control ? &control->fields : NULL)
              : inflateGetHeader(stream, &control->fields);
  if (*code != Z_OK)
    return VECTIS_OK;
  if (control)
    ++control->references;
  gzip_release(impl->header);
  impl->header = control;
  return VECTIS_OK;
}

vectis_status vectis_zlib_raw_reset(vectis_zlib_raw *receiver, int mode,
                                    int bits, int *code, vectis_error *error) {
  raw_impl *impl;
  z_streamp stream;
  vectis_status status;
  if (!code || mode < 0 || mode > 2)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "invalid raw reset arguments");
  status = vectis_zlib_raw_native(receiver, &stream, error);
  if (status != VECTIS_OK)
    return status;
  impl = (raw_impl *)receiver->impl;
  if (impl->back_mode) {
    *code = Z_STREAM_ERROR;
    return VECTIS_OK;
  }
  impl->active = 1;
  if (impl->compress)
    *code = mode == 0   ? deflateReset(stream)
            : mode == 1 ? deflateResetKeep(stream)
                        : Z_STREAM_ERROR;
  else
    *code = mode == 0   ? inflateReset(stream)
            : mode == 1 ? inflateResetKeep(stream)
                        : inflateReset2(stream, bits);
  impl->active = 0;
  if (*code == Z_OK && !impl->compress) {
    gzip_release(impl->header);
    impl->header = NULL;
  }
  return VECTIS_OK;
}

vectis_status vectis_zlib_raw_copy(vectis_zlib_raw *source,
                                   vectis_zlib_raw **out, vectis_error *error) {
  vectis_zlib_raw *copy;
  raw_impl *source_impl, *dest;
  z_streamp stream;
  vectis_status status;
  int code;
  if (out)
    *out = NULL;
  if (!out)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "raw zlib copy output required");
  status = vectis_zlib_raw_native(source, &stream, error);
  if (status != VECTIS_OK)
    return status;
  if (((raw_impl *)source->impl)->back_mode)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "native zlib does not provide back-state copy");
  copy = raw_shell();
  if (!copy)
    return raw_error(error, VECTIS_ERR_NOMEM, Z_MEM_ERROR,
                     "raw zlib copy owner allocation failed");
  source_impl = (raw_impl *)source->impl;
  dest = (raw_impl *)copy->impl;
  dest->compress = source_impl->compress;
  source_impl->active = 1;
  code = dest->compress ? deflateCopy(&dest->stream, stream)
                        : inflateCopy(&dest->stream, stream);
  source_impl->active = 0;
  if (code != Z_OK) {
    free(dest);
    free(copy);
    return raw_error(error,
                     code == Z_MEM_ERROR ? VECTIS_ERR_NOMEM : VECTIS_ERR_STATE,
                     code, "native raw zlib copy failed");
  }
  dest->initialized = 1;
  if (source_impl->header) {
    if (source_impl->header->references == SIZE_MAX) {
      (void)vectis_zlib_raw_close(copy, NULL);
      return raw_error(error, VECTIS_ERR_STATE, Z_STREAM_ERROR,
                       "gzip header reference count exhausted");
    }
    ++source_impl->header->references;
    dest->header = source_impl->header;
  }
  if (source_impl->input) {
    size_t size;
    status = vectis_buffer_span_size(source_impl->input, &size, error);
    if (status == VECTIS_OK)
      status = vectis_buffer_span_borrow(source_impl->input, 0, size, 1,
                                         &dest->input, error);
    if (status != VECTIS_OK) {
      (void)vectis_zlib_raw_close(copy, NULL);
      return status;
    }
  }
  if (source_impl->output) {
    size_t size;
    status = vectis_buffer_span_size(source_impl->output, &size, error);
    if (status == VECTIS_OK)
      status = vectis_buffer_span_borrow(source_impl->output, 0, size, 0,
                                         &dest->output, error);
    if (status != VECTIS_OK) {
      (void)vectis_zlib_raw_close(copy, NULL);
      return status;
    }
  }
  *out = copy;
  return VECTIS_OK;
}

vectis_status vectis_zlib_raw_end(vectis_zlib_raw *receiver, int *code,
                                  vectis_error *error) {
  raw_impl *impl;
  if (!receiver || !receiver->impl || !code)
    return raw_error(error, VECTIS_ERR_INVALID, Z_STREAM_ERROR,
                     "raw zlib owner/end result required");
  impl = (raw_impl *)receiver->impl;
  if (impl->active)
    return raw_error(error, VECTIS_ERR_STATE, Z_STREAM_ERROR,
                     "cannot end active raw zlib state");
  if (impl->initialized) {
    impl->active = 1;
    impl->last_end = impl->back_mode  ? inflateBackEnd(&impl->stream)
                     : impl->compress ? deflateEnd(&impl->stream)
                                      : inflateEnd(&impl->stream);
    impl->active = 0;
    impl->initialized = 0;
    impl->stream.next_in = Z_NULL;
    impl->stream.avail_in = 0;
    impl->stream.next_out = Z_NULL;
    impl->stream.avail_out = 0;
    vectis_buffer_span_close(impl->input);
    impl->input = NULL;
    vectis_buffer_span_close(impl->output);
    impl->output = NULL;
    gzip_release(impl->header);
    impl->header = NULL;
    vectis_buffer_span_close(impl->window);
    impl->window = NULL;
  }
  *code = impl->last_end;
  return VECTIS_OK;
}

vectis_status vectis_zlib_raw_close(vectis_zlib_raw *receiver,
                                    vectis_error *error) {
  int code;
  vectis_status status;
  if (!receiver)
    return VECTIS_OK;
  status = vectis_zlib_raw_end(receiver, &code, error);
  if (status != VECTIS_OK)
    return status;
  free(receiver->impl);
  free(receiver);
  return VECTIS_OK;
}

vectis_status vectis_zlib_buffer_transform(int operation, int size_width,
                                           vectis_buffer_span *destination,
                                           vectis_buffer_span *source,
                                           int level,
                                           vectis_zlib_raw_step *result,
                                           vectis_error *error) {
  const void *input = NULL;
  void *output;
  size_t input_size = 0, output_size;
  uLong input_long;
  uLongf output_long;
  z_size_t input_z, output_z;
  uintptr_t a, b;
  vectis_status status;
  if (result)
    memset(result, 0, sizeof(*result));
  if (!result || !destination || operation < 0 || operation > 3 ||
      (size_width != 0 && size_width != 1))
    return raw_error(error, VECTIS_ERR_INVALID, 0,
                     "invalid buffer transform arguments");
  if (source) {
    status = vectis_buffer_span_data(source, &input, &input_size, error);
    if (status != VECTIS_OK)
      return status;
  }
  status = vectis_buffer_span_data_mutable(destination, &output, &output_size,
                                           error);
  if (status != VECTIS_OK)
    return status;
  if (!size_width && ((uint64_t)input_size > (uint64_t)ULONG_MAX ||
                      (uint64_t)output_size > (uint64_t)ULONG_MAX))
    return raw_error(error, VECTIS_ERR_INVALID, 0,
                     "buffer capacity outside uLong domain");
  a = (uintptr_t)input;
  b = (uintptr_t)output;
  if (input_size && output_size &&
      (a <= b ? b - a < input_size : a - b < output_size))
    return raw_error(error, VECTIS_ERR_INVALID, 0,
                     "buffer transform ranges overlap");
  if (size_width) {
    input_z = input_size;
    output_z = output_size;
    switch (operation) {
    case 0:
      result->zlib_status = compress_z(output, &output_z, input, input_z);
      break;
    case 1:
      result->zlib_status =
          compress2_z(output, &output_z, input, input_z, level);
      break;
    case 2:
      result->zlib_status = uncompress_z(output, &output_z, input, input_z);
      break;
    default:
      result->zlib_status = uncompress2_z(output, &output_z, input, &input_z);
      break;
    }
    result->produced = output_z;
    if (operation == 3)
      result->consumed = input_z;
  } else {
    input_long = (uLong)input_size;
    output_long = (uLongf)output_size;
    switch (operation) {
    case 0:
      result->zlib_status = compress(output, &output_long, input, input_long);
      break;
    case 1:
      result->zlib_status =
          compress2(output, &output_long, input, input_long, level);
      break;
    case 2:
      result->zlib_status = uncompress(output, &output_long, input, input_long);
      break;
    default:
      result->zlib_status =
          uncompress2(output, &output_long, input, &input_long);
      break;
    }
    result->produced = (size_t)output_long;
    if (operation == 3)
      result->consumed = (size_t)input_long;
  }
  return VECTIS_OK;
}

vectis_status vectis_zlib_buffer_bound(uint64_t input, int size_width,
                                       uint64_t *out, vectis_error *error) {
  if (out)
    *out = 0;
  if (!out || (size_width != 0 && size_width != 1) ||
      input > (uint64_t)(size_width ? SIZE_MAX : ULONG_MAX))
    return raw_error(error, VECTIS_ERR_INVALID, 0,
                     "invalid native compression bound arguments");
  *out = size_width ? (uint64_t)compressBound_z((z_size_t)input)
                    : (uint64_t)compressBound((uLong)input);
  return VECTIS_OK;
}

vectis_status vectis_zlib_raw_checksum(int kind, int size_width, uLong seed,
                                       const void *data, size_t size,
                                       uLong *out, vectis_error *error) {
  if (out)
    *out = 0;
  if (!out || (kind != VECTIS_ZLIB_ADLER32 && kind != VECTIS_ZLIB_CRC32) ||
      (size_width != 0 && size_width != 1) || (!data && size) ||
      (!size_width && size > UINT_MAX))
    return raw_error(error, VECTIS_ERR_INVALID, 0,
                     "invalid native checksum arguments");
  if (size_width)
    *out = kind == VECTIS_ZLIB_CRC32 ? crc32_z(seed, data, size)
                                     : adler32_z(seed, data, size);
  else
    *out = kind == VECTIS_ZLIB_CRC32 ? crc32(seed, data, (uInt)size)
                                     : adler32(seed, data, (uInt)size);
  return VECTIS_OK;
}

static int raw_offset_length(int large, uint64_t length) {
  if ((large != 0 && large != 1) || length > (uint64_t)INT64_MAX)
    return 0;
#ifdef Z_LARGE64
  if (large)
    return sizeof(z_off64_t) >= sizeof(int64_t) || length <= INT32_MAX;
#else
  if (large && sizeof(z_off_t) < sizeof(int64_t))
    return 0;
#endif
  return sizeof(z_off_t) >= sizeof(int64_t) || length <= INT32_MAX;
}
vectis_status vectis_zlib_raw_checksum_combine(int kind, int large, uLong first,
                                               uLong second, uint64_t length,
                                               uLong *out,
                                               vectis_error *error) {
  if (out)
    *out = 0;
  if (!out || !raw_offset_length(large, length) ||
      (kind != VECTIS_ZLIB_ADLER32 && kind != VECTIS_ZLIB_CRC32))
    return raw_error(error, VECTIS_ERR_INVALID, 0,
                     "invalid native checksum combine arguments");
#ifdef Z_LARGE64
  if (large) {
    *out = kind == VECTIS_ZLIB_CRC32
               ? crc32_combine64(first, second, (z_off64_t)length)
               : adler32_combine64(first, second, (z_off64_t)length);
    return VECTIS_OK;
  }
#endif
  *out = kind == VECTIS_ZLIB_CRC32
             ? crc32_combine(first, second, (z_off_t)length)
             : adler32_combine(first, second, (z_off_t)length);
  return VECTIS_OK;
}
vectis_status vectis_zlib_raw_crc_gen(int large, uint64_t length, uLong *out,
                                      vectis_error *error) {
  if (out)
    *out = 0;
  if (!out || !raw_offset_length(large, length))
    return raw_error(error, VECTIS_ERR_INVALID, 0,
                     "invalid native CRC operator length");
#ifdef Z_LARGE64
  if (large) {
    *out = crc32_combine_gen64((z_off64_t)length);
    return VECTIS_OK;
  }
#endif
  *out = crc32_combine_gen((z_off_t)length);
  return VECTIS_OK;
}
vectis_status vectis_zlib_raw_crc_op(uLong first, uLong second, uLong operation,
                                     uLong *out, vectis_error *error) {
  if (out)
    *out = 0;
  if (!out || (operation && !(operation & 0xffffffffUL)))
    return raw_error(error, VECTIS_ERR_INVALID, 0,
                     "CRC operator has only out-of-polynomial bits");
  *out = crc32_combine_op(first, second, operation);
  return VECTIS_OK;
}

typedef struct owned_crc_table {
  vectis_zlib_crc_table receiver;
  const z_crc_t *values;
} owned_crc_table;
vectis_status vectis_zlib_crc_table_native(vectis_zlib_crc_table *table,
                                           const z_crc_t **out,
                                           vectis_error *error) {
  if (out)
    *out = NULL;
  if (!table || !table->impl || !out)
    return raw_error(error, VECTIS_ERR_INVALID, 0,
                     "invalid CRC table receiver");
  *out = *(const z_crc_t **)table->impl;
  return VECTIS_OK;
}
vectis_status vectis_zlib_crc_table_size(vectis_zlib_crc_table *table,
                                         size_t *out, vectis_error *error) {
  const z_crc_t *values;
  vectis_status status;
  if (!out)
    return raw_error(error, VECTIS_ERR_INVALID, 0,
                     "CRC table size result is required");
  *out = 0;
  status = vectis_zlib_crc_table_native(table, &values, error);
  if (status == VECTIS_OK)
    *out = 256;
  return status;
}
vectis_status vectis_zlib_crc_table_entry(vectis_zlib_crc_table *table,
                                          size_t index, z_crc_t *out,
                                          vectis_error *error) {
  const z_crc_t *values;
  vectis_status status;
  if (out)
    *out = 0;
  if (!out || index >= 256)
    return raw_error(error, VECTIS_ERR_INVALID, 0,
                     "CRC table index must be 0..255");
  status = vectis_zlib_crc_table_native(table, &values, error);
  if (status == VECTIS_OK)
    *out = values[index];
  return status;
}
void vectis_zlib_crc_table_close(vectis_zlib_crc_table *table) { free(table); }
vectis_status vectis_zlib_crc_table_new(vectis_zlib_crc_table **out,
                                        vectis_error *error) {
  owned_crc_table *table;
  if (!out)
    return raw_error(error, VECTIS_ERR_INVALID, 0,
                     "CRC table result is required");
  *out = NULL;
  table = (owned_crc_table *)calloc(1, sizeof(*table));
  if (!table)
    return raw_error(error, VECTIS_ERR_NOMEM, 0,
                     "cannot allocate CRC table receiver");
  table->values = get_crc_table();
  table->receiver.impl = &table->values;
  table->receiver.size = vectis_zlib_crc_table_size;
  table->receiver.entry = vectis_zlib_crc_table_entry;
  table->receiver.native = vectis_zlib_crc_table_native;
  table->receiver.close = vectis_zlib_crc_table_close;
  *out = &table->receiver;
  return VECTIS_OK;
}
