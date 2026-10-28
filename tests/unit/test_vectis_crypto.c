#include <assert.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vectis/crypto.h>
static vectis_error error;
static void check(vectis_status status) {
  if (status != VECTIS_OK)
    fprintf(stderr, "%s: %s\n", error.message, error.detail);
  assert(status == VECTIS_OK);
}
static void *foreign(void *context) {
  vectis_crypto_key *key = (vectis_crypto_key *)context;
  vectis_crypto_key_info info;
  vectis_error failure;
  assert(key->info(key, &info, &failure) == VECTIS_ERR_STATE);
  assert(key->close(key, &failure) == VECTIS_ERR_STATE);
  return NULL;
}
static void fixture_save(const char *directory, const char *name, BIO *bio) {
  char path[4096], *data;
  long size = BIO_get_mem_data(bio, &data);
  FILE *file;
  int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
  assert(size > 0 && length > 0 && (size_t)length < sizeof(path));
  file = fopen(path, "wb");
  assert(file != NULL && fwrite(data, 1u, (size_t)size, file) == (size_t)size &&
         fclose(file) == 0);
}
static void keys(const char *fixture_directory) {
  EVP_PKEY_CTX *generator = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
  EVP_PKEY *generated = NULL;
  BIO *private_bio = BIO_new(BIO_s_mem()), *public_bio = BIO_new(BIO_s_mem()),
      *encrypted_bio = BIO_new(BIO_s_mem()),
      *certificate_bio = BIO_new(BIO_s_mem());
  X509 *certificate = X509_new();
  X509_NAME *subject;
  char password[] = "fixture-password";
  char *pem, *public_pem, *encrypted_pem, *certificate_pem;
  long private_size, public_size, encrypted_size, certificate_size;
  vectis_crypto_key *private_key, *public_key, *encrypted, *bad,
      *certificate_key;
  vectis_crypto_key_info info;
  vectis_mutable_bytes signature;
  int verified, status;
  FILE *file;
  pid_t child;
  pthread_t thread;
  assert(generator != NULL && private_bio != NULL && public_bio != NULL &&
         encrypted_bio != NULL && certificate_bio != NULL &&
         certificate != NULL);
  assert(EVP_PKEY_keygen_init(generator) == 1 &&
         EVP_PKEY_CTX_set_rsa_keygen_bits(generator, 2048) == 1 &&
         EVP_PKEY_keygen(generator, &generated) == 1);
  assert(PEM_write_bio_PrivateKey(private_bio, generated, NULL, NULL, 0, NULL,
                                  NULL) == 1);
  assert(PEM_write_bio_PUBKEY(public_bio, generated) == 1);
  assert(PEM_write_bio_PrivateKey(encrypted_bio, generated, EVP_aes_256_cbc(),
                                  (unsigned char *)password,
                                  (int)strlen(password), NULL, NULL) == 1);
  assert(X509_set_version(certificate, 2) == 1 &&
         ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) == 1);
  assert(X509_gmtime_adj(X509_getm_notBefore(certificate), 0) != NULL &&
         X509_gmtime_adj(X509_getm_notAfter(certificate), 3600) != NULL);
  assert(X509_set_pubkey(certificate, generated) == 1);
  subject = X509_get_subject_name(certificate);
  assert(subject != NULL &&
         X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                    (const unsigned char *)"Vectis fixture", -1,
                                    -1, 0) == 1);
  assert(X509_set_issuer_name(certificate, subject) == 1 &&
         X509_sign(certificate, generated, EVP_sha256()) > 0);
  assert(PEM_write_bio_X509(certificate_bio, certificate) == 1);
  X509_free(certificate);
  private_size = BIO_get_mem_data(private_bio, &pem);
  public_size = BIO_get_mem_data(public_bio, &public_pem);
  encrypted_size = BIO_get_mem_data(encrypted_bio, &encrypted_pem);
  certificate_size = BIO_get_mem_data(certificate_bio, &certificate_pem);
  assert(private_size > 0 && public_size > 0 && encrypted_size > 0);
  if (fixture_directory != NULL) {
    fixture_save(fixture_directory, "private.pem", private_bio);
    fixture_save(fixture_directory, "public.pem", public_bio);
    fixture_save(fixture_directory, "encrypted.pem", encrypted_bio);
    fixture_save(fixture_directory, "certificate.pem", certificate_bio);
    BIO_free(private_bio);
    BIO_free(public_bio);
    BIO_free(encrypted_bio);
    BIO_free(certificate_bio);
    EVP_PKEY_free(generated);
    EVP_PKEY_CTX_free(generator);
    return;
  }
  check(vectis_crypto_key_load(VECTIS_CRYPTO_CERTIFICATE, certificate_pem,
                               (size_t)certificate_size, NULL, 0u,
                               &certificate_key, &error));
  check(vectis_crypto_key_load(VECTIS_CRYPTO_PRIVATE_KEY, pem,
                               (size_t)private_size, NULL, 0u, &private_key,
                               &error));
  check(vectis_crypto_key_load(VECTIS_CRYPTO_PUBLIC_KEY, public_pem,
                               (size_t)public_size, NULL, 0u, &public_key,
                               &error));
  check(vectis_crypto_key_load(VECTIS_CRYPTO_PRIVATE_KEY, encrypted_pem,
                               (size_t)encrypted_size, password,
                               strlen(password), &encrypted, &error));
  assert(vectis_crypto_key_load(VECTIS_CRYPTO_PRIVATE_KEY, encrypted_pem,
                                (size_t)encrypted_size, "wrong", 5u, &bad,
                                &error) != VECTIS_OK &&
         bad == NULL);
  assert(vectis_crypto_key_load(VECTIS_CRYPTO_PRIVATE_KEY, encrypted_pem,
                                (size_t)encrypted_size, NULL, 0u, &bad,
                                &error) != VECTIS_OK &&
         bad == NULL);
  file = fopen(VECTIS_CRYPTO_KEY_PATH, "wb");
  assert(file != NULL &&
         fwrite(pem, 1u, (size_t)private_size, file) == (size_t)private_size &&
         fclose(file) == 0);
  check(vectis_crypto_key_load_file(VECTIS_CRYPTO_PRIVATE_KEY,
                                    VECTIS_CRYPTO_KEY_PATH, NULL, 0u, &bad,
                                    &error));
  assert(unlink(VECTIS_CRYPTO_KEY_PATH) == 0);
  check(bad->close(bad, &error));
  BIO_free(private_bio);
  BIO_free(public_bio);
  BIO_free(encrypted_bio);
  BIO_free(certificate_bio);
  EVP_PKEY_free(generated);
  EVP_PKEY_CTX_free(generator);
  check(private_key->info(private_key, &info, &error));
  assert(!strcmp(info.type, "RSA") && info.bits == 2048 && info.private_key &&
         info.max_signature_bytes == 256u);
  check(public_key->info(public_key, &info, &error));
  assert(!info.private_key);
  check(private_key->sign(private_key, NULL, "a\0b", 3u, &signature, &error));
  assert(signature.size == 256u);
  check(public_key->verify(public_key, NULL, "a\0b", 3u, signature.data,
                           signature.size, &verified, &error));
  assert(verified);
  check(certificate_key->verify(certificate_key, NULL, "a\0b", 3u,
                                signature.data, signature.size, &verified,
                                &error));
  assert(verified);
  check(public_key->verify(public_key, NULL, "changed", 7u, signature.data,
                           signature.size, &verified, &error));
  assert(!verified);
  vectis_mutable_bytes_cleanup(&signature);
  check(encrypted->sign(encrypted, "sha512", NULL, 0u, &signature, &error));
  check(public_key->verify(public_key, "sha512", NULL, 0u, signature.data,
                           signature.size, &verified, &error));
  assert(verified);
  vectis_mutable_bytes_cleanup(&signature);
  assert(public_key->sign(public_key, NULL, "x", 1u, &signature, &error) ==
             VECTIS_ERR_INVALID &&
         signature.data == NULL);
  assert(private_key->sign(private_key, "not-a-digest", "x", 1u, &signature,
                           &error) == VECTIS_ERR_INVALID);
  assert(vectis_crypto_key_load(VECTIS_CRYPTO_PRIVATE_KEY, "invalid", 7u, NULL,
                                0u, &bad, &error) != VECTIS_OK &&
         bad == NULL);
  assert(pthread_create(&thread, NULL, foreign, private_key) == 0 &&
         pthread_join(thread, NULL) == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    assert(private_key->info(private_key, &info, &error) == VECTIS_ERR_STATE);
    assert(private_key->close(private_key, &error) == VECTIS_ERR_STATE);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0);
  check(private_key->sign(private_key, NULL, "after fork", 10u, &signature,
                          &error));
  vectis_mutable_bytes_cleanup(&signature);
  check(private_key->close(private_key, &error));
  check(public_key->close(public_key, &error));
  check(encrypted->close(encrypted, &error));
  check(certificate_key->close(certificate_key, &error));
}
static void encryption(void) {
  unsigned char key[32] = {0}, nonce[12] = {0}, data[16] = {0};
  static const unsigned char ciphertext[16] = {
      0x03, 0x88, 0xda, 0xce, 0x60, 0xb6, 0xa3, 0x92,
      0xf3, 0x28, 0xc2, 0xb9, 0x71, 0xb2, 0xfe, 0x78};
  static const unsigned char tag[16] = {0xab, 0x6e, 0x47, 0xd4, 0x2c, 0xec,
                                        0x13, 0xbd, 0xf5, 0x3a, 0x67, 0xb2,
                                        0x12, 0x57, 0xbd, 0xdf};
  vectis_crypto_gcm_result encrypted;
  vectis_mutable_bytes plaintext;
  size_t key_size;
  check(vectis_crypto_gcm_encrypt_materialized(
      key, 16u, nonce, 12u, data, sizeof(data), NULL, 0u, &encrypted, &error));
  assert(encrypted.ciphertext.size == 16u &&
         !memcmp(encrypted.ciphertext.data, ciphertext, 16u) &&
         !memcmp(encrypted.tag, tag, 16u));
  vectis_crypto_gcm_result_cleanup(&encrypted);
  for (key_size = 16u; key_size <= 32u; key_size += 8u) {
    check(vectis_crypto_gcm_encrypt_materialized(key, key_size, nonce, 12u,
                                                 data, sizeof(data), "aad\0",
                                                 4u, &encrypted, &error));
    check(vectis_crypto_gcm_decrypt_materialized(
        key, key_size, nonce, 12u, encrypted.ciphertext.data,
        encrypted.ciphertext.size, "aad\0", 4u, encrypted.tag, 16u, &plaintext,
        &error));
    assert(plaintext.size == 16u && !memcmp(plaintext.data, data, 16u));
    vectis_mutable_bytes_cleanup(&plaintext);
    encrypted.tag[0] ^= 1u;
    assert(vectis_crypto_gcm_decrypt_materialized(
               key, key_size, nonce, 12u, encrypted.ciphertext.data,
               encrypted.ciphertext.size, "aad\0", 4u, encrypted.tag, 16u,
               &plaintext, &error) != VECTIS_OK &&
           plaintext.data == NULL && plaintext.size == 0u);
    encrypted.tag[0] ^= 1u;
    assert(vectis_crypto_gcm_decrypt_materialized(
               key, key_size, nonce, 12u, encrypted.ciphertext.data,
               encrypted.ciphertext.size, "wrong", 5u, encrypted.tag, 16u,
               &plaintext, &error) != VECTIS_OK &&
           plaintext.data == NULL);
    vectis_crypto_gcm_result_cleanup(&encrypted);
    check(vectis_crypto_gcm_encrypt_materialized(
        key, key_size, nonce, 12u, NULL, 0u, NULL, 0u, &encrypted, &error));
    check(vectis_crypto_gcm_decrypt_materialized(
        key, key_size, nonce, 12u, encrypted.ciphertext.data, 0u, NULL, 0u,
        encrypted.tag, 16u, &plaintext, &error));
    assert(plaintext.size == 0u);
    vectis_mutable_bytes_cleanup(&plaintext);
    vectis_crypto_gcm_result_cleanup(&encrypted);
  }
  assert(vectis_crypto_gcm_encrypt_materialized(key, 15u, nonce, 12u, data, 16u,
                                                NULL, 0u, &encrypted,
                                                &error) == VECTIS_ERR_INVALID);
  assert(vectis_crypto_gcm_encrypt_materialized(key, 16u, nonce, 11u, data, 16u,
                                                NULL, 0u, &encrypted,
                                                &error) == VECTIS_ERR_INVALID);
  assert(vectis_crypto_gcm_encrypt_materialized(
             key, 16u, nonce, 12u, data, VECTIS_CRYPTO_MAX_BYTES + 1u, NULL, 0u,
             &encrypted, &error) == VECTIS_ERR_INVALID);
  assert(vectis_crypto_gcm_decrypt_materialized(key, 16u, nonce, 12u, data, 16u,
                                                NULL, 0u, tag, 15u, &plaintext,
                                                &error) == VECTIS_ERR_INVALID &&
         plaintext.data == NULL);
}
int main(int argc, char **argv) {
  assert(argc == 1 || argc == 2);
  if (argc == 2) {
    keys(argv[1]);
    return 0;
  }
  keys(NULL);
  encryption();
  return 0;
}
