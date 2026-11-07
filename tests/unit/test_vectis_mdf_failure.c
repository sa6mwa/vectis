#include <libmdf.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void terminal_detection_contract(void) {
  assert(unsetenv("DOMTERM") == 0 && unsetenv("WT_SESSION") == 0);
  assert(unsetenv("TERM_PROGRAM") == 0 && unsetenv("VTE_VERSION") == 0);
  assert(unsetenv("OSC8") == 0 && setenv("TERM", "xterm-256color", 1) == 0);
  assert(mdf_detect_osc8_support() == 1);
  assert(setenv("OSC8", "0", 1) == 0);
  assert(mdf_detect_osc8_support() == 0);
  assert(unsetenv("OSC8") == 0 && setenv("VTE_VERSION", "4000", 1) == 0);
  assert(mdf_detect_osc8_support() == 0);
}

static void literal_contract(void) {
  mdf_options options;
  mdf *renderer = NULL;
  char *escaped = NULL, *output = NULL;
  mdf_options_init(&options);
  options.ansi_mode = MDF_ANSI_OFF;
  assert(mdf_create(MDF_FORMAT_ANSI, &options, &renderer) == MDF_OK);
  assert(renderer->set_word_buffer(renderer, 8u, 1) == MDF_OK);
  assert(renderer->set_word_buffer(renderer, 65537u, 0) == MDF_ERROR_INVALID);
  assert(renderer->set_word_buffer(renderer, 4096u, 0) == MDF_OK);
  assert(renderer->escape_markdown(renderer, "**literal**", &escaped) ==
         MDF_OK);
  assert(strcmp(escaped, "\\*\\*literal\\*\\*") == 0);
  assert(renderer->render_cstr(renderer, escaped, &output) == MDF_OK);
  assert(strcmp(output, "**literal**\n") == 0);
  renderer->string_free(renderer, output);
  renderer->string_free(renderer, escaped);
  renderer->destroy(renderer);
}

static void palette_contract(void) {
  mdf_theme_palette palette, saved, configured;
  mdf_options options;
  mdf *renderer = NULL;
  char heading[MDF_THEME_FOREGROUND_SIZE], *output = NULL;
  int level;
  assert(strcmp(LIBMDF_VERSION, "0.23.0") == 0);
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

static void terminal_url_contract(void) {
  mdf_options options;
  mdf *renderer = NULL;
  char *output = NULL;
  int right;
  const char *url = "https://example.com/abcdefghijklmnopqrstuvwxyz";
  for (right = 0; right <= 2; right += 2) {
    mdf_options_init(&options);
    options.ansi_mode = MDF_ANSI_ON;
    options.width = 20;
    options.margin_left = 2;
    options.margin_right = right;
    options.osc8 = 0;
    assert(mdf_create(MDF_FORMAT_ANSI, &options, &renderer) == MDF_OK);
    assert(renderer->render_cstr(
               renderer,
               "- [report](https://example.com/abcdefghijklmnopqrstuvwxyz) "
               "after\n",
               &output) == MDF_OK);
    assert((strstr(output, url) != NULL) == (right == 0));
    renderer->string_free(renderer, output);
    renderer->destroy(renderer);
  }
}

int main(void) {
  mdf_options options;
  mdf *renderer;
  mdf_status status;
  char fixed_buffer[128];
  char *output;
  int mode;

  palette_contract();
  terminal_url_contract();
  terminal_detection_contract();
  literal_contract();
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
