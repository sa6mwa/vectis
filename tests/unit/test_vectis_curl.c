#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vectis/curl.h>
static void curl_cleanup_probe(CURL *);
#define curl_easy_cleanup curl_cleanup_probe
#include "../../src/vectis_curl.c"
#undef curl_easy_cleanup
static size_t cleanup_callbacks;
static void curl_cleanup_probe(CURL *easy) {
  char *private_data = NULL;
  char close_bytes[] = "close";
  curl_transfer *transfer;
  assert(curl_easy_getinfo(easy, CURLINFO_PRIVATE, &private_data) == CURLE_OK);
  transfer = (curl_transfer *)private_data;
  if (transfer != NULL) {
    assert(transfer->closing);
    assert(curl_progress(transfer, 0, 0, 0, 0) == 1);
    assert(curl_write_header(close_bytes, 1u, 5u, transfer) == 0u);
    assert(curl_write_body(close_bytes, 1u, 5u, transfer) == 0u);
    assert(curl_read_body(NULL, 1u, 5u, transfer) == CURL_READFUNC_ABORT);
    ++cleanup_callbacks;
  }
  curl_easy_cleanup(easy);
}
typedef struct fixture {
  const char *url;
  size_t received, uploaded;
  vectis_curl_session *session;
  int fail, cancel, reenter, upload, fail_config;
} fixture;
static vectis_status configure(CURL *easy, void *context, vectis_error *error) {
  fixture *state = (fixture *)context;
  (void)error;
  if (state->fail_config)
    return VECTIS_ERR_INVALID;
  assert(curl_easy_setopt(easy, CURLOPT_URL, state->url) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "file") == CURLE_OK);
  if (state->upload) {
    assert(curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L) == CURLE_OK);
    assert(curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE, (curl_off_t)9) ==
           CURLE_OK);
  }
  return VECTIS_OK;
}
static vectis_status write_bytes(void *context, const void *data, size_t size,
                                 vectis_error *error) {
  fixture *state = (fixture *)context;
  assert(data != NULL && size <= VECTIS_CURL_CHUNK_LIMIT);
  state->received += size;
  if (state->reenter)
    assert(state->session->close(state->session, error) == VECTIS_ERR_STATE);
  return state->fail ? VECTIS_ERR_STATE : VECTIS_OK;
}
static vectis_status read_bytes(void *context, void *data, size_t capacity,
                                size_t *size, vectis_error *error) {
  fixture *state = (fixture *)context;
  const char *body = "body\0data";
  (void)error;
  *size = 9u - state->uploaded;
  if (*size > capacity)
    *size = capacity;
  memcpy(data, body + state->uploaded, *size);
  state->uploaded += *size;
  return state->fail ? VECTIS_ERR_STATE : VECTIS_OK;
}
static vectis_status progress(void *context, const vectis_curl_progress *value,
                              int *cancel, vectis_error *error) {
  fixture *state = (fixture *)context;
  (void)error;
  assert(value->downloaded >= 0 && value->uploaded >= 0);
  *cancel = state->cancel;
  return VECTIS_OK;
}
static void *foreign_thread(void *context) {
  vectis_curl_session *session = (vectis_curl_session *)context;
  vectis_error error;
  assert(session->reset(session, &error) == VECTIS_ERR_STATE);
  assert(session->close(session, &error) == VECTIS_ERR_STATE);
  return NULL;
}
int main(void) {
  vectis_curl_session *session = NULL;
  vectis_curl_multi *multi = NULL;
  vectis_curl_request request;
  const vectis_curl_result *result;
  fixture state, second;
  vectis_error error;
  FILE *file;
  char chunk[4096], url[4096], output_url[4096];
  size_t i, running;
  unsigned long first_id, second_id;
  int ready, status;
  pthread_t thread;
  pid_t child;
  memset(chunk, 'x', sizeof(chunk));
  file = fopen(VECTIS_CURL_FIXTURE_PATH, "wb");
  assert(file != NULL);
  for (i = 0u; i < 2049u; ++i)
    assert(fwrite(chunk, 1u, sizeof(chunk), file) == sizeof(chunk));
  assert(fclose(file) == 0);
  assert(snprintf(url, sizeof(url), "file://%s", VECTIS_CURL_FIXTURE_PATH) > 0);
  assert(snprintf(output_url, sizeof(output_url), "file://%s",
                  VECTIS_CURL_OUTPUT_PATH) > 0);
  memset(&state, 0, sizeof(state));
  state.url = url;
  memset(&request, 0, sizeof(request));
  request.configure = configure;
  request.context = &state;
  assert(vectis_curl_session_new(&session, &error) == VECTIS_OK);
  state.session = session;
  memset(&error, 0, sizeof(error));
  state.fail_config = 1;
  assert(session->perform(session, &request, &result, &error) ==
             VECTIS_ERR_INVALID &&
         result == NULL && error.code == VECTIS_ERR_INVALID &&
         strcmp(error.message, "curl request configuration failed") == 0);
  state.fail_config = 0;
  assert(session->perform(session, &request, &result, &error) == VECTIS_OK &&
         result->code != CURLE_OK &&
         result->failure.code == VECTIS_ERR_INVALID);
  request.write = write_bytes;
  request.progress = progress;
  assert(session->perform(session, &request, &result, &error) == VECTIS_OK &&
         result->code == CURLE_OK && result->streamed &&
         result->body_size == 0u && state.received == sizeof(chunk) * 2049u);
  state.fail = 1;
  assert(session->perform(session, &request, &result, &error) == VECTIS_OK &&
         result->code != CURLE_OK && result->failure.code == VECTIS_ERR_STATE);
  state.fail = 0;
  state.cancel = 1;
  assert(session->perform(session, &request, &result, &error) == VECTIS_OK &&
         result->code != CURLE_OK);
  state.cancel = 0;
  state.reenter = 1;
  assert(session->perform(session, &request, &result, &error) == VECTIS_OK &&
         result->code != CURLE_OK);
  state.reenter = 0;
  state.url = output_url;
  state.upload = 1;
  request.read = read_bytes;
  assert(session->perform(session, &request, &result, &error) == VECTIS_OK &&
         result->code == CURLE_OK && state.uploaded == 9u);
  file = fopen(VECTIS_CURL_OUTPUT_PATH, "rb");
  assert(file != NULL && fread(chunk, 1u, sizeof(chunk), file) == 9u &&
         memcmp(chunk, "body\0data", 9u) == 0 && fclose(file) == 0);
  state.uploaded = 0u;
  state.fail = 1;
  assert(session->perform(session, &request, &result, &error) == VECTIS_OK &&
         result->code != CURLE_OK && result->failure.code == VECTIS_ERR_STATE);
  state.fail = 0;
  assert(session->reset(session, &error) == VECTIS_OK);
  assert(pthread_create(&thread, NULL, foreign_thread, session) == 0 &&
         pthread_join(thread, NULL) == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    foreign_thread(session);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0);
  assert(session->close(session, &error) == VECTIS_OK);
  state.upload = 0;
  state.url = url;
  request.read = NULL;
  state.received = 0;
  assert(vectis_curl_multi_new(&multi, &error) == VECTIS_OK);
  assert(multi->poll(multi, 0, &ready, &error) == VECTIS_OK && ready == 0);
  assert(multi->add(multi, &request, &first_id, &error) == VECTIS_OK);
  second = state;
  second.url = "file:///vectis-intentionally-missing-file";
  request.context = &second;
  assert(multi->add(multi, &request, &second_id, &error) == VECTIS_OK &&
         first_id != second_id);
  do {
    assert(multi->step(multi, &running, &error) == VECTIS_OK);
    assert(multi->poll(multi, 0, &ready, &error) == VECTIS_OK);
  } while (running != 0u);
  for (i = 0u; i < 2u; ++i) {
    assert(multi->completion(multi, i, &result, &error) == VECTIS_OK &&
           result != NULL);
    if (result->id == first_id)
      assert(result->code == CURLE_OK && state.received == 4096u * 2049u);
    else
      assert(result->id == second_id && result->code != CURLE_OK);
  }
  assert(multi->completion(multi, 2u, &result, &error) == VECTIS_OK &&
         result == NULL);
  assert(multi->consume(multi, 3u, &error) == VECTIS_ERR_INVALID);
  assert(multi->consume(multi, 2u, &error) == VECTIS_OK);
  for (i = 0u; i < VECTIS_CURL_MULTI_LIMIT; ++i)
    assert(multi->add(multi, &request, &second_id, &error) == VECTIS_OK);
  assert(multi->add(multi, &request, &second_id, &error) == VECTIS_ERR_STATE);
  assert(multi->close(multi, &error) == VECTIS_OK);
  assert(unlink(VECTIS_CURL_FIXTURE_PATH) == 0 &&
         unlink(VECTIS_CURL_OUTPUT_PATH) == 0);
  assert(cleanup_callbacks > 0u);
  return 0;
}
