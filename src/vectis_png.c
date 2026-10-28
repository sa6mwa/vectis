#include <limits.h>
#include <png.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vectis/png.h>

#define PNG_TEXT_COUNT 16u
#define PNG_TEXT_BYTES 65536u
typedef struct png_state {
  vectis_png receiver;
  png_structp native;
  png_infop header;
  FILE *file;
  vectis_png_read_fn read;
  vectis_png_write_fn write;
  void *context;
  vectis_png_info info;
  vectis_png_text text[PNG_TEXT_COUNT];
  vectis_png_limits limits;
  vectis_error failure;
  unsigned char *row;
  char *metadata;
  unsigned int rows;
  int writing, busy, violation, finished, passes;
  pid_t process;
  pthread_t thread;
} png_state;
static vectis_status png_failure(vectis_error *error, vectis_status status,
                                 const char *message) {
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->code = status;
    error->source = VECTIS_ERROR_SOURCE_CPKT;
    strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return status;
}
void vectis_png_limits_init(vectis_png_limits *limits) {
  if (limits != NULL) {
    limits->max_width = limits->max_height = 16384u;
    limits->max_row_bytes = 65536u;
    limits->max_bytes = 16777216u;
  }
}
static void png_native_error(png_structp native, png_const_charp message) {
  png_state *state = (png_state *)png_get_error_ptr(native);
  if (state->failure.code == VECTIS_OK)
    png_failure(&state->failure, VECTIS_ERR_STATE, message);
  png_longjmp(native, 1);
}
static void png_native_warning(png_structp native, png_const_charp message) {
  (void)native;
  (void)message;
}
static void png_read_input(png_structp native, png_bytep buffer,
                           png_size_t count) {
  png_state *state = (png_state *)png_get_io_ptr(native);
  size_t capacity, size;
  vectis_status status;
  while (count != 0u) {
    capacity = count > 65536u ? 65536u : (size_t)count;
    size = 0u;
    if (state->file != NULL) {
      size = fread(buffer, 1u, capacity, state->file);
      status = ferror(state->file)
                   ? png_failure(&state->failure, VECTIS_ERR_STATE,
                                 "reading PNG file")
                   : VECTIS_OK;
    } else
      status =
          state->read(state->context, buffer, capacity, &size, &state->failure);
    if (status != VECTIS_OK || size == 0u || size > capacity ||
        state->violation) {
      if (state->failure.code == VECTIS_OK)
        png_failure(&state->failure,
                    status == VECTIS_OK ? VECTIS_ERR_STATE : status,
                    "PNG source failed, truncated or reentered receiver");
      png_error(native, "PNG source failed");
    }
    buffer += size;
    count -= (png_size_t)size;
  }
}
static void png_write_output(png_structp native, png_bytep buffer,
                             png_size_t count) {
  png_state *state = (png_state *)png_get_io_ptr(native);
  size_t size;
  vectis_status status;
  while (count != 0u) {
    size = count > 65536u ? 65536u : (size_t)count;
    if (state->file != NULL)
      status = fwrite(buffer, 1u, size, state->file) == size
                   ? VECTIS_OK
                   : png_failure(&state->failure, VECTIS_ERR_STATE,
                                 "writing PNG file");
    else
      status = state->write(state->context, buffer, size, &state->failure);
    if (status != VECTIS_OK || state->violation) {
      if (state->failure.code == VECTIS_OK)
        png_failure(&state->failure,
                    status == VECTIS_OK ? VECTIS_ERR_STATE : status,
                    "PNG sink failed or reentered receiver");
      png_error(native, "PNG sink failed");
    }
    buffer += size;
    count -= (png_size_t)size;
  }
}
static void png_flush_output(png_structp native) { (void)native; }
static vectis_status png_check(png_state *state, vectis_error *error) {
  if (state == NULL)
    return png_failure(error, VECTIS_ERR_INVALID, "missing PNG receiver");
  if (state->process != getpid() ||
      !pthread_equal(state->thread, pthread_self()))
    return png_failure(error, VECTIS_ERR_STATE,
                       "PNG receiver belongs to another process/thread");
  if (state->busy) {
    state->violation = 1;
    return png_failure(error, VECTIS_ERR_STATE,
                       "cannot reenter or close active PNG receiver");
  }
  if (state->failure.code != VECTIS_OK) {
    if (error != NULL)
      *error = state->failure;
    return state->failure.code;
  }
  return VECTIS_OK;
}
static vectis_status png_report(png_state *state, vectis_error *error) {
  state->busy = 0;
  if (error != NULL)
    *error = state->failure;
  return state->failure.code;
}
static void png_release(png_state *state) {
  if (state->native != NULL) {
    if (state->writing)
      png_destroy_write_struct(&state->native, &state->header);
    else
      png_destroy_read_struct(&state->native, &state->header, NULL);
  }
  free(state->row);
  free(state->metadata);
  free(state);
}
vectis_status vectis_png_close(vectis_png *receiver, vectis_error *error) {
  png_state *state;
  int result = 0;
  if (receiver == NULL)
    return VECTIS_OK;
  state = (png_state *)receiver->impl;
  if (state->busy) {
    state->violation = 1;
    return png_failure(error, VECTIS_ERR_STATE,
                       "cannot close active PNG receiver");
  }
  if (state->process == getpid() &&
      !pthread_equal(state->thread, pthread_self()))
    return png_failure(error, VECTIS_ERR_STATE,
                       "cannot close PNG receiver on foreign thread");
  if (state->file != NULL)
    result = fclose(state->file);
  png_release(state);
  return result == 0 ? VECTIS_OK
                     : png_failure(error, VECTIS_ERR_STATE, "closing PNG file");
}
static vectis_status png_geometry(vectis_png_info *info,
                                  const vectis_png_limits *limits,
                                  vectis_error *error) {
  size_t channels = info->format == VECTIS_PNG_RGBA8   ? 4u
                    : info->format == VECTIS_PNG_RGB8  ? 3u
                    : info->format == VECTIS_PNG_GRAY8 ? 1u
                                                       : 0u;
  if (info->width == 0u || info->height == 0u ||
      info->width > limits->max_width || info->height > limits->max_height ||
      info->width > PNG_UINT_31_MAX || info->height > PNG_UINT_31_MAX ||
      channels == 0u || info->width > (size_t)-1 / channels)
    return png_failure(
        error, VECTIS_ERR_INVALID,
        "invalid PNG dimensions/format or dimension limit exceeded");
  info->row_bytes = (size_t)info->width * channels;
  if (info->row_bytes > limits->max_row_bytes)
    return png_failure(error, VECTIS_ERR_INVALID,
                       "PNG row exceeds max_row_bytes");
  return VECTIS_OK;
}
static vectis_status png_metadata_valid(const vectis_png_info *info,
                                        vectis_error *error) {
  size_t i, key, value, total = 0u;
  if (info->text_count > PNG_TEXT_COUNT ||
      (info->text_count != 0u && info->text == NULL))
    return png_failure(error, VECTIS_ERR_INVALID,
                       "PNG text entry limit exceeded");
  for (i = 0u; i < info->text_count; ++i) {
    if (info->text[i].key == NULL || info->text[i].value == NULL)
      return png_failure(error, VECTIS_ERR_INVALID,
                         "missing PNG text key/value");
    key = strlen(info->text[i].key);
    value = strlen(info->text[i].value);
    if (key == 0u || key > 79u || key > PNG_TEXT_BYTES - total ||
        value > PNG_TEXT_BYTES - total - key)
      return png_failure(
          error, VECTIS_ERR_INVALID,
          "invalid PNG text key or metadata byte limit exceeded");
    total += key + value;
  }
  if ((info->physical_present != 0 && info->physical_present != 1) ||
      (info->physical_meter != 0 && info->physical_meter != 1) ||
      info->physical_x > PNG_UINT_31_MAX || info->physical_y > PNG_UINT_31_MAX)
    return png_failure(error, VECTIS_ERR_INVALID,
                       "invalid PNG physical resolution");
  return VECTIS_OK;
}
static vectis_status png_metadata_read(png_state *state, vectis_error *error) {
  png_textp text = NULL;
  int count = 0, i, unit;
  png_uint_32 x, y;
  (void)png_get_text(state->native, state->header, &text, &count);
  if (count < 0 || count > (int)PNG_TEXT_COUNT)
    return png_failure(error, VECTIS_ERR_INVALID,
                       "PNG text entry limit exceeded");
  for (i = 0; i < count; ++i) {
    state->text[i].key = text[i].key;
    state->text[i].value = text[i].text == NULL ? "" : text[i].text;
  }
  state->info.text = state->text;
  state->info.text_count = (size_t)count;
  state->info.physical_present =
      png_get_pHYs(state->native, state->header, &x, &y, &unit) != 0;
  if (state->info.physical_present) {
    state->info.physical_x = x;
    state->info.physical_y = y;
    state->info.physical_meter = unit == PNG_RESOLUTION_METER;
  }
  return png_metadata_valid(&state->info, error);
}
static vectis_status png_create(int writing, const char *path,
                                vectis_png_read_fn read,
                                vectis_png_write_fn write, void *context,
                                const vectis_png_info *image,
                                const vectis_png_limits *limits, int interlace,
                                vectis_png **out, vectis_error *error) {
  vectis_png_limits defaults;
  vectis_png_info geometry;
  png_state *state;
  png_text text[PNG_TEXT_COUNT];
  size_t i;
  char *copy;
  int color, depth, alpha;
  vectis_status status;
  if (out != NULL)
    *out = NULL;
  vectis_png_limits_init(&defaults);
  if (limits == NULL)
    limits = &defaults;
  if (out == NULL ||
      (path == NULL && (writing ? write == NULL : read == NULL)) ||
      (writing && image == NULL) || limits->max_width == 0u ||
      limits->max_height == 0u || limits->max_row_bytes == 0u ||
      limits->max_bytes == 0u)
    return png_failure(
        error, VECTIS_ERR_INVALID,
        "missing PNG path/callback/configuration or invalid limits");
  if (writing) {
    geometry = *image;
    status = png_geometry(&geometry, limits, error);
    if (status != VECTIS_OK)
      return status;
    status = png_metadata_valid(&geometry, error);
    if (status != VECTIS_OK)
      return status;
  }
  state = (png_state *)calloc(1u, sizeof(*state));
  if (state == NULL)
    return png_failure(error, VECTIS_ERR_NOMEM, "allocating PNG receiver");
  state->writing = writing;
  state->read = read;
  state->write = write;
  state->context = context;
  state->limits = *limits;
  state->process = getpid();
  state->thread = pthread_self();
  if (writing && geometry.text_count != 0u) {
    state->metadata = (char *)malloc(PNG_TEXT_BYTES + PNG_TEXT_COUNT * 2u);
    if (state->metadata == NULL) {
      png_release(state);
      return png_failure(error, VECTIS_ERR_NOMEM, "copying PNG metadata");
    }
  }
  if (writing)
    state->native = png_create_write_struct(
        PNG_LIBPNG_VER_STRING, state, png_native_error, png_native_warning);
  else
    state->native = png_create_read_struct(
        PNG_LIBPNG_VER_STRING, state, png_native_error, png_native_warning);
  if (state->native == NULL) {
    png_release(state);
    return png_failure(error, VECTIS_ERR_NOMEM, "allocating native PNG state");
  }
  state->header = png_create_info_struct(state->native);
  if (state->header == NULL) {
    png_release(state);
    return png_failure(error, VECTIS_ERR_NOMEM, "allocating PNG metadata");
  }
  if (path != NULL) {
    state->file = fopen(path, writing ? "wb" : "rb");
    if (state->file == NULL) {
      png_release(state);
      return png_failure(error, VECTIS_ERR_STATE, "opening PNG file");
    }
  }
  if (setjmp(png_jmpbuf(state->native))) {
    status = png_report(state, error);
    if (state->file != NULL)
      fclose(state->file);
    png_release(state);
    return status;
  }
  state->busy = 1;
  if (writing) {
    state->info = geometry;
    color = geometry.format == VECTIS_PNG_RGBA8  ? PNG_COLOR_TYPE_RGBA
            : geometry.format == VECTIS_PNG_RGB8 ? PNG_COLOR_TYPE_RGB
                                                 : PNG_COLOR_TYPE_GRAY;
    state->info.bit_depth = 8;
    state->info.color_type = color;
    state->info.interlaced = 0;
    png_set_write_fn(state->native, state, png_write_output, png_flush_output);
    png_set_IHDR(state->native, state->header, geometry.width, geometry.height,
                 8, color, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    memset(text, 0, sizeof(text));
    copy = state->metadata;
    for (i = 0u; i < geometry.text_count; ++i) {
      text[i].compression = PNG_TEXT_COMPRESSION_NONE;
      text[i].key = copy;
      memcpy(copy, geometry.text[i].key, strlen(geometry.text[i].key) + 1u);
      copy += strlen(geometry.text[i].key) + 1u;
      text[i].text = copy;
      memcpy(copy, geometry.text[i].value, strlen(geometry.text[i].value) + 1u);
      copy += strlen(geometry.text[i].value) + 1u;
    }
    if (geometry.text_count != 0u)
      png_set_text(state->native, state->header, text,
                   (int)geometry.text_count);
    if (geometry.physical_present)
      png_set_pHYs(state->native, state->header, geometry.physical_x,
                   geometry.physical_y,
                   geometry.physical_meter ? PNG_RESOLUTION_METER
                                           : PNG_RESOLUTION_UNKNOWN);
    png_write_info(state->native, state->header);
  } else {
    png_set_read_fn(state->native, state, png_read_input);
    png_set_user_limits(state->native, state->limits.max_width,
                        state->limits.max_height);
    png_set_chunk_malloc_max(state->native, PNG_TEXT_BYTES);
    png_set_chunk_cache_max(state->native, PNG_TEXT_COUNT + 2u);
    png_read_info(state->native, state->header);
    state->info.width = png_get_image_width(state->native, state->header);
    state->info.height = png_get_image_height(state->native, state->header);
    depth = png_get_bit_depth(state->native, state->header);
    color = png_get_color_type(state->native, state->header);
    state->info.bit_depth = depth;
    state->info.color_type = color;
    state->info.interlaced =
        png_get_interlace_type(state->native, state->header) !=
        PNG_INTERLACE_NONE;
    state->info.format = VECTIS_PNG_RGBA8;
    if (state->info.interlaced && !interlace)
      png_error(state->native, "interlaced PNG requires decode_materialized");
    if (png_geometry(&state->info, &state->limits, &state->failure) !=
        VECTIS_OK)
      png_error(state->native, "PNG dimensions exceed limits");
    alpha = (color & PNG_COLOR_MASK_ALPHA) != 0 ||
            png_get_valid(state->native, state->header, PNG_INFO_tRNS) != 0;
    if (color == PNG_COLOR_TYPE_PALETTE)
      png_set_palette_to_rgb(state->native);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8)
      png_set_expand_gray_1_2_4_to_8(state->native);
    if (png_get_valid(state->native, state->header, PNG_INFO_tRNS))
      png_set_tRNS_to_alpha(state->native);
    if (depth == 16)
      png_set_scale_16(state->native);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
      png_set_gray_to_rgb(state->native);
    if (!alpha)
      png_set_add_alpha(state->native, 255u, PNG_FILLER_AFTER);
    state->passes = png_set_interlace_handling(state->native);
    png_read_update_info(state->native, state->header);
    if (png_get_rowbytes(state->native, state->header) != state->info.row_bytes)
      png_error(state->native, "unexpected transformed PNG row layout");
    state->row = (unsigned char *)malloc(state->info.row_bytes);
    if (state->row == NULL) {
      png_failure(&state->failure, VECTIS_ERR_NOMEM, "allocating PNG row");
      png_error(state->native, "allocating PNG row");
    }
  }
  if (png_metadata_read(state, &state->failure) != VECTIS_OK)
    png_error(state->native, "PNG metadata exceeds limits");
  state->busy = 0;
  free(state->metadata);
  state->metadata = NULL;
  state->receiver.info = vectis_png_get_info;
  state->receiver.next_row = vectis_png_next_row;
  state->receiver.write_row = vectis_png_write_row;
  state->receiver.finish = vectis_png_finish;
  state->receiver.close = vectis_png_close;
  state->receiver.impl = state;
  *out = &state->receiver;
  return VECTIS_OK;
}
vectis_status vectis_png_reader(vectis_png_read_fn read, void *context,
                                const vectis_png_limits *limits,
                                vectis_png **out, vectis_error *error) {
  return png_create(0, NULL, read, NULL, context, NULL, limits, 0, out, error);
}
vectis_status vectis_png_reader_file(const char *path,
                                     const vectis_png_limits *limits,
                                     vectis_png **out, vectis_error *error) {
  return png_create(0, path, NULL, NULL, NULL, NULL, limits, 0, out, error);
}
vectis_status vectis_png_writer(vectis_png_write_fn write, void *context,
                                const vectis_png_info *image,
                                const vectis_png_limits *limits,
                                vectis_png **out, vectis_error *error) {
  return png_create(1, NULL, NULL, write, context, image, limits, 0, out,
                    error);
}
vectis_status vectis_png_writer_file(const char *path,
                                     const vectis_png_info *image,
                                     const vectis_png_limits *limits,
                                     vectis_png **out, vectis_error *error) {
  return png_create(1, path, NULL, NULL, NULL, image, limits, 0, out, error);
}
vectis_status vectis_png_get_info(vectis_png *receiver, vectis_png_info *info,
                                  vectis_error *error) {
  png_state *state = receiver == NULL ? NULL : (png_state *)receiver->impl;
  vectis_status status = png_check(state, error);
  if (status != VECTIS_OK)
    return status;
  if (info == NULL)
    return png_failure(error, VECTIS_ERR_INVALID, "missing PNG info output");
  *info = state->info;
  return VECTIS_OK;
}
vectis_status vectis_png_finish(vectis_png *receiver, vectis_error *error) {
  png_state *state = receiver == NULL ? NULL : (png_state *)receiver->impl;
  vectis_status status = png_check(state, error);
  if (status != VECTIS_OK)
    return status;
  if (state->finished)
    return VECTIS_OK;
  if (state->rows != state->info.height)
    return png_failure(error, VECTIS_ERR_STATE, "PNG finish requires all rows");
  if (setjmp(png_jmpbuf(state->native)))
    return png_report(state, error);
  state->busy = 1;
  if (state->writing) {
    png_write_end(state->native, state->header);
    if (state->file != NULL && fflush(state->file) != 0)
      png_error(state->native, "flushing PNG output file");
  } else
    png_read_end(state->native, state->header);
  if (png_metadata_read(state, &state->failure) != VECTIS_OK)
    png_error(state->native, "PNG metadata exceeds limits");
  state->finished = 1;
  state->busy = 0;
  return VECTIS_OK;
}
vectis_status vectis_png_next_row(vectis_png *receiver, const void **data,
                                  size_t *size, vectis_error *error) {
  png_state *state = receiver == NULL ? NULL : (png_state *)receiver->impl;
  vectis_status status;
  if (data != NULL)
    *data = NULL;
  if (size != NULL)
    *size = 0u;
  status = png_check(state, error);
  if (status != VECTIS_OK)
    return status;
  if (data == NULL || size == NULL || state->writing || state->info.interlaced)
    return png_failure(error, VECTIS_ERR_INVALID,
                       "PNG next_row requires noninterlaced reader and output");
  if (state->rows == state->info.height)
    return vectis_png_finish(receiver, error);
  if (setjmp(png_jmpbuf(state->native)))
    return png_report(state, error);
  state->busy = 1;
  png_read_row(state->native, state->row, NULL);
  ++state->rows;
  state->busy = 0;
  *data = state->row;
  *size = state->info.row_bytes;
  return VECTIS_OK;
}
vectis_status vectis_png_write_row(vectis_png *receiver, const void *data,
                                   size_t size, vectis_error *error) {
  png_state *state = receiver == NULL ? NULL : (png_state *)receiver->impl;
  vectis_status status = png_check(state, error);
  if (status != VECTIS_OK)
    return status;
  if (!state->writing || data == NULL || size != state->info.row_bytes ||
      state->rows == state->info.height)
    return png_failure(
        error, VECTIS_ERR_INVALID,
        "PNG write_row requires a writer and one complete remaining row");
  if (setjmp(png_jmpbuf(state->native)))
    return png_report(state, error);
  state->busy = 1;
  png_write_row(state->native, (png_const_bytep)data);
  ++state->rows;
  state->busy = 0;
  return VECTIS_OK;
}
typedef struct png_memory {
  const unsigned char *input;
  size_t size, offset, limit;
  unsigned char *output;
} png_memory;
static vectis_status png_memory_read(void *context, void *data, size_t capacity,
                                     size_t *size, vectis_error *error) {
  png_memory *memory = (png_memory *)context;
  (void)error;
  *size = memory->size - memory->offset;
  if (*size > capacity)
    *size = capacity;
  if (*size != 0u)
    memcpy(data, memory->input + memory->offset, *size);
  memory->offset += *size;
  return VECTIS_OK;
}
static vectis_status png_memory_write(void *context, const void *data,
                                      size_t size, vectis_error *error) {
  png_memory *memory = (png_memory *)context;
  unsigned char *next;
  if (size > memory->limit - memory->size)
    return png_failure(error, VECTIS_ERR_INVALID,
                       "encoded PNG exceeds max_bytes");
  next = (unsigned char *)realloc(memory->output, memory->size + size);
  if (next == NULL)
    return png_failure(error, VECTIS_ERR_NOMEM, "allocating encoded PNG bytes");
  memcpy(next + memory->size, data, size);
  memory->output = next;
  memory->size += size;
  return VECTIS_OK;
}
void vectis_png_materialized_cleanup(vectis_png_materialized *image) {
  if (image != NULL) {
    vectis_png_close((vectis_png *)image->impl, NULL);
    vectis_mutable_bytes_cleanup(&image->pixels);
    memset(image, 0, sizeof(*image));
  }
}
vectis_status vectis_png_decode_materialized(const void *data, size_t size,
                                             const vectis_png_limits *limits,
                                             vectis_png_materialized *out,
                                             vectis_error *error) {
  png_memory memory;
  vectis_png *receiver;
  png_state *state;
  vectis_status status;
  int pass;
  unsigned int row;
  if (out == NULL)
    return png_failure(error, VECTIS_ERR_INVALID,
                       "missing materialized PNG output");
  memset(out, 0, sizeof(*out));
  memset(&memory, 0, sizeof(memory));
  if (data == NULL || size == 0u)
    return png_failure(error, VECTIS_ERR_INVALID,
                       "missing materialized PNG input");
  memory.input = (const unsigned char *)data;
  memory.size = size;
  status = png_create(0, NULL, png_memory_read, NULL, &memory, NULL, limits, 1,
                      &receiver, error);
  if (status != VECTIS_OK)
    return status;
  state = (png_state *)receiver->impl;
  if (state->info.height > state->limits.max_bytes / state->info.row_bytes) {
    vectis_png_close(receiver, NULL);
    return png_failure(error, VECTIS_ERR_INVALID,
                       "PNG pixels exceed max_bytes");
  }
  out->impl = receiver;
  out->pixels.size = state->info.row_bytes * state->info.height;
  out->pixels.data = (unsigned char *)calloc(1u, out->pixels.size);
  if (out->pixels.data == NULL) {
    vectis_png_materialized_cleanup(out);
    return png_failure(error, VECTIS_ERR_NOMEM, "allocating PNG pixels");
  }
  if (setjmp(png_jmpbuf(state->native))) {
    status = png_report(state, error);
    vectis_png_materialized_cleanup(out);
    return status;
  }
  state->busy = 1;
  for (pass = 0; pass < state->passes; ++pass)
    for (row = 0u; row < state->info.height; ++row)
      png_read_row(state->native,
                   (unsigned char *)out->pixels.data +
                       (size_t)row * state->info.row_bytes,
                   NULL);
  state->rows = state->info.height;
  state->busy = 0;
  status = vectis_png_finish(receiver, error);
  if (status != VECTIS_OK) {
    vectis_png_materialized_cleanup(out);
    return status;
  }
  out->info = state->info;
  state->read = NULL;
  state->context = NULL; /* The stack reader has completed. */
  return VECTIS_OK;
}
vectis_status vectis_png_encode_materialized(const vectis_png_info *info,
                                             const void *data, size_t size,
                                             const vectis_png_limits *limits,
                                             vectis_mutable_bytes *out,
                                             vectis_error *error) {
  vectis_png_limits defaults;
  vectis_png_info image;
  png_memory memory;
  vectis_png *writer;
  vectis_status status;
  unsigned int row;
  if (out == NULL)
    return png_failure(error, VECTIS_ERR_INVALID, "missing encoded PNG output");
  memset(out, 0, sizeof(*out));
  vectis_png_limits_init(&defaults);
  if (limits == NULL)
    limits = &defaults;
  if (info == NULL || data == NULL)
    return png_failure(error, VECTIS_ERR_INVALID, "missing PNG image/pixels");
  image = *info;
  status = png_geometry(&image, limits, error);
  if (status != VECTIS_OK)
    return status;
  if (image.height > limits->max_bytes / image.row_bytes ||
      size != image.row_bytes * image.height)
    return png_failure(error, VECTIS_ERR_INVALID,
                       "PNG packed pixel size mismatch or exceeds max_bytes");
  memset(&memory, 0, sizeof(memory));
  memory.limit = limits->max_bytes;
  status = vectis_png_writer(png_memory_write, &memory, &image, limits, &writer,
                             error);
  if (status == VECTIS_OK) {
    for (row = 0u; row < image.height && status == VECTIS_OK; ++row)
      status = vectis_png_write_row(
          writer, (const unsigned char *)data + (size_t)row * image.row_bytes,
          image.row_bytes, error);
    if (status == VECTIS_OK)
      status = vectis_png_finish(writer, error);
    vectis_png_close(writer, NULL);
  }
  if (status != VECTIS_OK) {
    free(memory.output);
    return status;
  }
  out->data = memory.output;
  out->size = memory.size;
  return VECTIS_OK;
}
