#define _POSIX_C_SOURCE 200809L
#include <lql/lql.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct fixture {
  lql *client;
  lql_selector *selector;
  lql_stream_request request;
  lql_stream_result result;
  lql_error error;
  size_t offset, length;
  char *input;
} fixture;

static lql_status read_input(void *user, unsigned char *buffer, size_t capacity,
                             size_t *length, lql_error *error) {
  fixture *f = (fixture *)user;
  size_t n = f->length - f->offset;
  (void)error;
  if (n > capacity)
    n = capacity;
  if (n > 257u)
    n = 257u;
  memcpy(buffer, f->input + f->offset, n);
  f->offset += n;
  *length = n;
  return LQL_STATUS_OK;
}

static void *worker(void *user) {
  fixture *f = (fixture *)calloc(1u, sizeof(*f));
  int *failed = (int *)user;
  size_t i;
  lql_status status;
  *failed = 1;
  if (f == NULL)
    return NULL;
  if (lql_new(&f->client, &f->error) != LQL_STATUS_OK)
    goto done;
  if (strcmp(f->client->version(f->client), "0.4.0") != 0)
    goto done;
  if (f->client->selector_parse(f->client, "/.../kind=\"commit\"", &f->selector,
                                &f->error) != LQL_STATUS_OK)
    goto done;
  /* Deep JSON tests the allocator-backed scanner on a real worker stack. */
  f->input = (char *)malloc(127u * 6u + 19u);
  if (f->input == NULL)
    goto done;
  for (i = 0u; i < 127u; ++i) {
    memcpy(f->input + f->length, "{\"x\":", 5u);
    f->length += 5u;
  }
  memcpy(f->input + f->length, "{\"kind\":\"commit\"}", 17u);
  f->length += 17u;
  for (i = 0u; i < 127u; ++i)
    f->input[f->length++] = '}';
  f->input[f->length++] = '\n';
  f->request.reader = read_input;
  f->request.reader_user = f;
  f->request.selector = f->selector;
  status =
      f->client->stream_apply(f->client, &f->request, &f->result, &f->error);
  if (status != LQL_STATUS_OK || f->result.records_seen != 1u ||
      f->result.records_matched != 1u)
    goto done;
  f->offset = 0u;
  status = f->client->stream_apply_spooled(f->client, &f->request, &f->result,
                                           &f->error);
  if (status != LQL_STATUS_OK || f->result.records_matched != 1u)
    goto done;
  *failed = 0;
done:
  if (*failed)
    fprintf(stderr, "LQL worker failure: %s\n", f->error.message);
  if (f->client != NULL) {
    f->client->selector_destroy(f->client, f->selector);
    f->client->destroy(f->client);
  }
  free(f->input);
  free(f);
  return NULL;
}

int main(int argc, char **argv) {
  pthread_t threads[2];
  pthread_attr_t attributes;
  int failed[2], i;
  size_t stack_size = 64u * 1024u;
  long minimum = sysconf(_SC_THREAD_STACK_MIN);
  if (pthread_attr_init(&attributes) != 0)
    return 1;
  if (minimum > 0 && (size_t)minimum > stack_size)
    stack_size = (size_t)minimum;
  if (argc == 2 && strcmp(argv[1], "small") == 0 &&
      pthread_attr_setstacksize(&attributes, stack_size) != 0)
    return 1;
  for (i = 0; i < 2; ++i)
    if (pthread_create(threads + i, argc == 2 ? &attributes : NULL, worker,
                       failed + i) != 0)
      return 1;
  for (i = 0; i < 2; ++i)
    if (pthread_join(threads[i], NULL) != 0 || failed[i])
      return 1;
  pthread_attr_destroy(&attributes);
  puts("LQL 0.4.0 worker streaming and spooled cleanup passed");
  return 0;
}
