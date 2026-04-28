#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <lonejson.h>
#include <vectis/vectis.h>

typedef struct workflow_event {
  char id[64];
  char type[64];
} workflow_event;

static const lonejson_field workflow_event_fields[] = {
    LONEJSON_FIELD_STRING_FIXED_REQ(workflow_event, id, "id", LONEJSON_OVERFLOW_FAIL),
    LONEJSON_FIELD_STRING_FIXED_REQ(workflow_event, type, "type", LONEJSON_OVERFLOW_FAIL)};

LONEJSON_MAP_DEFINE(workflow_event_map, workflow_event, workflow_event_fields);

static const char *env_or_default(const char *name, const char *fallback) {
  const char *value;

  value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  return value;
}

static int print_error(const char *operation, const vectis_error *error) {
  fprintf(stderr, "%s failed", operation);
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, ": %s", error->message);
  }
  if (error != NULL && error->detail[0] != '\0') {
    fprintf(stderr, " (%s)", error->detail);
  }
  fprintf(stderr, "\n");
  return 1;
}

int main(void) {
  vectis_mqtt_config mqtt;
  vectis_error error;
  workflow_event event;
  const char raw_payload[] = "ready";

  vectis_mqtt_config_init(&mqtt);
  vectis_error_clear(&error);
  mqtt.broker_url = env_or_default("VECTIS_MQTT_URL", "mqtt://127.0.0.1:21883");
  mqtt.username = getenv("VECTIS_MQTT_USERNAME");
  mqtt.password = getenv("VECTIS_MQTT_PASSWORD");
  mqtt.client_bundle_path = getenv("VECTIS_MQTT_CLIENT_BUNDLE");
  mqtt.ca_bundle_path = getenv("VECTIS_MQTT_CA_BUNDLE");

  event.id[0] = '1';
  event.id[1] = '\0';
  event.type[0] = 'o';
  event.type[1] = 'r';
  event.type[2] = 'd';
  event.type[3] = 'e';
  event.type[4] = 'r';
  event.type[5] = '\0';

  if (vectis_mqtt_publish(&mqtt,
                          "workflow/orders/raw",
                          raw_payload,
                          sizeof(raw_payload) - 1u,
                          "text/plain",
                          &error) != VECTIS_OK) {
    return print_error("vectis_mqtt_publish", &error);
  }
  if (vectis_mqtt_publish_json(&mqtt,
                               "workflow/orders/json",
                               &workflow_event_map,
                               &event,
                               &error) != VECTIS_OK) {
    return print_error("vectis_mqtt_publish_json", &error);
  }
  return 0;
}
