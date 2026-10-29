#include <libmdf.h>

#include <assert.h>
#include <string.h>

static void palette_contract(void) {
  mdf_theme_palette palette, saved, configured;
  mdf_options options;
  mdf *renderer = NULL;
  char heading[MDF_THEME_FOREGROUND_SIZE], *output = NULL;
  int level;
  assert(strcmp(LIBMDF_VERSION, "0.18.0") == 0);
  memset(&palette, 0xa5, sizeof(palette));
  assert(mdf_theme_get_palette("DEFAULT", &palette) == MDF_OK);
  assert(strcmp(palette.name, "default") == 0);
  for (level = 1; level <= 6; ++level) {
    assert(mdf_theme_heading_foreground(NULL, level, heading,
                                        sizeof(heading)) == MDF_OK);
    assert(strcmp(heading, palette.heading[level - 1]) == 0);
  }
  saved = palette;
  assert(mdf_theme_get_palette("missing-theme", &palette) == MDF_ERROR_INVALID);
  assert(memcmp(&palette, &saved, sizeof(palette)) == 0);
  strcpy(heading, "unchanged");
  assert(mdf_theme_heading_foreground(NULL, 0, heading, sizeof(heading)) ==
         MDF_ERROR_INVALID);
  assert(mdf_theme_heading_foreground(NULL, 1, heading, 1u) ==
         MDF_ERROR_INVALID);
  assert(strcmp(heading, "unchanged") == 0);
  assert(mdf_get_theme_palette(NULL, &palette) == MDF_ERROR_INVALID);
  assert(memcmp(&palette, &saved, sizeof(palette)) == 0);
  mdf_options_init(&options);
  options.theme_name = "one-dark";
  options.ansi_mode = MDF_ANSI_OFF;
  options.boring = 1;
  assert(mdf_create(MDF_FORMAT_ANSI, &options, &renderer) == MDF_OK);
  assert(renderer->get_theme_palette(renderer, &configured) == MDF_OK);
  assert(mdf_theme_get_palette("one-dark", &palette) == MDF_OK);
  assert(memcmp(&configured, &palette, sizeof(palette)) == 0);
  assert(renderer->render_cstr(renderer, "# Palette\n", &output) == MDF_OK);
  assert(strstr(output, "Palette") != NULL && strchr(output, '\033') == NULL);
  renderer->string_free(renderer, output);
  renderer->destroy(renderer);
}

int main(void) {
  mdf_options options;
  mdf *renderer;
  mdf_status status;
  char fixed_buffer[128];
  char *output;
  int mode;

  palette_contract();
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
