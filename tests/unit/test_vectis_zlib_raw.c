#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <vectis/zlib_raw.h>

static void test_buffer_helpers(void) {
  vectis_buffer *input, *output, *plain;
  vectis_buffer_span *in, *out, *decoded, *compressed, *small, *readonly;
  vectis_buffer_lock *lock;
  vectis_zlib_raw_step result;
  vectis_error error;
  vectis_zlib_crc_table *table, *other;
  const z_crc_t *values;
  z_crc_t entry;
  char bytes[9];
  size_t size;
  uLong checksum, combined, operator_value;
  uint64_t bound;
  int operation, width, kind;
  assert(vectis_buffer_new(9, &input, &error) == VECTIS_OK);
  assert(vectis_buffer_write(input, 0, "123456789", 9, &error) == VECTIS_OK);
  assert(vectis_buffer_new(128, &output, &error) == VECTIS_OK);
  assert(vectis_buffer_new(9, &plain, &error) == VECTIS_OK);
  assert(input->borrow(input, 0, 9, 1, &in, &error) == VECTIS_OK);
  assert(output->borrow(output, 0, 128, 0, &out, &error) == VECTIS_OK);
  assert(plain->borrow(plain, 0, 9, 0, &decoded, &error) == VECTIS_OK);
  for (width = 0; width < 2; ++width) {
    assert(vectis_zlib_buffer_bound(9, width, &bound, &error) == VECTIS_OK);
    assert(bound == (uint64_t)(width ? compressBound_z(9) : compressBound(9)));
    assert(vectis_zlib_buffer_bound(width ? SIZE_MAX : ULONG_MAX, width, &bound,
                                    &error) == VECTIS_OK);
    assert(bound == (width ? (uint64_t)compressBound_z(SIZE_MAX)
                           : (uint64_t)compressBound(ULONG_MAX)));
    for (operation = 0; operation < 2; ++operation) {
      assert(vectis_zlib_buffer_transform(operation, width, out, in, 6, &result,
                                          &error) == VECTIS_OK);
      assert(result.zlib_status == Z_OK && result.produced > 0);
      assert(out->borrow(out, 0, result.produced, 1, &compressed, &error) ==
             VECTIS_OK);
      assert(vectis_zlib_buffer_transform(2, width, decoded, compressed, 0,
                                          &result, &error) == VECTIS_OK);
      assert(result.zlib_status == Z_OK && result.produced == 9);
      assert(decoded->read(decoded, 0, bytes, 9, &error) == VECTIS_OK &&
             memcmp(bytes, "123456789", 9) == 0);
      assert(vectis_zlib_buffer_transform(3, width, decoded, compressed, 0,
                                          &result, &error) == VECTIS_OK);
      assert(result.zlib_status == Z_OK && result.consumed > 0 &&
             result.produced == 9);
      compressed->close(compressed);
    }
    for (kind = 0; kind < 2; ++kind) {
      assert(vectis_zlib_raw_checksum(kind, width, ULONG_MAX, NULL, 0,
                                      &checksum, &error) == VECTIS_OK);
      assert(checksum == (kind == 0 ? 1u : 0u));
      assert(vectis_zlib_raw_checksum(kind, width, 123, "", 0, &checksum,
                                      &error) == VECTIS_OK);
      assert(checksum == 123);
      assert(vectis_zlib_raw_checksum(kind, width, ULONG_MAX, "123456789", 9,
                                      &checksum, &error) == VECTIS_OK);
      assert(checksum ==
             (kind == 0 ? adler32_z(ULONG_MAX, (const Bytef *)"123456789", 9)
                        : crc32_z(ULONG_MAX, (const Bytef *)"123456789", 9)));
      assert(vectis_zlib_raw_checksum_combine(kind, width, ULONG_MAX, ULONG_MAX,
                                              9, &combined,
                                              &error) == VECTIS_OK);
      assert(combined == (kind == 0 ? adler32_combine(ULONG_MAX, ULONG_MAX, 9)
                                    : crc32_combine(ULONG_MAX, ULONG_MAX, 9)));
    }
  }
  assert(out->borrow(out, 0, 1, 0, &small, &error) == VECTIS_OK);
  assert(vectis_zlib_buffer_transform(0, 0, small, in, 0, &result, &error) ==
         VECTIS_OK);
  assert(result.zlib_status == Z_BUF_ERROR && result.produced <= 1);
  small->close(small);
  assert(vectis_zlib_buffer_transform(1, 0, out, in, 99, &result, &error) ==
         VECTIS_OK);
  assert(result.zlib_status == Z_STREAM_ERROR);
  assert(vectis_zlib_buffer_transform(2, 0, decoded, in, 0, &result, &error) ==
         VECTIS_OK);
  assert(result.zlib_status == Z_DATA_ERROR);
  assert(vectis_zlib_buffer_transform(0, 0, out, out, 0, &result, &error) ==
         VECTIS_ERR_INVALID);
  assert(out->borrow(out, 0, 128, 1, &readonly, &error) == VECTIS_OK);
  assert(vectis_zlib_buffer_transform(0, 0, readonly, in, 0, &result, &error) ==
         VECTIS_ERR_STATE);
  readonly->close(readonly);
  assert(vectis_buffer_span_lock_writes(out, &lock, &error) == VECTIS_OK);
  assert(vectis_zlib_buffer_transform(0, 0, out, in, 0, &result, &error) ==
         VECTIS_ERR_STATE);
  lock->close(lock);
  assert(vectis_zlib_raw_crc_gen(0, 9, &operator_value, &error) == VECTIS_OK);
  assert(vectis_zlib_raw_crc_op(ULONG_MAX, ULONG_MAX, operator_value, &combined,
                                &error) == VECTIS_OK);
  assert(combined == crc32_combine_op(ULONG_MAX, ULONG_MAX, operator_value));
  assert(vectis_zlib_raw_crc_op(0, 0, 0, &combined, &error) == VECTIS_OK &&
         combined == 0);
  assert(vectis_zlib_raw_crc_op(ULONG_MAX, ULONG_MAX, ULONG_MAX, &combined,
                                &error) == VECTIS_OK);
  assert(combined == crc32_combine_op(ULONG_MAX, ULONG_MAX, ULONG_MAX));
  assert(vectis_zlib_raw_checksum_combine(1, 0, 0, 0, (uint64_t)INT64_MAX + 1u,
                                          &combined,
                                          &error) == VECTIS_ERR_INVALID);
  assert(vectis_zlib_raw_checksum(1, 0, 0, NULL, 1, &combined, &error) ==
         VECTIS_ERR_INVALID);
#if SIZE_MAX > UINT_MAX
  assert(vectis_zlib_raw_checksum(1, 0, 0, "", (size_t)UINT_MAX + 1u, &combined,
                                  &error) == VECTIS_ERR_INVALID);
#endif
  assert(vectis_zlib_raw_crc_gen(1, INT64_MAX, &combined, &error) == VECTIS_OK);
#ifdef Z_LARGE64
  assert(combined == crc32_combine_gen64(INT64_MAX));
#else
  assert(combined == crc32_combine_gen((z_off_t)INT64_MAX));
#endif
  assert(vectis_zlib_crc_table_new(&table, &error) == VECTIS_OK);
  assert(vectis_zlib_crc_table_new(&other, &error) == VECTIS_OK);
  assert(table->size(table, &size, &error) == VECTIS_OK && size == 256);
  assert(table->native(table, &values, &error) == VECTIS_OK &&
         values == get_crc_table());
  for (size = 0; size < 256; ++size) {
    assert(table->entry(table, size, &entry, &error) == VECTIS_OK);
    assert(entry == values[size]);
  }
  assert(table->entry(table, 256, &entry, &error) == VECTIS_ERR_INVALID);
  table->close(table);
  assert(other->entry(other, 1, &entry, &error) == VECTIS_OK &&
         entry == 0x77073096u);
  other->close(other);
  in->close(in);
  out->close(out);
  decoded->close(decoded);
  input->close(input);
  output->close(output);
  plain->close(plain);
}

static int fail_alloc;
static size_t live;
static vectis_zlib_raw *active_owner;
static voidpf allocation(voidpf user, uInt items, uInt size) {
  void *bytes;
  vectis_error error;
  (void)user;
  if (active_owner)
    assert(active_owner->close(active_owner, &error) == VECTIS_ERR_STATE);
  if (fail_alloc)
    return Z_NULL;
  bytes = calloc(items, size);
  if (bytes)
    ++live;
  return bytes;
}
static void release(voidpf user, voidpf bytes) {
  (void)user;
  if (bytes) {
    --live;
    free(bytes);
  }
}

int main(void) {
  vectis_zlib_raw_options options;
  vectis_zlib_raw *encoder, *copy, *decoder;
  vectis_zlib_raw_step step;
  vectis_error error;
  z_streamp stream;
  unsigned char output[512], other[512], plain[512], dict[32768];
  const char *dictionary = "hello dictionary",
             *message = "hello dictionary hello dictionary payload";
  size_t prefix, tail, total;
  uInt dict_size;
  uLongf plain_size;
  vectis_buffer *input_buffer, *output_buffer;
  vectis_buffer_span *input_span, *output_span, *next_span;
  vectis_zlib_gzip_header_options header_options;
  vectis_zlib_gzip_header *write_header, *read_header;
  gz_headerp read_fields;
  int code;
  test_buffer_helpers();
  vectis_zlib_raw_options_init(&options);
  options.alloc = allocation;
  options.free = release;
  fail_alloc = 1;
  assert(vectis_zlib_raw_new(&options, &encoder, &error) == VECTIS_ERR_NOMEM &&
         !encoder && live == 0);
  fail_alloc = 0;
  options.version = "incompatible";
  assert(vectis_zlib_raw_new(&options, &encoder, &error) ==
             VECTIS_ERR_INVALID &&
         !encoder);
  assert(error.dependency_code == Z_VERSION_ERROR && live == 0);
  options.version = ZLIB_VERSION;
  options.stream_size = 0;
  assert(vectis_zlib_raw_new(&options, &encoder, &error) ==
             VECTIS_ERR_INVALID &&
         !encoder);
  assert(error.dependency_code == Z_VERSION_ERROR && live == 0);
  options.stream_size = (int)sizeof(z_stream);
  assert(vectis_zlib_raw_new(&options, &encoder, &error) == VECTIS_OK);
  assert(encoder->native(encoder, &stream, &error) == VECTIS_OK);
  assert(deflateSetDictionary(stream, (const Bytef *)dictionary,
                              (uInt)strlen(dictionary)) == Z_OK);
  assert(deflateGetDictionary(stream, dict, &dict_size) == Z_OK &&
         dict_size == strlen(dictionary));
  assert(memcmp(dict, dictionary, dict_size) == 0);
  assert(encoder->process(encoder, message, strlen(message), output,
                          sizeof(output), Z_NO_FLUSH, &step,
                          &error) == VECTIS_OK);
  assert(step.consumed == strlen(message) && step.zlib_status == Z_OK);
  prefix = step.produced;
  assert(stream->next_in == Z_NULL && stream->next_out == Z_NULL &&
         !stream->avail_in && !stream->avail_out);
  active_owner = encoder;
  fail_alloc = 1;
  assert(encoder->copy(encoder, &copy, &error) == VECTIS_ERR_NOMEM && !copy);
  fail_alloc = 0;
  assert(encoder->copy(encoder, &copy, &error) == VECTIS_OK);
  active_owner = NULL;
  assert(encoder->process(encoder, NULL, 0, output + prefix,
                          sizeof(output) - prefix, Z_FINISH, &step,
                          &error) == VECTIS_OK);
  assert(step.zlib_status == Z_STREAM_END);
  tail = step.produced;
  total = prefix + tail;
  assert(copy->process(copy, NULL, 0, other, sizeof(other), Z_FINISH, &step,
                       &error) == VECTIS_OK);
  assert(step.zlib_status == Z_STREAM_END && step.produced == tail &&
         memcmp(other, output + prefix, tail) == 0);
  assert(encoder->end(encoder, &code, &error) == VECTIS_OK && code == Z_OK);
  assert(encoder->native(encoder, &stream, &error) == VECTIS_ERR_STATE &&
         !stream);
  assert(encoder->end(encoder, &code, &error) == VECTIS_OK && code == Z_OK);
  assert(encoder->close(encoder, &error) == VECTIS_OK);
  assert(copy->close(copy, &error) == VECTIS_OK);
  options.compress = 0;
  assert(vectis_zlib_raw_new(&options, &decoder, &error) == VECTIS_OK);
  assert(decoder->process(decoder, output, total, plain, sizeof(plain),
                          Z_NO_FLUSH, &step, &error) == VECTIS_OK);
  assert(step.zlib_status == Z_NEED_DICT && step.produced == 0);
  prefix = step.consumed;
  assert(decoder->native(decoder, &stream, &error) == VECTIS_OK);
  assert(inflateSetDictionary(stream, (const Bytef *)dictionary,
                              (uInt)strlen(dictionary)) == Z_OK);
  assert(decoder->process(decoder, output + prefix, total - prefix, plain,
                          sizeof(plain), Z_FINISH, &step, &error) == VECTIS_OK);
  assert(step.zlib_status == Z_STREAM_END && step.produced == strlen(message));
  assert(memcmp(plain, message, step.produced) == 0);
  assert(inflateResetKeep(stream) == Z_OK);
  assert(inflateReset2(stream, MAX_WBITS) == Z_OK);
  assert(decoder->process(decoder, plain, 2, plain, 3, Z_NO_FLUSH, &step,
                          &error) == VECTIS_ERR_INVALID);
  assert(decoder->process(decoder, NULL, 1, plain, 3, Z_NO_FLUSH, &step,
                          &error) == VECTIS_ERR_INVALID);
  assert(decoder->process(decoder, NULL, 0, plain, 65537u, Z_NO_FLUSH, &step,
                          &error) == VECTIS_ERR_INVALID);
  assert(decoder->process(decoder, NULL, 0, plain, sizeof(plain), Z_NO_FLUSH,
                          &step, &error) == VECTIS_OK);
  assert(step.zlib_status == Z_BUF_ERROR);
  assert(decoder->close(decoder, &error) == VECTIS_OK && live == 0);
  options.compress = 1;
  assert(vectis_buffer_new(strlen(message), &input_buffer, &error) ==
         VECTIS_OK);
  assert(vectis_buffer_write(input_buffer, 0, message, strlen(message),
                             &error) == VECTIS_OK);
  assert(vectis_buffer_new(512, &output_buffer, &error) == VECTIS_OK);
  assert(vectis_buffer_borrow(input_buffer, 0, strlen(message), 1, &input_span,
                              &error) == VECTIS_OK);
  assert(vectis_buffer_borrow(output_buffer, 0, 512, 0, &output_span, &error) ==
         VECTIS_OK);
  assert(vectis_zlib_raw_new(&options, &encoder, &error) == VECTIS_OK);
  assert(encoder->bind(encoder, 1, input_span, 0, strlen(message), &error) ==
         VECTIS_OK);
  assert(encoder->bind(encoder, 0, output_span, 0, 512, &error) == VECTIS_OK);
  assert(encoder->bind(encoder, 0, input_span, 0, 1, &error) ==
         VECTIS_ERR_STATE);
  assert(encoder->availability(encoder, 1, strlen(message) + 1, &error) ==
         VECTIS_ERR_INVALID);
  assert(encoder->process(encoder, NULL, 0, plain, sizeof(plain), Z_NO_FLUSH,
                          &step, &error) == VECTIS_ERR_STATE);
  assert(encoder->copy(encoder, &copy, &error) == VECTIS_OK);
  assert(encoder->close(encoder, &error) == VECTIS_OK);
  vectis_buffer_span_close(input_span);
  vectis_buffer_close(input_buffer);
  assert(copy->run(copy, Z_FINISH, &step, &error) == VECTIS_OK &&
         step.zlib_status == Z_STREAM_END);
  total = step.produced;
  assert(vectis_buffer_span_read(output_span, 0, output, total, &error) ==
         VECTIS_OK);
  assert(copy->io(copy, 0, &next_span, &error) == VECTIS_OK);
  assert(next_span->size(next_span, &tail, &error) == VECTIS_OK &&
         tail == 512 - total);
  vectis_buffer_span_close(next_span);
  assert(copy->bind(copy, 0, NULL, 0, 0, &error) == VECTIS_OK);
  assert(copy->io(copy, 0, &next_span, &error) == VECTIS_OK &&
         next_span == NULL);
  assert(copy->close(copy, &error) == VECTIS_OK && live == 0);
  vectis_buffer_span_close(output_span);
  assert(output_buffer->resize(output_buffer, 1024, &error) == VECTIS_OK);
  vectis_buffer_close(output_buffer);
  plain_size = sizeof(plain);
  assert(uncompress(plain, &plain_size, output, (uLong)total) == Z_OK);
  assert(plain_size == strlen(message) &&
         memcmp(plain, message, (size_t)plain_size) == 0);
  vectis_zlib_gzip_header_options_init(&header_options);
  assert(vectis_buffer_new(3, &input_buffer, &error) == VECTIS_OK);
  assert(input_buffer->write(input_buffer, 0, "C\0", 2, &error) == VECTIS_OK);
  assert(input_buffer->borrow(input_buffer, 0, 3, 1, &input_span, &error) ==
         VECTIS_OK);
  header_options.name = input_span;
  header_options.fields.hcrc = 1;
  header_options.fields.time = 123u;
  assert(vectis_zlib_gzip_header_new(&header_options, &write_header, &error) ==
         VECTIS_OK);
  options.window_bits = 31;
  assert(vectis_zlib_raw_new(&options, &encoder, &error) == VECTIS_OK);
  assert(encoder->header(encoder, write_header, &code, &error) == VECTIS_OK &&
         code == Z_OK);
  assert(encoder->copy(encoder, &copy, &error) == VECTIS_OK);
  write_header->close(write_header);
  input_span->close(input_span);
  input_buffer->close(input_buffer);
  assert(encoder->close(encoder, &error) == VECTIS_OK);
  assert(copy->process(copy, message, strlen(message), output, sizeof(output),
                       Z_FINISH, &step, &error) == VECTIS_OK);
  assert(step.zlib_status == Z_STREAM_END && (output[3] & 8));
  total = step.produced;
  assert(copy->close(copy, &error) == VECTIS_OK && live == 0);
  assert(vectis_buffer_new(16, &output_buffer, &error) == VECTIS_OK);
  assert(output_buffer->borrow(output_buffer, 0, 16, 0, &output_span, &error) ==
         VECTIS_OK);
  vectis_zlib_gzip_header_options_init(&header_options);
  header_options.read_mode = 1;
  header_options.name = output_span;
  header_options.fields.name_max = 16;
  assert(vectis_zlib_gzip_header_new(&header_options, &read_header, &error) ==
         VECTIS_OK);
  options.compress = 0;
  assert(vectis_zlib_raw_new(&options, &decoder, &error) == VECTIS_OK);
  assert(decoder->header(decoder, read_header, &code, &error) == VECTIS_OK &&
         code == Z_OK);
  assert(read_header->native(read_header, &read_fields, &error) == VECTIS_OK);
  assert(decoder->copy(decoder, &copy, &error) == VECTIS_OK);
  read_header->close(read_header);
  assert(decoder->close(decoder, &error) == VECTIS_OK);
  assert(copy->process(copy, output, total, plain, sizeof(plain), Z_FINISH,
                       &step, &error) == VECTIS_OK);
  assert(step.zlib_status == Z_STREAM_END && read_fields->done == 1 &&
         read_fields->hcrc == 1 && read_fields->time == 123u);
  assert(output_span->read(output_span, 0, other, 2, &error) == VECTIS_OK &&
         memcmp(other, "C\0", 2) == 0);
  assert(copy->reset(copy, 1, 0, &code, &error) == VECTIS_OK && code == Z_OK);
  output_span->close(output_span);
  assert(output_buffer->resize(output_buffer, 32, &error) == VECTIS_OK);
  output_buffer->close(output_buffer);
  assert(copy->close(copy, &error) == VECTIS_OK && live == 0);
  assert(vectis_zlib_raw_close(NULL, &error) == VECTIS_OK);
  return 0;
}
