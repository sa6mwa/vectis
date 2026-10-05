#include <libmdf.h>

#include <assert.h>
#include <string.h>

int main(void) {
  mdf_options options;
  mdf *renderer;
  mdf_status status;
  char fixed_buffer[128];
  char *output;
  int mode;

  for (mode = 0; mode < 3; ++mode) {
    mdf_options_init(&options);
    options.ansi_mode = MDF_ANSI_ON;
    options.boring = mode == 2;
    options.emission_buffer.data = fixed_buffer;
    options.emission_buffer.cap = mode == 0 ? sizeof(fixed_buffer) : 8;
    options.emission_buffer.max_cap = 8;
    options.emission_buffer.fixed = 1;
    renderer = NULL;
    assert(mdf_create(MDF_FORMAT_ANSI, &options, &renderer) == MDF_OK);
    assert(renderer != NULL);
    output = NULL;
    status = renderer->render_cstr(
        renderer,
        mode == 2
            ? "https://example.test/abcdefghijklmnopqrstuvwxyz0123456789\n"
            : "# Too Large\n",
        &output);
    assert(status == MDF_ERROR_NOMEM);
    assert(output == NULL);
    assert(strcmp(renderer->error(renderer), "out of memory") == 0);
    renderer->destroy(renderer);
  }
  return 0;
}
