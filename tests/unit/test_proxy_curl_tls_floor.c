#include "vectis_proxy_curl.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct tls_origin {
  SSL_CTX *context;
  int listener;
  unsigned short port;
  pthread_t thread;
  int handshakes[3];
  int requests[3];
} tls_origin;

static EVP_PKEY *make_key(void) {
  EVP_PKEY_CTX *context;
  EVP_PKEY *key;

  context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
  assert(context != NULL);
  assert(EVP_PKEY_keygen_init(context) == 1);
  assert(EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) == 1);
  key = NULL;
  assert(EVP_PKEY_keygen(context, &key) == 1);
  EVP_PKEY_CTX_free(context);
  return key;
}

static X509 *make_cert(EVP_PKEY *key) {
  X509_NAME *name;
  X509 *cert;

  cert = X509_new();
  assert(cert != NULL);
  assert(X509_set_version(cert, 2) == 1);
  assert(ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1);
  assert(X509_gmtime_adj(X509_getm_notBefore(cert), -60) != NULL);
  assert(X509_gmtime_adj(X509_getm_notAfter(cert), 3600) != NULL);
  assert(X509_set_pubkey(cert, key) == 1);
  name = X509_get_subject_name(cert);
  assert(name != NULL);
  assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    (const unsigned char *)"localhost", -1, -1,
                                    0) == 1);
  assert(X509_set_issuer_name(cert, name) == 1);
  assert(X509_sign(cert, key, EVP_sha256()) > 0);
  return cert;
}

static void *serve(void *userdata) {
  static const char response[] =
      "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
  tls_origin *origin;
  struct timeval timeout;
  SSL *ssl;
  char request[1024];
  int fd;
  int count;
  int sent;
  int i;

  origin = (tls_origin *)userdata;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  for (i = 0; i < 3; ++i) {
    fd = accept(origin->listener, NULL, NULL);
    assert(fd >= 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
           0);
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ==
           0);
    ssl = SSL_new(origin->context);
    assert(ssl != NULL);
    assert(SSL_set_fd(ssl, fd) == 1);
    if (SSL_accept(ssl) == 1) {
      origin->handshakes[i] = 1;
      assert(SSL_version(ssl) == TLS1_1_VERSION);
      count = SSL_read(ssl, request, sizeof(request) - 1u);
      assert(count > 0);
      request[count] = '\0';
      assert(strstr(request, "GET /tls-floor HTTP/1.1\r\n") == request);
      origin->requests[i] = 1;
      sent = SSL_write(ssl, response, sizeof(response) - 1u);
      assert(sent == (int)sizeof(response) - 1);
    }
    SSL_free(ssl);
    assert(close(fd) == 0);
  }
  return NULL;
}

static size_t discard(char *data, size_t size, size_t count, void *userdata) {
  (void)data;
  (void)userdata;
  return size * count;
}

static CURLcode request(tls_origin *origin, int mode) {
  CURLcode result;
  CURL *easy;
  long status;
  char url[128];

  easy = curl_easy_init();
  assert(easy != NULL);
  assert(snprintf(url, sizeof(url), "https://localhost:%u/tls-floor",
                  (unsigned)origin->port) > 0);
  assert(curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_SSL_CIPHER_LIST,
                          "DEFAULT:@SECLEVEL=0") == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_1) ==
         CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOPROXY, "*") == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 3000L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK);
  assert(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, discard) == CURLE_OK);
  if (mode == 1)
    assert(curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,
                            CURL_HTTP_VERSION_2TLS) == CURLE_OK);
  else
    assert(vectis_proxy_curl_configure_http(easy, mode == 0));
  result = curl_easy_perform(easy);
  status = 0L;
  assert(curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK);
  if (mode != 2)
    assert(result == CURLE_OK && status == 204L);
  else
    assert(result != CURLE_OK && status == 0L);
  curl_easy_cleanup(easy);
  return result;
}

int main(void) {
  struct sockaddr_in address;
  socklen_t address_length;
  tls_origin origin;
  EVP_PKEY *key;
  X509 *cert;

  (void)signal(SIGPIPE, SIG_IGN);
  assert(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK);
  key = make_key();
  cert = make_cert(key);
  memset(&origin, 0, sizeof(origin));
  origin.context = SSL_CTX_new(TLS_server_method());
  assert(origin.context != NULL);
  SSL_CTX_set_security_level(origin.context, 0);
  assert(SSL_CTX_set_min_proto_version(origin.context, TLS1_1_VERSION) == 1);
  assert(SSL_CTX_set_max_proto_version(origin.context, TLS1_1_VERSION) == 1);
  assert(SSL_CTX_set_cipher_list(origin.context, "DEFAULT:@SECLEVEL=0") == 1);
  assert(SSL_CTX_use_certificate(origin.context, cert) == 1);
  assert(SSL_CTX_use_PrivateKey(origin.context, key) == 1);
  origin.listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(origin.listener >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(origin.listener, (struct sockaddr *)&address, sizeof(address)) ==
         0);
  assert(listen(origin.listener, 3) == 0);
  address_length = sizeof(address);
  assert(getsockname(origin.listener, (struct sockaddr *)&address,
                     &address_length) == 0);
  origin.port = ntohs(address.sin_port);
  assert(pthread_create(&origin.thread, NULL, serve, &origin) == 0);
  assert(request(&origin, 0) == CURLE_OK);
  assert(request(&origin, 1) == CURLE_OK);
  assert(request(&origin, 2) != CURLE_OK);
  assert(pthread_join(origin.thread, NULL) == 0);
  assert(origin.handshakes[0] == 1 && origin.requests[0] == 1);
  assert(origin.handshakes[1] == 1 && origin.requests[1] == 1);
  assert(origin.handshakes[2] == 0 && origin.requests[2] == 0);
  assert(close(origin.listener) == 0);
  SSL_CTX_free(origin.context);
  X509_free(cert);
  EVP_PKEY_free(key);
  curl_global_cleanup();
  return 0;
}
