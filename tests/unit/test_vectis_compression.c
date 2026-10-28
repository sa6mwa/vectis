#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vectis/compression.h>

typedef struct bytes {
  unsigned char data[32768];
  size_t size;
  size_t calls;
} bytes;
static vectis_status collect(void *context, const void *data, size_t size,
                             vectis_error *error) {
  bytes *out = (bytes *)context;
  (void)error;
  assert(size > 0u && size <= 65536u && size <= sizeof(out->data) - out->size);
  memcpy(out->data + out->size, data, size);
  out->size += size;
  ++out->calls;
  return VECTIS_OK;
}
static vectis_status refuse(void *context, const void *data, size_t size,
                            vectis_error *error) {
  (void)context;
  (void)data;
  (void)size;
  memset(error, 0, sizeof(*error));
  error->code = VECTIS_ERR_CONFLICT;
  strcpy(error->message, "sink sentinel");
  return VECTIS_ERR_CONFLICT;
}
static vectis_status reenter(void *context, const void *data, size_t size,
                             vectis_error *error) {
  vectis_compression *receiver = (vectis_compression *)context;
  vectis_error nested;
  (void)data;
  (void)size;
  (void)error;
  assert(receiver->close(receiver, &nested) == VECTIS_ERR_STATE);
  return VECTIS_OK;
}
static void *foreign(void *context) {
  vectis_compression *receiver = (vectis_compression *)context;
  vectis_error error;
  bytes out;
  memset(&out, 0, sizeof(out));
  assert(receiver->process(receiver, "x", 1u, collect, &out, &error) ==
         VECTIS_ERR_STATE);
  assert(receiver->close(receiver, &error) == VECTIS_ERR_STATE);
  return NULL;
}
typedef struct pipeline {
  vectis_compression *decoder;
  size_t count, output_calls;
  unsigned int random_state;
} pipeline;
static unsigned char next_byte(unsigned int *state) {
  *state ^= *state << 13;
  *state ^= *state >> 17;
  *state ^= *state << 5;
  return (unsigned char)*state;
}
static vectis_status verify(void *context, const void *data, size_t size,
                            vectis_error *error) {
  pipeline *pipe = (pipeline *)context;
  const unsigned char *input = (const unsigned char *)data;
  size_t i;
  (void)error;
  assert(size > 0u && size <= 65536u);
  for (i = 0u; i < size; ++i)
    assert(input[i] == next_byte(&pipe->random_state));
  pipe->count += size;
  ++pipe->output_calls;
  return VECTIS_OK;
}
static vectis_status forward(void *context, const void *data, size_t size,
                             vectis_error *error) {
  pipeline *pipe = (pipeline *)context;
  return pipe->decoder->process(pipe->decoder, data, size, verify, pipe, error);
}
int main(void) {
  const char *algorithms[] = {"zstd", "lz4", "xz"};
  size_t a, i, step;
  vectis_compression *encoder, *decoder;
  vectis_compression_config config;
  vectis_error error, latched;
  bytes encoded, decoded;
  unsigned char input[32768];
  pthread_t thread;
  pid_t child;
  int wait_status;
  pipeline pipe;
  unsigned int random_state;
  const char payload[] = "hello\0bounded bytes\377";
  for (a = 0u; a < 3u; ++a) {
    memset(&encoded, 0, sizeof(encoded));
    memset(&decoded, 0, sizeof(decoded));
    assert(vectis_compression_new(algorithms[a], "compress", NULL, &encoder,
                                  &error) == VECTIS_OK);
    assert(encoder->process(encoder, payload, 3u, collect, &encoded, &error) ==
           VECTIS_OK);
    assert(encoder->process(encoder, payload + 3u, sizeof(payload) - 3u,
                            collect, &encoded, &error) == VECTIS_OK);
    assert(encoder->finish(encoder, collect, &encoded, &error) == VECTIS_OK);
    i = encoded.size;
    assert(encoder->finish(encoder, collect, &encoded, &error) == VECTIS_OK &&
           i == encoded.size);
    assert(encoder->process(encoder, "", 0u, collect, &encoded, &error) ==
           VECTIS_ERR_STATE);
    assert(encoder->close(encoder, &error) == VECTIS_OK);
    assert(vectis_compression_new(algorithms[a], "decompress", NULL, &decoder,
                                  &error) == VECTIS_OK);
    for (i = 0u; i < encoded.size; ++i)
      assert(decoder->process(decoder, encoded.data + i, 1u, collect, &decoded,
                              &error) == VECTIS_OK);
    assert(decoder->finish(decoder, collect, &decoded, &error) == VECTIS_OK);
    assert(decoded.size == sizeof(payload) &&
           memcmp(decoded.data, payload, sizeof(payload)) == 0);
    assert(decoder->close(decoder, &error) == VECTIS_OK);
    /* Native defaults, empty frames and decoder completion. */
    memset(&encoded, 0, sizeof(encoded));
    memset(&decoded, 0, sizeof(decoded));
    assert(vectis_compression_new(algorithms[a], "compress", NULL, &encoder,
                                  &error) == VECTIS_OK);
    assert(encoder->finish(encoder, collect, &encoded, &error) == VECTIS_OK);
    assert(encoder->close(encoder, &error) == VECTIS_OK);
    assert(vectis_compression_new(algorithms[a], "decompress", NULL, &decoder,
                                  &error) == VECTIS_OK);
    assert(decoder->process(decoder, encoded.data, encoded.size, collect,
                            &decoded, &error) == VECTIS_OK);
    assert(decoder->finish(decoder, collect, &decoded, &error) == VECTIS_OK &&
           decoded.size == 0u);
    assert(decoder->close(decoder, &error) == VECTIS_OK);
    /* Incomplete frame, trailing bytes, corruption and failures stay failed. */
    assert(vectis_compression_new(algorithms[a], "decompress", NULL, &decoder,
                                  &error) == VECTIS_OK);
    assert(decoder->process(decoder, encoded.data, encoded.size - 1u, collect,
                            &decoded, &error) == VECTIS_OK);
    assert(decoder->finish(decoder, collect, &decoded, &latched) != VECTIS_OK);
    assert(decoder->finish(decoder, collect, &decoded, &error) == latched.code);
    assert(strcmp(error.message, latched.message) == 0);
    assert(decoder->close(decoder, &error) == VECTIS_OK);
    encoded.data[encoded.size] = 42u;
    assert(vectis_compression_new(algorithms[a], "decompress", NULL, &decoder,
                                  &error) == VECTIS_OK);
    assert(decoder->process(decoder, encoded.data, encoded.size + 1u, collect,
                            &decoded, &error) != VECTIS_OK);
    assert(decoder->close(decoder, &error) == VECTIS_OK);
    assert(vectis_compression_new(algorithms[a], "compress", NULL, &encoder,
                                  &error) == VECTIS_OK);
    assert(encoder->finish(encoder, refuse, NULL, &error) ==
           VECTIS_ERR_CONFLICT);
    assert(strcmp(error.message, "sink sentinel") == 0);
    assert(encoder->finish(encoder, collect, &decoded, &error) ==
           VECTIS_ERR_CONFLICT);
    assert(encoder->close(encoder, &error) == VECTIS_OK);
    assert(vectis_compression_new(algorithms[a], "compress", NULL, &encoder,
                                  &error) == VECTIS_OK);
    assert(encoder->finish(encoder, reenter, encoder, &error) ==
           VECTIS_ERR_STATE);
    assert(encoder->close(encoder, &error) == VECTIS_OK);
    /* Producer -> codec -> codec -> consumer with no whole-value collection. */
    assert(vectis_compression_new(algorithms[a], "compress", NULL, &encoder,
                                  &error) == VECTIS_OK);
    assert(vectis_compression_new(algorithms[a], "decompress", NULL,
                                  &pipe.decoder, &error) == VECTIS_OK);
    pipe.count = pipe.output_calls = 0u;
    pipe.random_state = random_state = 1u;
    for (step = 0u; step < 32u; ++step) {
      for (i = 0u; i < sizeof(input); ++i)
        input[i] = next_byte(&random_state);
      assert(encoder->process(encoder, input, sizeof(input), forward, &pipe,
                              &error) == VECTIS_OK);
    }
    assert(pipe.count > 0u); /* Output reached the consumer before finish. */
    assert(encoder->finish(encoder, forward, &pipe, &error) == VECTIS_OK);
    assert(pipe.decoder->finish(pipe.decoder, verify, &pipe, &error) ==
           VECTIS_OK);
    assert(pipe.count == 32u * sizeof(input) && pipe.output_calls > 1u);
    assert(encoder->close(encoder, &error) == VECTIS_OK);
    assert(pipe.decoder->close(pipe.decoder, &error) == VECTIS_OK);
  }
  vectis_compression_config_init(&config);
  config.level_set = 1;
  config.level = 10;
  assert(vectis_compression_new("xz", "compress", &config, &encoder, &error) ==
             VECTIS_ERR_INVALID &&
         encoder == NULL);
  config.level = 0;
  assert(vectis_compression_new("xz", "compress", &config, &encoder, &error) ==
         VECTIS_OK);
  assert(encoder->close(encoder, &error) == VECTIS_OK);
  assert(vectis_compression_new("lzo", "compress", NULL, &encoder, &error) ==
         VECTIS_ERR_INVALID);
  assert(vectis_compression_new("zstd", "compress", NULL, &encoder, &error) ==
         VECTIS_OK);
  assert(pthread_create(&thread, NULL, foreign, encoder) == 0);
  assert(pthread_join(thread, NULL) == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    assert(encoder->finish(encoder, collect, &encoded, &error) ==
           VECTIS_ERR_STATE);
    assert(encoder->close(encoder, &error) == VECTIS_OK);
    _exit(0);
  }
  assert(waitpid(child, &wait_status, 0) == child && WIFEXITED(wait_status) &&
         WEXITSTATUS(wait_status) == 0);
  assert(encoder->close(encoder, &error) == VECTIS_OK);
  return 0;
}
