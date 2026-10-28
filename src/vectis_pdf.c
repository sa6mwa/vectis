#include <cpkt/pdf.h>
#include <vectis/pdf.h>

#include <float.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct pdf_state {
  CPKT_PDF_Doc native;
  CPKT_PDF_STATUS code;
  CPKT_PDF_STATUS detail;
  size_t references;
  pid_t process;
  pthread_t thread;
} pdf_state;
typedef struct pdf_child {
  pdf_state *owner;
  CPKT_PDF_HANDLE native;
} pdf_child;
typedef struct page_owner {
  vectis_pdf_page receiver;
  pdf_child child;
} page_owner;
typedef struct font_owner {
  vectis_pdf_font receiver;
  pdf_child child;
} font_owner;
typedef struct image_owner {
  vectis_pdf_image receiver;
  pdf_child child;
} image_owner;

static vectis_status pdf_error(vectis_error *error, vectis_status status,
                               const char *message) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->code = status;
    error->source = VECTIS_ERROR_SOURCE_CPKT;
    strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return status;
}
static void native_error(CPKT_PDF_STATUS code, CPKT_PDF_STATUS detail,
                         void *context) {
  pdf_state *state = (pdf_state *)context;
  state->code = code;
  state->detail = detail;
}
static vectis_status native_status(pdf_state *state, CPKT_PDF_STATUS code,
                                   vectis_error *error) {
  if (code == CPKT_PDF_OK && state->code == CPKT_PDF_OK)
    return VECTIS_OK;
  if (code != CPKT_PDF_OK)
    state->code = code;
  pdf_error(error, VECTIS_ERR_STATE,
            "libHaru operation failed; reset_error or close the document");
  if (error != NULL) {
    if (state->code <= (CPKT_PDF_STATUS)LONG_MAX)
      error->dependency_code = (long)state->code;
    snprintf(error->detail, sizeof(error->detail), "status=%lu detail=%lu",
             (unsigned long)state->code, (unsigned long)state->detail);
  }
  return VECTIS_ERR_STATE;
}
static vectis_status affinity(pdf_state *state, vectis_error *error) {
  if (state->process != getpid() ||
      !pthread_equal(state->thread, pthread_self()))
    return pdf_error(error, VECTIS_ERR_STATE,
                     "PDF receiver belongs to another process/thread");
  if (state->native == NULL)
    return pdf_error(error, VECTIS_ERR_STATE, "PDF document is closed");
  return VECTIS_OK;
}
static vectis_status check(pdf_state *state, vectis_error *error) {
  vectis_status status = affinity(state, error);
  return status != VECTIS_OK ? status
                             : native_status(state, CPKT_PDF_OK, error);
}
static int real_value(double value) {
  return isfinite(value) && value >= -FLT_MAX && value <= FLT_MAX;
}
static int color_valid(const double *color) {
  return real_value(color[0]) && real_value(color[1]) && real_value(color[2]) &&
         color[0] >= 0.0 && color[0] <= 1.0 && color[1] >= 0.0 &&
         color[1] <= 1.0 && color[2] >= 0.0 && color[2] <= 1.0;
}
static void unref(pdf_state *state) {
  if (--state->references == 0u)
    free(state);
}
void vectis_pdf_config_init(vectis_pdf_config *config) {
  memset(config, 0, sizeof(*config));
}
void vectis_pdf_page_config_init(vectis_pdf_page_config *config) {
  config->width = 595.276;
  config->height = 841.89;
}
void vectis_pdf_text_config_init(vectis_pdf_text_config *config) {
  memset(config, 0, sizeof(*config));
  config->size = 12.0;
}
void vectis_pdf_draw_config_init(vectis_pdf_draw_config *config) {
  memset(config, 0, sizeof(*config));
  config->line_width = 1.0;
  config->stroke = 1;
}
vectis_status vectis_pdf_new(const vectis_pdf_config *config, vectis_pdf **out,
                             vectis_error *error) {
  vectis_pdf_config defaults;
  vectis_pdf *receiver;
  pdf_state *state;
  vectis_status status;
  CPKT_PDF_STATUS code = CPKT_PDF_OK;
  if (out == NULL)
    return pdf_error(error, VECTIS_ERR_INVALID, "PDF output is required");
  *out = NULL;
  vectis_pdf_config_init(&defaults);
  if (config == NULL)
    config = &defaults;
  receiver = (vectis_pdf *)calloc(1u, sizeof(*receiver));
  state = (pdf_state *)calloc(1u, sizeof(*state));
  if (receiver == NULL || state == NULL) {
    free(receiver);
    free(state);
    return pdf_error(error, VECTIS_ERR_NOMEM, "PDF owner allocation failed");
  }
  state->references = 1u;
  state->process = getpid();
  state->thread = pthread_self();
  state->native = cpkt_pdf_new(native_error, state);
  if (state->native == NULL) {
    free(receiver);
    free(state);
    return pdf_error(error, VECTIS_ERR_NOMEM,
                     "libHaru document allocation failed");
  }
  receiver->impl = state;
  receiver->add_page = vectis_pdf_add_page;
  receiver->font = vectis_pdf_get_font;
  receiver->load_font = vectis_pdf_load_font;
  receiver->load_image = vectis_pdf_load_image;
  receiver->load_image_materialized = vectis_pdf_load_image_materialized;
  receiver->save = vectis_pdf_save;
  receiver->bytes_materialized = vectis_pdf_bytes_materialized;
  receiver->reset_error = vectis_pdf_reset_error;
  receiver->close = vectis_pdf_close;
  code = cpkt_pdf_use_utf_encodings(state->native);
  if (code == CPKT_PDF_OK && config->title != NULL)
    code = cpkt_pdf_set_info_attr(state->native, CPKT_PDF_INFO_TITLE,
                                  config->title);
  if (code == CPKT_PDF_OK && config->author != NULL)
    code = cpkt_pdf_set_info_attr(state->native, CPKT_PDF_INFO_AUTHOR,
                                  config->author);
  if (code == CPKT_PDF_OK && config->subject != NULL)
    code = cpkt_pdf_set_info_attr(state->native, CPKT_PDF_INFO_SUBJECT,
                                  config->subject);
  status = native_status(state, code, error);
  if (status != VECTIS_OK) {
    vectis_pdf_close(receiver, NULL);
    return status;
  }
  *out = receiver;
  return VECTIS_OK;
}
vectis_status vectis_pdf_add_page(vectis_pdf *document,
                                  const vectis_pdf_page_config *config,
                                  vectis_pdf_page **out, vectis_error *error) {
  vectis_pdf_page_config defaults;
  page_owner *page;
  pdf_state *state;
  CPKT_PDF_STATUS code;
  vectis_status status;
  if (out != NULL)
    *out = NULL;
  vectis_pdf_page_config_init(&defaults);
  if (config == NULL)
    config = &defaults;
  if (document == NULL || out == NULL || !real_value(config->width) ||
      !real_value(config->height) || config->width < CPKT_PDF_MIN_PAGE_SIZE ||
      config->width > CPKT_PDF_MAX_PAGE_SIZE ||
      config->height < CPKT_PDF_MIN_PAGE_SIZE ||
      config->height > CPKT_PDF_MAX_PAGE_SIZE)
    return pdf_error(error, VECTIS_ERR_INVALID, "invalid PDF page dimensions");
  state = (pdf_state *)document->impl;
  status = check(state, error);
  if (status != VECTIS_OK)
    return status;
  page = (page_owner *)calloc(1u, sizeof(*page));
  if (page == NULL)
    return pdf_error(error, VECTIS_ERR_NOMEM, "PDF page allocation failed");
  page->child.native = cpkt_pdf_add_page(state->native);
  code = page->child.native == NULL
             ? cpkt_pdf_get_error(state->native)
             : cpkt_pdf_page_set_width(page->child.native,
                                       (CPKT_PDF_REAL)config->width);
  if (code == CPKT_PDF_OK && page->child.native != NULL)
    code = cpkt_pdf_page_set_height(page->child.native,
                                    (CPKT_PDF_REAL)config->height);
  status = native_status(state, code, error);
  if (status != VECTIS_OK || page->child.native == NULL) {
    free(page);
    return status != VECTIS_OK
               ? status
               : pdf_error(error, VECTIS_ERR_STATE, "libHaru returned no page");
  }
  page->child.owner = state;
  state->references++;
  page->receiver.impl = &page->child;
  page->receiver.text = vectis_pdf_page_text;
  page->receiver.line = vectis_pdf_page_line;
  page->receiver.rectangle = vectis_pdf_page_rectangle;
  page->receiver.image = vectis_pdf_page_image;
  page->receiver.close = vectis_pdf_page_close;
  *out = &page->receiver;
  return VECTIS_OK;
}
static vectis_status font(vectis_pdf *document, const char *name,
                          const char *encoding, int file, int embed,
                          vectis_pdf_font **out, vectis_error *error) {
  pdf_state *state;
  font_owner *result;
  vectis_status status;
  if (out != NULL)
    *out = NULL;
  if (document == NULL || out == NULL || name == NULL || name[0] == '\0')
    return pdf_error(error, VECTIS_ERR_INVALID,
                     "PDF font name/path is required");
  state = (pdf_state *)document->impl;
  status = check(state, error);
  if (status != VECTIS_OK)
    return status;
  result = (font_owner *)calloc(1u, sizeof(*result));
  if (result == NULL)
    return pdf_error(error, VECTIS_ERR_NOMEM, "PDF font allocation failed");
  if (file)
    name = cpkt_pdf_load_tt_font_from_file(state->native, name, embed != 0);
  if (name != NULL)
    result->child.native = cpkt_pdf_get_font(state->native, name, encoding);
  status = native_status(state, cpkt_pdf_get_error(state->native), error);
  if (status != VECTIS_OK || result->child.native == NULL) {
    free(result);
    return status != VECTIS_OK
               ? status
               : pdf_error(error, VECTIS_ERR_STATE, "libHaru returned no font");
  }
  result->child.owner = state;
  state->references++;
  result->receiver.impl = &result->child;
  result->receiver.close = vectis_pdf_font_close;
  *out = &result->receiver;
  return VECTIS_OK;
}
vectis_status vectis_pdf_get_font(vectis_pdf *document, const char *name,
                                  const char *encoding, vectis_pdf_font **out,
                                  vectis_error *error) {
  return font(document, name, encoding, 0, 0, out, error);
}
vectis_status vectis_pdf_load_font(vectis_pdf *document, const char *path,
                                   const char *encoding, int embed,
                                   vectis_pdf_font **out, vectis_error *error) {
  return font(document, path, encoding, 1, embed, out, error);
}
static vectis_status load_image(vectis_pdf *document, const void *input,
                                size_t input_size, int file, const char *format,
                                vectis_pdf_image **out, vectis_error *error) {
  pdf_state *state;
  image_owner *image;
  vectis_status status;
  if (out != NULL)
    *out = NULL;
  if (document == NULL || input == NULL || format == NULL || out == NULL ||
      (!file &&
       (input_size == 0u || input_size > VECTIS_PDF_DEFAULT_MAX_BYTES)) ||
      (strcmp(format, "png") != 0 && strcmp(format, "jpeg") != 0))
    return pdf_error(error, VECTIS_ERR_INVALID,
                     "PDF image requires a path or bounded encoded bytes and "
                     "png/jpeg format");
  state = (pdf_state *)document->impl;
  status = check(state, error);
  if (status != VECTIS_OK)
    return status;
  image = (image_owner *)calloc(1u, sizeof(*image));
  if (image == NULL)
    return pdf_error(error, VECTIS_ERR_NOMEM, "PDF image allocation failed");
  if (file)
    image->child.native = strcmp(format, "png") == 0
                              ? cpkt_pdf_load_png_image_from_file(
                                    state->native, (const char *)input)
                              : cpkt_pdf_load_jpeg_image_from_file(
                                    state->native, (const char *)input);
  else
    image->child.native =
        strcmp(format, "png") == 0
            ? cpkt_pdf_load_png_image_from_mem(state->native,
                                               (const CPKT_PDF_BYTE *)input,
                                               (CPKT_PDF_UINT)input_size)
            : cpkt_pdf_load_jpeg_image_from_mem(state->native,
                                                (const CPKT_PDF_BYTE *)input,
                                                (CPKT_PDF_UINT)input_size);
  status = native_status(state, cpkt_pdf_get_error(state->native), error);
  if (status != VECTIS_OK || image->child.native == NULL) {
    free(image);
    return status != VECTIS_OK ? status
                               : pdf_error(error, VECTIS_ERR_STATE,
                                           "libHaru returned no image");
  }
  image->child.owner = state;
  image->receiver.width = cpkt_pdf_image_get_width(image->child.native);
  image->receiver.height = cpkt_pdf_image_get_height(image->child.native);
  status = native_status(state, cpkt_pdf_get_error(state->native), error);
  if (status != VECTIS_OK) {
    free(image);
    return status;
  }
  image->receiver.impl = &image->child;
  image->receiver.close = vectis_pdf_image_close;
  state->references++;
  *out = &image->receiver;
  return VECTIS_OK;
}
vectis_status vectis_pdf_load_image(vectis_pdf *document, const char *path,
                                    const char *format, vectis_pdf_image **out,
                                    vectis_error *error) {
  return load_image(document, path, 0u, 1, format, out, error);
}
vectis_status vectis_pdf_load_image_materialized(vectis_pdf *document,
                                                 const void *data, size_t size,
                                                 const char *format,
                                                 vectis_pdf_image **out,
                                                 vectis_error *error) {
  return load_image(document, data, size, 0, format, out, error);
}
vectis_status vectis_pdf_page_text(vectis_pdf_page *page, double x, double y,
                                   const char *text,
                                   const vectis_pdf_text_config *config,
                                   vectis_error *error) {
  vectis_pdf_text_config defaults;
  pdf_child *child, *selected;
  pdf_state *state;
  CPKT_PDF_Font selected_font;
  CPKT_PDF_STATUS code, ended, saved_detail;
  vectis_status status;
  vectis_pdf_text_config_init(&defaults);
  if (config == NULL)
    config = &defaults;
  if (page == NULL || text == NULL ||
      strlen(text) > VECTIS_PDF_DEFAULT_MAX_BYTES || !real_value(x) ||
      !real_value(y) || !real_value(config->size) || config->size <= 0.0 ||
      !color_valid(config->color))
    return pdf_error(error, VECTIS_ERR_INVALID,
                     "invalid PDF text/position/style");
  child = (pdf_child *)page->impl;
  state = child->owner;
  status = check(state, error);
  if (status != VECTIS_OK)
    return status;
  if (config->font != NULL) {
    selected = (pdf_child *)config->font->impl;
    if (selected->owner != state)
      return pdf_error(error, VECTIS_ERR_INVALID,
                       "font belongs to another PDF document");
    selected_font = selected->native;
  } else
    selected_font = cpkt_pdf_get_font(state->native, "Helvetica", NULL);
  if (selected_font == NULL) {
    status = native_status(state, cpkt_pdf_get_error(state->native), error);
    return status != VECTIS_OK ? status
                               : pdf_error(error, VECTIS_ERR_STATE,
                                           "libHaru returned no default font");
  }
  code = cpkt_pdf_page_begin_text(child->native);
  if (code != CPKT_PDF_OK)
    return native_status(state, code, error);
  code = cpkt_pdf_page_set_font_and_size(child->native, selected_font,
                                         (CPKT_PDF_REAL)config->size);
  if (code == CPKT_PDF_OK)
    code = cpkt_pdf_page_set_rgb_fill(
        child->native, (CPKT_PDF_REAL)config->color[0],
        (CPKT_PDF_REAL)config->color[1], (CPKT_PDF_REAL)config->color[2]);
  if (code == CPKT_PDF_OK)
    code = cpkt_pdf_page_text_out(child->native, (CPKT_PDF_REAL)x,
                                  (CPKT_PDF_REAL)y, text);
  saved_detail = state->detail;
  ended = cpkt_pdf_page_end_text(child->native);
  if (code == CPKT_PDF_OK)
    code = ended;
  else {
    state->code = code;
    state->detail = saved_detail;
  }
  return native_status(state, code, error);
}
static vectis_status draw(vectis_pdf_page *page, double x, double y, double a,
                          double b, const vectis_pdf_draw_config *config,
                          int rectangle, vectis_error *error) {
  vectis_pdf_draw_config defaults;
  pdf_child *child;
  pdf_state *state;
  vectis_status status;
  CPKT_PDF_STATUS code;
  vectis_pdf_draw_config_init(&defaults);
  if (config == NULL)
    config = &defaults;
  if (page == NULL || !real_value(x) || !real_value(y) || !real_value(a) ||
      !real_value(b) || !real_value(config->line_width) ||
      config->line_width < 0.0 || !color_valid(config->color) ||
      (!config->fill && !config->stroke) ||
      (!rectangle && (config->fill || !config->stroke)) ||
      (rectangle && (a <= 0.0 || b <= 0.0)))
    return pdf_error(error, VECTIS_ERR_INVALID,
                     "invalid PDF shape/position/style");
  child = (pdf_child *)page->impl;
  state = child->owner;
  status = check(state, error);
  if (status != VECTIS_OK)
    return status;
  code = cpkt_pdf_page_set_line_width(child->native,
                                      (CPKT_PDF_REAL)config->line_width);
  if (code == CPKT_PDF_OK)
    code = cpkt_pdf_page_set_rgb_stroke(
        child->native, (CPKT_PDF_REAL)config->color[0],
        (CPKT_PDF_REAL)config->color[1], (CPKT_PDF_REAL)config->color[2]);
  if (code == CPKT_PDF_OK)
    code = cpkt_pdf_page_set_rgb_fill(
        child->native, (CPKT_PDF_REAL)config->color[0],
        (CPKT_PDF_REAL)config->color[1], (CPKT_PDF_REAL)config->color[2]);
  if (code == CPKT_PDF_OK && rectangle)
    code = cpkt_pdf_page_rectangle(child->native, (CPKT_PDF_REAL)x,
                                   (CPKT_PDF_REAL)y, (CPKT_PDF_REAL)a,
                                   (CPKT_PDF_REAL)b);
  if (code == CPKT_PDF_OK && !rectangle) {
    code = cpkt_pdf_page_move_to(child->native, (CPKT_PDF_REAL)x,
                                 (CPKT_PDF_REAL)y);
    if (code == CPKT_PDF_OK)
      code = cpkt_pdf_page_line_to(child->native, (CPKT_PDF_REAL)a,
                                   (CPKT_PDF_REAL)b);
  }
  if (code == CPKT_PDF_OK) {
    if (config->fill && config->stroke)
      code = cpkt_pdf_page_fill_stroke(child->native);
    else if (config->fill)
      code = cpkt_pdf_page_fill(child->native);
    else
      code = cpkt_pdf_page_stroke(child->native);
  }
  return native_status(state, code, error);
}
vectis_status vectis_pdf_page_line(vectis_pdf_page *page, double x, double y,
                                   double a, double b,
                                   const vectis_pdf_draw_config *config,
                                   vectis_error *error) {
  return draw(page, x, y, a, b, config, 0, error);
}
vectis_status vectis_pdf_page_rectangle(vectis_pdf_page *page, double x,
                                        double y, double width, double height,
                                        const vectis_pdf_draw_config *config,
                                        vectis_error *error) {
  return draw(page, x, y, width, height, config, 1, error);
}
vectis_status vectis_pdf_page_image(vectis_pdf_page *page,
                                    vectis_pdf_image *image, double x, double y,
                                    double width, double height,
                                    vectis_error *error) {
  pdf_child *p, *i;
  vectis_status status;
  if (page == NULL || image == NULL || !real_value(x) || !real_value(y) ||
      !real_value(width) || !real_value(height) || width <= 0.0 ||
      height <= 0.0)
    return pdf_error(error, VECTIS_ERR_INVALID,
                     "invalid PDF image position/size");
  p = (pdf_child *)page->impl;
  i = (pdf_child *)image->impl;
  status = check(p->owner, error);
  if (status != VECTIS_OK)
    return status;
  if (p->owner != i->owner)
    return pdf_error(error, VECTIS_ERR_INVALID,
                     "image belongs to another PDF document");
  return native_status(
      p->owner,
      cpkt_pdf_page_draw_image(p->native, i->native, (CPKT_PDF_REAL)x,
                               (CPKT_PDF_REAL)y, (CPKT_PDF_REAL)width,
                               (CPKT_PDF_REAL)height),
      error);
}
vectis_status vectis_pdf_save(vectis_pdf *document, const char *path,
                              vectis_error *error) {
  pdf_state *state;
  vectis_status status;
  if (document == NULL || path == NULL || path[0] == '\0')
    return pdf_error(error, VECTIS_ERR_INVALID, "PDF output path is required");
  state = (pdf_state *)document->impl;
  status = check(state, error);
  return status != VECTIS_OK
             ? status
             : native_status(state, cpkt_pdf_save_to_file(state->native, path),
                             error);
}
vectis_status vectis_pdf_bytes_materialized(vectis_pdf *document, size_t limit,
                                            vectis_mutable_bytes *out,
                                            vectis_error *error) {
  pdf_state *state;
  vectis_status status;
  CPKT_PDF_UINT32 size, read_size;
  void *data;
  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (document == NULL || out == NULL)
    return pdf_error(error, VECTIS_ERR_INVALID,
                     "PDF document/output are required");
  state = (pdf_state *)document->impl;
  status = check(state, error);
  if (status != VECTIS_OK)
    return status;
  status = native_status(state, cpkt_pdf_save_to_stream(state->native), error);
  if (status != VECTIS_OK)
    return status;
  size = cpkt_pdf_get_stream_size(state->native);
  if (limit == 0u)
    limit = VECTIS_PDF_DEFAULT_MAX_BYTES;
  if ((size_t)size > limit)
    return pdf_error(error, VECTIS_ERR_INVALID,
                     "materialized PDF byte limit exceeded");
  data = malloc(size == 0u ? 1u : (size_t)size);
  if (data == NULL)
    return pdf_error(error, VECTIS_ERR_NOMEM,
                     "PDF byte copy allocation failed");
  read_size = size;
  status = native_status(state, cpkt_pdf_reset_stream(state->native), error);
  if (status == VECTIS_OK)
    status =
        native_status(state,
                      cpkt_pdf_read_from_stream(
                          state->native, (CPKT_PDF_BYTE *)data, &read_size),
                      error);
  if (status != VECTIS_OK || read_size != size) {
    free(data);
    return status != VECTIS_OK ? status
                               : pdf_error(error, VECTIS_ERR_STATE,
                                           "libHaru PDF byte count mismatch");
  }
  out->data = data;
  out->size = size;
  return VECTIS_OK;
}
vectis_status vectis_pdf_reset_error(vectis_pdf *document,
                                     vectis_error *error) {
  pdf_state *state;
  vectis_status status;
  if (document == NULL)
    return pdf_error(error, VECTIS_ERR_INVALID, "PDF document is required");
  state = (pdf_state *)document->impl;
  status = affinity(state, error);
  if (status != VECTIS_OK)
    return status;
  cpkt_pdf_reset_error(state->native);
  state->code = state->detail = 0;
  return VECTIS_OK;
}
vectis_status vectis_pdf_close(vectis_pdf *document, vectis_error *error) {
  pdf_state *state;
  vectis_status status;
  if (document == NULL)
    return VECTIS_OK;
  state = (pdf_state *)document->impl;
  status = affinity(state, error);
  if (status != VECTIS_OK)
    return status;
  cpkt_pdf_free(state->native);
  state->native = NULL;
  free(document);
  unref(state);
  return VECTIS_OK;
}
void vectis_pdf_page_close(vectis_pdf_page *page) {
  pdf_state *state;
  if (page == NULL)
    return;
  state = ((pdf_child *)page->impl)->owner;
  free(page);
  unref(state);
}
void vectis_pdf_font_close(vectis_pdf_font *font_receiver) {
  pdf_state *state;
  if (font_receiver == NULL)
    return;
  state = ((pdf_child *)font_receiver->impl)->owner;
  free(font_receiver);
  unref(state);
}
void vectis_pdf_image_close(vectis_pdf_image *image) {
  pdf_state *state;
  if (image == NULL)
    return;
  state = ((pdf_child *)image->impl)->owner;
  free(image);
  unref(state);
}
