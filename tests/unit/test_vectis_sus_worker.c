#define _POSIX_C_SOURCE 200809L

#include "vectis_internal.h"
#include <assert.h>
#include <string.h>
#include <vectis/vectis.h>

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

static void test_sus_worker_service_missing_model_reply(void) {
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
  assert(app->sus_worker_service != NULL);
  vectis_sus_worker_service_config_init(&worker_config);
  worker_config.name = "sus-worker-test";
  worker_config.request_mailbox = mailbox;
  worker_config.reply_broker = broker;
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

  assert(app->start(app, &error) == VECTIS_OK);
  assert(service->state(service, &service_state, &error) == VECTIS_OK);
  assert(service_state.materialized);
  assert(service_state.started);

  vectis_sus_worker_transcribe_pcm_request_init(&pcm);
  pcm.frames = frames;
  pcm.frame_count = sizeof(frames) / sizeof(frames[0]);
  assert(vectis_sus_worker_transcribe_pcm_event_build(&pcm, &request_event,
                                                      &error) == VECTIS_OK);
  vectis_mailbox_event_init(&reply_event);
  status = broker->request(broker, &request_event.message, 3000L, &reply_event,
                           &correlation_id, &error);
  vectis_sus_worker_event_cleanup(&request_event);
  assert(status == VECTIS_OK);
  assert(correlation_id != 0u);
  vectis_sus_worker_response_init(&response);
  assert(vectis_sus_worker_response_decode(&reply_event, &response, &error) ==
         VECTIS_OK);
  assert(response.status == VECTIS_ERR_INVALID);
  assert(response.source == VECTIS_ERROR_SOURCE_VECTIS);
  assert(response.operation != NULL);
  assert(strcmp(response.operation, "transcribe_pcm") == 0);
  assert(response.message[0] != '\0');
  assert(strstr(response.message, "model_path or cached_model") != NULL);
  vectis_sus_worker_response_cleanup(&response);
  vectis_mailbox_event_cleanup(&reply_event);

  assert(app->stop(app, &error) == VECTIS_OK);
  service->close(service);
  app->close(app);
  broker->destroy(broker);
  mailbox->destroy(mailbox);
}

int main(void) {
  test_sus_worker_envelopes();
  test_sus_worker_service_missing_model_reply();
  return 0;
}
