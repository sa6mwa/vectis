#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vectis/pdf.h>

static int contains_bytes(const vectis_mutable_bytes *bytes, const void *text,
                          size_t size) {
  size_t i;
  if (size > bytes->size)
    return 0;
  for (i = 0; i <= bytes->size - size; ++i) {
    if (memcmp((const unsigned char *)bytes->data + i, text, size) == 0)
      return 1;
  }
  return 0;
}
static int contains(const vectis_mutable_bytes *bytes, const char *text) {
  return contains_bytes(bytes, text, strlen(text));
}
static size_t read_fixture(const char *path, unsigned char *data,
                           size_t capacity) {
  FILE *file = fopen(path, "rb");
  size_t size;
  assert(file != NULL);
  size = fread(data, 1u, capacity, file);
  assert(size != 0u && feof(file));
  assert(fclose(file) == 0);
  return size;
}
static void *foreign_pdf(void *context) {
  vectis_pdf *doc = (vectis_pdf *)context;
  vectis_mutable_bytes bytes;
  vectis_error error;
  assert(doc->bytes_materialized(doc, 0u, &bytes, &error) == VECTIS_ERR_STATE);
  assert(doc->close(doc, &error) == VECTIS_ERR_STATE);
  return NULL;
}
static void must(vectis_status status, const vectis_error *error) {
  if (status != VECTIS_OK)
    fprintf(stderr, "PDF error: %s (%ld, %s)\n", error->message,
            error->dependency_code, error->detail);
  assert(status == VECTIS_OK);
}
int main(void) {
  vectis_pdf_config config;
  vectis_pdf_page_config page_config;
  vectis_pdf_text_config text;
  vectis_pdf_draw_config draw;
  vectis_pdf *doc, *other;
  vectis_pdf_page *page, *other_page;
  vectis_pdf_font *standard, *loaded;
  vectis_pdf_image *png, *jpeg;
  vectis_pdf_image *memory_png, *memory_jpeg;
  vectis_mutable_bytes bytes;
  vectis_error error;
  char signature[6];
  FILE *file;
  pid_t child;
  int child_status;
  pthread_t thread;
  unsigned char encoded[4096];
  size_t encoded_size;
  void *temporary;
  vectis_pdf_config_init(&config);
  config.title = "Vectis report";
  config.author = "Vectis test";
  config.subject = "Public C89 facade";
  must(vectis_pdf_new(&config, &doc, &error), &error);
  vectis_pdf_page_config_init(&page_config);
  page_config.width = 320.0;
  page_config.height = 240.0;
  must(doc->add_page(doc, &page_config, &page, &error), &error);
  must(doc->font(doc, "Helvetica", NULL, &standard, &error), &error);
  must(doc->load_font(doc, VECTIS_PDF_FIXTURES "/VectisTest.ttf", "UTF-8", 1,
                      &loaded, &error),
       &error);
  vectis_pdf_text_config_init(&text);
  text.font = standard;
  text.size = 18.0;
  text.color[0] = 1.0;
  must(page->text(page, 20.0, 200.0, "Hello Vectis", &text, &error), &error);
  text.font = loaded;
  must(page->text(page, 20.0, 170.0, "A", &text, &error), &error);
  must(page->text(page, 40.0, 170.0, "\xe4\xb8\xad", &text, &error), &error);
  vectis_pdf_draw_config_init(&draw);
  draw.color[2] = 1.0;
  draw.line_width = 0.0;
  must(page->line(page, 10.0, 150.0, 300.0, 150.0, &draw, &error), &error);
  draw.fill = 1;
  must(page->rectangle(page, 10.0, 100.0, 40.0, 30.0, &draw, &error), &error);
  must(vectis_pdf_load_image(doc, VECTIS_PDF_FIXTURES "/pixels.png", "png",
                             &png, &error),
       &error);
  must(vectis_pdf_load_image(doc, VECTIS_PDF_FIXTURES "/pixels.jpg", "jpeg",
                             &jpeg, &error),
       &error);
  assert(png->width == 2u && png->height == 1u && jpeg->width == 2u &&
         jpeg->height == 1u);
  must(page->image(page, png, 10.0, 20.0, 80.0, 40.0, &error), &error);
  must(page->image(page, jpeg, 100.0, 20.0, 80.0, 40.0, &error), &error);
  assert(page->image(page, png, 0.0, 0.0, -1.0, 1.0, &error) ==
         VECTIS_ERR_INVALID);
  assert(doc->bytes_materialized(doc, 8u, &bytes, &error) ==
         VECTIS_ERR_INVALID);
  assert(bytes.data == NULL && bytes.size == 0u);
  must(doc->bytes_materialized(doc, 0u, &bytes, &error), &error);
  assert(bytes.size > 500u && memcmp(bytes.data, "%PDF-", 5u) == 0);
  assert(contains(&bytes, "%%EOF") && contains(&bytes, "Hello Vectis"));
  assert(contains(&bytes, "Vectis report") && contains(&bytes, "FontFile2"));
  assert(contains(&bytes, "/Count 1") && contains(&bytes, "320 240"));
  vectis_mutable_bytes_cleanup(&bytes);
  vectis_mutable_bytes_cleanup(&bytes);
  must(doc->save(doc, VECTIS_TEST_BINARY_DIR "/pdf-report.pdf", &error),
       &error);
  file = fopen(VECTIS_TEST_BINARY_DIR "/pdf-report.pdf", "rb");
  assert(file != NULL && fread(signature, 1u, 5u, file) == 5u);
  assert(memcmp(signature, "%PDF-", 5u) == 0);
  assert(fclose(file) == 0);
  must(vectis_pdf_new(NULL, &other, &error), &error);
  must(other->add_page(other, NULL, &other_page, &error), &error);
  assert(other_page->image(other_page, png, 0.0, 0.0, 1.0, 1.0, &error) ==
         VECTIS_ERR_INVALID);
  assert(other_page->text(other_page, 0.0, 0.0, "A", &text, &error) ==
         VECTIS_ERR_INVALID);
  encoded_size =
      read_fixture(VECTIS_PDF_FIXTURES "/pixels.png", encoded, sizeof(encoded));
  temporary = malloc(encoded_size);
  assert(temporary != NULL);
  memcpy(temporary, encoded, encoded_size);
  must(other->load_image_materialized(other, temporary, encoded_size, "png",
                                      &memory_png, &error),
       &error);
  memset(temporary, 0, encoded_size);
  free(temporary);
  must(other_page->image(other_page, memory_png, 0.0, 0.0, 20.0, 10.0, &error),
       &error);
  encoded_size =
      read_fixture(VECTIS_PDF_FIXTURES "/pixels.jpg", encoded, sizeof(encoded));
  temporary = malloc(encoded_size);
  assert(temporary != NULL);
  memcpy(temporary, encoded, encoded_size);
  must(other->load_image_materialized(other, temporary, encoded_size, "jpeg",
                                      &memory_jpeg, &error),
       &error);
  memset(temporary, 0, encoded_size);
  free(temporary);
  must(
      other_page->image(other_page, memory_jpeg, 30.0, 0.0, 20.0, 10.0, &error),
      &error);
  must(other->bytes_materialized(other, 0u, &bytes, &error), &error);
  assert(contains_bytes(&bytes, encoded, encoded_size));
  vectis_mutable_bytes_cleanup(&bytes);
  must(other->close(other, &error), &error);
  memory_png->close(memory_png);
  memory_jpeg->close(memory_jpeg);
  other_page->close(other_page);
  assert(pthread_create(&thread, NULL, foreign_pdf, doc) == 0);
  assert(pthread_join(thread, NULL) == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    assert(doc->bytes_materialized(doc, 0u, &bytes, &error) ==
           VECTIS_ERR_STATE);
    assert(doc->close(doc, &error) == VECTIS_ERR_STATE);
    _exit(0);
  }
  assert(waitpid(child, &child_status, 0) == child);
  assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
  assert(doc->save(doc,
                   VECTIS_TEST_BINARY_DIR "/pdf-no-such-directory/report.pdf",
                   &error) == VECTIS_ERR_STATE);
  assert(error.dependency_code != 0);
  assert(doc->bytes_materialized(doc, 0u, &bytes, &error) == VECTIS_ERR_STATE);
  must(doc->reset_error(doc, &error), &error);
  must(doc->bytes_materialized(doc, 0u, &bytes, &error), &error);
  vectis_mutable_bytes_cleanup(&bytes);
  must(doc->close(doc, &error), &error);
  assert(page->text(page, 0.0, 0.0, "closed", NULL, &error) ==
         VECTIS_ERR_STATE);
  page->close(page);
  standard->close(standard);
  loaded->close(loaded);
  png->close(png);
  jpeg->close(jpeg);
  return 0;
}
