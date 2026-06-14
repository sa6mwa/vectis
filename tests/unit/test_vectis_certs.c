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
  STACK_OF(GENERAL_NAME) * names;
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
  assert(X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName,
                                   common_name, (int)sizeof(common_name)) > 0);
  assert(strcmp(common_name, "api.local") == 0);

  found_dns = 0;
  found_ip = 0;
  names = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
  assert(names != NULL);
  for (i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
    name = sk_GENERAL_NAME_value(names, i);
    if (name->type == GEN_DNS && ASN1_STRING_length(name->d.dNSName) == 9 &&
        memcmp(ASN1_STRING_get0_data(name->d.dNSName), "api.local", 9u) == 0) {
      found_dns = 1;
    }
    if (name->type == GEN_IPADD && ASN1_STRING_length(name->d.iPAddress) == 4 &&
        memcmp(ASN1_STRING_get0_data(name->d.iPAddress), expected_ip,
               sizeof(expected_ip)) == 0) {
      found_ip = 1;
    }
  }
  GENERAL_NAMES_free(names);
  assert(found_dns);
  assert(found_ip);

  EVP_PKEY_free(key);
  X509_free(cert);
}

static void assert_generated_key_is_parseable(const char *path) {
  FILE *fp;
  EVP_PKEY *key;

  fp = fopen(path, "rb");
  assert(fp != NULL);
  key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
  assert(key != NULL);
  assert(EVP_PKEY_base_id(key) == EVP_PKEY_RSA);
  fclose(fp);
  EVP_PKEY_free(key);
}

static void assert_generated_csr_is_parseable(const char *csr_path,
                                              const char *key_path) {
  FILE *fp;
  X509_REQ *request;
  EVP_PKEY *key;
  char common_name[128];
  STACK_OF(X509_EXTENSION) * extensions;
  STACK_OF(GENERAL_NAME) * names;
  X509_EXTENSION *extension;
  int found_dns;
  int found_ip;
  int i;
  int nid;
  const GENERAL_NAME *name;
  const unsigned char expected_ip[] = {127u, 0u, 0u, 1u};

  fp = fopen(csr_path, "rb");
  assert(fp != NULL);
  request = PEM_read_X509_REQ(fp, NULL, NULL, NULL);
  assert(request != NULL);
  fclose(fp);

  fp = fopen(key_path, "rb");
  assert(fp != NULL);
  key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
  assert(key != NULL);
  fclose(fp);

  assert(X509_REQ_verify(request, key) == 1);
  memset(common_name, 0, sizeof(common_name));
  assert(X509_NAME_get_text_by_NID(X509_REQ_get_subject_name(request),
                                   NID_commonName, common_name,
                                   (int)sizeof(common_name)) > 0);
  assert(strcmp(common_name, "csr.local") == 0);

  found_dns = 0;
  found_ip = 0;
  extensions = X509_REQ_get_extensions(request);
  assert(extensions != NULL);
  for (i = 0; i < sk_X509_EXTENSION_num(extensions); ++i) {
    extension = sk_X509_EXTENSION_value(extensions, i);
    nid = OBJ_obj2nid(X509_EXTENSION_get_object(extension));
    if (nid == NID_subject_alt_name) {
      names = X509V3_EXT_d2i(extension);
      assert(names != NULL);
      for (nid = 0; nid < sk_GENERAL_NAME_num(names); ++nid) {
        name = sk_GENERAL_NAME_value(names, nid);
        if (name->type == GEN_DNS && ASN1_STRING_length(name->d.dNSName) == 9 &&
            memcmp(ASN1_STRING_get0_data(name->d.dNSName), "csr.local", 9u) ==
                0) {
          found_dns = 1;
        }
        if (name->type == GEN_IPADD &&
            ASN1_STRING_length(name->d.iPAddress) == 4 &&
            memcmp(ASN1_STRING_get0_data(name->d.iPAddress), expected_ip,
                   sizeof(expected_ip)) == 0) {
          found_ip = 1;
        }
      }
      GENERAL_NAMES_free(names);
    }
  }
  sk_X509_EXTENSION_pop_free(extensions, X509_EXTENSION_free);
  assert(found_dns);
  assert(found_ip);

  EVP_PKEY_free(key);
  X509_REQ_free(request);
}

static void assert_bundle_is_signed_by(const char *bundle_path,
                                       const char *ca_bundle_path,
                                       const char *issuer_cn) {
  FILE *fp;
  X509 *cert;
  X509 *ca_cert;
  EVP_PKEY *ca_key;
  EVP_PKEY *key;
  char common_name[128];

  fp = fopen(bundle_path, "rb");
  assert(fp != NULL);
  cert = PEM_read_X509(fp, NULL, NULL, NULL);
  assert(cert != NULL);
  key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
  assert(key != NULL);
  fclose(fp);

  fp = fopen(ca_bundle_path, "rb");
  assert(fp != NULL);
  ca_cert = PEM_read_X509(fp, NULL, NULL, NULL);
  assert(ca_cert != NULL);
  ca_key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
  assert(ca_key != NULL);
  fclose(fp);

  memset(common_name, 0, sizeof(common_name));
  assert(X509_NAME_get_text_by_NID(X509_get_issuer_name(cert), NID_commonName,
                                   common_name, (int)sizeof(common_name)) > 0);
  assert(strcmp(common_name, issuer_cn) == 0);
  assert(X509_check_private_key(cert, key) == 1);
  assert(X509_check_private_key(ca_cert, ca_key) == 1);
  assert(X509_verify(cert, ca_key) == 1);

  EVP_PKEY_free(ca_key);
  X509_free(ca_cert);
  EVP_PKEY_free(key);
  X509_free(cert);
}

static void write_text_file(const char *path, const char *text) {
  FILE *fp;

  fp = fopen(path, "wb");
  assert(fp != NULL);
  assert(fwrite(text, 1u, strlen(text), fp) == strlen(text));
  assert(fclose(fp) == 0);
}

static void write_expired_bundle(const char *path) {
  EVP_PKEY_CTX *key_ctx;
  EVP_PKEY *key;
  X509 *cert;
  X509_NAME *name;
  FILE *fp;

  key_ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
  assert(key_ctx != NULL);
  key = NULL;
  assert(EVP_PKEY_keygen_init(key_ctx) > 0);
  assert(EVP_PKEY_CTX_set_rsa_keygen_bits(key_ctx, 1024) > 0);
  assert(EVP_PKEY_keygen(key_ctx, &key) > 0);

  cert = X509_new();
  assert(cert != NULL);
  assert(X509_set_version(cert, 2L) == 1);
  assert(ASN1_INTEGER_set(X509_get_serialNumber(cert), 42L) == 1);
  assert(X509_gmtime_adj(X509_get_notBefore(cert), -2L * 24L * 60L * 60L) !=
         NULL);
  assert(X509_gmtime_adj(X509_get_notAfter(cert), -1L * 24L * 60L * 60L) !=
         NULL);
  assert(X509_set_pubkey(cert, key) == 1);
  name = X509_get_subject_name(cert);
  assert(name != NULL);
  assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    (const unsigned char *)"expired.local", -1,
                                    -1, 0) == 1);
  assert(X509_set_issuer_name(cert, name) == 1);
  assert(X509_sign(cert, key, EVP_sha256()) > 0);

  fp = fopen(path, "wb");
  assert(fp != NULL);
  assert(PEM_write_X509(fp, cert) == 1);
  assert(PEM_write_PrivateKey(fp, key, NULL, NULL, 0, NULL, NULL) == 1);
  assert(fclose(fp) == 0);

  X509_free(cert);
  EVP_PKEY_free(key);
  EVP_PKEY_CTX_free(key_ctx);
}

int main(void) {
  vectis_cert_bundle_config config;
  vectis_cert_bundle_config ca_config;
  vectis_private_key_config key_config;
  vectis_csr_config csr_config;
  vectis_error error;
  vectis_status status;
  vectis_source source;
  vectis_source cert_source;
  vectis_source key_source;
  vectis_source ca_source;
  char bundle_path[128];
  char ca_bundle_path[128];
  char signed_bundle_path[128];
  char signed_cert_path[128];
  char signed_key_path[128];
  char csr_key_path[128];
  char csr_path[128];
  char malformed_path[128];
  char expired_path[128];

  vectis_error_clear(&error);
  status = vectis_cert_generate_private_key(NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "config") != NULL);

  vectis_private_key_config_init(&key_config);
  status = vectis_cert_generate_private_key(&key_config, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "output_key_path") != NULL);

  make_temp_path(csr_key_path, sizeof(csr_key_path), "csr-key");
  key_config.output_key_path = csr_key_path;
  key_config.key_bits = 0u;
  status = vectis_cert_generate_private_key(&key_config, &error);
  assert(status == VECTIS_OK);
  assert_generated_key_is_parseable(csr_key_path);

  status = vectis_cert_generate_csr(NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "config") != NULL);

  vectis_csr_config_init(&csr_config);
  status = vectis_cert_generate_csr(&csr_config, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "common_name") != NULL);

  make_temp_path(csr_path, sizeof(csr_path), "csr");
  csr_config.subject.common_name = "csr.local";
  csr_config.subject.organization = "Vectis";
  csr_config.dns_names = "csr.local,csr.internal";
  csr_config.ip_addresses = "127.0.0.1";
  csr_config.private_key_path = csr_key_path;
  csr_config.output_csr_path = csr_path;
  status = vectis_cert_generate_csr(&csr_config, &error);
  assert(status == VECTIS_OK);
  assert_generated_csr_is_parseable(csr_path, csr_key_path);
  remove(csr_path);
  remove(csr_key_path);

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
  config.valid_days = -1L;
  status = vectis_cert_generate_bundle(&config, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "valid_days") != NULL);

  config.valid_days = 397L;
  config.ca_cert_path = "/tmp/ca.pem";
  config.ca_key_path = NULL;
  status = vectis_cert_generate_bundle(&config, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "ca_cert_path") != NULL);
  assert(error.source == VECTIS_ERROR_SOURCE_OPENSSL);

  vectis_cert_bundle_config_init(&config);
  make_temp_path(bundle_path, sizeof(bundle_path), "cert-bundle");
  config.subject.common_name = "api.local";
  config.subject.organization = "Vectis";
  config.dns_names = "api.local, api.internal";
  config.ip_addresses = "127.0.0.1";
  config.output_bundle_path = bundle_path;
  config.key_bits = 0u;
  config.valid_days = 0L;
  status = vectis_cert_generate_bundle(&config, &error);
  assert(status == VECTIS_OK);
  assert(error.code == VECTIS_OK);
  assert_generated_bundle_is_parseable(bundle_path);
  source = vectis_source_from_path(bundle_path);
  status = vectis_cert_validate_bundle(&source, &error);
  assert(status == VECTIS_OK);
  remove(bundle_path);

  make_temp_path(malformed_path, sizeof(malformed_path), "malformed-cert");
  write_text_file(malformed_path, "not pem\n");
  source = vectis_source_from_path(malformed_path);
  status = vectis_cert_validate_bundle(&source, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "parse certificate") != NULL);
  remove(malformed_path);

  source = vectis_source_from_path("/tmp/vectis-missing-cert.pem");
  status = vectis_cert_validate_bundle(&source, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "open") != NULL);

  make_temp_path(expired_path, sizeof(expired_path), "expired-cert");
  write_expired_bundle(expired_path);
  source = vectis_source_from_path(expired_path);
  status = vectis_cert_validate_bundle(&source, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "expired") != NULL);
  remove(expired_path);

  vectis_cert_bundle_config_init(&ca_config);
  vectis_cert_bundle_config_init(&config);
  make_temp_path(ca_bundle_path, sizeof(ca_bundle_path), "ca-bundle");
  make_temp_path(signed_bundle_path, sizeof(signed_bundle_path),
                 "signed-bundle");
  make_temp_path(signed_cert_path, sizeof(signed_cert_path), "signed-cert");
  make_temp_path(signed_key_path, sizeof(signed_key_path), "signed-key");
  ca_config.subject.common_name = "Vectis Test CA";
  ca_config.output_bundle_path = ca_bundle_path;
  ca_config.is_ca = 1;
  ca_config.valid_days = 30L;
  status = vectis_cert_generate_bundle(&ca_config, &error);
  assert(status == VECTIS_OK);

  config.subject.common_name = "client.local";
  config.output_bundle_path = signed_bundle_path;
  config.output_cert_path = signed_cert_path;
  config.output_key_path = signed_key_path;
  config.ca_cert_path = ca_bundle_path;
  config.ca_key_path = ca_bundle_path;
  config.valid_days = 30L;
  status = vectis_cert_generate_bundle(&config, &error);
  assert(status == VECTIS_OK);
  assert_bundle_is_signed_by(signed_bundle_path, ca_bundle_path,
                             "Vectis Test CA");
  source = vectis_source_from_path(signed_bundle_path);
  status = vectis_cert_validate_bundle(&source, &error);
  assert(status == VECTIS_OK);
  cert_source = vectis_source_from_path(signed_cert_path);
  key_source = vectis_source_from_path(signed_key_path);
  ca_source = vectis_source_from_path(ca_bundle_path);
  status =
      vectis_cert_validate_pair(&cert_source, &key_source, &ca_source, &error);
  assert(status == VECTIS_OK);
  remove(ca_bundle_path);
  remove(signed_bundle_path);
  remove(signed_cert_path);
  remove(signed_key_path);
  return 0;
}
