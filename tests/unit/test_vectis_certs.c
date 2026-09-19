#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

static int string_array_contains(char **items, size_t count,
                                 const char *value) {
  size_t i;

  for (i = 0u; i < count; ++i) {
    if (items[i] != NULL && strcmp(items[i], value) == 0) {
      return 1;
    }
  }
  return 0;
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

static void assert_secret_file_mode(const char *path) {
  struct stat st;

  assert(stat(path, &st) == 0);
  assert((st.st_mode & 0777) == 0600);
}

static X509 *read_certificate(const char *path) {
  FILE *fp;
  X509 *cert;

  fp = fopen(path, "rb");
  assert(fp != NULL);
  cert = PEM_read_X509(fp, NULL, NULL, NULL);
  assert(fclose(fp) == 0);
  assert(cert != NULL);
  return cert;
}

static void assert_distinct_bundle_serials(const char *const *paths,
                                           size_t path_count) {
  X509 *certificates[8];
  size_t i;
  size_t j;

  assert(path_count <= sizeof(certificates) / sizeof(certificates[0]));
  for (i = 0u; i < path_count; ++i) {
    BIGNUM *serial;

    certificates[i] = read_certificate(paths[i]);
    serial = ASN1_INTEGER_to_BN(X509_get_serialNumber(certificates[i]), NULL);
    assert(serial != NULL);
    assert(!BN_is_negative(serial));
    assert(!BN_is_zero(serial));
    BN_free(serial);
  }
  for (i = 0u; i < path_count; ++i) {
    for (j = 0u; j < i; ++j) {
      assert(ASN1_INTEGER_cmp(X509_get_serialNumber(certificates[i]),
                              X509_get_serialNumber(certificates[j])) != 0);
    }
  }
  for (i = 0u; i < path_count; ++i) {
    X509_free(certificates[i]);
  }
}

static void append_certificate_from_bundle(const char *bundle_path,
                                           const char *output_path,
                                           const char *mode) {
  FILE *bundle;
  FILE *output;
  X509 *cert;

  bundle = fopen(bundle_path, "rb");
  assert(bundle != NULL);
  cert = PEM_read_X509(bundle, NULL, NULL, NULL);
  assert(cert != NULL);
  assert(fclose(bundle) == 0);
  output = fopen(output_path, mode);
  assert(output != NULL);
  assert(PEM_write_X509(output, cert) == 1);
  assert(fclose(output) == 0);
  X509_free(cert);
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

static void write_test_bundle_with_validity(const char *path,
                                            int malformed_time) {
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
  if (malformed_time) {
    assert(ASN1_STRING_set(X509_getm_notBefore(cert), "991301000000Z", 13) ==
           1);
  }
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

static void write_expired_bundle(const char *path) {
  write_test_bundle_with_validity(path, 0);
}

static void write_invalid_time_bundle(const char *path) {
  write_test_bundle_with_validity(path, 1);
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
  vectis_cert_info info;
  char bundle_path[128];
  char ca_bundle_path[128];
  char signed_bundle_path[128];
  char signed_cert_path[128];
  char signed_key_path[128];
  char csr_key_path[128];
  char csr_path[128];
  char malformed_path[128];
  char expired_path[128];
  char invalid_time_path[128];
  char serial_paths[5][128];
  const char *serial_path_refs[5];
  char second_ca_bundle_path[128];
  char combined_ca_bundle_path[128];
  char second_signed_bundle_path[128];
  char intermediate_ca_bundle_path[128];
  char chained_leaf_bundle_path[128];
  char chained_certificate_path[128];
  size_t i;

  vectis_error_clear(&error);
  vectis_cert_info_init(&info);
  status = vectis_cert_generate_private_key(NULL, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "config") != NULL);

  vectis_private_key_config_init(&key_config);
  status = vectis_cert_generate_private_key(&key_config, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "output_key_path") != NULL);

  make_temp_path(csr_key_path, sizeof(csr_key_path), "csr-key");
  write_text_file(csr_key_path, "old private key");
  assert(chmod(csr_key_path, 0644) == 0);
  key_config.output_key_path = csr_key_path;
  key_config.key_bits = 0u;
  status = vectis_cert_generate_private_key(&key_config, &error);
  assert(status == VECTIS_OK);
  assert_generated_key_is_parseable(csr_key_path);
  assert_secret_file_mode(csr_key_path);

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
  write_text_file(bundle_path, "old certificate bundle");
  assert(chmod(bundle_path, 0644) == 0);
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
  assert_secret_file_mode(bundle_path);
  source = vectis_source_from_path(bundle_path);
  status = vectis_cert_validate_bundle(&source, &error);
  assert(status == VECTIS_OK);
  status = vectis_cert_inspect_bundle(&source, &info, &error);
  assert(status == VECTIS_OK);
  assert(info.version == 3L);
  assert(info.serial_hex != NULL && info.serial_hex[0] != '\0');
  assert(info.not_before != NULL && info.not_before[0] != '\0');
  assert(info.not_after != NULL && info.not_after[0] != '\0');
  assert(info.is_ca == 0);
  assert(strcmp(info.public_key_type, "rsa") == 0);
  assert(info.public_key_bits >= 1024u);
  assert(strcmp(info.subject.common_name, "api.local") == 0);
  assert(strcmp(info.subject.organization, "Vectis") == 0);
  assert(strcmp(info.issuer.common_name, "api.local") == 0);
  assert(info.subject_alt_names.dns_name_count == 2u);
  assert(string_array_contains(info.subject_alt_names.dns_names,
                               info.subject_alt_names.dns_name_count,
                               "api.local"));
  assert(string_array_contains(info.subject_alt_names.dns_names,
                               info.subject_alt_names.dns_name_count,
                               "api.internal"));
  assert(info.subject_alt_names.ip_address_count == 1u);
  assert(string_array_contains(info.subject_alt_names.ip_addresses,
                               info.subject_alt_names.ip_address_count,
                               "127.0.0.1"));
  vectis_cert_info_cleanup(&info);
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

  make_temp_path(invalid_time_path, sizeof(invalid_time_path), "invalid-time");
  write_invalid_time_bundle(invalid_time_path);
  source = vectis_source_from_path(invalid_time_path);
  status = vectis_cert_validate_bundle(&source, &error);
  assert(status == VECTIS_ERR_INVALID);
  assert(strstr(error.message, "validity timestamp") != NULL);
  remove(invalid_time_path);

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
  assert_secret_file_mode(signed_bundle_path);
  assert_secret_file_mode(signed_key_path);
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

  /* Certificate sources can carry the leaf followed by untrusted
   * intermediates; the supplied CA bundle remains the trust anchor. */
  make_temp_path(intermediate_ca_bundle_path,
                 sizeof(intermediate_ca_bundle_path), "intermediate-ca");
  make_temp_path(chained_leaf_bundle_path, sizeof(chained_leaf_bundle_path),
                 "chained-leaf");
  make_temp_path(chained_certificate_path, sizeof(chained_certificate_path),
                 "chained-certificate");
  vectis_cert_bundle_config_init(&ca_config);
  ca_config.subject.common_name = "Vectis Intermediate CA";
  ca_config.output_bundle_path = intermediate_ca_bundle_path;
  ca_config.ca_cert_path = ca_bundle_path;
  ca_config.ca_key_path = ca_bundle_path;
  ca_config.is_ca = 1;
  ca_config.key_bits = 1024u;
  ca_config.valid_days = 30L;
  status = vectis_cert_generate_bundle(&ca_config, &error);
  assert(status == VECTIS_OK);
  vectis_cert_bundle_config_init(&config);
  config.subject.common_name = "chained-client.local";
  config.output_bundle_path = chained_leaf_bundle_path;
  config.ca_cert_path = intermediate_ca_bundle_path;
  config.ca_key_path = intermediate_ca_bundle_path;
  config.key_bits = 1024u;
  config.valid_days = 30L;
  status = vectis_cert_generate_bundle(&config, &error);
  assert(status == VECTIS_OK);
  append_certificate_from_bundle(chained_leaf_bundle_path,
                                 chained_certificate_path, "wb");
  append_certificate_from_bundle(intermediate_ca_bundle_path,
                                 chained_certificate_path, "ab");
  cert_source = vectis_source_from_path(chained_certificate_path);
  key_source = vectis_source_from_path(chained_leaf_bundle_path);
  ca_source = vectis_source_from_path(ca_bundle_path);
  status =
      vectis_cert_validate_pair(&cert_source, &key_source, &ca_source, &error);
  assert(status == VECTIS_OK);
  remove(intermediate_ca_bundle_path);
  remove(chained_leaf_bundle_path);
  remove(chained_certificate_path);
  remove(signed_bundle_path);
  remove(signed_cert_path);
  remove(signed_key_path);

  /* Serial values must remain unique even when multiple issuances land in one
   * wall-clock second. */
  vectis_cert_bundle_config_init(&config);
  config.subject.common_name = "serial.local";
  config.key_bits = 1024u;
  for (i = 0u; i < sizeof(serial_paths) / sizeof(serial_paths[0]); ++i) {
    make_temp_path(serial_paths[i], sizeof(serial_paths[i]), "cert-serial");
    serial_path_refs[i] = serial_paths[i];
    config.output_bundle_path = serial_paths[i];
    status = vectis_cert_generate_bundle(&config, &error);
    assert(status == VECTIS_OK);
  }
  assert_distinct_bundle_serials(
      serial_path_refs, sizeof(serial_path_refs) / sizeof(serial_path_refs[0]));
  for (i = 0u; i < sizeof(serial_paths) / sizeof(serial_paths[0]); ++i) {
    remove(serial_paths[i]);
  }

  /* A CA bundle is a trust set: validation must consider every certificate,
   * not merely the first PEM object. */
  make_temp_path(second_ca_bundle_path, sizeof(second_ca_bundle_path),
                 "second-ca-bundle");
  make_temp_path(combined_ca_bundle_path, sizeof(combined_ca_bundle_path),
                 "combined-ca-bundle");
  make_temp_path(second_signed_bundle_path, sizeof(second_signed_bundle_path),
                 "second-signed-bundle");
  vectis_cert_bundle_config_init(&ca_config);
  ca_config.subject.common_name = "Vectis Second Test CA";
  ca_config.output_bundle_path = second_ca_bundle_path;
  ca_config.is_ca = 1;
  ca_config.key_bits = 1024u;
  status = vectis_cert_generate_bundle(&ca_config, &error);
  assert(status == VECTIS_OK);
  append_certificate_from_bundle(ca_bundle_path, combined_ca_bundle_path, "wb");
  append_certificate_from_bundle(second_ca_bundle_path, combined_ca_bundle_path,
                                 "ab");
  vectis_cert_bundle_config_init(&config);
  config.subject.common_name = "second-ca-client.local";
  config.output_bundle_path = second_signed_bundle_path;
  config.ca_cert_path = second_ca_bundle_path;
  config.ca_key_path = second_ca_bundle_path;
  config.key_bits = 1024u;
  status = vectis_cert_generate_bundle(&config, &error);
  assert(status == VECTIS_OK);
  cert_source = vectis_source_from_path(second_signed_bundle_path);
  key_source = vectis_source_from_path(second_signed_bundle_path);
  ca_source = vectis_source_from_path(combined_ca_bundle_path);
  status =
      vectis_cert_validate_pair(&cert_source, &key_source, &ca_source, &error);
  assert(status == VECTIS_OK);
  remove(second_ca_bundle_path);
  remove(combined_ca_bundle_path);
  remove(second_signed_bundle_path);
  remove(ca_bundle_path);
  vectis_cert_info_cleanup(&info);
  return 0;
}
