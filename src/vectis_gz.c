#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE 1
#endif
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <vectis/gz.h>
#include <zlib.h>
#ifdef Z_LARGE64
#define native_open gzopen64
#define native_seek gzseek64
#define native_tell gztell64
#define native_offset gzoffset64
typedef z_off64_t gz_off;
#else
#define native_open gzopen
#define native_seek gzseek
#define native_tell gztell
#define native_offset gzoffset
typedef z_off_t gz_off;
#endif
typedef char gz_offset_fits_int64[(sizeof(gz_off) <= sizeof(int64_t)) ? 1 : -1];
typedef struct gz_impl {
  gzFile file;
  int writing;
  int close_code;
  vectis_gz_error last;
} gz_impl;
static int fail(vectis_gz_error *e, int code, const char *message) {
  if (e) {
    memset(e, 0, sizeof(*e));
    e->code = code;
    strncpy(e->message, message, sizeof(e->message) - 1u);
  }
  return -1;
}
static gz_impl *live(vectis_gz *g, vectis_gz_error *e) {
  gz_impl *p;
  p = g ? (gz_impl *)g->impl : NULL;
  if (!p || !p->file) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is closed");
    return NULL;
  }
  return p;
}
static void snapshot(gz_impl *p, vectis_gz_error *e) {
  const char *s;
  int code;
  int saved;
  saved = errno;
  s = gzerror(p->file, &code);
  if (code == Z_ERRNO && saved == 0 && p->last.code == Z_ERRNO)
    saved = p->last.system_errno;
  memset(&p->last, 0, sizeof(p->last));
  p->last.code = code;
  p->last.system_errno = code == Z_ERRNO ? saved : 0;
  strncpy(p->last.message, s, sizeof(p->last.message) - 1u);
  if (e)
    *e = p->last;
}
static int mode_kind(const char *s) {
  int kind;
  kind = -1;
  if (!s)
    return -1;
  while (*s) {
    if (*s == 'r')
      kind = 0;
    else if (*s == 'w' || *s == 'a')
      kind = 1;
    else if (!strchr("b0123456789fhRFTGNex", *s))
      return -1;
    ++s;
  }
  return kind;
}
static vectis_status acquire(const char *path, int fd, const char *mode,
                             vectis_gz **out, vectis_gz_error *e) {
  vectis_gz *g;
  gz_impl *p;
  int kind;
  if (out)
    *out = NULL;
  kind = mode_kind(mode);
  if (!out || kind < 0 || (!path && fd < 0)) {
    fail(e, Z_STREAM_ERROR, "invalid gzip acquisition");
    return VECTIS_ERR_INVALID;
  }
  g = (vectis_gz *)calloc(1, sizeof(*g));
  p = (gz_impl *)calloc(1, sizeof(*p));
  if (!g || !p) {
    free(g);
    free(p);
    fail(e, Z_MEM_ERROR, "gzip owner allocation failed");
    return VECTIS_ERR_NOMEM;
  }
  errno = 0;
  p->file = path ? native_open(path, mode) : gzdopen(fd, mode);
  if (!p->file) {
    int saved;
    saved = errno;
    fail(e, saved ? Z_ERRNO : Z_MEM_ERROR,
         saved ? strerror(saved) : "gzip open failed");
    if (e)
      e->system_errno = saved;
    free(p);
    free(g);
    return VECTIS_ERR_STATE;
  }
  p->writing = kind;
  g->impl = p;
  g->read = vectis_gz_read;
  g->write = vectis_gz_write;
  g->fread = vectis_gz_fread;
  g->fwrite = vectis_gz_fwrite;
  g->gets = vectis_gz_gets;
  g->puts = vectis_gz_puts;
  g->buffer = vectis_gz_buffer;
  g->setparams = vectis_gz_setparams;
  g->getc = vectis_gz_getc;
  g->getc_ = vectis_gz_getc_;
  g->putc = vectis_gz_putc;
  g->ungetc = vectis_gz_ungetc;
  g->flush = vectis_gz_flush;
  g->rewind = vectis_gz_rewind;
  g->seek = vectis_gz_seek;
  g->tell = vectis_gz_tell;
  g->offset = vectis_gz_offset;
  g->eof = vectis_gz_eof;
  g->direct = vectis_gz_direct;
  g->error = vectis_gz_get_error;
  g->clearerr = vectis_gz_clearerr;
  g->close = vectis_gz_close;
  g->destroy = vectis_gz_destroy;
  *out = g;
  if (e)
    memset(e, 0, sizeof(*e));
  return VECTIS_OK;
}
vectis_status vectis_gz_open(const char *p, const char *m, vectis_gz **g,
                             vectis_gz_error *e) {
  return acquire(p, -1, m, g, e);
}
vectis_status vectis_gz_dopen(int fd, const char *m, vectis_gz **g,
                              vectis_gz_error *e) {
  return acquire(NULL, fd, m, g, e);
}
int vectis_gz_read(vectis_gz *g, void *buffer, size_t length,
                   vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  if (p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not readable");
    return -1;
  }
  if (length > VECTIS_GZ_CHUNK_BYTES || (!buffer && length)) {
    fail(e, Z_STREAM_ERROR, "invalid gzip buffer length");
    return -1;
  }
  errno = 0;
  result = gzread(p->file, buffer, (unsigned)length);
  snapshot(p, e);
  return result;
}
int vectis_gz_write(vectis_gz *g, const void *buffer, size_t length,
                    vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  if (!p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not writable");
    return -1;
  }
  if (length > VECTIS_GZ_CHUNK_BYTES || (!buffer && length)) {
    fail(e, Z_STREAM_ERROR, "invalid gzip buffer length");
    return -1;
  }
  errno = 0;
  result = gzwrite(p->file, buffer, (unsigned)length);
  snapshot(p, e);
  return result;
}
size_t vectis_gz_fread(vectis_gz *g, void *buffer, size_t size, size_t count,
                       vectis_gz_error *e) {
  gz_impl *p;
  size_t result;
  p = live(g, e);
  if (!p)
    return 0;
  if (p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not readable");
    return 0;
  }
  if ((size && count > VECTIS_GZ_CHUNK_BYTES / size) ||
      (!buffer && size && count)) {
    fail(e, Z_STREAM_ERROR, "invalid gzip element product");
    return 0;
  }
  errno = 0;
  result = gzfread(buffer, size, count, p->file);
  snapshot(p, e);
  return result;
}
size_t vectis_gz_fwrite(vectis_gz *g, const void *buffer, size_t size,
                        size_t count, vectis_gz_error *e) {
  gz_impl *p;
  size_t result;
  p = live(g, e);
  if (!p)
    return 0;
  if (!p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not writable");
    return 0;
  }
  if ((size && count > VECTIS_GZ_CHUNK_BYTES / size) ||
      (!buffer && size && count)) {
    fail(e, Z_STREAM_ERROR, "invalid gzip element product");
    return 0;
  }
  errno = 0;
  result = gzfwrite(buffer, size, count, p->file);
  snapshot(p, e);
  return result;
}
int vectis_gz_gets(vectis_gz *g, char *buffer, size_t length,
                   vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  if (p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not readable");
    return -1;
  }
  if (length > VECTIS_GZ_CHUNK_BYTES || (!buffer && length) || length < 1) {
    fail(e, Z_STREAM_ERROR, "invalid gzip buffer length");
    return -1;
  }
  errno = 0;
  result = gzgets(p->file, buffer, (int)length) != NULL;
  snapshot(p, e);
  return result;
}
int vectis_gz_puts(vectis_gz *g, const char *text, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  if (!p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not writable");
    return -1;
  }
  if (!text || strlen(text) > VECTIS_GZ_CHUNK_BYTES)
    return fail(e, Z_STREAM_ERROR, "invalid gzip string length");
  errno = 0;
  result = gzputs(p->file, text);
  snapshot(p, e);
  return result;
}
int vectis_gz_buffer(vectis_gz *g, unsigned size, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  if (!size || size > VECTIS_GZ_CHUNK_BYTES)
    return fail(e, Z_STREAM_ERROR, "invalid gzip buffer size");
  errno = 0;
  result = gzbuffer(p->file, size);
  snapshot(p, e);
  return result;
}
int vectis_gz_setparams(vectis_gz *g, int level, int strategy,
                        vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  if (!p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not writable");
    return -1;
  }
  errno = 0;
  result = gzsetparams(p->file, level, strategy);
  snapshot(p, e);
  return result;
}
int vectis_gz_getc(vectis_gz *g, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  if (p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not readable");
    return -1;
  }
  errno = 0;
  result = (gzgetc)(p->file);
  snapshot(p, e);
  return result;
}
int vectis_gz_getc_(vectis_gz *g, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  if (p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not readable");
    return -1;
  }
  errno = 0;
  result = gzgetc_(p->file);
  snapshot(p, e);
  return result;
}
int vectis_gz_putc(vectis_gz *g, int value, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  if (!p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not writable");
    return -1;
  }
  errno = 0;
  result = gzputc(p->file, value);
  snapshot(p, e);
  return result;
}
int vectis_gz_ungetc(vectis_gz *g, int value, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  if (p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not readable");
    return -1;
  }
  errno = 0;
  result = gzungetc(value, p->file);
  snapshot(p, e);
  return result;
}
int vectis_gz_flush(vectis_gz *g, int value, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  if (!p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not writable");
    return -1;
  }
  errno = 0;
  result = gzflush(p->file, value);
  snapshot(p, e);
  return result;
}
int vectis_gz_rewind(vectis_gz *g, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  if (p->writing) {
    fail(e, Z_STREAM_ERROR, "gzip receiver is not readable");
    return -1;
  }
  errno = 0;
  result = gzrewind(p->file);
  snapshot(p, e);
  return result;
}
int64_t vectis_gz_seek(vectis_gz *g, int64_t offset, int whence,
                       vectis_gz_error *e) {
  gz_impl *p;
  int64_t result;
  p = live(g, e);
  if (!p)
    return -1;
  if (sizeof(gz_off) < sizeof(int64_t)) {
    int64_t maximum;
    maximum =
        (int64_t)(((uint64_t)1u << (sizeof(gz_off) * CHAR_BIT - 1u)) - 1u);
    if (offset > maximum || offset < (-maximum - 1))
      return fail(e, Z_STREAM_ERROR, "offset outside native domain");
  }
  if (whence == 1) {
    gz_off position;
    position = native_tell(p->file);
    if (position >= 0 && offset > 0 && offset > INT64_MAX - (int64_t)position)
      return fail(e, Z_STREAM_ERROR, "relative seek overflows signed offset");
  }
  errno = 0;
  result = native_seek(p->file, (gz_off)offset, whence);
  snapshot(p, e);
  return result;
}
int64_t vectis_gz_tell(vectis_gz *g, vectis_gz_error *e) {
  gz_impl *p;
  int64_t result;
  p = live(g, e);
  if (!p)
    return -1;
  errno = 0;
  result = native_tell(p->file);
  snapshot(p, e);
  return result;
}
int64_t vectis_gz_offset(vectis_gz *g, vectis_gz_error *e) {
  gz_impl *p;
  int64_t result;
  p = live(g, e);
  if (!p)
    return -1;
  errno = 0;
  result = native_offset(p->file);
  snapshot(p, e);
  return result;
}
int vectis_gz_eof(vectis_gz *g, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  errno = 0;
  result = gzeof(p->file);
  snapshot(p, e);
  return result;
}
int vectis_gz_direct(vectis_gz *g, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  errno = 0;
  result = gzdirect(p->file);
  snapshot(p, e);
  return result;
}
int vectis_gz_get_error(vectis_gz *g, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  errno = 0;
  result = 0;
  snapshot(p, e);
  result = p->last.code;
  return result;
}
int vectis_gz_clearerr(vectis_gz *g, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  p = live(g, e);
  if (!p)
    return -1;
  errno = 0;
  result = (gzclearerr(p->file), 0);
  snapshot(p, e);
  return result;
}
int vectis_gz_close(vectis_gz *g, int form, vectis_gz_error *e) {
  gz_impl *p;
  int result;
  int saved;
  vectis_gz_error prior;
  p = g ? (gz_impl *)g->impl : NULL;
  if (!p)
    return fail(e, Z_STREAM_ERROR, "invalid gzip receiver");
  if (!p->file) {
    if (e)
      *e = p->last;
    return p->close_code;
  }
  if (form < 0 || form > 2 || (form == 1 && p->writing) ||
      (form == 2 && !p->writing)) {
    fail(e, Z_STREAM_ERROR, "wrong gzip close form");
    return Z_STREAM_ERROR;
  }
  snapshot(p, &prior);
  errno = 0;
  result = form == 1   ? gzclose_r(p->file)
           : form == 2 ? gzclose_w(p->file)
                       : gzclose(p->file);
  saved = errno;
  p->file = NULL;
  p->close_code = result;
  if (result != Z_OK) {
    fail(&p->last, result,
         result == Z_ERRNO ? strerror(saved) : zError(result));
    p->last.system_errno = result == Z_ERRNO ? saved : 0;
    if (prior.code == result && prior.message[0])
      p->last = prior;
  } else
    memset(&p->last, 0, sizeof(p->last));
  if (e)
    *e = p->last;
  return result;
}
void vectis_gz_destroy(vectis_gz *g) {
  if (g) {
    vectis_gz_close(g, 0, NULL);
    free(g->impl);
    free(g);
  }
}
