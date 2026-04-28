#include <vectis/vectis.h>

int main(void) {
  vectis_cert_bundle_config server;
  vectis_cert_bundle_config client;
  vectis_error error;

  vectis_cert_bundle_config_init(&server);
  server.subject.common_name = "orders-api.internal";
  server.subject.organization = "Example";
  server.dns_names = "orders-api.internal,orders-api";
  server.output_bundle_path = "certs/server.pem";
  server.key_bits = 4096u;
  server.valid_days = 90L;
  (void)vectis_cert_generate_bundle(&server, &error);

  vectis_cert_bundle_config_init(&client);
  client.subject.common_name = "orders-api.lockd-client";
  client.subject.organization = "Example";
  client.ca_cert_path = "certs/lockd-ca.crt";
  client.ca_key_path = "certs/lockd-ca.key";
  client.output_bundle_path = "certs/lockd-client.pem";
  client.valid_days = 90L;
  (void)vectis_cert_generate_bundle(&client, &error);

  return 0;
}
