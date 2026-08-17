#define _POSIX_C_SOURCE 200809L

#include "vectis_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vectis/vectis.h>

static void fill_test_frames(double *frames, size_t count) {
  size_t i;

  for (i = 0u; i < count; ++i) {
    frames[i] = (double)((int)(i % 32u) - 16) / 32.0;
  }
}

static void test_audio_worker_envelopes(void) {
  vectis_audio_worker_decode_request decode;
  vectis_audio_worker_encode_request encode;
  vectis_audio_worker_event event;
  vectis_audio_worker_response response;
  vectis_mailbox_event reply_event;
  vectis_error error;
  double frames[4];
  char reply_json[] =
      "{\"status\":0,\"source_code\":0,\"dependency_code\":0,"
      "\"operation\":\"decode\",\"path\":\"input.wav\",\"sample_rate\":16000,"
      "\"channels\":1,\"frames\":[0.25,-0.25]}";

  fill_test_frames(frames, sizeof(frames) / sizeof(frames[0]));
  vectis_audio_worker_decode_request_init(&decode);
  assert(decode.max_frames == VECTIS_AUDIO_WORKER_DEFAULT_MAX_FRAMES);
  decode.path = "input.wav";
  decode.encoding = "wav";
  decode.max_frames = 128u;
  assert(vectis_audio_worker_decode_event_build(&decode, &event, &error) ==
         VECTIS_OK);
  assert(strcmp(event.message.kind, VECTIS_AUDIO_WORKER_DECODE_KIND) == 0);
  assert(event.message.expects_reply);
  assert(strstr((const char *)event.message.payload,
                "\"path\":\"input.wav\"") != NULL);
  vectis_audio_worker_event_cleanup(&event);

  vectis_audio_worker_encode_request_init(&encode);
  encode.path = "output.wav";
  encode.format = "wav";
  encode.frames = frames;
  encode.frame_count = sizeof(frames) / sizeof(frames[0]);
  assert(vectis_audio_worker_encode_event_build(&encode, &event, &error) ==
         VECTIS_OK);
  assert(strcmp(event.message.kind, VECTIS_AUDIO_WORKER_ENCODE_KIND) == 0);
  assert(strstr((const char *)event.message.payload, "\"frames\"") != NULL);
  vectis_audio_worker_event_cleanup(&event);

  vectis_audio_worker_response_init(&response);
  vectis_mailbox_event_init(&reply_event);
  reply_event.kind = VECTIS_AUDIO_WORKER_REPLY_KIND;
  reply_event.payload = reply_json;
  reply_event.payload_size = sizeof(reply_json) - 1u;
  assert(vectis_audio_worker_response_decode(&reply_event, &response, &error) ==
         VECTIS_OK);
  assert(response.status == VECTIS_OK);
  assert(response.source == VECTIS_ERROR_SOURCE_NONE);
  assert(response.operation != NULL);
  assert(strcmp(response.operation, "decode") == 0);
  assert(response.frame_count == 2u);
  assert(response.frames[0] > 0.24 && response.frames[0] < 0.26);
  vectis_audio_worker_response_cleanup(&response);
}

static void test_audio_worker_service_roundtrip(void) {
  vectis_app_config app_config;
  vectis_app *app;
  vectis_error error;
  vectis_mailbox_config mailbox_config;
  vectis_mailbox *mailbox;
  vectis_mailbox_broker_config broker_config;
  vectis_mailbox_broker *broker;
  vectis_audio_worker_service_config worker_config;
  vectis_managed_service *service;
  vectis_managed_service_state service_state;
  vectis_audio_worker_encode_request encode;
  vectis_audio_worker_decode_request decode;
  vectis_audio_worker_event request_event;
  vectis_mailbox_event reply_event;
  vectis_audio_worker_response response;
  vectis_status status;
  unsigned long correlation_id;
  double frames[160];
  char path[128];
  int written;

  written = snprintf(path, sizeof(path), "/tmp/vectis-audio-worker-%ld.wav",
                     (long)getpid());
  assert(written > 0);
  assert((size_t)written < sizeof(path));
  (void)remove(path);
  fill_test_frames(frames, sizeof(frames) / sizeof(frames[0]));

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
  assert(app->audio_worker_service != NULL);
  vectis_audio_worker_service_config_init(&worker_config);
  worker_config.name = "audio-worker-test";
  worker_config.request_mailbox = mailbox;
  worker_config.reply_broker = broker;
  worker_config.poll_timeout_ms = 10L;
  worker_config.max_frames = 512u;
  service = NULL;
  status = app->audio_worker_service(app, &worker_config, &service, &error);
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

  vectis_audio_worker_encode_request_init(&encode);
  encode.path = path;
  encode.format = "wav";
  encode.frames = frames;
  encode.frame_count = sizeof(frames) / sizeof(frames[0]);
  assert(vectis_audio_worker_encode_event_build(&encode, &request_event,
                                                &error) == VECTIS_OK);
  vectis_mailbox_event_init(&reply_event);
  status = broker->request(broker, &request_event.message, 3000L, &reply_event,
                           &correlation_id, &error);
  vectis_audio_worker_event_cleanup(&request_event);
  assert(status == VECTIS_OK);
  assert(correlation_id != 0u);
  vectis_audio_worker_response_init(&response);
  assert(vectis_audio_worker_response_decode(&reply_event, &response, &error) ==
         VECTIS_OK);
  assert(response.status == VECTIS_OK);
  assert(response.operation != NULL);
  assert(strcmp(response.operation, "encode") == 0);
  vectis_audio_worker_response_cleanup(&response);
  vectis_mailbox_event_cleanup(&reply_event);

  vectis_audio_worker_decode_request_init(&decode);
  decode.path = path;
  decode.encoding = "wav";
  decode.max_frames = 512u;
  assert(vectis_audio_worker_decode_event_build(&decode, &request_event,
                                                &error) == VECTIS_OK);
  vectis_mailbox_event_init(&reply_event);
  status = broker->request(broker, &request_event.message, 3000L, &reply_event,
                           &correlation_id, &error);
  vectis_audio_worker_event_cleanup(&request_event);
  assert(status == VECTIS_OK);
  vectis_audio_worker_response_init(&response);
  assert(vectis_audio_worker_response_decode(&reply_event, &response, &error) ==
         VECTIS_OK);
  assert(response.status == VECTIS_OK);
  assert(response.operation != NULL);
  assert(strcmp(response.operation, "decode") == 0);
  assert(response.sample_rate == 16000u);
  assert(response.channels == 1u);
  assert(response.frame_count > 0u);
  assert(response.frame_count <= 512u);
  vectis_audio_worker_response_cleanup(&response);
  vectis_mailbox_event_cleanup(&reply_event);

  assert(app->stop(app, &error) == VECTIS_OK);
  service->close(service);
  app->close(app);
  broker->destroy(broker);
  mailbox->destroy(mailbox);
  (void)remove(path);
}

int main(void) {
  test_audio_worker_envelopes();
  test_audio_worker_service_roundtrip();
  return 0;
}
