#include <vectis/vectis.h>

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

int main(void) {
  test_publish_and_drain();
  test_bounds_and_close();
  test_request_reply_correlation();
  return failures == 0 ? 0 : 1;
}
