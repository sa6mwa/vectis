#include <vectis/vectis.h>

#include "vectis_internal.h"

#include <lc/lc.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "failure: %s\n", message);
    failures++;
  }
}

static void expect_status(vectis_status actual, vectis_status expected,
                          const char *message) {
  if (actual != expected) {
    fprintf(stderr, "failure: %s: got %s expected %s\n", message,
            vectis_status_string(actual), vectis_status_string(expected));
    failures++;
  }
}

static int bytes_contains(const void *data, size_t size, const char *needle) {
  const unsigned char *bytes;
  size_t needle_size;
  size_t i;

  if (data == NULL || needle == NULL) {
    return 0;
  }
  bytes = (const unsigned char *)data;
  needle_size = strlen(needle);
  if (needle_size == 0u || needle_size > size) {
    return 0;
  }
  for (i = 0u; i + needle_size <= size; ++i) {
    if (memcmp(bytes + i, needle, needle_size) == 0) {
      return 1;
    }
  }
  return 0;
}

static void test_publish_and_drain(void) {
  vectis_mailbox_config config;
  vectis_mailbox_message message;
  vectis_mailbox_event event;
  vectis_mailbox_stats stats;
  vectis_mailbox *mailbox;
  vectis_error error;
  vectis_status status;
  const char payload[] = "hello";

  vectis_mailbox_config_init(&config);
  config.capacity = 2u;
  config.max_payload_bytes = 16u;
  status = vectis_mailbox_new(&config, &mailbox, &error);
  expect_status(status, VECTIS_OK, "mailbox creation");

  vectis_mailbox_message_init(&message);
  message.kind = "opcua.value";
  message.payload = payload;
  message.payload_size = sizeof(payload) - 1u;
  message.correlation_id = 42UL;
  message.expects_reply = 1;
  status = mailbox->publish(mailbox, &message, &error);
  expect_status(status, VECTIS_OK, "mailbox publish");
  expect(mailbox->depth(mailbox) == 1u, "mailbox depth after publish");
  status = mailbox->stats(mailbox, &stats, &error);
  expect_status(status, VECTIS_OK, "mailbox stats after publish");
  expect(stats.capacity == 2u, "mailbox stats capacity");
  expect(stats.max_payload_bytes == 16u, "mailbox stats max payload");
  expect(stats.current_depth == 1u, "mailbox stats current depth");
  expect(stats.high_water_depth == 1u, "mailbox stats high water");
  expect(stats.published == 1UL, "mailbox stats published");

  vectis_mailbox_event_init(&event);
  status = mailbox->next(mailbox, &event, &error);
  expect_status(status, VECTIS_OK, "mailbox next");
  expect(event.kind != NULL && strcmp(event.kind, "opcua.value") == 0,
         "mailbox event kind");
  expect(event.payload_size == sizeof(payload) - 1u, "mailbox payload size");
  expect(event.payload != NULL &&
             memcmp(event.payload, payload, sizeof(payload) - 1u) == 0,
         "mailbox payload copy");
  expect(event.correlation_id == 42UL, "mailbox correlation id");
  expect(event.expects_reply == 1, "mailbox expects reply");
  vectis_mailbox_event_cleanup(&event);
  expect(mailbox->depth(mailbox) == 0u, "mailbox depth after drain");
  status = vectis_mailbox_stats_get(mailbox, &stats, &error);
  expect_status(status, VECTIS_OK, "mailbox stats after drain");
  expect(stats.current_depth == 0u, "mailbox stats drained depth");
  expect(stats.drained == 1UL, "mailbox stats drained count");

  mailbox->destroy(mailbox);
}

static void test_bounds_and_close(void) {
  vectis_mailbox_config config;
  vectis_mailbox_message message;
  vectis_mailbox_event event;
  vectis_mailbox_stats stats;
  vectis_mailbox *mailbox;
  vectis_error error;
  vectis_status status;
  const char payload[] = "abcdef";

  vectis_mailbox_config_init(&config);
  config.capacity = 1u;
  config.max_payload_bytes = 4u;
  status = vectis_mailbox_new(&config, &mailbox, &error);
  expect_status(status, VECTIS_OK, "bounded mailbox creation");

  vectis_mailbox_message_init(&message);
  message.kind = "too.large";
  message.payload = payload;
  message.payload_size = sizeof(payload) - 1u;
  status = vectis_mailbox_publish(mailbox, &message, &error);
  expect_status(status, VECTIS_ERR_INVALID, "payload size limit");
  status = mailbox->stats(mailbox, &stats, &error);
  expect_status(status, VECTIS_OK, "stats after size failure");
  expect(stats.publish_failures == 1UL, "stats size publish failure");

  message.kind = "one";
  message.payload_size = 1u;
  status = vectis_mailbox_publish(mailbox, &message, &error);
  expect_status(status, VECTIS_OK, "first bounded publish");
  message.kind = "two";
  status = vectis_mailbox_publish(mailbox, &message, &error);
  expect_status(status, VECTIS_ERR_CONFLICT, "bounded publish rejects full");
  status = mailbox->stats(mailbox, &stats, &error);
  expect_status(status, VECTIS_OK, "stats after full failure");
  expect(stats.full_failures == 1UL, "stats full failure");
  expect(stats.publish_failures == 2UL, "stats publish failures");
  expect(stats.high_water_depth == 1u, "stats full high water");

  vectis_mailbox_event_init(&event);
  status = vectis_mailbox_wait_next(mailbox, &event, 0L, &error);
  expect_status(status, VECTIS_OK, "bounded drain");
  vectis_mailbox_event_cleanup(&event);
  status = vectis_mailbox_wait_next(mailbox, &event, 0L, &error);
  expect_status(status, VECTIS_ERR_TIMEOUT, "empty mailbox poll timeout");
  status = mailbox->stats(mailbox, &stats, &error);
  expect_status(status, VECTIS_OK, "stats after timeout");
  expect(stats.timeout_failures == 1UL, "stats timeout failure");

  mailbox->close(mailbox);
  status = vectis_mailbox_publish(mailbox, &message, &error);
  expect_status(status, VECTIS_ERR_STATE, "closed mailbox publish");
  status = vectis_mailbox_wait_next(mailbox, &event, 0L, &error);
  expect_status(status, VECTIS_ERR_STATE, "closed empty mailbox drain");
  status = mailbox->stats(mailbox, &stats, &error);
  expect_status(status, VECTIS_OK, "stats after closed failures");
  expect(stats.closed_failures == 2UL, "stats closed failures");
  mailbox->destroy(mailbox);
}

static void test_request_reply_correlation(void) {
  vectis_mailbox_config config;
  vectis_mailbox_message request;
  vectis_mailbox_message reply;
  vectis_mailbox_event event;
  vectis_mailbox_stats stats;
  vectis_mailbox *requests;
  vectis_mailbox *replies;
  vectis_error error;
  vectis_status status;
  unsigned long correlation_id;
  const char request_payload[] = "read";
  const char reply_payload[] = "ok";

  vectis_mailbox_config_init(&config);
  config.capacity = 2u;
  status = vectis_mailbox_new(&config, &requests, &error);
  expect_status(status, VECTIS_OK, "request mailbox creation");
  status = vectis_mailbox_new(&config, &replies, &error);
  expect_status(status, VECTIS_OK, "reply mailbox creation");

  vectis_mailbox_message_init(&request);
  request.kind = "worker.opcua";
  request.payload = request_payload;
  request.payload_size = sizeof(request_payload) - 1u;
  status =
      requests->publish_request(requests, &request, &correlation_id, &error);
  expect_status(status, VECTIS_OK, "publish request");
  expect(correlation_id != 0UL, "request correlation assigned");
  status = requests->stats(requests, &stats, &error);
  expect_status(status, VECTIS_OK, "request stats after publish");
  expect(stats.requests_published == 1UL, "request stats requests published");
  expect(stats.correlation_ids_issued == 1UL, "request stats correlation ids");

  vectis_mailbox_event_init(&event);
  status = requests->next(requests, &event, &error);
  expect_status(status, VECTIS_OK, "drain request");
  expect(event.correlation_id == correlation_id, "request correlation carried");
  expect(event.expects_reply == 1, "request expects reply");
  vectis_mailbox_event_cleanup(&event);

  vectis_mailbox_message_init(&reply);
  reply.kind = "worker.result";
  reply.payload = reply_payload;
  reply.payload_size = sizeof(reply_payload) - 1u;
  status = replies->reply(replies, correlation_id, &reply, &error);
  expect_status(status, VECTIS_OK, "publish reply");
  status = replies->stats(replies, &stats, &error);
  expect_status(status, VECTIS_OK, "reply stats after publish");
  expect(stats.replies_published == 1UL, "reply stats replies published");
  status = replies->next(replies, &event, &error);
  expect_status(status, VECTIS_OK, "drain reply");
  expect(event.correlation_id == correlation_id, "reply correlation carried");
  expect(event.expects_reply == 0, "reply does not expect reply");
  expect(event.payload != NULL && memcmp(event.payload, reply_payload,
                                         sizeof(reply_payload) - 1u) == 0,
         "reply payload copy");
  vectis_mailbox_event_cleanup(&event);

  requests->destroy(requests);
  replies->destroy(replies);
}

typedef struct broker_worker_context {
  vectis_mailbox *requests;
  vectis_mailbox_broker *broker;
  int handled;
} broker_worker_context;

static void *broker_worker_main(void *userdata) {
  broker_worker_context *context;
  vectis_mailbox_event request;
  vectis_mailbox_message reply;
  vectis_error error;
  vectis_status status;
  const char payload[] = "{\"ok\":true}";

  context = (broker_worker_context *)userdata;
  vectis_mailbox_event_init(&request);
  status =
      context->requests->wait_next(context->requests, &request, 1000L, &error);
  if (status != VECTIS_OK) {
    return NULL;
  }
  vectis_mailbox_message_init(&reply);
  reply.kind = "worker.result";
  reply.payload = payload;
  reply.payload_size = sizeof(payload) - 1u;
  status = context->broker->reply(context->broker, request.correlation_id,
                                  &reply, &error);
  if (status == VECTIS_OK) {
    context->handled = 1;
  }
  vectis_mailbox_event_cleanup(&request);
  return NULL;
}

static void test_broker_request_reply(void) {
  vectis_mailbox_config mailbox_config;
  vectis_mailbox_broker_config broker_config;
  vectis_mailbox_message request;
  vectis_mailbox_event reply;
  vectis_mailbox_stats stats;
  vectis_mailbox *requests;
  vectis_mailbox_broker *broker;
  vectis_error error;
  vectis_status status;
  broker_worker_context context;
  pthread_t worker;
  unsigned long correlation_id;
  const char payload[] = "{\"op\":\"write\"}";
  int rc;

  requests = NULL;
  broker = NULL;
  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 4u;
  mailbox_config.max_payload_bytes = 128u;
  status = vectis_mailbox_new(&mailbox_config, &requests, &error);
  expect_status(status, VECTIS_OK, "broker request mailbox creation");

  vectis_mailbox_broker_config_init(&broker_config);
  broker_config.request_mailbox = requests;
  broker_config.reply_mailbox.max_payload_bytes = 128u;
  status = vectis_mailbox_broker_new(&broker_config, &broker, &error);
  expect_status(status, VECTIS_OK, "mailbox broker creation");

  context.requests = requests;
  context.broker = broker;
  context.handled = 0;
  rc = pthread_create(&worker, NULL, broker_worker_main, &context);
  expect(rc == 0, "broker worker started");
  if (rc != 0) {
    broker->destroy(broker);
    requests->destroy(requests);
    return;
  }

  vectis_mailbox_message_init(&request);
  request.kind = "route.worker";
  request.payload = payload;
  request.payload_size = sizeof(payload) - 1u;
  vectis_mailbox_event_init(&reply);
  status =
      broker->request(broker, &request, 1000L, &reply, &correlation_id, &error);
  expect_status(status, VECTIS_OK, "broker request reply");
  expect(correlation_id != 0UL, "broker correlation id assigned");
  expect(reply.correlation_id == correlation_id, "broker reply correlation");
  expect(reply.payload != NULL && memcmp(reply.payload, "{\"ok\":true}",
                                         sizeof("{\"ok\":true}") - 1u) == 0,
         "broker reply payload");
  vectis_mailbox_event_cleanup(&reply);
  rc = pthread_join(worker, NULL);
  expect(rc == 0, "broker worker joined");
  expect(context.handled == 1, "broker worker handled request");

  status = requests->stats(requests, &stats, &error);
  expect_status(status, VECTIS_OK, "broker request mailbox stats");
  expect(stats.requests_published == 1UL, "broker request counted");
  expect(stats.drained == 1UL, "broker request drained");

  broker->destroy(broker);
  requests->destroy(requests);
}

static void test_broker_timeout_cleanup(void) {
  vectis_mailbox_config mailbox_config;
  vectis_mailbox_broker_config broker_config;
  vectis_mailbox_message request;
  vectis_mailbox_message late_reply;
  vectis_mailbox_event reply;
  vectis_mailbox *requests;
  vectis_mailbox_broker *broker;
  vectis_error error;
  vectis_status status;
  unsigned long correlation_id;
  const char payload[] = "read";

  requests = NULL;
  broker = NULL;
  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 4u;
  mailbox_config.max_payload_bytes = 128u;
  status = vectis_mailbox_new(&mailbox_config, &requests, &error);
  expect_status(status, VECTIS_OK, "timeout request mailbox creation");

  vectis_mailbox_broker_config_init(&broker_config);
  broker_config.request_mailbox = requests;
  broker_config.reply_mailbox.max_payload_bytes = 128u;
  status = vectis_mailbox_broker_new(&broker_config, &broker, &error);
  expect_status(status, VECTIS_OK, "timeout broker creation");

  vectis_mailbox_message_init(&request);
  request.kind = "route.timeout";
  request.payload = payload;
  request.payload_size = sizeof(payload) - 1u;
  vectis_mailbox_event_init(&reply);
  status =
      broker->request(broker, &request, 1L, &reply, &correlation_id, &error);
  expect_status(status, VECTIS_ERR_TIMEOUT, "broker request timeout");
  expect(correlation_id != 0UL, "timeout correlation id returned");

  vectis_mailbox_message_init(&late_reply);
  late_reply.kind = "worker.result";
  late_reply.payload = payload;
  late_reply.payload_size = sizeof(payload) - 1u;
  status = broker->reply(broker, correlation_id, &late_reply, &error);
  expect_status(status, VECTIS_ERR_TIMEOUT, "late broker reply rejected");

  broker->destroy(broker);
  requests->destroy(requests);
}

typedef struct fake_lockd_payload {
  const char *bytes;
  size_t size;
} fake_lockd_payload;

static int fake_lockd_write_payload(lc_message *self, lc_sink *dst,
                                    size_t *written, lc_error *error) {
  fake_lockd_payload *payload;

  payload = (fake_lockd_payload *)self->impl;
  if (payload == NULL || dst == NULL || dst->write == NULL) {
    if (error != NULL) {
      lc_error_cleanup(error);
      lc_error_init(error);
      error->code = LC_ERR_INVALID;
    }
    return LC_ERR_INVALID;
  }
  if (!dst->write(dst, payload->bytes, payload->size, error)) {
    return error != NULL && error->code != LC_OK ? error->code
                                                 : LC_ERR_PROTOCOL;
  }
  if (written != NULL) {
    *written = payload->size;
  }
  return LC_OK;
}

static void fake_lockd_message_init(lc_consumer_message *consumer,
                                    lc_message *message,
                                    fake_lockd_payload *payload) {
  memset(consumer, 0, sizeof(*consumer));
  memset(message, 0, sizeof(*message));
  consumer->name = "unit-consumer";
  consumer->queue = "jobs";
  consumer->with_state = 1;
  consumer->message = message;
  message->namespace_name = "unit";
  message->queue = "jobs";
  message->message_id = "msg-1";
  message->attempts = 2;
  message->max_attempts = 5;
  message->failure_attempts = 1;
  message->visibility_timeout_seconds = 30L;
  message->payload_content_type = "application/json";
  message->correlation_id = "lockd-correlation";
  message->lease_id = "lease-1";
  message->fencing_token = 7L;
  message->txn_id = "txn-1";
  message->write_payload = fake_lockd_write_payload;
  message->impl = payload;
}

static void test_lockd_consumer_event_builder(void) {
  vectis_lockd_consumer_event_config config;
  vectis_lockd_consumer_event event;
  lc_consumer_message consumer;
  lc_message message;
  fake_lockd_payload payload;
  vectis_error error;
  vectis_status status;

  payload.bytes = "{\"job\":\"render\"}";
  payload.size = strlen(payload.bytes);
  fake_lockd_message_init(&consumer, &message, &payload);
  vectis_lockd_consumer_event_config_init(&config);
  config.include_payload = 1;
  config.max_payload_bytes = 64u;
  vectis_lockd_consumer_event_init(&event);
  status = vectis_lockd_consumer_event_from_message(&consumer, &config, &event,
                                                    &error);
  expect_status(status, VECTIS_OK, "lockd consumer event build");
  expect(event.message.kind != NULL &&
             strcmp(event.message.kind, "vectis.lockd.consumer") == 0,
         "lockd consumer event kind");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"type\":\"vectis.lockd.consumer\""),
         "lockd consumer event type");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"queue\":\"jobs\""),
         "lockd consumer event queue");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"message_id\":\"msg-1\""),
         "lockd consumer event message id");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"with_state\":true"),
         "lockd consumer event with state");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"payload\":{\"included\":true"),
         "lockd consumer event payload included");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"encoding\":\"base64\""),
         "lockd consumer event payload encoding");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"content_base64\":\"eyJqb2IiOiJyZW5kZXIifQ==\""),
         "lockd consumer event payload base64");
  vectis_lockd_consumer_event_cleanup(&event);

  {
    static const char binary_payload[] = {(char)0xff, '\0', (char)0x80};
    payload.bytes = binary_payload;
    payload.size = sizeof(binary_payload);
    status = vectis_lockd_consumer_event_from_message(&consumer, &config,
                                                      &event, &error);
    expect_status(status, VECTIS_OK, "binary lockd consumer event build");
    expect(bytes_contains(event.message.payload, event.message.payload_size,
                          "\"content_base64\":\"/wCA\""),
           "binary lockd consumer event base64");
    expect(!bytes_contains(event.message.payload, event.message.payload_size,
                           "\xff"),
           "binary lockd consumer event avoids raw high bytes");
    vectis_lockd_consumer_event_cleanup(&event);
    payload.bytes = "{\"job\":\"render\"}";
    payload.size = strlen(payload.bytes);
  }

  config.kind = "custom.lockd.consumer";
  status = vectis_lockd_consumer_event_from_message(&consumer, &config, &event,
                                                    &error);
  expect_status(status, VECTIS_OK, "custom lockd consumer event build");
  expect(event.message.kind != NULL &&
             strcmp(event.message.kind, "custom.lockd.consumer") == 0,
         "custom lockd consumer event kind");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"type\":\"custom.lockd.consumer\""),
         "custom lockd consumer event type");
  vectis_lockd_consumer_event_cleanup(&event);

  config.max_payload_bytes = 4u;
  status = vectis_lockd_consumer_event_from_message(&consumer, &config, &event,
                                                    &error);
  expect_status(status, VECTIS_ERR_INVALID, "lockd consumer payload limit");
  vectis_lockd_consumer_event_cleanup(&event);
}

static void test_lockd_consumer_mailbox_receiver_publish(void) {
  vectis_mailbox_config mailbox_config;
  vectis_lockd_consumer_mailbox_receiver_config receiver_config;
  vectis_consumer_receiver_adapter adapter;
  vectis_consumer_receiver receiver;
  vectis_mailbox_event queued;
  vectis_mailbox *mailbox;
  lc_consumer_message consumer;
  lc_message message;
  fake_lockd_payload payload;
  lc_error lcerr;
  vectis_error error;
  vectis_status status;
  char event_kind[64];
  int rc;

  payload.bytes = "payload";
  payload.size = strlen(payload.bytes);
  fake_lockd_message_init(&consumer, &message, &payload);
  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 2u;
  mailbox_config.max_payload_bytes = 4096u;
  status = vectis_mailbox_new(&mailbox_config, &mailbox, &error);
  expect_status(status, VECTIS_OK, "lockd mailbox target");
  vectis_lockd_consumer_mailbox_receiver_config_init(&receiver_config);
  receiver_config.mailbox = mailbox;
  receiver_config.event.include_payload = 1;
  strcpy(event_kind, "custom.lockd.mailbox");
  receiver_config.event.kind = event_kind;
  status = vectis_lockd_consumer_mailbox_receiver_adapter(&adapter, &error);
  expect_status(status, VECTIS_OK, "lockd mailbox adapter factory");
  status = adapter.create(adapter.context, &receiver_config, &receiver, &error);
  expect_status(status, VECTIS_OK, "lockd mailbox receiver create");
  strcpy(event_kind, "mutated.lockd.mailbox");
  lc_error_init(&lcerr);
  rc = receiver.handle(receiver.context, &consumer, &lcerr);
  expect(rc == LC_OK, "lockd mailbox receiver handle publish");
  vectis_mailbox_event_init(&queued);
  status = mailbox->wait_next(mailbox, &queued, 0L, &error);
  expect_status(status, VECTIS_OK, "lockd mailbox event queued");
  expect(queued.expects_reply == 0, "lockd mailbox event is fire and forget");
  expect(bytes_contains(queued.payload, queued.payload_size,
                        "\"payload\":{\"included\":true"),
         "lockd mailbox event has payload");
  expect(bytes_contains(queued.payload, queued.payload_size,
                        "\"type\":\"custom.lockd.mailbox\""),
         "lockd mailbox owns event kind");
  expect(!bytes_contains(queued.payload, queued.payload_size,
                         "\"type\":\"mutated.lockd.mailbox\""),
         "lockd mailbox ignores mutated event kind storage");
  vectis_mailbox_event_cleanup(&queued);
  lc_error_cleanup(&lcerr);
  receiver.cleanup(receiver.context);
  mailbox->destroy(mailbox);
}

typedef struct lockd_broker_worker_context {
  vectis_mailbox *requests;
  vectis_mailbox_broker *broker;
  int saw_event;
} lockd_broker_worker_context;

static void *lockd_broker_worker_main(void *userdata) {
  lockd_broker_worker_context *context;
  vectis_mailbox_event request;
  vectis_mailbox_message reply;
  vectis_error error;
  vectis_status status;
  const char payload[] = "ok";

  context = (lockd_broker_worker_context *)userdata;
  vectis_mailbox_event_init(&request);
  status =
      context->requests->wait_next(context->requests, &request, 1000L, &error);
  if (status != VECTIS_OK) {
    return NULL;
  }
  if (request.expects_reply &&
      bytes_contains(request.payload, request.payload_size,
                     "\"type\":\"vectis.lockd.consumer\"") &&
      bytes_contains(request.payload, request.payload_size,
                     "\"message_id\":\"msg-1\"")) {
    context->saw_event = 1;
  }
  vectis_mailbox_message_init(&reply);
  reply.kind = "lockd.worker.result";
  reply.payload = payload;
  reply.payload_size = sizeof(payload) - 1u;
  (void)context->broker->reply(context->broker, request.correlation_id, &reply,
                               &error);
  vectis_mailbox_event_cleanup(&request);
  return NULL;
}

static void test_lockd_consumer_mailbox_receiver_broker(void) {
  vectis_mailbox_config mailbox_config;
  vectis_mailbox_broker_config broker_config;
  vectis_lockd_consumer_mailbox_receiver_config receiver_config;
  vectis_consumer_receiver_adapter adapter;
  vectis_consumer_receiver receiver;
  vectis_mailbox *requests;
  vectis_mailbox_broker *broker;
  lc_consumer_message consumer;
  lc_message message;
  fake_lockd_payload payload;
  lc_error lcerr;
  vectis_error error;
  vectis_status status;
  lockd_broker_worker_context context;
  pthread_t worker;
  int rc;

  payload.bytes = "payload";
  payload.size = strlen(payload.bytes);
  fake_lockd_message_init(&consumer, &message, &payload);
  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 2u;
  mailbox_config.max_payload_bytes = 4096u;
  status = vectis_mailbox_new(&mailbox_config, &requests, &error);
  expect_status(status, VECTIS_OK, "lockd broker request mailbox");
  vectis_mailbox_broker_config_init(&broker_config);
  broker_config.request_mailbox = requests;
  status = vectis_mailbox_broker_new(&broker_config, &broker, &error);
  expect_status(status, VECTIS_OK, "lockd broker");
  vectis_lockd_consumer_mailbox_receiver_config_init(&receiver_config);
  receiver_config.broker = broker;
  receiver_config.reply_timeout_ms = 1000L;
  status = vectis_lockd_consumer_mailbox_receiver_adapter(&adapter, &error);
  expect_status(status, VECTIS_OK, "lockd broker adapter factory");
  status = adapter.create(adapter.context, &receiver_config, &receiver, &error);
  expect_status(status, VECTIS_OK, "lockd broker receiver create");
  context.requests = requests;
  context.broker = broker;
  context.saw_event = 0;
  rc = pthread_create(&worker, NULL, lockd_broker_worker_main, &context);
  expect(rc == 0, "lockd broker worker started");
  if (rc == 0) {
    lc_error_init(&lcerr);
    rc = receiver.handle(receiver.context, &consumer, &lcerr);
    expect(rc == LC_OK, "lockd broker receiver handled reply");
    lc_error_cleanup(&lcerr);
    (void)pthread_join(worker, NULL);
    expect(context.saw_event == 1, "lockd broker worker saw event");
  }
  receiver.cleanup(receiver.context);
  broker->destroy(broker);
  requests->destroy(requests);
}

static void test_opcua_monitor_mailbox_data_change(void) {
  vectis_mailbox_config mailbox_config;
  vectis_opcua_monitor_mailbox_config adapter_config;
  vectis_opcua_monitor_mailbox_stats stats;
  vectis_opcua_monitor_mailbox *adapter;
  vectis_mailbox_event queued;
  vectis_mailbox *mailbox;
  cpkt_opcua_value value;
  vectis_error error;
  vectis_status status;

  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 2u;
  mailbox_config.max_payload_bytes = 4096u;
  status = vectis_mailbox_new(&mailbox_config, &mailbox, &error);
  expect_status(status, VECTIS_OK, "OPC UA mailbox target");
  vectis_opcua_monitor_mailbox_config_init(&adapter_config);
  adapter_config.mailbox = mailbox;
  adapter_config.event.data_change_kind = "custom.opcua.value";
  status = vectis_opcua_monitor_mailbox_new(&adapter_config, &adapter, &error);
  expect_status(status, VECTIS_OK, "OPC UA monitor mailbox adapter");

  memset(&value, 0, sizeof(value));
  value.type = CPKT_OPCUA_VALUE_INTEGER;
  value.integer_value = 42L;
  adapter->data_change(3UL, 4UL, &value, 0UL, adapter->user);

  vectis_mailbox_event_init(&queued);
  status = mailbox->wait_next(mailbox, &queued, 0L, &error);
  expect_status(status, VECTIS_OK, "OPC UA data change queued");
  expect(queued.kind != NULL && strcmp(queued.kind, "custom.opcua.value") == 0,
         "OPC UA data change kind");
  expect(bytes_contains(queued.payload, queued.payload_size,
                        "\"type\":\"custom.opcua.value\""),
         "OPC UA data change type");
  expect(bytes_contains(queued.payload, queued.payload_size,
                        "\"subscription_id\":3"),
         "OPC UA data change subscription id");
  expect(bytes_contains(queued.payload, queued.payload_size,
                        "\"monitored_item_id\":4"),
         "OPC UA data change monitored item id");
  expect(bytes_contains(queued.payload, queued.payload_size,
                        "\"type_name\":\"integer\""),
         "OPC UA data change value type");
  expect(bytes_contains(queued.payload, queued.payload_size, "\"value\":42"),
         "OPC UA data change value");
  vectis_mailbox_event_cleanup(&queued);

  status = adapter->stats(adapter, &stats, &error);
  expect_status(status, VECTIS_OK, "OPC UA monitor stats");
  expect(stats.data_changes == 1UL, "OPC UA data change stats");
  expect(stats.publish_failures == 0UL, "OPC UA publish failure stats");

  adapter->destroy(adapter);
  mailbox->destroy(mailbox);
}

static void test_opcua_monitor_mailbox_event_fields(void) {
  vectis_mailbox_config mailbox_config;
  vectis_opcua_monitor_mailbox_config adapter_config;
  vectis_opcua_monitor_mailbox_stats stats;
  vectis_opcua_monitor_mailbox *adapter;
  vectis_mailbox_event queued;
  vectis_mailbox *mailbox;
  cpkt_opcua_event_field fields[2];
  cpkt_opcua_string_view strings[2];
  vectis_error error;
  vectis_status status;

  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 2u;
  mailbox_config.max_payload_bytes = 4096u;
  status = vectis_mailbox_new(&mailbox_config, &mailbox, &error);
  expect_status(status, VECTIS_OK, "OPC UA event fields mailbox target");
  vectis_opcua_monitor_mailbox_config_init(&adapter_config);
  adapter_config.mailbox = mailbox;
  status = vectis_opcua_monitor_mailbox_new(&adapter_config, &adapter, &error);
  expect_status(status, VECTIS_OK, "OPC UA event fields adapter");

  memset(fields, 0, sizeof(fields));
  fields[0].name = "Message";
  fields[0].name_length = strlen(fields[0].name);
  fields[0].value.type = CPKT_OPCUA_VALUE_LOCALIZED_TEXT;
  fields[0].value.localized_text_locale = "en-US";
  fields[0].value.localized_text_locale_length = 5u;
  fields[0].value.localized_text = "Alarm";
  fields[0].value.localized_text_length = 5u;
  fields[0].status = 0UL;
  strings[0].data = "a";
  strings[0].length = 1u;
  strings[1].data = "b";
  strings[1].length = 1u;
  fields[1].name = "Tags";
  fields[1].name_length = strlen(fields[1].name);
  fields[1].value.type = CPKT_OPCUA_VALUE_STRING_ARRAY;
  fields[1].value.string_array_values = strings;
  fields[1].value.string_array_length = 2u;
  fields[1].status = 0UL;
  adapter->event_fields(5UL, 6UL, fields, 2u, 0UL, adapter->user);

  vectis_mailbox_event_init(&queued);
  status = mailbox->wait_next(mailbox, &queued, 0L, &error);
  expect_status(status, VECTIS_OK, "OPC UA event fields queued");
  expect(queued.kind != NULL &&
             strcmp(queued.kind, "vectis.opcua.event_fields") == 0,
         "OPC UA event fields kind");
  expect(bytes_contains(queued.payload, queued.payload_size,
                        "\"name\":\"Message\""),
         "OPC UA event field message name");
  expect(bytes_contains(queued.payload, queued.payload_size,
                        "\"type_name\":\"localized_text\""),
         "OPC UA event field localized text");
  expect(bytes_contains(queued.payload, queued.payload_size,
                        "\"type_name\":\"string_array\""),
         "OPC UA event field string array");
  expect(bytes_contains(queued.payload, queued.payload_size,
                        "\"value\":[\"a\",\"b\"]"),
         "OPC UA event field string array values");
  vectis_mailbox_event_cleanup(&queued);

  status = adapter->stats(adapter, &stats, &error);
  expect_status(status, VECTIS_OK, "OPC UA event fields stats read");
  expect(stats.event_fields == 1UL, "OPC UA event fields stats");

  adapter->destroy(adapter);
  mailbox->destroy(mailbox);
}

static void test_opcua_monitor_mailbox_event_and_limit(void) {
  vectis_mailbox_config mailbox_config;
  vectis_opcua_monitor_mailbox_config adapter_config;
  vectis_opcua_monitor_mailbox_stats stats;
  vectis_opcua_monitor_mailbox *adapter;
  vectis_mailbox_event queued;
  vectis_mailbox *mailbox;
  cpkt_opcua_event event;
  unsigned char event_id[3];
  vectis_error error;
  vectis_status status;

  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 2u;
  mailbox_config.max_payload_bytes = 4096u;
  status = vectis_mailbox_new(&mailbox_config, &mailbox, &error);
  expect_status(status, VECTIS_OK, "OPC UA event mailbox target");
  vectis_opcua_monitor_mailbox_config_init(&adapter_config);
  adapter_config.mailbox = mailbox;
  adapter_config.event.max_payload_bytes = 512u;
  status = vectis_opcua_monitor_mailbox_new(&adapter_config, &adapter, &error);
  expect_status(status, VECTIS_OK, "OPC UA event adapter");

  event_id[0] = 0x01u;
  event_id[1] = 0xabu;
  event_id[2] = 0xffu;
  memset(&event, 0, sizeof(event));
  event.event_id = event_id;
  event.event_id_length = sizeof(event_id);
  event.source_name = "Machine";
  event.source_name_length = strlen(event.source_name);
  event.message = "High temperature";
  event.message_length = strlen(event.message);
  event.severity = 700UL;
  adapter->event(7UL, 8UL, &event, 0UL, adapter->user);

  vectis_mailbox_event_init(&queued);
  status = mailbox->wait_next(mailbox, &queued, 0L, &error);
  expect_status(status, VECTIS_OK, "OPC UA event queued");
  expect(bytes_contains(queued.payload, queued.payload_size,
                        "\"event_id\":\"01abff\""),
         "OPC UA event id hex");
  expect(
      bytes_contains(queued.payload, queued.payload_size, "\"severity\":700"),
      "OPC UA event severity");
  vectis_mailbox_event_cleanup(&queued);
  adapter->destroy(adapter);

  vectis_opcua_monitor_mailbox_config_init(&adapter_config);
  adapter_config.mailbox = mailbox;
  adapter_config.event.max_payload_bytes = 32u;
  status = vectis_opcua_monitor_mailbox_new(&adapter_config, &adapter, &error);
  expect_status(status, VECTIS_OK, "OPC UA bounded adapter");
  adapter->event(7UL, 8UL, &event, 0UL, adapter->user);
  status = mailbox->wait_next(mailbox, &queued, 0L, &error);
  expect_status(status, VECTIS_ERR_TIMEOUT,
                "OPC UA oversized event not queued");
  status = adapter->stats(adapter, &stats, &error);
  expect_status(status, VECTIS_OK, "OPC UA bounded stats read");
  expect(stats.publish_failures == 1UL, "OPC UA bounded failure stats");
  expect(stats.last_error.code == VECTIS_ERR_INVALID,
         "OPC UA bounded last error");

  adapter->destroy(adapter);
  mailbox->destroy(mailbox);
}

typedef struct opcua_broker_worker_context {
  vectis_mailbox *requests;
  vectis_mailbox_broker *broker;
  int saw_event;
} opcua_broker_worker_context;

static void *opcua_broker_worker_main(void *userdata) {
  opcua_broker_worker_context *context;
  vectis_mailbox_event request;
  vectis_mailbox_message reply;
  vectis_error error;
  vectis_status status;
  const char payload[] = "ok";

  context = (opcua_broker_worker_context *)userdata;
  vectis_mailbox_event_init(&request);
  status =
      context->requests->wait_next(context->requests, &request, 1000L, &error);
  if (status != VECTIS_OK) {
    return NULL;
  }
  if (request.expects_reply &&
      bytes_contains(request.payload, request.payload_size,
                     "\"type\":\"vectis.opcua.data_change\"")) {
    context->saw_event = 1;
  }
  vectis_mailbox_message_init(&reply);
  reply.kind = "opcua.worker.result";
  reply.payload = payload;
  reply.payload_size = sizeof(payload) - 1u;
  (void)context->broker->reply(context->broker, request.correlation_id, &reply,
                               &error);
  vectis_mailbox_event_cleanup(&request);
  return NULL;
}

static void test_opcua_monitor_mailbox_broker(void) {
  vectis_mailbox_config mailbox_config;
  vectis_mailbox_broker_config broker_config;
  vectis_opcua_monitor_mailbox_config adapter_config;
  vectis_opcua_monitor_mailbox_stats stats;
  vectis_opcua_monitor_mailbox *adapter;
  vectis_mailbox *requests;
  vectis_mailbox_broker *broker;
  cpkt_opcua_value value;
  vectis_error error;
  vectis_status status;
  opcua_broker_worker_context context;
  pthread_t worker;
  int rc;

  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 2u;
  mailbox_config.max_payload_bytes = 4096u;
  status = vectis_mailbox_new(&mailbox_config, &requests, &error);
  expect_status(status, VECTIS_OK, "OPC UA broker request mailbox");
  vectis_mailbox_broker_config_init(&broker_config);
  broker_config.request_mailbox = requests;
  status = vectis_mailbox_broker_new(&broker_config, &broker, &error);
  expect_status(status, VECTIS_OK, "OPC UA broker");
  vectis_opcua_monitor_mailbox_config_init(&adapter_config);
  adapter_config.broker = broker;
  adapter_config.reply_timeout_ms = 1000L;
  status = vectis_opcua_monitor_mailbox_new(&adapter_config, &adapter, &error);
  expect_status(status, VECTIS_OK, "OPC UA broker adapter");

  context.requests = requests;
  context.broker = broker;
  context.saw_event = 0;
  rc = pthread_create(&worker, NULL, opcua_broker_worker_main, &context);
  expect(rc == 0, "OPC UA broker worker started");
  if (rc == 0) {
    memset(&value, 0, sizeof(value));
    value.type = CPKT_OPCUA_VALUE_BOOLEAN;
    value.boolean_value = 1;
    adapter->data_change(9UL, 10UL, &value, 0UL, adapter->user);
    (void)pthread_join(worker, NULL);
    expect(context.saw_event == 1, "OPC UA broker worker saw event");
  }
  status = adapter->stats(adapter, &stats, &error);
  expect_status(status, VECTIS_OK, "OPC UA broker stats");
  expect(stats.data_changes == 1UL, "OPC UA broker data change stats");
  expect(stats.request_failures == 0UL, "OPC UA broker failure stats");

  adapter->destroy(adapter);
  broker->destroy(broker);
  requests->destroy(requests);
}

typedef struct route_worker_context {
  vectis_mailbox *requests;
  vectis_mailbox_broker *broker;
  int saw_event;
} route_worker_context;

static void *route_worker_main(void *userdata) {
  route_worker_context *context;
  vectis_mailbox_event request;
  vectis_mailbox_message reply;
  vectis_error error;
  vectis_status status;
  const char payload[] = "route reply";

  context = (route_worker_context *)userdata;
  vectis_mailbox_event_init(&request);
  status =
      context->requests->wait_next(context->requests, &request, 1000L, &error);
  if (status != VECTIS_OK) {
    return NULL;
  }
  if (bytes_contains(request.payload, request.payload_size,
                     "\"method\":\"POST\"") &&
      bytes_contains(request.payload, request.payload_size,
                     "\"path\":\"/machines/alpha\"") &&
      bytes_contains(request.payload, request.payload_size,
                     "\"id\":\"alpha\"") &&
      bytes_contains(request.payload, request.payload_size,
                     "\"content-type\":\"application/json\"") &&
      bytes_contains(request.payload, request.payload_size,
                     "\"encoding\":\"base64\"") &&
      bytes_contains(request.payload, request.payload_size,
                     "\"content_base64\":\"eyJ2YWx1ZSI6NDJ9\"")) {
    context->saw_event = 1;
  }
  vectis_mailbox_message_init(&reply);
  reply.kind = "route.reply";
  reply.payload = payload;
  reply.payload_size = sizeof(payload) - 1u;
  (void)context->broker->reply(context->broker, request.correlation_id, &reply,
                               &error);
  vectis_mailbox_event_cleanup(&request);
  return NULL;
}

static void test_route_event_builder(void) {
  vectis_route_event_config config;
  vectis_route_event event;
  vectis_request *request;
  vectis_error error;
  vectis_status status;
  const char *params[] = {"id"};
  const char *query[] = {"expand"};
  const char *headers[] = {"content-type"};
  const char body[] = "{\"value\":42}";

  request = vectis_internal_request_new(&error);
  expect(request != NULL, "route event request allocated");
  vectis_internal_request_set_method(request, VECTIS_HTTP_POST);
  status = vectis_internal_request_set_path(request, "/machines/alpha", &error);
  expect_status(status, VECTIS_OK, "route event path");
  status =
      vectis_internal_request_add_path_param(request, "id", "alpha", &error);
  expect_status(status, VECTIS_OK, "route event path param");
  status =
      vectis_internal_request_add_query(request, "expand", "state", &error);
  expect_status(status, VECTIS_OK, "route event query");
  status = vectis_internal_request_add_header(request, "content-type",
                                              "application/json", &error);
  expect_status(status, VECTIS_OK, "route event header");
  status = vectis_internal_request_set_body(request, body, sizeof(body) - 1u,
                                            &error);
  expect_status(status, VECTIS_OK, "route event body");

  vectis_route_event_config_init(&config);
  config.path_params = params;
  config.path_param_count = 1u;
  config.query = query;
  config.query_count = 1u;
  config.headers = headers;
  config.header_count = 1u;
  config.include_body = 1;
  config.max_body_bytes = 64u;
  vectis_route_event_init(&event);
  status = vectis_route_event_from_request(request, &config, &event, &error);
  expect_status(status, VECTIS_OK, "route event build");
  expect(event.message.kind != NULL &&
             strcmp(event.message.kind, "vectis.route") == 0,
         "route event kind");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"query\":{\"expand\":\"state\"}"),
         "route event selected query");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"body\":{\"included\":true"),
         "route event body included");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"encoding\":\"base64\""),
         "route event body encoding");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"content_base64\":\"eyJ2YWx1ZSI6NDJ9\""),
         "route event body base64");
  vectis_route_event_cleanup(&event);

  {
    static const char binary_body[] = {(char)0xff, '\0', (char)0x80};
    status = vectis_internal_request_set_body(request, binary_body,
                                              sizeof(binary_body), &error);
    expect_status(status, VECTIS_OK, "route event binary body");
    status = vectis_route_event_from_request(request, &config, &event, &error);
    expect_status(status, VECTIS_OK, "binary route event build");
    expect(bytes_contains(event.message.payload, event.message.payload_size,
                          "\"content_base64\":\"/wCA\""),
           "binary route event body base64");
    expect(!bytes_contains(event.message.payload, event.message.payload_size,
                           "\xff"),
           "binary route event avoids raw high bytes");
    vectis_route_event_cleanup(&event);
    status = vectis_internal_request_set_body(request, body, sizeof(body) - 1u,
                                              &error);
    expect_status(status, VECTIS_OK, "route event restore text body");
  }

  config.kind = "custom.route";
  status = vectis_route_event_from_request(request, &config, &event, &error);
  expect_status(status, VECTIS_OK, "custom route event build");
  expect(event.message.kind != NULL &&
             strcmp(event.message.kind, "custom.route") == 0,
         "custom route event kind");
  expect(bytes_contains(event.message.payload, event.message.payload_size,
                        "\"type\":\"custom.route\""),
         "custom route event type");
  vectis_route_event_cleanup(&event);

  config.max_body_bytes = 4u;
  status = vectis_route_event_from_request(request, &config, &event, &error);
  expect_status(status, VECTIS_ERR_INVALID, "route event body limit");
  vectis_route_event_cleanup(&event);
  vectis_internal_request_free(request);
}

static void test_route_mailbox_request_response(void) {
  vectis_mailbox_config mailbox_config;
  vectis_mailbox_broker_config broker_config;
  vectis_route_event_config route_config;
  vectis_request *request;
  vectis_response *response;
  vectis_mailbox *requests;
  vectis_mailbox_broker *broker;
  vectis_bytes response_body;
  vectis_error error;
  vectis_status status;
  route_worker_context context;
  pthread_t worker;
  unsigned long correlation_id;
  const char *params[] = {"id"};
  const char *headers[] = {"content-type"};
  const char body[] = "{\"value\":42}";
  int rc;

  request = vectis_internal_request_new(&error);
  response = vectis_internal_response_new(&error);
  expect(request != NULL, "route mailbox request allocated");
  expect(response != NULL, "route mailbox response allocated");
  vectis_internal_request_set_method(request, VECTIS_HTTP_POST);
  status = vectis_internal_request_set_path(request, "/machines/alpha", &error);
  expect_status(status, VECTIS_OK, "route mailbox path");
  status =
      vectis_internal_request_add_path_param(request, "id", "alpha", &error);
  expect_status(status, VECTIS_OK, "route mailbox path param");
  status = vectis_internal_request_add_header(request, "content-type",
                                              "application/json", &error);
  expect_status(status, VECTIS_OK, "route mailbox header");
  status = vectis_internal_request_set_body(request, body, sizeof(body) - 1u,
                                            &error);
  expect_status(status, VECTIS_OK, "route mailbox body");

  vectis_mailbox_config_init(&mailbox_config);
  mailbox_config.capacity = 4u;
  mailbox_config.max_payload_bytes = 4096u;
  status = vectis_mailbox_new(&mailbox_config, &requests, &error);
  expect_status(status, VECTIS_OK, "route mailbox requests");
  vectis_mailbox_broker_config_init(&broker_config);
  broker_config.request_mailbox = requests;
  broker_config.reply_mailbox.max_payload_bytes = 128u;
  status = vectis_mailbox_broker_new(&broker_config, &broker, &error);
  expect_status(status, VECTIS_OK, "route mailbox broker");

  vectis_route_event_config_init(&route_config);
  route_config.path_params = params;
  route_config.path_param_count = 1u;
  route_config.headers = headers;
  route_config.header_count = 1u;
  route_config.include_body = 1;
  route_config.reply_status_code = 202;
  route_config.reply_content_type = "text/plain";
  context.requests = requests;
  context.broker = broker;
  context.saw_event = 0;
  rc = pthread_create(&worker, NULL, route_worker_main, &context);
  expect(rc == 0, "route worker started");
  if (rc == 0) {
    status =
        vectis_route_mailbox_request(broker, request, response, &route_config,
                                     1000L, &correlation_id, &error);
    expect_status(status, VECTIS_OK, "route mailbox request response");
    (void)pthread_join(worker, NULL);
    expect(context.saw_event == 1, "route worker saw typed event");
    expect(correlation_id != 0UL, "route mailbox correlation id");
    response_body = vectis_internal_response_body(response);
    expect(vectis_internal_response_status_code(response) == 202,
           "route mailbox response status");
    expect(strcmp(vectis_internal_response_content_type(response),
                  "text/plain") == 0,
           "route mailbox response content type");
    expect(response_body.size == sizeof("route reply") - 1u &&
               memcmp(response_body.data, "route reply",
                      sizeof("route reply") - 1u) == 0,
           "route mailbox response body");
  }

  status = vectis_route_mailbox_request(NULL, request, response, &route_config,
                                        1000L, &correlation_id, &error);
  expect_status(status, VECTIS_ERR_INVALID, "route mailbox broker required");
  expect(strstr(error.message, "broker") != NULL,
         "route mailbox broker error message");

  broker->destroy(broker);
  requests->destroy(requests);
  vectis_internal_response_free(response);
  vectis_internal_request_free(request);
}

int main(void) {
  test_publish_and_drain();
  test_bounds_and_close();
  test_request_reply_correlation();
  test_broker_request_reply();
  test_broker_timeout_cleanup();
  test_lockd_consumer_event_builder();
  test_lockd_consumer_mailbox_receiver_publish();
  test_lockd_consumer_mailbox_receiver_broker();
  test_opcua_monitor_mailbox_data_change();
  test_opcua_monitor_mailbox_event_fields();
  test_opcua_monitor_mailbox_event_and_limit();
  test_opcua_monitor_mailbox_broker();
  test_route_event_builder();
  test_route_mailbox_request_response();
  return failures == 0 ? 0 : 1;
}
