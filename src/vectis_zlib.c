#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE 1
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <vectis/zlib.h>
#include <zlib.h>

typedef struct vectis_zlib_impl {
  z_stream stream;
  vectis_zlib_options options;
  vectis_zlib_result result;
  vectis_error failure;
  int initialized;
  int boundary;
  int finishing;
  size_t finish_remaining;
  int gzip;
  unsigned char prefix[2];
  size_t prefix_size;
} vectis_zlib_impl;

static vectis_status zlib_error(vectis_error *error, vectis_status code,
                                const char *message) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->code = code;
    error->source = VECTIS_ERROR_SOURCE_VECTIS;
    strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return code;
}

static vectis_status zlib_fail(vectis_zlib_impl *impl, vectis_error *error,
                               vectis_status code, const char *message) {
  (void)zlib_error(&impl->failure, code, message);
  if (error != NULL)
    *error = impl->failure;
  return code;
}

void vectis_zlib_options_init(vectis_zlib_options *options) {
  if (options == NULL)
    return;
  memset(options, 0, sizeof(*options));
  options->compress = 1;
  options->level = -1;
  options->max_output_bytes = UINT64_MAX;
}

vectis_status vectis_zlib_new(const vectis_zlib_options *options,
                              vectis_zlib **out, vectis_error *error) {
  vectis_zlib *receiver;
  vectis_zlib_impl *impl;
  int bits;
  int ret;
  if (out != NULL)
    *out = NULL;
  if (out == NULL || options == NULL ||
      (options->compress != 0 && options->compress != 1) ||
      options->format < VECTIS_ZLIB_ZLIB ||
      options->format > VECTIS_ZLIB_AUTO ||
      (options->compress && options->format == VECTIS_ZLIB_AUTO) ||
      options->level < -1 || options->level > 9)
    return zlib_error(error, VECTIS_ERR_INVALID, "invalid zlib stream options");
  receiver = (vectis_zlib *)calloc(1u, sizeof(*receiver));
  impl = (vectis_zlib_impl *)calloc(1u, sizeof(*impl));
  if (receiver == NULL || impl == NULL) {
    free(receiver);
    free(impl);
    return zlib_error(error, VECTIS_ERR_NOMEM, "zlib stream allocation failed");
  }
  receiver->impl = impl;
  receiver->process = vectis_zlib_process;
  receiver->result = vectis_zlib_get_result;
  receiver->close = vectis_zlib_close;
  impl->options = *options;
  impl->gzip = options->format == VECTIS_ZLIB_GZIP;
  bits = MAX_WBITS + (options->format == VECTIS_ZLIB_GZIP   ? 16
                      : options->format == VECTIS_ZLIB_AUTO ? 32
                                                            : 0);
  ret = options->compress
            ? deflateInit2(&impl->stream, options->level, Z_DEFLATED, bits, 8,
                           Z_DEFAULT_STRATEGY)
            : inflateInit2(&impl->stream, bits);
  if (ret != Z_OK) {
    vectis_zlib_close(receiver);
    return zlib_error(error,
                      ret == Z_MEM_ERROR ? VECTIS_ERR_NOMEM : VECTIS_ERR_STATE,
                      "zlib stream initialization failed");
  }
  impl->initialized = 1;
  *out = receiver;
  return VECTIS_OK;
}

vectis_status vectis_zlib_process(vectis_zlib *receiver, const void *input,
                                  size_t size, size_t *consumed, void *output,
                                  size_t capacity, size_t *produced,
                                  vectis_zlib_flush flush,
                                  vectis_error *error) {
  vectis_zlib_impl *impl;
  const unsigned char *bytes;
  size_t used;
  size_t emitted;
  size_t i;
  int ret;
  int native_flush;
  if (consumed != NULL)
    *consumed = 0u;
  if (produced != NULL)
    *produced = 0u;
  if (receiver == NULL || receiver->impl == NULL)
    return zlib_error(error, VECTIS_ERR_INVALID, "zlib receiver is required");
  impl = (vectis_zlib_impl *)receiver->impl;
  if (impl->failure.code != VECTIS_OK) {
    if (error != NULL)
      *error = impl->failure;
    return impl->failure.code;
  }
  if (consumed == NULL || produced == NULL || output == NULL ||
      (input == NULL && size != 0u) || size > VECTIS_ZLIB_CHUNK_BYTES ||
      capacity == 0u || capacity > VECTIS_ZLIB_CHUNK_BYTES || size > UINT_MAX ||
      capacity > UINT_MAX || flush < VECTIS_ZLIB_NO_FLUSH ||
      flush > VECTIS_ZLIB_FINISH ||
      (flush == VECTIS_ZLIB_SYNC_FLUSH && capacity <= 6u) ||
      (!impl->options.compress && flush == VECTIS_ZLIB_SYNC_FLUSH))
    return zlib_fail(impl, error, VECTIS_ERR_INVALID,
                     "invalid zlib stream buffers or flush");
  if (impl->result.finished) {
    if (size != 0u || flush != VECTIS_ZLIB_FINISH)
      return zlib_error(error, VECTIS_ERR_STATE, "zlib stream is finalized");
    return VECTIS_OK;
  }
  if (impl->finishing && flush != VECTIS_ZLIB_FINISH)
    return zlib_fail(impl, error, VECTIS_ERR_STATE, "zlib stream is finishing");
  if (impl->finishing && size != impl->finish_remaining)
    return zlib_fail(impl, error, VECTIS_ERR_STATE,
                     "zlib stream received new input after EOF");
  if (flush == VECTIS_ZLIB_FINISH && !impl->finishing) {
    impl->finishing = 1;
    impl->finish_remaining = size;
  }
  if (impl->boundary) {
    if (size == 0u) {
      if (impl->finishing)
        impl->result.finished = 1;
      return VECTIS_OK;
    }
    if (!impl->gzip)
      return zlib_fail(impl, error, VECTIS_ERR_INVALID,
                       "zlib stream has trailing input");
    ret = inflateReset2(&impl->stream, MAX_WBITS + 16);
    if (ret != Z_OK)
      return zlib_fail(impl, error, VECTIS_ERR_STATE,
                       "zlib member reset failed");
    impl->boundary = 0;
  }
  /* zlib's public input pointer predates const correctness. It never writes
   * input; copy the pointer representation without casting away qualifiers. */
  memcpy(&impl->stream.next_in, &input, sizeof(input));
  impl->stream.avail_in = (uInt)size;
  impl->stream.next_out = (Bytef *)output;
  impl->stream.avail_out = (uInt)capacity;
  native_flush = flush == VECTIS_ZLIB_FINISH       ? Z_FINISH
                 : flush == VECTIS_ZLIB_SYNC_FLUSH ? Z_SYNC_FLUSH
                                                   : Z_NO_FLUSH;
  ret = impl->options.compress ? deflate(&impl->stream, native_flush)
                               : inflate(&impl->stream, Z_NO_FLUSH);
  used = size - impl->stream.avail_in;
  emitted = capacity - impl->stream.avail_out;
  if (impl->finishing)
    impl->finish_remaining -= used;
  impl->stream.next_in = Z_NULL;
  impl->stream.avail_in = 0u;
  impl->stream.next_out = Z_NULL;
  impl->stream.avail_out = 0u;
  bytes = (const unsigned char *)input;
  for (i = 0u; i < used && impl->prefix_size < 2u; ++i)
    impl->prefix[impl->prefix_size++] = bytes[i];
  if (impl->options.format == VECTIS_ZLIB_AUTO && impl->prefix_size == 2u)
    impl->gzip = impl->prefix[0] == 0x1fu && impl->prefix[1] == 0x8bu;
  if ((uint64_t)used > UINT64_MAX - impl->result.input_bytes ||
      (uint64_t)emitted > UINT64_MAX - impl->result.output_bytes)
    return zlib_fail(impl, error, VECTIS_ERR_STATE,
                     "zlib byte count exceeds UINT64_MAX");
  if (impl->result.output_bytes > impl->options.max_output_bytes ||
      (uint64_t)emitted >
          impl->options.max_output_bytes - impl->result.output_bytes)
    return zlib_fail(impl, error, VECTIS_ERR_INVALID,
                     "zlib output exceeds max_output_bytes");
  if (ret != Z_OK && ret != Z_BUF_ERROR && ret != Z_STREAM_END)
    return zlib_fail(impl, error,
                     ret == Z_MEM_ERROR ? VECTIS_ERR_NOMEM : VECTIS_ERR_INVALID,
                     ret == Z_NEED_DICT ? "zlib dictionary is required"
                                        : "zlib stream transform failed");
  impl->result.input_bytes += (uint64_t)used;
  impl->result.output_bytes += (uint64_t)emitted;
  *consumed = used;
  *produced = emitted;
  if (ret == Z_STREAM_END) {
    if (impl->result.members == UINT64_MAX)
      return zlib_fail(impl, error, VECTIS_ERR_STATE,
                       "zlib member count exceeds UINT64_MAX");
    ++impl->result.members;
    impl->boundary = 1;
    if (impl->options.compress)
      impl->result.finished = 1;
  } else if (used == 0u && emitted == 0u) {
    if (impl->finishing)
      return zlib_fail(impl, error, VECTIS_ERR_INVALID,
                       "zlib input ended before stream finished");
    if (size != 0u)
      return zlib_fail(impl, error, VECTIS_ERR_STATE,
                       "zlib stream made no progress");
  }
  return VECTIS_OK;
}

vectis_status vectis_zlib_get_result(vectis_zlib *receiver,
                                     vectis_zlib_result *result,
                                     vectis_error *error) {
  vectis_zlib_impl *impl;
  if (result != NULL)
    memset(result, 0, sizeof(*result));
  if (receiver == NULL || receiver->impl == NULL || result == NULL)
    return zlib_error(error, VECTIS_ERR_INVALID,
                      "zlib receiver and result are required");
  impl = (vectis_zlib_impl *)receiver->impl;
  if (impl->failure.code != VECTIS_OK) {
    if (error != NULL)
      *error = impl->failure;
    return impl->failure.code;
  }
  *result = impl->result;
  return VECTIS_OK;
}

void vectis_zlib_close(vectis_zlib *receiver) {
  vectis_zlib_impl *impl;
  if (receiver == NULL)
    return;
  impl = (vectis_zlib_impl *)receiver->impl;
  if (impl != NULL) {
    if (impl->initialized) {
      if (impl->options.compress)
        (void)deflateEnd(&impl->stream);
      else
        (void)inflateEnd(&impl->stream);
    }
    free(impl);
  }
  free(receiver);
}

vectis_status vectis_zlib_checksum(vectis_zlib_checksum_kind kind,
                                   uint32_t seed, const void *data, size_t size,
                                   uint32_t *out, vectis_error *error) {
  if (out != NULL)
    *out = 0u;
  if (out == NULL || (data == NULL && size != 0u) ||
      (kind != VECTIS_ZLIB_ADLER32 && kind != VECTIS_ZLIB_CRC32))
    return zlib_error(error, VECTIS_ERR_INVALID, "invalid checksum arguments");
  *out = (uint32_t)(kind == VECTIS_ZLIB_CRC32
                        ? crc32_z((uLong)seed, (const Bytef *)data, size)
                        : adler32_z((uLong)seed, (const Bytef *)data, size));
  return VECTIS_OK;
}

/* Large-file entry points are declared where zlib supports them; otherwise
 * the public native offset type determines the accepted exact length. */
#ifdef Z_LARGE64
typedef z_off64_t checksum_offset;
#define CHECKSUM_ADLER_COMBINE adler32_combine64
#define CHECKSUM_CRC_COMBINE crc32_combine64
#define CHECKSUM_CRC_GEN crc32_combine_gen64
#else
typedef z_off_t checksum_offset;
#define CHECKSUM_ADLER_COMBINE adler32_combine
#define CHECKSUM_CRC_COMBINE crc32_combine
#define CHECKSUM_CRC_GEN crc32_combine_gen
#endif

static int checksum_length(uint64_t length, checksum_offset *out) {
  if (length > (uint64_t)INT64_MAX)
    return 0;
  if (sizeof(*out) < sizeof(int64_t) && length > (uint64_t)INT32_MAX)
    return 0;
  *out = (checksum_offset)length;
  return *out >= 0 && (uint64_t)*out == length;
}

vectis_status vectis_zlib_checksum_combine(vectis_zlib_checksum_kind kind,
                                           uint32_t first, uint32_t second,
                                           uint64_t length, uint32_t *out,
                                           vectis_error *error) {
  checksum_offset offset;
  if (out != NULL)
    *out = 0u;
  if (out == NULL || !checksum_length(length, &offset) ||
      (kind != VECTIS_ZLIB_ADLER32 && kind != VECTIS_ZLIB_CRC32))
    return zlib_error(error, VECTIS_ERR_INVALID,
                      "invalid checksum kind, result or signed offset length");
  *out = (uint32_t)(kind == VECTIS_ZLIB_CRC32
                        ? CHECKSUM_CRC_COMBINE(first, second, offset)
                        : CHECKSUM_ADLER_COMBINE(first, second, offset));
  return VECTIS_OK;
}

vectis_status vectis_zlib_crc32_combine_gen(uint64_t length, uint32_t *out,
                                            vectis_error *error) {
  checksum_offset offset;
  if (out != NULL)
    *out = 0u;
  if (out == NULL || !checksum_length(length, &offset))
    return zlib_error(error, VECTIS_ERR_INVALID,
                      "invalid CRC operator result or signed offset length");
  *out = (uint32_t)CHECKSUM_CRC_GEN(offset);
  return VECTIS_OK;
}

uint32_t vectis_zlib_crc32_combine_op(uint32_t first, uint32_t second,
                                      uint32_t operation) {
  return (uint32_t)crc32_combine_op(first, second, operation);
}
