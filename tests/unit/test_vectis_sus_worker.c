#define _POSIX_C_SOURCE 200809L

#include "vectis_internal.h"
#include <assert.h>
#include <cpkt/audio.h>
#include <cpkt/sus.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vectis/vectis.h>

#if defined(VECTIS_TEST_WRAP_SUS)
static int g_sus_open_path_calls;
static int g_sus_open_cached_calls;
static int g_sus_model_destroy_calls;
static int g_sus_create_transcriber_calls;
static int g_sus_transcribe_pcm_calls;
static int g_sus_transcriber_destroy_calls;
static int g_sus_string_free_calls;

static char *test_sus_strdup(const char *value) {
  size_t size;
  char *copy;

  assert(value != NULL);
  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  assert(copy != NULL);
  memcpy(copy, value, size);
  return copy;
}

static cpkt_sus_result
test_sus_transcribe_f32_mono_16k(cpkt_sus_transcriber *self,
                                 const float *samples,
                                 unsigned long sample_count) {
  assert(self != NULL);
  assert(samples != NULL);
  assert(sample_count > 0ul);
  return CPKT_SUS_OK;
}

static cpkt_sus_result test_sus_transcribe_f32_mono_16k_text(
    cpkt_sus_transcriber *self, const float *samples,
    unsigned long sample_count, char **text_out) {
  assert(self != NULL);
  assert(samples != NULL);
  assert(sample_count == 4ul);
  assert(text_out != NULL);
  ++g_sus_transcribe_pcm_calls;
  *text_out = test_sus_strdup("mock transcript");
  return CPKT_SUS_OK;
}

static cpkt_sus_result test_sus_transcribe_audio_decoder_segmented(
    cpkt_sus_transcriber *self, cpkt_audio_decoder *decoder,
    const cpkt_sus_segmented_config *config) {
  assert(self != NULL);
  assert(decoder != NULL);
  (void)config;
  return CPKT_SUS_OK;
}

static cpkt_sus_result
test_sus_transcribe_audio_vox_segment(cpkt_sus_transcriber *self,
                                      cpkt_audio_vox_segment *segment,
                                      const cpkt_sus_segmented_config *config) {
  assert(self != NULL);
  assert(segment != NULL);
  (void)config;
  return CPKT_SUS_OK;
}

static cpkt_sus_result test_sus_transcribe_audio_decoder_segmented_text(
    cpkt_sus_transcriber *self, cpkt_audio_decoder *decoder,
    const cpkt_sus_segmented_config *config, char **text_out) {
  assert(self != NULL);
  assert(decoder != NULL);
  assert(text_out != NULL);
  (void)config;
  *text_out = test_sus_strdup("mock transcript");
  return CPKT_SUS_OK;
}

static cpkt_sus_result test_sus_revised_text(cpkt_sus_transcriber *self,
                                             char **text_out) {
  assert(self != NULL);
  assert(text_out != NULL);
  *text_out = test_sus_strdup("mock transcript");
  return CPKT_SUS_OK;
}

static void test_sus_transcriber_destroy(cpkt_sus_transcriber *self) {
  assert(self != NULL);
  ++g_sus_transcriber_destroy_calls;
  free(self);
}

static cpkt_sus_result test_sus_info(const cpkt_sus *self,
                                     cpkt_sus_info *info) {
  assert(self != NULL);
  assert(info != NULL);
  memset(info, 0, sizeof(*info));
  return CPKT_SUS_OK;
}

static cpkt_sus_result
test_sus_create_transcriber(cpkt_sus *self, cpkt_sus_transcriber **out,
                            const cpkt_sus_transcriber_config *config) {
  cpkt_sus_transcriber *transcriber;

  assert(self != NULL);
  assert(out != NULL);
  (void)config;
  transcriber = (cpkt_sus_transcriber *)calloc(1u, sizeof(*transcriber));
  assert(transcriber != NULL);
  transcriber->transcribe_f32_mono_16k = test_sus_transcribe_f32_mono_16k;
  transcriber->transcribe_f32_mono_16k_text =
      test_sus_transcribe_f32_mono_16k_text;
  transcriber->transcribe_audio_decoder_segmented =
      test_sus_transcribe_audio_decoder_segmented;
  transcriber->transcribe_audio_vox_segment =
      test_sus_transcribe_audio_vox_segment;
  transcriber->transcribe_audio_decoder_segmented_text =
      test_sus_transcribe_audio_decoder_segmented_text;
  transcriber->revised_text = test_sus_revised_text;
  transcriber->destroy = test_sus_transcriber_destroy;
  ++g_sus_create_transcriber_calls;
  *out = transcriber;
  return CPKT_SUS_OK;
}

static cpkt_sus_result test_sus_reset_transcript_spacing(cpkt_sus *self) {
  assert(self != NULL);
  return CPKT_SUS_OK;
}

static void test_sus_model_destroy(cpkt_sus *self) {
  assert(self != NULL);
  ++g_sus_model_destroy_calls;
  free(self);
}

static void test_sus_reset_counts(void) {
  g_sus_open_path_calls = 0;
  g_sus_open_cached_calls = 0;
  g_sus_model_destroy_calls = 0;
  g_sus_create_transcriber_calls = 0;
  g_sus_transcribe_pcm_calls = 0;
  g_sus_transcriber_destroy_calls = 0;
  g_sus_string_free_calls = 0;
}

static cpkt_sus *test_sus_model_new(void) {
  cpkt_sus *sus;

  sus = (cpkt_sus *)calloc(1u, sizeof(*sus));
  assert(sus != NULL);
  sus->info = test_sus_info;
  sus->create_transcriber = test_sus_create_transcriber;
  sus->reset_transcript_spacing = test_sus_reset_transcript_spacing;
  sus->destroy = test_sus_model_destroy;
  return sus;
}

cpkt_sus_result __wrap_cpkt_sus_open_path(cpkt_sus **out,
                                          const cpkt_sus_config *config) {
  assert(out != NULL);
  assert(config != NULL);
  assert(config->model_path != NULL);
  assert(strcmp(config->model_path, "mock-model.gguf") == 0);
  ++g_sus_open_path_calls;
  *out = test_sus_model_new();
  return CPKT_SUS_OK;
}

cpkt_sus_result
__wrap_cpkt_sus_open_cached(cpkt_sus **out,
                            const cpkt_sus_cache_config *config) {
  assert(out != NULL);
  assert(config != NULL);
  assert(config->model != NULL);
  ++g_sus_open_cached_calls;
  *out = test_sus_model_new();
  return CPKT_SUS_OK;
}

void __wrap_cpkt_sus_string_free(char *value) {
  ++g_sus_string_free_calls;
  free(value);
}
#endif

static void test_sus_worker_envelopes(void) {
  vectis_sus_worker_transcribe_pcm_request pcm;
  vectis_sus_worker_transcribe_file_request file;
  vectis_sus_worker_event event;
  vectis_sus_worker_response response;
  vectis_mailbox_event reply_event;
  vectis_error error;
  double frames[4];
  char reply_json[] =
      "{\"status\":0,\"source_code\":0,\"dependency_code\":0,"
      "\"operation\":\"transcribe_pcm\",\"text\":\"hello world\","
      "\"path\":\"input.wav\",\"output_path\":\"transcript.txt\"}";

  frames[0] = 0.0;
  frames[1] = 0.25;
  frames[2] = -0.25;
  frames[3] = 0.0;

  vectis_sus_worker_transcribe_pcm_request_init(&pcm);
  assert(pcm.max_text_bytes == VECTIS_SUS_WORKER_DEFAULT_MAX_TEXT_BYTES);
  pcm.frames = frames;
  pcm.frame_count = sizeof(frames) / sizeof(frames[0]);
  pcm.language = "en";
  pcm.initial_prompt = "prompt";
  pcm.max_text_bytes = 4096u;
  assert(vectis_sus_worker_transcribe_pcm_event_build(&pcm, &event, &error) ==
         VECTIS_OK);
  assert(strcmp(event.message.kind, VECTIS_SUS_WORKER_TRANSCRIBE_PCM_KIND) ==
         0);
  assert(event.message.expects_reply);
  assert(strstr((const char *)event.message.payload, "\"frames\"") != NULL);
  assert(strstr((const char *)event.message.payload, "\"language\":\"en\"") !=
         NULL);
  vectis_sus_worker_event_cleanup(&event);

  vectis_sus_worker_transcribe_file_request_init(&file);
  file.path = "input.wav";
  file.encoding = "wav";
  file.output_mode = VECTIS_SUS_WORKER_OUTPUT_FILE;
  file.output_path = "transcript.txt";
  assert(vectis_sus_worker_transcribe_file_event_build(&file, &event, &error) ==
         VECTIS_OK);
  assert(strcmp(event.message.kind, VECTIS_SUS_WORKER_TRANSCRIBE_FILE_KIND) ==
         0);
  assert(strstr((const char *)event.message.payload,
                "\"path\":\"input.wav\"") != NULL);
  assert(strstr((const char *)event.message.payload, "\"output\":\"file\"") !=
         NULL);
  vectis_sus_worker_event_cleanup(&event);

  vectis_sus_worker_response_init(&response);
  vectis_mailbox_event_init(&reply_event);
  reply_event.kind = VECTIS_SUS_WORKER_REPLY_KIND;
  reply_event.payload = reply_json;
  reply_event.payload_size = sizeof(reply_json) - 1u;
  assert(vectis_sus_worker_response_decode(&reply_event, &response, &error) ==
         VECTIS_OK);
  assert(response.status == VECTIS_OK);
  assert(response.source == VECTIS_ERROR_SOURCE_NONE);
  assert(response.operation != NULL);
  assert(strcmp(response.operation, "transcribe_pcm") == 0);
  assert(response.text != NULL);
  assert(strcmp(response.text, "hello world") == 0);
  assert(response.path != NULL);
  assert(strcmp(response.path, "input.wav") == 0);
  assert(response.output_path != NULL);
  assert(strcmp(response.output_path, "transcript.txt") == 0);
  vectis_sus_worker_response_cleanup(&response);
}

static void test_sus_worker_rejects_unrepresentable_size_limits(void) {
#if SIZE_MAX > INT64_MAX
  vectis_sus_worker_transcribe_pcm_request pcm;
  vectis_sus_worker_transcribe_file_request file;
  vectis_sus_worker_event event;
  vectis_error error;
  double frames[2];

  frames[0] = 0.0;
  frames[1] = 0.25;

  vectis_sus_worker_transcribe_pcm_request_init(&pcm);
  pcm.frames = frames;
  pcm.frame_count = sizeof(frames) / sizeof(frames[0]);
  pcm.max_text_bytes = (size_t)INT64_MAX + (size_t)1u;
  assert(vectis_sus_worker_transcribe_pcm_event_build(&pcm, &event, &error) ==
         VECTIS_ERR_INVALID);
  assert(event.message.payload == NULL);
  assert(strstr(error.message, "max_text_bytes") != NULL);

  vectis_sus_worker_transcribe_file_request_init(&file);
  file.path = "input.wav";
  file.max_text_bytes = (size_t)INT64_MAX + (size_t)1u;
  assert(vectis_sus_worker_transcribe_file_event_build(&file, &event, &error) ==
         VECTIS_ERR_INVALID);
  assert(event.message.payload == NULL);
  assert(strstr(error.message, "max_text_bytes") != NULL);
#endif
}

static void test_sus_worker_service_rejects_missing_model_on_start(void) {
  vectis_app_config app_config;
  vectis_app *app;
  vectis_error error;
  vectis_mailbox_config mailbox_config;
  vectis_mailbox *mailbox;
  vectis_sus_worker_service_config worker_config;
  vectis_managed_service *service;
  vectis_managed_service_state service_state;
  vectis_status status;

  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 8u;
  mailbox = NULL;
  assert(vectis_mailbox_new(&mailbox_config, &mailbox, &error) == VECTIS_OK);

  vectis_app_config_init(&app_config);
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  assert(app->sus_worker_service != NULL);
  vectis_sus_worker_service_config_init(&worker_config);
  worker_config.name = "sus-worker-test";
  worker_config.request_mailbox = mailbox;
  worker_config.poll_timeout_ms = 10L;
  worker_config.max_frames = 16u;
  service = NULL;
  status = app->sus_worker_service(app, &worker_config, &service, &error);
  assert(status == VECTIS_OK);
  assert(service != NULL);
  assert(service->state(service, &service_state, &error) == VECTIS_OK);
  assert(service_state.declared);
  assert(service_state.start_requested);
  assert(!service_state.materialized);

  status = app->start(app, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "model_path or cached_model") != NULL);
  assert(service->state(service, &service_state, &error) == VECTIS_OK);
  assert(!service_state.materialized);
  assert(!service_state.started);
  assert(service_state.failed);

  service->close(service);
  app->close(app);
  mailbox->destroy(mailbox);
}

static void test_sus_worker_service_reuses_model_for_requests(void) {
#if defined(VECTIS_TEST_WRAP_SUS)
  vectis_app_config app_config;
  vectis_app *app;
  vectis_error error;
  vectis_mailbox_config mailbox_config;
  vectis_mailbox *mailbox;
  vectis_mailbox_broker_config broker_config;
  vectis_mailbox_broker *broker;
  vectis_sus_worker_service_config worker_config;
  vectis_managed_service *service;
  vectis_managed_service_state service_state;
  vectis_sus_worker_transcribe_pcm_request pcm;
  vectis_sus_worker_event request_event;
  vectis_mailbox_event reply_event;
  vectis_sus_worker_response response;
  vectis_status status;
  unsigned long correlation_id;
  double frames[4];
  int i;

  test_sus_reset_counts();
  frames[0] = 0.0;
  frames[1] = 0.0;
  frames[2] = 0.0;
  frames[3] = 0.0;

  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 8u;
  mailbox = NULL;
  assert(vectis_mailbox_new(&mailbox_config, &mailbox, &error) == VECTIS_OK);
  vectis_mailbox_broker_config_init(&broker_config);
  broker_config.request_mailbox = mailbox;
  broker = NULL;
  assert(vectis_mailbox_broker_new(&broker_config, &broker, &error) ==
         VECTIS_OK);

  vectis_app_config_init(&app_config);
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  vectis_sus_worker_service_config_init(&worker_config);
  worker_config.name = "sus-worker-reuse-test";
  worker_config.request_mailbox = mailbox;
  worker_config.reply_broker = broker;
  worker_config.model_path = "mock-model.gguf";
  worker_config.poll_timeout_ms = 10L;
  worker_config.max_frames = 16u;
  service = NULL;
  assert(app->sus_worker_service(app, &worker_config, &service, &error) ==
         VECTIS_OK);
  assert(service != NULL);

  assert(app->start(app, &error) == VECTIS_OK);
  assert(g_sus_open_path_calls == 1);
  assert(g_sus_open_cached_calls == 0);
  assert(g_sus_model_destroy_calls == 0);
  assert(service->state(service, &service_state, &error) == VECTIS_OK);
  assert(service_state.materialized);
  assert(service_state.started);

  for (i = 0; i < 2; ++i) {
    vectis_sus_worker_transcribe_pcm_request_init(&pcm);
    pcm.frames = frames;
    pcm.frame_count = sizeof(frames) / sizeof(frames[0]);
    assert(vectis_sus_worker_transcribe_pcm_event_build(&pcm, &request_event,
                                                        &error) == VECTIS_OK);
    vectis_mailbox_event_init(&reply_event);
    status = broker->request(broker, &request_event.message, 3000L,
                             &reply_event, &correlation_id, &error);
    vectis_sus_worker_event_cleanup(&request_event);
    assert(status == VECTIS_OK);
    assert(correlation_id != 0u);
    vectis_sus_worker_response_init(&response);
    assert(vectis_sus_worker_response_decode(&reply_event, &response, &error) ==
           VECTIS_OK);
    assert(response.status == VECTIS_OK);
    assert(response.text != NULL);
    assert(strcmp(response.text, "mock transcript") == 0);
    vectis_sus_worker_response_cleanup(&response);
    vectis_mailbox_event_cleanup(&reply_event);
  }

  assert(g_sus_open_path_calls == 1);
  assert(g_sus_create_transcriber_calls == 2);
  assert(g_sus_transcribe_pcm_calls == 2);
  assert(g_sus_transcriber_destroy_calls == 2);
  assert(g_sus_string_free_calls == 2);

  assert(app->stop(app, &error) == VECTIS_OK);
  service->close(service);
  app->close(app);
  assert(g_sus_model_destroy_calls == 1);
  broker->destroy(broker);
  mailbox->destroy(mailbox);
#endif
}

int main(void) {
  test_sus_worker_envelopes();
  test_sus_worker_rejects_unrepresentable_size_limits();
  test_sus_worker_service_rejects_missing_model_on_start();
  test_sus_worker_service_reuses_model_for_requests();
  return 0;
}
