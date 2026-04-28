#include <stddef.h>

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

int main(void) {
  vectis_mqtt_config mqtt;
  vectis_error error;
  workflow_event event;
  const char raw_payload[] = "ready";

  vectis_mqtt_config_init(&mqtt);
  mqtt.broker_url = "mqtts://broker.example.com:8883";
  mqtt.client_bundle = vectis_source_from_path("/etc/vectis/mqtt-client.pem");
  mqtt.ca_bundle = vectis_source_from_path("/etc/vectis/ca.pem");

  event.id[0] = '1';
  event.id[1] = '\0';
  event.type[0] = 'o';
  event.type[1] = 'r';
  event.type[2] = 'd';
  event.type[3] = 'e';
  event.type[4] = 'r';
  event.type[5] = '\0';

  (void)vectis_mqtt_publish(&mqtt,
                            "workflow/orders/raw",
                            raw_payload,
                            sizeof(raw_payload) - 1u,
                            "text/plain",
                            &error);
  (void)vectis_mqtt_publish_json(&mqtt,
                                 "workflow/orders/json",
                                 &workflow_event_map,
                                 &event,
                                 &error);
  return 0;
}
