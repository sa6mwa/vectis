#include <stdio.h>
#include <vectis/vectis.h>

int main(void) {
  vectis_cert_bundle_config ca;
  vectis_cert_bundle_config server;
  vectis_cert_bundle_config client;
  vectis_private_key_config key;
  vectis_csr_config csr;
  vectis_source bundle;
  vectis_error error;

  vectis_private_key_config_init(&key);
  key.output_key_path = "/tmp/vectis-example-csr-key.pem";
  if (vectis_cert_generate_private_key(&key, &error) != VECTIS_OK) {
    fprintf(stderr, "private key generation failed: %s\n", error.message);
    return 1;
  }

  vectis_csr_config_init(&csr);
  csr.subject.common_name = "csr.orders-api.internal";
  csr.subject.organization = "Example";
  csr.dns_names = "csr.orders-api.internal,csr-orders-api";
  csr.private_key_path = key.output_key_path;
  csr.output_csr_path = "/tmp/vectis-example-csr.pem";
  if (vectis_cert_generate_csr(&csr, &error) != VECTIS_OK) {
    fprintf(stderr, "CSR generation failed: %s\n", error.message);
    return 1;
  }

  vectis_cert_bundle_config_init(&ca);
  ca.subject.common_name = "Example Service CA";
  ca.subject.organization = "Example";
  ca.is_ca = 1;
  ca.output_bundle_path = "/tmp/vectis-example-ca.pem";
  ca.valid_days = 365L;
  if (vectis_cert_generate_bundle(&ca, &error) != VECTIS_OK) {
    fprintf(stderr, "CA generation failed: %s\n", error.message);
    return 1;
  }

  vectis_cert_bundle_config_init(&server);
  server.subject.common_name = "orders-api.internal";
  server.subject.organization = "Example";
  server.dns_names = "orders-api.internal,orders-api";
  server.ca_cert_path = ca.output_bundle_path;
  server.ca_key_path = ca.output_bundle_path;
  server.output_bundle_path = "/tmp/vectis-example-server.pem";
  server.key_bits = 4096u;
  server.valid_days = 90L;
  if (vectis_cert_generate_bundle(&server, &error) != VECTIS_OK) {
    fprintf(stderr, "server certificate generation failed: %s\n",
            error.message);
    return 1;
  }
  bundle = vectis_source_from_path(server.output_bundle_path);
  if (vectis_cert_validate_bundle(&bundle, &error) != VECTIS_OK) {
    fprintf(stderr, "server bundle validation failed: %s\n", error.message);
    return 1;
  }

  vectis_cert_bundle_config_init(&client);
  client.subject.common_name = "orders-api.lockd-client";
  client.subject.organization = "Example";
  client.ca_cert_path = ca.output_bundle_path;
  client.ca_key_path = ca.output_bundle_path;
  client.output_bundle_path = "/tmp/vectis-example-lockd-client.pem";
  client.valid_days = 90L;
  if (vectis_cert_generate_bundle(&client, &error) != VECTIS_OK) {
    fprintf(stderr, "client certificate generation failed: %s\n",
            error.message);
    return 1;
  }
  bundle = vectis_source_from_path(client.output_bundle_path);
  if (vectis_cert_validate_bundle(&bundle, &error) != VECTIS_OK) {
    fprintf(stderr, "client bundle validation failed: %s\n", error.message);
    return 1;
  }

  return 0;
}
