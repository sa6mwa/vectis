#ifndef VECTIS_PDF_H
#define VECTIS_PDF_H
#include <vectis/vectis.h>
#ifdef __cplusplus
extern "C" {
#endif

#define VECTIS_PDF_DEFAULT_MAX_BYTES 16777216u
typedef struct vectis_pdf vectis_pdf;
typedef struct vectis_pdf_page vectis_pdf_page;
typedef struct vectis_pdf_font vectis_pdf_font;
typedef struct vectis_pdf_image vectis_pdf_image;

typedef struct vectis_pdf_config {
  const char *title;
  const char *author;
  const char *subject;
} vectis_pdf_config;
typedef struct vectis_pdf_page_config {
  double width;
  double height;
} vectis_pdf_page_config;
typedef struct vectis_pdf_text_config {
  vectis_pdf_font *font; /* NULL selects Helvetica. Must belong to this doc. */
  double size;
  double color[3];
} vectis_pdf_text_config;
typedef struct vectis_pdf_draw_config {
  double color[3];
  double line_width;
  int fill;
  int stroke;
} vectis_pdf_draw_config;

/* Caller-serialized, process/thread-confined document. Native errors latch;
 * reset_error acknowledges them but does not undo partial operations. C close
 * frees the shell and all native objects; retained child shells become invalid
 * and remain caller-owned until their own close. */
struct vectis_pdf {
  vectis_status (*add_page)(vectis_pdf *, const vectis_pdf_page_config *,
                            vectis_pdf_page **, vectis_error *);
  vectis_status (*font)(vectis_pdf *, const char *, const char *,
                        vectis_pdf_font **, vectis_error *);
  vectis_status (*load_font)(vectis_pdf *, const char *, const char *, int,
                             vectis_pdf_font **, vectis_error *);
  vectis_status (*load_image)(vectis_pdf *, const char *, const char *,
                              vectis_pdf_image **, vectis_error *);
  vectis_status (*load_image_materialized)(vectis_pdf *, const void *, size_t,
                                           const char *, vectis_pdf_image **,
                                           vectis_error *);
  vectis_status (*save)(vectis_pdf *, const char *, vectis_error *);
  vectis_status (*bytes_materialized)(vectis_pdf *, size_t,
                                      vectis_mutable_bytes *, vectis_error *);
  vectis_status (*reset_error)(vectis_pdf *, vectis_error *);
  vectis_status (*close)(vectis_pdf *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
struct vectis_pdf_page {
  vectis_status (*text)(vectis_pdf_page *, double, double, const char *,
                        const vectis_pdf_text_config *, vectis_error *);
  vectis_status (*line)(vectis_pdf_page *, double, double, double, double,
                        const vectis_pdf_draw_config *, vectis_error *);
  vectis_status (*rectangle)(vectis_pdf_page *, double, double, double, double,
                             const vectis_pdf_draw_config *, vectis_error *);
  vectis_status (*image)(vectis_pdf_page *, vectis_pdf_image *, double, double,
                         double, double, vectis_error *);
  void (*close)(vectis_pdf_page *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
struct vectis_pdf_font {
  void (*close)(vectis_pdf_font *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
struct vectis_pdf_image {
  unsigned int width;
  unsigned int height;
  void (*close)(vectis_pdf_image *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};

void vectis_pdf_config_init(vectis_pdf_config *);
void vectis_pdf_page_config_init(vectis_pdf_page_config *);
void vectis_pdf_text_config_init(vectis_pdf_text_config *);
void vectis_pdf_draw_config_init(vectis_pdf_draw_config *);
vectis_status vectis_pdf_new(const vectis_pdf_config *, vectis_pdf **,
                             vectis_error *);
vectis_status vectis_pdf_add_page(vectis_pdf *, const vectis_pdf_page_config *,
                                  vectis_pdf_page **, vectis_error *);
vectis_status vectis_pdf_get_font(vectis_pdf *, const char *, const char *,
                                  vectis_pdf_font **, vectis_error *);
vectis_status vectis_pdf_load_font(vectis_pdf *, const char *, const char *,
                                   int, vectis_pdf_font **, vectis_error *);
vectis_status vectis_pdf_load_image(vectis_pdf *, const char *, const char *,
                                    vectis_pdf_image **, vectis_error *);
/* Encoded input is borrowed for this call only. No file or staging is used. */
vectis_status vectis_pdf_load_image_materialized(vectis_pdf *, const void *,
                                                 size_t, const char *,
                                                 vectis_pdf_image **,
                                                 vectis_error *);
vectis_status vectis_pdf_page_text(vectis_pdf_page *, double, double,
                                   const char *, const vectis_pdf_text_config *,
                                   vectis_error *);
vectis_status vectis_pdf_page_line(vectis_pdf_page *, double, double, double,
                                   double, const vectis_pdf_draw_config *,
                                   vectis_error *);
vectis_status vectis_pdf_page_rectangle(vectis_pdf_page *, double, double,
                                        double, double,
                                        const vectis_pdf_draw_config *,
                                        vectis_error *);
vectis_status vectis_pdf_page_image(vectis_pdf_page *, vectis_pdf_image *,
                                    double, double, double, double,
                                    vectis_error *);
vectis_status vectis_pdf_save(vectis_pdf *, const char *, vectis_error *);
/* Complete native document buffer plus caller-owned copy. Never streaming.
 * limit=0 selects the default. Clean output with vectis_mutable_bytes_cleanup.
 */
vectis_status vectis_pdf_bytes_materialized(vectis_pdf *, size_t,
                                            vectis_mutable_bytes *,
                                            vectis_error *);
vectis_status vectis_pdf_reset_error(vectis_pdf *, vectis_error *);
vectis_status vectis_pdf_close(vectis_pdf *, vectis_error *);
void vectis_pdf_page_close(vectis_pdf_page *);
void vectis_pdf_font_close(vectis_pdf_font *);
void vectis_pdf_image_close(vectis_pdf_image *);

#ifdef __cplusplus
}
#endif
#endif
