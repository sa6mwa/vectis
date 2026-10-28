#ifndef VECTIS_PNG_H
#define VECTIS_PNG_H
#include <vectis/vectis.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct vectis_png vectis_png;
typedef enum vectis_png_format {
  VECTIS_PNG_RGBA8 = 1,
  VECTIS_PNG_RGB8 = 2,
  VECTIS_PNG_GRAY8 = 3
} vectis_png_format;
typedef struct vectis_png_limits {
  unsigned int max_width, max_height;
  size_t max_row_bytes, max_bytes;
} vectis_png_limits;
typedef struct vectis_png_text {
  const char *key;
  const char *value;
} vectis_png_text;
typedef struct vectis_png_info {
  unsigned int width, height;
  vectis_png_format format;
  int bit_depth, color_type, interlaced;
  size_t row_bytes;
  const vectis_png_text *text;
  size_t text_count;
  unsigned int physical_x, physical_y;
  int physical_present, physical_meter;
} vectis_png_info;
typedef vectis_status (*vectis_png_read_fn)(void *, void *, size_t, size_t *,
                                            vectis_error *);
typedef vectis_status (*vectis_png_write_fn)(void *, const void *, size_t,
                                             vectis_error *);
/* Caller-serialized, process/thread-confined. Callback contexts remain caller
 * owned until close. Callbacks must not longjmp or reenter the active receiver.
 * Native errors latch. File paths/metadata are borrowed only during creation.
 * Info/row views are borrowed until next operation or close. */
struct vectis_png {
  vectis_status (*info)(vectis_png *, vectis_png_info *, vectis_error *);
  vectis_status (*next_row)(vectis_png *, const void **, size_t *,
                            vectis_error *);
  vectis_status (*write_row)(vectis_png *, const void *, size_t,
                             vectis_error *);
  vectis_status (*finish)(vectis_png *, vectis_error *);
  vectis_status (*close)(vectis_png *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
void vectis_png_limits_init(vectis_png_limits *);
vectis_status vectis_png_reader(vectis_png_read_fn, void *,
                                const vectis_png_limits *, vectis_png **,
                                vectis_error *);
vectis_status vectis_png_reader_file(const char *, const vectis_png_limits *,
                                     vectis_png **, vectis_error *);
vectis_status vectis_png_writer(vectis_png_write_fn, void *,
                                const vectis_png_info *,
                                const vectis_png_limits *, vectis_png **,
                                vectis_error *);
vectis_status vectis_png_writer_file(const char *, const vectis_png_info *,
                                     const vectis_png_limits *, vectis_png **,
                                     vectis_error *);
vectis_status vectis_png_get_info(vectis_png *, vectis_png_info *,
                                  vectis_error *);
vectis_status vectis_png_next_row(vectis_png *, const void **, size_t *,
                                  vectis_error *);
vectis_status vectis_png_write_row(vectis_png *, const void *, size_t,
                                   vectis_error *);
vectis_status vectis_png_finish(vectis_png *, vectis_error *);
/* Close releases native state/file without finishing or invoking user
 * callbacks. C close frees the shell; NULL is valid. Failed state can still be
 * closed. */
vectis_status vectis_png_close(vectis_png *, vectis_error *);
typedef struct vectis_png_materialized {
  vectis_png_info info;
  vectis_mutable_bytes pixels;
  void *impl; /* Owned metadata state, released by cleanup. */
} vectis_png_materialized;
/* Output is owned even if Lua copying fails. Pixels use packed RGBA8. */
vectis_status vectis_png_decode_materialized(const void *, size_t,
                                             const vectis_png_limits *,
                                             vectis_png_materialized *,
                                             vectis_error *);
void vectis_png_materialized_cleanup(vectis_png_materialized *);
vectis_status vectis_png_encode_materialized(const vectis_png_info *,
                                             const void *, size_t,
                                             const vectis_png_limits *,
                                             vectis_mutable_bytes *,
                                             vectis_error *);
#ifdef __cplusplus
}
#endif
#endif
