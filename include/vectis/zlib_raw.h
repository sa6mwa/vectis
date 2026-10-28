#ifndef VECTIS_ZLIB_RAW_H
#define VECTIS_ZLIB_RAW_H

#include <stdint.h>
#include <vectis/buffer.h>
#include <vectis/vectis.h>
#include <zlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vectis_zlib_raw_options {
  int compress;
  int extended;
  int level;
  int method;
  int window_bits;
  int memory_level;
  int strategy;
  const char *version;
  int stream_size;
  alloc_func alloc;
  free_func free;
  voidpf opaque;
} vectis_zlib_raw_options;

typedef struct vectis_zlib_raw_step {
  size_t consumed;
  size_t produced;
  int zlib_status;
} vectis_zlib_raw_step;
typedef struct vectis_zlib_back_result {
  uint64_t supplied;
  uint64_t consumed;
  uint64_t output;
  int zlib_status;
  int input_failed;
} vectis_zlib_back_result;

typedef struct vectis_zlib_raw vectis_zlib_raw;
typedef struct vectis_zlib_gzip_header vectis_zlib_gzip_header;
typedef struct vectis_zlib_gzip_header_options {
  int read_mode;
  gz_header fields;
  vectis_buffer_span *extra;
  vectis_buffer_span *name;
  vectis_buffer_span *comment;
} vectis_zlib_gzip_header_options;
struct vectis_zlib_gzip_header {
  vectis_status (*native)(vectis_zlib_gzip_header *, gz_headerp *,
                          vectis_error *);
  vectis_status (*io)(vectis_zlib_gzip_header *, int, vectis_buffer_span **,
                      vectis_error *);
  void (*close)(vectis_zlib_gzip_header *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
void vectis_zlib_gzip_header_options_init(vectis_zlib_gzip_header_options *);
vectis_status
vectis_zlib_gzip_header_new(const vectis_zlib_gzip_header_options *,
                            vectis_zlib_gzip_header **, vectis_error *);
vectis_status vectis_zlib_gzip_header_native(vectis_zlib_gzip_header *,
                                             gz_headerp *, vectis_error *);
/* field is 0 extra, 1 name or 2 comment. Returned span is independently owned;
 * native NULL pointer returns NULL even while original cleanup lease is held.
 */
vectis_status vectis_zlib_gzip_header_io(vectis_zlib_gzip_header *, int,
                                         vectis_buffer_span **, vectis_error *);
void vectis_zlib_gzip_header_close(vectis_zlib_gzip_header *);
/* Caller-serialized native state. Borrowed chunk IO is at most 65536 bytes per
 * call and is cleared before return. Native return codes are preserved in
 * step.zlib_status, including recoverable NEED_DICT/BUF_ERROR. Native field
 * access is through the public z_stream only; state/reserved remain private.
 * End invalidates native state; close frees the shell. Callback contexts and
 * directly installed public IO/header pointers must outlive every native copy.
 */
struct vectis_zlib_raw {
  vectis_status (*native)(vectis_zlib_raw *, z_streamp *, vectis_error *);
  vectis_status (*process)(vectis_zlib_raw *, const void *, size_t, void *,
                           size_t, int, vectis_zlib_raw_step *, vectis_error *);
  vectis_status (*copy)(vectis_zlib_raw *, vectis_zlib_raw **, vectis_error *);
  vectis_status (*bind)(vectis_zlib_raw *, int, vectis_buffer_span *, size_t,
                        size_t, vectis_error *);
  vectis_status (*io)(vectis_zlib_raw *, int, vectis_buffer_span **,
                      vectis_error *);
  vectis_status (*availability)(vectis_zlib_raw *, int, size_t, vectis_error *);
  vectis_status (*run)(vectis_zlib_raw *, int, vectis_zlib_raw_step *,
                       vectis_error *);
  vectis_status (*params)(vectis_zlib_raw *, int, int, vectis_zlib_raw_step *,
                          vectis_error *);
  vectis_status (*bound)(vectis_zlib_raw *, uint64_t, int, uint64_t *,
                         vectis_error *);
  vectis_status (*header)(vectis_zlib_raw *, vectis_zlib_gzip_header *, int *,
                          vectis_error *);
  vectis_status (*reset)(vectis_zlib_raw *, int, int, int *, vectis_error *);
  vectis_status (*back)(vectis_zlib_raw *, in_func, void *, out_func, void *,
                        vectis_zlib_back_result *, vectis_error *);
  vectis_status (*end)(vectis_zlib_raw *, int *, vectis_error *);
  vectis_status (*close)(vectis_zlib_raw *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};

void vectis_zlib_raw_options_init(vectis_zlib_raw_options *);
vectis_status vectis_zlib_raw_new(const vectis_zlib_raw_options *,
                                  vectis_zlib_raw **, vectis_error *);
vectis_status vectis_zlib_raw_back_new(const vectis_zlib_raw_options *,
                                       vectis_buffer_span *, vectis_zlib_raw **,
                                       vectis_error *);
/* Window is retained and write-locked across the native callback frame. Input
 * callbacks own their borrowed byte lifetime, including unused next_in after
 * return. Use a typed bind before the next call when input comes from a
 * transient source. Back state cannot be passed to ordinary inflate methods. */
vectis_status vectis_zlib_raw_back(vectis_zlib_raw *, in_func, void *, out_func,
                                   void *, vectis_zlib_back_result *,
                                   vectis_error *);
vectis_status vectis_zlib_raw_native(vectis_zlib_raw *, z_streamp *,
                                     vectis_error *);
vectis_status vectis_zlib_raw_process(vectis_zlib_raw *, const void *, size_t,
                                      void *, size_t, int,
                                      vectis_zlib_raw_step *, vectis_error *);
vectis_status vectis_zlib_raw_copy(vectis_zlib_raw *, vectis_zlib_raw **,
                                   vectis_error *);
/* input is exactly 0 (output) or 1 (input). Bind retains an independent lease;
 * NULL clears the binding and requires zero offset/capacity. Native next/avail
 * fields advance within these typed ranges. Copies retain independent leases.
 * Borrowed process is rejected while a retained binding exists. io returns
 * an owned span for the current next/avail region, or NULL for a null pointer.
 */
vectis_status vectis_zlib_raw_bind(vectis_zlib_raw *, int, vectis_buffer_span *,
                                   size_t, size_t, vectis_error *);
vectis_status vectis_zlib_raw_io(vectis_zlib_raw *, int, vectis_buffer_span **,
                                 vectis_error *);
vectis_status vectis_zlib_raw_availability(vectis_zlib_raw *, int, size_t,
                                           vectis_error *);
vectis_status vectis_zlib_raw_run(vectis_zlib_raw *, int,
                                  vectis_zlib_raw_step *, vectis_error *);
vectis_status vectis_zlib_raw_params(vectis_zlib_raw *, int, int,
                                     vectis_zlib_raw_step *, vectis_error *);
vectis_status vectis_zlib_raw_bound(vectis_zlib_raw *, uint64_t, int,
                                    uint64_t *, vectis_error *);
vectis_status vectis_zlib_raw_header(vectis_zlib_raw *,
                                     vectis_zlib_gzip_header *, int *,
                                     vectis_error *);
/* mode 0 reset, 1 reset-keep, 2 inflate-reset2. Deflate resets retain headers
 * in pinned zlib 1.3.2; explicit header=NULL restores defaults. Inflate
 * successful reset discards its capture. Copies retain a shared record
 * independently. */
vectis_status vectis_zlib_raw_reset(vectis_zlib_raw *, int, int, int *,
                                    vectis_error *);
vectis_status vectis_zlib_raw_end(vectis_zlib_raw *, int *, vectis_error *);
vectis_status vectis_zlib_raw_close(vectis_zlib_raw *, vectis_error *);

/* Whole-buffer operations, not streaming. Spans are borrowed only for the call.
 * NULL source means NULL/zero bytes. Native error/partial output is in result;
 * Vectis failures reject the call before writing. width is 0 (uLong) or 1
 * (size_t); operation is 0 compress, 1 compress2, 2 uncompress, 3 uncompress2.
 * consumed is defined only for operation 3. No payload copies are made. */
vectis_status vectis_zlib_buffer_transform(int, int, vectis_buffer_span *,
                                           vectis_buffer_span *, int,
                                           vectis_zlib_raw_step *,
                                           vectis_error *);
/* Native bound arithmetic, including its unsigned wrap behavior. */
vectis_status vectis_zlib_buffer_bound(uint64_t, int, uint64_t *,
                                       vectis_error *);
/* Native-width checksum helpers. kind is VECTIS_ZLIB_ADLER32/CRC32.
 * size_width selects size_t instead of uInt; large selects the 64-bit offset
 * entry point where declared. NULL data requires zero length. */
vectis_status vectis_zlib_raw_checksum(int, int, uLong, const void *, size_t,
                                       uLong *, vectis_error *);
vectis_status vectis_zlib_raw_checksum_combine(int, int, uLong, uLong, uint64_t,
                                               uLong *, vectis_error *);
vectis_status vectis_zlib_raw_crc_gen(int, uint64_t, uLong *, vectis_error *);
vectis_status vectis_zlib_raw_crc_op(uLong, uLong, uLong, uLong *,
                                     vectis_error *);

typedef struct vectis_zlib_crc_table vectis_zlib_crc_table;
/* Independent owned shell; immutable values remain owned by the provider.
 * entry indices are 0..255. Native pointer borrows end at shell close. */
struct vectis_zlib_crc_table {
  vectis_status (*size)(vectis_zlib_crc_table *, size_t *, vectis_error *);
  vectis_status (*entry)(vectis_zlib_crc_table *, size_t, z_crc_t *,
                         vectis_error *);
  vectis_status (*native)(vectis_zlib_crc_table *, const z_crc_t **,
                          vectis_error *);
  void (*close)(vectis_zlib_crc_table *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
vectis_status vectis_zlib_crc_table_new(vectis_zlib_crc_table **,
                                        vectis_error *);
vectis_status vectis_zlib_crc_table_size(vectis_zlib_crc_table *, size_t *,
                                         vectis_error *);
vectis_status vectis_zlib_crc_table_entry(vectis_zlib_crc_table *, size_t,
                                          z_crc_t *, vectis_error *);
vectis_status vectis_zlib_crc_table_native(vectis_zlib_crc_table *,
                                           const z_crc_t **, vectis_error *);
void vectis_zlib_crc_table_close(vectis_zlib_crc_table *);

#ifdef __cplusplus
}
#endif
#endif
