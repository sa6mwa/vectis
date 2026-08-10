#include <stdio.h>
#include <string.h>

#include <libmdf.h>

typedef struct example_source {
  const char *text;
  size_t offset;
} example_source;

static size_t read_markdown(void *userdata, char *dst, size_t cap, int *err) {
  example_source *source;
  size_t remaining;
  size_t n;

  source = (example_source *)userdata;
  *err = 0;
  remaining = strlen(source->text) - source->offset;
  if (remaining == 0u) {
    return 0u;
  }
  n = remaining < cap ? remaining : cap;
  if (n > 8u) {
    n = 8u;
  }
  memcpy(dst, source->text + source->offset, n);
  source->offset += n;
  return n;
}

static int write_stdout(void *userdata, const char *src, size_t len) {
  FILE *out;

  out = (FILE *)userdata;
  return fwrite(src, 1u, len, out) == len ? 0 : -1;
}

static int render_html_cstr(void) {
  mdf_options options;
  mdf *renderer;
  char *html;
  mdf_status status;

  renderer = NULL;
  html = NULL;
  mdf_options_init(&options);
  options.width = 72;

  status = mdf_create(MDF_FORMAT_HTML, &options, &renderer);
  if (status != MDF_OK) {
    fprintf(stderr, "mdf_create failed: %s\n", mdf_status_string(status));
    return 1;
  }

  status = renderer->render_cstr(renderer, "# Vectis\n\n**libmdf** example\n",
                                 &html);
  if (status != MDF_OK) {
    fprintf(stderr, "render_cstr failed: %s: %s\n", mdf_status_string(status),
            renderer->error(renderer));
    renderer->destroy(renderer);
    return 1;
  }

  fputs(html, stdout);
  renderer->string_free(renderer, html);
  renderer->destroy(renderer);
  return 0;
}

static int render_stream(void) {
  const char markdown[] =
      "\n## Streaming\n\n- bounded reads\n- direct sink writes\n";
  example_source input;
  mdf_source source;
  mdf_sink sink;
  mdf_options options;
  mdf *renderer;
  mdf_status status;

  input.text = markdown;
  input.offset = 0u;
  source.userdata = &input;
  source.read = read_markdown;
  sink.userdata = stdout;
  sink.write = write_stdout;
  renderer = NULL;

  mdf_options_init(&options);
  options.width = 72;
  status = mdf_create(MDF_FORMAT_HTML, &options, &renderer);
  if (status != MDF_OK) {
    fprintf(stderr, "mdf_create failed: %s\n", mdf_status_string(status));
    return 1;
  }

  status = renderer->render(renderer, &source, &sink);
  if (status != MDF_OK) {
    fprintf(stderr, "render failed: %s: %s\n", mdf_status_string(status),
            renderer->error(renderer));
    renderer->destroy(renderer);
    return 1;
  }

  renderer->destroy(renderer);
  return 0;
}

int main(void) {
  if (render_html_cstr() != 0) {
    return 1;
  }
  if (render_stream() != 0) {
    return 1;
  }
  return 0;
}
