#include <assert.h>
#include <png.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vectis/png.h>
typedef struct memory {
  unsigned char data[32768];
  size_t size, offset, calls;
  int fail;
} memory;
static vectis_status write_bytes(void *context, const void *data, size_t size,
                                 vectis_error *error) {
  memory *m = (memory *)context;
  assert(size > 0u && size <= 65536u && size <= sizeof(m->data) - m->size);
  ++m->calls;
  if (m->fail) {
    memset(error, 0, sizeof(*error));
    error->code = VECTIS_ERR_CONFLICT;
    strcpy(error->message, "sink sentinel");
    return VECTIS_ERR_CONFLICT;
  }
  memcpy(m->data + m->size, data, size);
  m->size += size;
  return VECTIS_OK;
}
static vectis_status read_bytes(void *context, void *data, size_t capacity,
                                size_t *size, vectis_error *error) {
  memory *m = (memory *)context;
  assert(capacity > 0u && capacity <= 65536u);
  ++m->calls;
  if (m->fail) {
    memset(error, 0, sizeof(*error));
    error->code = VECTIS_ERR_CONFLICT;
    strcpy(error->message, "source sentinel");
    return VECTIS_ERR_CONFLICT;
  }
  *size = m->offset < m->size ? 1u : 0u;
  if (*size != 0u) {
    memcpy(data, m->data + m->offset, *size);
    m->offset += *size;
  }
  return VECTIS_OK;
}
static void fixture_write(png_structp native, png_bytep data, png_size_t size) {
  vectis_error error;
  assert(write_bytes(png_get_io_ptr(native), data, size, &error) == VECTIS_OK);
}
static void fixture_flush(png_structp native) { (void)native; }
/* Independent public libpng producer verifies palette/tRNS, interlace and
 * 16-bit gray/alpha input, rather than round-tripping only our own encoder. */
static void fixture(memory *m, int type) {
  png_structp native =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop info = png_create_info_struct(native);
  unsigned char palette_pixels[2] = {0, 1};
  unsigned char gray16[8] = {0xff, 0xff, 0x80, 0x80, 0, 0, 0xff, 0xff};
  unsigned char rgba[8] = {255, 0, 0, 127, 0, 0, 255, 255};
  png_byte *rows[1];
  png_color colors[2];
  png_byte transparency[2] = {127, 255};
  assert(native != NULL && info != NULL);
  assert(setjmp(png_jmpbuf(native)) == 0);
  memset(m, 0, sizeof(*m));
  png_set_write_fn(native, m, fixture_write, fixture_flush);
  png_set_IHDR(native, info, 2, 1, type == 2 ? 16 : 8,
               type == 0   ? PNG_COLOR_TYPE_PALETTE
               : type == 1 ? PNG_COLOR_TYPE_RGBA
                           : PNG_COLOR_TYPE_GRAY_ALPHA,
               type == 1 ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE, 0, 0);
  if (type == 0) {
    memset(colors, 0, sizeof(colors));
    colors[0].red = 255;
    colors[1].blue = 255;
    png_set_PLTE(native, info, colors, 2);
    png_set_tRNS(native, info, transparency, 2, NULL);
  }
  png_write_info(native, info);
  rows[0] = type == 0 ? palette_pixels : type == 1 ? rgba : gray16;
  png_write_image(native, rows);
  png_write_end(native, info);
  png_destroy_write_struct(&native, &info);
}
static void *foreign(void *context) {
  vectis_png *reader = (vectis_png *)context;
  vectis_png_info info;
  vectis_error error;
  assert(reader->info(reader, &info, &error) == VECTIS_ERR_STATE);
  assert(reader->close(reader, &error) == VECTIS_ERR_STATE);
  return NULL;
}
int main(void) {
  vectis_png *writer, *reader;
  vectis_png_info image, info;
  vectis_png_limits limits;
  vectis_png_text text[1];
  vectis_png_materialized decoded;
  vectis_mutable_bytes encoded;
  vectis_error error, latched;
  const void *row;
  size_t size, calls;
  memory m;
  unsigned char pixels[8] = {255, 0, 0, 127, 0, 0, 255, 255};
  int type, status;
  pthread_t thread;
  pid_t child;
  memset(&image, 0, sizeof(image));
  image.width = 2;
  image.height = 1;
  image.format = VECTIS_PNG_RGBA8;
  text[0].key = "Title";
  text[0].value = "Native report";
  image.text = text;
  image.text_count = 1u;
  image.physical_present = 1;
  image.physical_meter = 1;
  image.physical_x = image.physical_y = 3780;
  memset(&m, 0, sizeof(m));
  assert(vectis_png_writer(write_bytes, &m, &image, NULL, &writer, &error) ==
         VECTIS_OK);
  assert(writer->finish(writer, &error) == VECTIS_ERR_STATE);
  assert(writer->write_row(writer, pixels, 7u, &error) == VECTIS_ERR_INVALID);
  assert(writer->write_row(writer, pixels, sizeof(pixels), &error) ==
         VECTIS_OK);
  assert(writer->write_row(writer, pixels, sizeof(pixels), &error) ==
         VECTIS_ERR_INVALID);
  assert(writer->finish(writer, &error) == VECTIS_OK);
  calls = m.calls;
  assert(writer->finish(writer, &error) == VECTIS_OK && m.calls == calls);
  assert(writer->close(writer, &error) == VECTIS_OK && m.calls == calls);
  assert(vectis_png_reader(read_bytes, &m, NULL, &reader, &error) == VECTIS_OK);
  assert(reader->info(reader, &info, &error) == VECTIS_OK);
  assert(info.width == 2 && info.height == 1 && info.row_bytes == 8u &&
         info.bit_depth == 8 && info.color_type == PNG_COLOR_TYPE_RGBA);
  assert(info.text_count == 1u &&
         strcmp(info.text[0].value, "Native report") == 0);
  assert(info.physical_present && info.physical_meter &&
         info.physical_x == 3780);
  assert(reader->next_row(reader, &row, &size, &error) == VECTIS_OK &&
         size == 8u && memcmp(row, pixels, size) == 0);
  assert(reader->next_row(reader, &row, &size, &error) == VECTIS_OK &&
         row == NULL && size == 0u);
  assert(reader->next_row(reader, &row, &size, &error) == VECTIS_OK &&
         row == NULL);
  assert(reader->close(reader, &error) == VECTIS_OK);
  assert(vectis_png_decode_materialized(m.data, m.size, NULL, &decoded,
                                        &error) == VECTIS_OK);
  assert(decoded.pixels.size == 8u &&
         memcmp(decoded.pixels.data, pixels, 8u) == 0);
  assert(vectis_png_encode_materialized(&decoded.info, decoded.pixels.data,
                                        decoded.pixels.size, NULL, &encoded,
                                        &error) == VECTIS_OK);
  vectis_mutable_bytes_cleanup(&encoded);
  vectis_png_materialized_cleanup(&decoded);
  for (type = 0; type < 3; ++type) {
    fixture(&m, type);
    if (type == 1)
      assert(vectis_png_reader(read_bytes, &m, NULL, &reader, &error) !=
                 VECTIS_OK &&
             reader == NULL);
    assert(vectis_png_decode_materialized(m.data, m.size, NULL, &decoded,
                                          &error) == VECTIS_OK);
    if (type < 2)
      assert(memcmp(decoded.pixels.data, pixels, 8u) == 0);
    else {
      const unsigned char *result = (const unsigned char *)decoded.pixels.data;
      assert(result[0] == 255 && result[1] == 255 && result[2] == 255 &&
             result[3] == 128 && result[4] == 0 && result[7] == 255);
    }
    assert(decoded.info.bit_depth == (type == 2 ? 16 : 8));
    vectis_png_materialized_cleanup(&decoded);
  }
  fixture(&m, 0);
  assert(vectis_png_reader(read_bytes, &m, NULL, &reader, &error) == VECTIS_OK);
  m.fail = 1;
  assert(reader->next_row(reader, &row, &size, &latched) ==
         VECTIS_ERR_CONFLICT);
  assert(reader->next_row(reader, &row, &size, &error) == VECTIS_ERR_CONFLICT &&
         strcmp(error.message, latched.message) == 0);
  assert(reader->close(reader, &error) == VECTIS_OK);
  memset(&m, 0, sizeof(m));
  assert(vectis_png_writer(write_bytes, &m, &image, NULL, &writer, &error) ==
         VECTIS_OK);
  m.fail = 1;
  if (writer->write_row(writer, pixels, sizeof(pixels), &error) == VECTIS_OK)
    assert(writer->finish(writer, &error) == VECTIS_ERR_CONFLICT);
  else
    assert(error.code == VECTIS_ERR_CONFLICT);
  calls = m.calls;
  assert(writer->finish(writer, &error) == VECTIS_ERR_CONFLICT &&
         m.calls == calls);
  assert(writer->close(writer, &error) == VECTIS_OK && m.calls == calls);
  fixture(&m, 0);
  vectis_png_limits_init(&limits);
  limits.max_width = 1;
  assert(vectis_png_decode_materialized(m.data, m.size, &limits, &decoded,
                                        &error) != VECTIS_OK &&
         decoded.impl == NULL);
  vectis_png_limits_init(&limits);
  limits.max_bytes = 7u;
  assert(vectis_png_decode_materialized(m.data, m.size, &limits, &decoded,
                                        &error) != VECTIS_OK);
  assert(vectis_png_encode_materialized(&image, pixels, sizeof(pixels), &limits,
                                        &encoded, &error) != VECTIS_OK);
  assert(vectis_png_decode_materialized(m.data, m.size - 1u, NULL, &decoded,
                                        &error) != VECTIS_OK);
  assert(vectis_png_reader_file("/nonexistent/vectis/no.png", NULL, &reader,
                                &error) != VECTIS_OK);
  assert(vectis_png_writer_file(VECTIS_PNG_OUTPUT, &image, NULL, &writer,
                                &error) == VECTIS_OK);
  assert(writer->write_row(writer, pixels, sizeof(pixels), &error) ==
         VECTIS_OK);
  assert(writer->finish(writer, &error) == VECTIS_OK &&
         writer->close(writer, &error) == VECTIS_OK);
  assert(vectis_png_reader_file(VECTIS_PNG_OUTPUT, NULL, &reader, &error) ==
         VECTIS_OK);
  assert(pthread_create(&thread, NULL, foreign, reader) == 0 &&
         pthread_join(thread, NULL) == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    assert(reader->info(reader, &info, &error) == VECTIS_ERR_STATE);
    assert(reader->close(reader, &error) == VECTIS_OK);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0);
  assert(reader->next_row(reader, &row, &size, &error) == VECTIS_OK &&
         memcmp(row, pixels, size) == 0);
  assert(reader->close(reader, &error) == VECTIS_OK);
  assert(unlink(VECTIS_PNG_OUTPUT) == 0);
  return 0;
}
