#ifndef VECTIS_GZ_H
#define VECTIS_GZ_H
#include <stdint.h>
#include <vectis/vectis.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VECTIS_GZ_CHUNK_BYTES 65536u
/* Native zlib code and errno, with provider message copied before destruction.
 */
typedef struct vectis_gz_error {
  int code;
  int system_errno;
  char message[512];
} vectis_gz_error;
typedef struct vectis_gz vectis_gz;
/* Caller serialized. Buffers borrowed only during calls, capped at 64 KiB.
 * close form: 0 auto, 1 read, 2 write. Retains shell; destroy releases shell.
 * Native returns preserved; always inspect error, including after partial IO.
 * gets returns 1 for native non-NULL, 0 for EOF, -1 for validation failure.
 * fread may consume a partial final item: use tell delta for consumed bytes.
 * puts uses native NUL termination. All signed offsets are exact int64.
 */
struct vectis_gz {
  int (*read)(vectis_gz *, void *buffer, size_t length, vectis_gz_error *);
  int (*write)(vectis_gz *, const void *buffer, size_t length,
               vectis_gz_error *);
  size_t (*fread)(vectis_gz *, void *buffer, size_t size, size_t count,
                  vectis_gz_error *);
  size_t (*fwrite)(vectis_gz *, const void *buffer, size_t size, size_t count,
                   vectis_gz_error *);
  int (*gets)(vectis_gz *, char *buffer, size_t length, vectis_gz_error *);
  int (*puts)(vectis_gz *, const char *text, vectis_gz_error *);
  int (*buffer)(vectis_gz *, unsigned size, vectis_gz_error *);
  int (*setparams)(vectis_gz *, int level, int strategy, vectis_gz_error *);
  int (*getc)(vectis_gz *, vectis_gz_error *);
  int (*getc_)(vectis_gz *, vectis_gz_error *);
  int (*putc)(vectis_gz *, int value, vectis_gz_error *);
  int (*ungetc)(vectis_gz *, int value, vectis_gz_error *);
  int (*flush)(vectis_gz *, int value, vectis_gz_error *);
  int (*rewind)(vectis_gz *, vectis_gz_error *);
  int64_t (*seek)(vectis_gz *, int64_t offset, int whence, vectis_gz_error *);
  int64_t (*tell)(vectis_gz *, vectis_gz_error *);
  int64_t (*offset)(vectis_gz *, vectis_gz_error *);
  int (*eof)(vectis_gz *, vectis_gz_error *);
  int (*direct)(vectis_gz *, vectis_gz_error *);
  int (*error)(vectis_gz *, vectis_gz_error *);
  int (*clearerr)(vectis_gz *, vectis_gz_error *);
  int (*close)(vectis_gz *, int form, vectis_gz_error *);
  void (*destroy)(vectis_gz *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
vectis_status vectis_gz_open(const char *, const char *, vectis_gz **,
                             vectis_gz_error *);
vectis_status vectis_gz_dopen(int, const char *, vectis_gz **,
                              vectis_gz_error *);
int vectis_gz_read(vectis_gz *, void *buffer, size_t length, vectis_gz_error *);
int vectis_gz_write(vectis_gz *, const void *buffer, size_t length,
                    vectis_gz_error *);
size_t vectis_gz_fread(vectis_gz *, void *buffer, size_t size, size_t count,
                       vectis_gz_error *);
size_t vectis_gz_fwrite(vectis_gz *, const void *buffer, size_t size,
                        size_t count, vectis_gz_error *);
int vectis_gz_gets(vectis_gz *, char *buffer, size_t length, vectis_gz_error *);
int vectis_gz_puts(vectis_gz *, const char *text, vectis_gz_error *);
int vectis_gz_buffer(vectis_gz *, unsigned size, vectis_gz_error *);
int vectis_gz_setparams(vectis_gz *, int level, int strategy,
                        vectis_gz_error *);
int vectis_gz_getc(vectis_gz *, vectis_gz_error *);
int vectis_gz_getc_(vectis_gz *, vectis_gz_error *);
int vectis_gz_putc(vectis_gz *, int value, vectis_gz_error *);
int vectis_gz_ungetc(vectis_gz *, int value, vectis_gz_error *);
int vectis_gz_flush(vectis_gz *, int value, vectis_gz_error *);
int vectis_gz_rewind(vectis_gz *, vectis_gz_error *);
int64_t vectis_gz_seek(vectis_gz *, int64_t offset, int whence,
                       vectis_gz_error *);
int64_t vectis_gz_tell(vectis_gz *, vectis_gz_error *);
int64_t vectis_gz_offset(vectis_gz *, vectis_gz_error *);
int vectis_gz_eof(vectis_gz *, vectis_gz_error *);
int vectis_gz_direct(vectis_gz *, vectis_gz_error *);
int vectis_gz_get_error(vectis_gz *, vectis_gz_error *);
int vectis_gz_clearerr(vectis_gz *, vectis_gz_error *);
int vectis_gz_close(vectis_gz *, int form, vectis_gz_error *);
void vectis_gz_destroy(vectis_gz *);
#ifdef __cplusplus
}
#endif
#endif
