#include <assert.h>
#include <string.h>
#include <vectis/zlib_raw.h>
typedef struct fixture {
  const unsigned char *data;
  size_t size, offset;
  unsigned char output[1024];
  size_t written;
  vectis_zlib_raw *owner;
  vectis_buffer_span *window;
} fixture;
static unsigned input(void *user, z_const unsigned char **out) {
  fixture *state = (fixture *)user;
  size_t size = state->size - state->offset;
  const void *data = state->data + state->offset;
  if (size > 3u)
    size = 3u;
  memcpy(out, &data, sizeof(data));
  state->offset += size;
  return (unsigned)size;
}
static int output(void *user, unsigned char *data, unsigned size) {
  fixture *state = (fixture *)user;
  vectis_error error;
  void *pointer;
  size_t capacity;
  assert(state->owner->close(state->owner, &error) == VECTIS_ERR_STATE);
  assert(state->window->write(state->window, 0, "x", 1, &error) ==
         VECTIS_ERR_STATE);
  assert(state->window->data_mutable(state->window, &pointer, &capacity,
                                     &error) == VECTIS_ERR_STATE);
  assert(state->written + size <= sizeof(state->output));
  memcpy(state->output + state->written, data, size);
  state->written += size;
  return 0;
}
int main(void) {
  vectis_zlib_raw_options options;
  vectis_zlib_raw *encoder, *decoder;
  vectis_buffer *window;
  vectis_buffer_span *span;
  vectis_zlib_raw_step step;
  vectis_zlib_back_result result;
  vectis_error error;
  unsigned char compressed[512];
  fixture state;
  const char *text = "public C89 back-inflate callbacks";
  vectis_zlib_raw_options_init(&options);
  options.window_bits = -15;
  assert(vectis_zlib_raw_new(&options, &encoder, &error) == VECTIS_OK);
  assert(encoder->process(encoder, text, strlen(text), compressed,
                          sizeof(compressed), Z_FINISH, &step,
                          &error) == VECTIS_OK &&
         step.zlib_status == Z_STREAM_END);
  assert(encoder->close(encoder, &error) == VECTIS_OK);
  assert(vectis_buffer_new(32768, &window, &error) == VECTIS_OK);
  assert(window->borrow(window, 0, 32768, 0, &span, &error) == VECTIS_OK);
  options.compress = 0;
  options.window_bits = 15;
  assert(vectis_zlib_raw_back_new(&options, span, &decoder, &error) ==
         VECTIS_OK);
  window->close(window);
  memset(&state, 0, sizeof(state));
  state.data = compressed;
  state.size = step.produced;
  state.owner = decoder;
  state.window = span;
  assert(decoder->back(decoder, input, &state, output, &state, &result,
                       &error) == VECTIS_OK);
  assert(result.zlib_status == Z_STREAM_END && result.output == strlen(text) &&
         result.consumed == state.size);
  assert(state.written == strlen(text) &&
         memcmp(state.output, text, state.written) == 0);
  assert(span->write(span, 0, "x", 1, &error) == VECTIS_OK);
  assert(decoder->close(decoder, &error) == VECTIS_OK);
  span->close(span);
  return 0;
}
