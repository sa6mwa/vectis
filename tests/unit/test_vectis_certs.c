#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <vectis/vectis.h>

static void make_temp_path(char *path, size_t path_size, const char *label) {
  char templ[128];
  int fd;

  assert(path_size > 0u);
  assert(snprintf(templ, sizeof(templ), "/tmp/vectis-%s-XXXXXX", label) > 0);
  fd = mkstemp(templ);
  assert(fd >= 0);
  close(fd);
  remove(templ);
  assert(strlen(templ) + 1u <= path_size);
  strcpy(path, templ);
}

static void assert_generated_bundle_is_parseable(const char *path) {
  FILE *fp;
  X509 *cert;
  EVP_PKEY *key;
  char common_name[128];
  STACK_OF(GENERAL_NAME) *names;
  int found_dns;
  int found_ip;
  int i;
  const GENERAL_NAME *name;
  const unsigned char expected_ip[] = {127u, 0u, 0u, 1u};

  fp = fopen(path, "rb");
  assert(fp != NULL);
  cert = PEM_read_X509(fp, NULL, NULL, NULL);
  assert(cert != NULL);
  key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
  assert(key != NULL);
  fclose(fp);

  assert(X509_check_private_key(cert, key) == 1);
  memset(common_name, 0, sizeof(common_name));
  assert(X509_NAME_get_text_by_NID(X509_get_subject_name(cert),
                                   NID_commonName,
                                   common_name,
                                   (int)sizeof(common_name)) > 0);
  assert(strcmp(common_name, "api.local") == 0);

  found_dns = 0;
  found_ip = 0;
  names = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
  assert(names != NULL);
  for (i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
    name = sk_GENERAL_NAME_value(names, i);
    if (name->type == GEN_DNS &&
        ASN1_STRING_length(name->d.dNSName) == 9 &&
        memcmp(ASN1_STRING_get0_data(name->d.dNSName), "api.local", 9u) == 0) {
      found_dns = 1;
    }
    if (name->type == GEN_IPADD &&
        ASN1_STRING_length(name->d.iPAddress) == 4 &&
        memcmp(ASN1_STRING_get0_data(name->d.iPAddress), expected_ip, sizeof(expected_ip)) == 0) {
      found_ip = 1;
    }
  }
  GENERAL_NAMES_free(names);
  assert(found_dns);
  assert(found_ip);

  EVP_PKEY_free(key);
  X509_free(cert);
}

int main(void) {
  vectis_cert_bundle_config config;
  vectis_error error;
  vectis_status status;
  char bundle_path[128];

  vectis_error_clear(&error);
  status = vectis_cert_generate_bundle(NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "config") != NULL);

  vectis_cert_bundle_config_init(&config);
  status = vectis_cert_generate_bundle(&config, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "common_name") != NULL);

  config.subject.common_name = "api.local";
  status = vectis_cert_generate_bundle(&config, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "output") != NULL);

  config.output_bundle_path = "/tmp/vectis-missing-directory/server.pem";
  status = vectis_cert_generate_bundle(&config, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(error.source == VECTIS_ERROR_SOURCE_OPENSSL);

  config.output_bundle_path = "/tmp/vectis-expired.pem";
  config.valid_days = 0L;
  status = vectis_cert_generate_bundle(&config, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "valid_days") != NULL);

  config.valid_days = 397L;
  config.ca_cert_path = "/tmp/ca.pem";
  config.ca_key_path = "/tmp/ca.key";
  status = vectis_cert_generate_bundle(&config, &error);
  assert(status == VECTIS_ERR_NOT_IMPLEMENTED);
  assert(error.source == VECTIS_ERROR_SOURCE_OPENSSL);

  vectis_cert_bundle_config_init(&config);
  make_temp_path(bundle_path, sizeof(bundle_path), "cert-bundle");
  config.subject.common_name = "api.local";
  config.subject.organization = "Vectis";
  config.dns_names = "api.local, api.internal";
  config.ip_addresses = "127.0.0.1";
  config.output_bundle_path = bundle_path;
  status = vectis_cert_generate_bundle(&config, &error);
  assert(status == VECTIS_OK);
  assert(error.code == VECTIS_OK);
  assert_generated_bundle_is_parseable(bundle_path);
  remove(bundle_path);
  return 0;
}
