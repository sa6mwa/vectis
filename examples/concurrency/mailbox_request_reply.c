#include <vectis/vectis.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct worker_context {
  vectis_mailbox *requests;
  vectis_mailbox_broker *broker;
  int handled;
} worker_context;

static void *worker_main(void *userdata) {
  worker_context *context;
  vectis_mailbox_event request;
  vectis_mailbox_message reply;
  vectis_error error;
  vectis_status status;
  const char response[] = "{\"status\":\"ok\",\"action\":\"opcua.write\"}";

  context = (worker_context *)userdata;
  vectis_mailbox_event_init(&request);
  vectis_error_clear(&error);
  status =
      context->requests->wait_next(context->requests, &request, 1000L, &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "worker failed to receive request: %s\n", error.message);
    return NULL;
  }
  if (request.correlation_id == 0UL || request.payload == NULL ||
      request.payload_size == 0u) {
    fprintf(stderr, "worker received invalid request\n");
    vectis_mailbox_event_cleanup(&request);
    return NULL;
  }

  vectis_mailbox_message_init(&reply);
  reply.kind = "worker.result";
  reply.payload = response;
  reply.payload_size = sizeof(response) - 1u;
  status = context->broker->reply(context->broker, request.correlation_id,
                                  &reply, &error);
  vectis_mailbox_event_cleanup(&request);
  if (status != VECTIS_OK) {
    fprintf(stderr, "worker failed to publish reply: %s\n", error.message);
    return NULL;
  }
  context->handled = 1;
  return NULL;
}

static int run_direct_handoff(void) {
  vectis_mailbox_config config;
  vectis_mailbox_message input;
  vectis_mailbox_message output;
  vectis_mailbox_event event;
  vectis_mailbox_event queued;
  vectis_mailbox *opcua_events;
  vectis_mailbox *lockd_queue;
  vectis_error error;
  vectis_status status;
  const char payload[] = "{\"node\":\"ns=1;s=temperature\",\"value\":21}";

  opcua_events = NULL;
  lockd_queue = NULL;
  vectis_mailbox_config_init(&config);
  config.capacity = 2u;
  config.max_payload_bytes = 256u;
  vectis_error_clear(&error);
  status = vectis_mailbox_new(&config, &opcua_events, &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "failed to create OPC UA event mailbox: %s\n",
            error.message);
    return 1;
  }
  status = vectis_mailbox_new(&config, &lockd_queue, &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "failed to create lockd queue mailbox: %s\n",
            error.message);
    opcua_events->destroy(opcua_events);
    return 1;
  }

  vectis_mailbox_message_init(&input);
  input.kind = "opcua.value";
  input.payload = payload;
  input.payload_size = sizeof(payload) - 1u;
  status = opcua_events->publish(opcua_events, &input, &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "failed to publish OPC UA event: %s\n", error.message);
    opcua_events->destroy(opcua_events);
    lockd_queue->destroy(lockd_queue);
    return 1;
  }

  vectis_mailbox_event_init(&event);
  status = opcua_events->next(opcua_events, &event, &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "failed to drain OPC UA event: %s\n", error.message);
    opcua_events->destroy(opcua_events);
    lockd_queue->destroy(lockd_queue);
    return 1;
  }
  vectis_mailbox_message_init(&output);
  output.kind = "lockd.enqueue";
  output.payload = event.payload;
  output.payload_size = event.payload_size;
  status = lockd_queue->publish(lockd_queue, &output, &error);
  vectis_mailbox_event_cleanup(&event);
  if (status != VECTIS_OK) {
    fprintf(stderr, "failed to publish lockd handoff: %s\n", error.message);
    opcua_events->destroy(opcua_events);
    lockd_queue->destroy(lockd_queue);
    return 1;
  }

  vectis_mailbox_event_init(&queued);
  status = lockd_queue->next(lockd_queue, &queued, &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "failed to drain lockd handoff: %s\n", error.message);
    opcua_events->destroy(opcua_events);
    lockd_queue->destroy(lockd_queue);
    return 1;
  }
  if (queued.kind == NULL || strcmp(queued.kind, "lockd.enqueue") != 0 ||
      queued.payload == NULL ||
      memcmp(queued.payload, "{\"node\":\"ns=1", 13u) != 0) {
    fprintf(stderr, "unexpected lockd handoff\n");
    vectis_mailbox_event_cleanup(&queued);
    opcua_events->destroy(opcua_events);
    lockd_queue->destroy(lockd_queue);
    return 1;
  }
  vectis_mailbox_event_cleanup(&queued);
  opcua_events->destroy(opcua_events);
  lockd_queue->destroy(lockd_queue);
  return 0;
}

int main(void) {
  vectis_mailbox_config config;
  vectis_mailbox_broker_config broker_config;
  vectis_mailbox_message request;
  vectis_mailbox_event reply;
  vectis_mailbox_stats request_stats;
  vectis_mailbox *requests;
  vectis_mailbox_broker *broker;
  vectis_error error;
  worker_context context;
  pthread_t worker;
  vectis_status status;
  unsigned long correlation_id;
  const char payload[] = "{\"path\":\"/machine/1\",\"value\":42}";
  int joined;
  int rc;

  requests = NULL;
  broker = NULL;
  joined = 0;
  vectis_mailbox_config_init(&config);
  config.capacity = 4u;
  config.max_payload_bytes = 256u;
  vectis_error_clear(&error);
  status = vectis_mailbox_new(&config, &requests, &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "failed to create request mailbox: %s\n", error.message);
    return 1;
  }
  vectis_mailbox_broker_config_init(&broker_config);
  broker_config.request_mailbox = requests;
  broker_config.reply_mailbox.max_payload_bytes = 256u;
  status = vectis_mailbox_broker_new(&broker_config, &broker, &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "failed to create mailbox broker: %s\n", error.message);
    requests->destroy(requests);
    return 1;
  }

  context.requests = requests;
  context.broker = broker;
  context.handled = 0;
  rc = pthread_create(&worker, NULL, worker_main, &context);
  if (rc != 0) {
    fprintf(stderr, "failed to start worker\n");
    broker->destroy(broker);
    requests->destroy(requests);
    return 1;
  }

  vectis_mailbox_message_init(&request);
  request.kind = "route.opcua.write";
  request.payload = payload;
  request.payload_size = sizeof(payload) - 1u;
  vectis_mailbox_event_init(&reply);
  status =
      broker->request(broker, &request, 1000L, &reply, &correlation_id, &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "failed to complete broker request: %s\n", error.message);
    broker->close(broker);
    requests->close(requests);
    (void)pthread_join(worker, NULL);
    broker->destroy(broker);
    requests->destroy(requests);
    return 1;
  }

  rc = pthread_join(worker, NULL);
  joined = 1;
  if (rc != 0) {
    fprintf(stderr, "failed to join worker\n");
    vectis_mailbox_event_cleanup(&reply);
    broker->destroy(broker);
    requests->destroy(requests);
    return 1;
  }
  if (!context.handled || reply.correlation_id != correlation_id ||
      reply.payload == NULL ||
      memcmp(reply.payload, "{\"status\":\"ok\"", 14u) != 0) {
    fprintf(stderr, "unexpected mailbox reply\n");
    vectis_mailbox_event_cleanup(&reply);
    broker->destroy(broker);
    requests->destroy(requests);
    return 1;
  }
  vectis_mailbox_event_cleanup(&reply);

  status = requests->stats(requests, &request_stats, &error);
  if (status != VECTIS_OK) {
    fprintf(stderr, "failed to read request stats: %s\n", error.message);
    broker->destroy(broker);
    requests->destroy(requests);
    return 1;
  }
  if (request_stats.requests_published != 1UL || request_stats.drained != 1UL) {
    fprintf(stderr, "unexpected mailbox stats\n");
    broker->destroy(broker);
    requests->destroy(requests);
    return 1;
  }
  if (run_direct_handoff() != 0) {
    broker->destroy(broker);
    requests->destroy(requests);
    return 1;
  }

  if (!joined) {
    (void)pthread_join(worker, NULL);
  }
  broker->destroy(broker);
  requests->destroy(requests);
  puts("mailbox request/reply completed");
  return 0;
}
