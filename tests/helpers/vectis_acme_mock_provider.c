#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct acme_mock {
  char base_url[128];
  EVP_PKEY *ca_key;
  X509 *ca_cert;
  char *cert_pem;
  int challenge_posted;
  int cert_ready;
} acme_mock;

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int sig) {
  (void)sig;
  stop_requested = 1;
}

static int write_all(int fd, const char *data, size_t size) {
  size_t offset;
  ssize_t n;

  offset = 0u;
  while (offset < size) {
    n = send(fd, data + offset, size - offset, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return 0;
    }
    if (n == 0) {
      return 0;
    }
    offset += (size_t)n;
  }
  return 1;
}

static int send_response(int fd, int status, const char *reason,
                         const char *extra_headers, const char *content_type,
                         const char *body) {
  char header[1024];
  size_t body_size;
  int written;

  body_size = body != NULL ? strlen(body) : 0u;
  written =
      snprintf(header, sizeof(header),
               "HTTP/1.1 %d %s\r\n"
               "Connection: close\r\n"
               "Replay-Nonce: nonce-%ld\r\n"
               "%s"
               "%s%s%s"
               "Content-Length: %lu\r\n\r\n",
               status, reason, (long)time(NULL),
               extra_headers != NULL ? extra_headers : "",
               content_type != NULL ? "Content-Type: " : "",
               content_type != NULL ? content_type : "",
               content_type != NULL ? "\r\n" : "", (unsigned long)body_size);
  if (written < 0 || (size_t)written >= sizeof(header)) {
    return 0;
  }
  if (!write_all(fd, header, (size_t)written)) {
    return 0;
  }
  if (body_size > 0u && !write_all(fd, body, body_size)) {
    return 0;
  }
  return 1;
}

static int send_head_nonce(int fd) {
  char header[256];
  int written;

  written = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Connection: close\r\n"
                     "Replay-Nonce: nonce-%ld\r\n"
                     "Content-Length: 0\r\n\r\n",
                     (long)time(NULL));
  if (written < 0 || (size_t)written >= sizeof(header)) {
    return 0;
  }
  return write_all(fd, header, (size_t)written);
}

static int b64url_value(int c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '-') {
    return 62;
  }
  if (c == '_') {
    return 63;
  }
  return -1;
}

static unsigned char *b64url_decode(const char *input, size_t *out_size) {
  unsigned char *out;
  size_t len;
  size_t capacity;
  size_t i;
  size_t written;
  int values[4];
  int count;
  int value;
  unsigned triple;

  len = strlen(input);
  capacity = (len / 4u + 2u) * 3u;
  out = (unsigned char *)malloc(capacity + 1u);
  if (out == NULL) {
    return NULL;
  }
  written = 0u;
  count = 0;
  for (i = 0u; i < len; ++i) {
    value = b64url_value((unsigned char)input[i]);
    if (value < 0) {
      free(out);
      return NULL;
    }
    values[count++] = value;
    if (count == 4) {
      triple = ((unsigned)values[0] << 18u) | ((unsigned)values[1] << 12u) |
               ((unsigned)values[2] << 6u) | (unsigned)values[3];
      out[written++] = (unsigned char)((triple >> 16u) & 0xffu);
      out[written++] = (unsigned char)((triple >> 8u) & 0xffu);
      out[written++] = (unsigned char)(triple & 0xffu);
      count = 0;
    }
  }
  if (count == 2) {
    triple = ((unsigned)values[0] << 18u) | ((unsigned)values[1] << 12u);
    out[written++] = (unsigned char)((triple >> 16u) & 0xffu);
  } else if (count == 3) {
    triple = ((unsigned)values[0] << 18u) | ((unsigned)values[1] << 12u) |
             ((unsigned)values[2] << 6u);
    out[written++] = (unsigned char)((triple >> 16u) & 0xffu);
    out[written++] = (unsigned char)((triple >> 8u) & 0xffu);
  } else if (count != 0) {
    free(out);
    return NULL;
  }
  out[written] = '\0';
  *out_size = written;
  return out;
}

static char *json_string_value(const char *json, const char *key) {
  char needle[96];
  const char *cursor;
  const char *start;
  const char *end;
  char *out;
  size_t size;
  int written;

  written = snprintf(needle, sizeof(needle), "\"%s\":\"", key);
  if (written < 0 || (size_t)written >= sizeof(needle)) {
    return NULL;
  }
  cursor = strstr(json, needle);
  if (cursor == NULL) {
    return NULL;
  }
  start = cursor + strlen(needle);
  end = start;
  while (*end != '\0' && *end != '"') {
    end++;
  }
  if (*end != '"') {
    return NULL;
  }
  size = (size_t)(end - start);
  out = (char *)malloc(size + 1u);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, start, size);
  out[size] = '\0';
  return out;
}

static EVP_PKEY *generate_rsa_key(void) {
  EVP_PKEY_CTX *ctx;
  EVP_PKEY *key;

  ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
  if (ctx == NULL) {
    return NULL;
  }
  key = NULL;
  if (EVP_PKEY_keygen_init(ctx) != 1 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) != 1 ||
      EVP_PKEY_keygen(ctx, &key) != 1) {
    EVP_PKEY_free(key);
    key = NULL;
  }
  EVP_PKEY_CTX_free(ctx);
  return key;
}

static int add_name_entry(X509_NAME *name, const char *field,
                          const char *value) {
  return X509_NAME_add_entry_by_txt(name, field, MBSTRING_ASC,
                                    (const unsigned char *)value, -1, -1,
                                    0) == 1;
}

static int init_ca(acme_mock *mock) {
  X509_NAME *name;

  mock->ca_key = generate_rsa_key();
  mock->ca_cert = X509_new();
  if (mock->ca_key == NULL || mock->ca_cert == NULL) {
    return 0;
  }
  if (X509_set_version(mock->ca_cert, 2L) != 1 ||
      ASN1_INTEGER_set(X509_get_serialNumber(mock->ca_cert), 1L) != 1 ||
      X509_gmtime_adj(X509_get_notBefore(mock->ca_cert), 0L) == NULL ||
      X509_gmtime_adj(X509_get_notAfter(mock->ca_cert), 86400L) == NULL ||
      X509_set_pubkey(mock->ca_cert, mock->ca_key) != 1) {
    return 0;
  }
  name = X509_get_subject_name(mock->ca_cert);
  if (name == NULL || !add_name_entry(name, "CN", "Vectis Mock ACME CA") ||
      X509_set_issuer_name(mock->ca_cert, name) != 1 ||
      X509_sign(mock->ca_cert, mock->ca_key, EVP_sha256()) == 0) {
    return 0;
  }
  return 1;
}

static char *sign_csr(acme_mock *mock, const unsigned char *der,
                      size_t der_size) {
  const unsigned char *cursor;
  X509_REQ *request;
  X509 *cert;
  EVP_PKEY *pubkey;
  STACK_OF(X509_EXTENSION) * exts;
  BIO *bio;
  BUF_MEM *mem;
  char *pem;
  size_t pem_size;
  int i;

  cursor = der;
  request = d2i_X509_REQ(NULL, &cursor, (long)der_size);
  if (request == NULL) {
    return NULL;
  }
  cert = X509_new();
  pubkey = X509_REQ_get_pubkey(request);
  bio = NULL;
  pem = NULL;
  if (cert == NULL || pubkey == NULL || X509_set_version(cert, 2L) != 1 ||
      ASN1_INTEGER_set(X509_get_serialNumber(cert), (long)time(NULL)) != 1 ||
      X509_gmtime_adj(X509_get_notBefore(cert), 0L) == NULL ||
      X509_gmtime_adj(X509_get_notAfter(cert), 86400L) == NULL ||
      X509_set_subject_name(cert, X509_REQ_get_subject_name(request)) != 1 ||
      X509_set_issuer_name(cert, X509_get_subject_name(mock->ca_cert)) != 1 ||
      X509_set_pubkey(cert, pubkey) != 1) {
    goto done;
  }
  exts = X509_REQ_get_extensions(request);
  if (exts != NULL) {
    for (i = 0; i < sk_X509_EXTENSION_num(exts); ++i) {
      if (X509_add_ext(cert, sk_X509_EXTENSION_value(exts, i), -1) != 1) {
        sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
        goto done;
      }
    }
    sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);
  }
  if (X509_sign(cert, mock->ca_key, EVP_sha256()) == 0) {
    goto done;
  }
  bio = BIO_new(BIO_s_mem());
  if (bio == NULL || PEM_write_bio_X509(bio, cert) != 1) {
    goto done;
  }
  BIO_get_mem_ptr(bio, &mem);
  pem_size = mem != NULL ? mem->length : 0u;
  pem = (char *)malloc(pem_size + 1u);
  if (pem == NULL) {
    goto done;
  }
  memcpy(pem, mem->data, pem_size);
  pem[pem_size] = '\0';

done:
  BIO_free(bio);
  EVP_PKEY_free(pubkey);
  X509_free(cert);
  X509_REQ_free(request);
  return pem;
}

static int finalize_order(acme_mock *mock, const char *body) {
  char *payload_b64;
  unsigned char *payload;
  size_t payload_size;
  char *csr_b64;
  unsigned char *csr;
  size_t csr_size;
  char *pem;

  payload_b64 = json_string_value(body, "payload");
  if (payload_b64 == NULL) {
    return 0;
  }
  payload = b64url_decode(payload_b64, &payload_size);
  free(payload_b64);
  if (payload == NULL) {
    return 0;
  }
  csr_b64 = json_string_value((const char *)payload, "csr");
  free(payload);
  if (csr_b64 == NULL) {
    return 0;
  }
  csr = b64url_decode(csr_b64, &csr_size);
  free(csr_b64);
  if (csr == NULL) {
    return 0;
  }
  pem = sign_csr(mock, csr, csr_size);
  free(csr);
  if (pem == NULL) {
    return 0;
  }
  free(mock->cert_pem);
  mock->cert_pem = pem;
  mock->cert_ready = 1;
  return 1;
}

static int read_request(int fd, char *method, size_t method_size, char *path,
                        size_t path_size, char **out_body) {
  char *buffer;
  size_t capacity;
  size_t size;
  ssize_t n;
  char *headers_end;
  char *line_end;
  char *content_length_header;
  size_t header_size;
  size_t content_length;
  int scanned;

  capacity = 262144u;
  buffer = (char *)malloc(capacity + 1u);
  if (buffer == NULL) {
    return 0;
  }
  size = 0u;
  headers_end = NULL;
  content_length = 0u;
  while (size < capacity) {
    n = recv(fd, buffer + size, capacity - size, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      free(buffer);
      return 0;
    }
    if (n == 0) {
      break;
    }
    size += (size_t)n;
    buffer[size] = '\0';
    headers_end = strstr(buffer, "\r\n\r\n");
    if (headers_end != NULL) {
      content_length_header = strstr(buffer, "Content-Length:");
      if (content_length_header != NULL &&
          content_length_header < headers_end) {
        content_length = (size_t)strtoul(content_length_header + 15, NULL, 10);
      }
      header_size = (size_t)(headers_end + 4 - buffer);
      if (size >= header_size + content_length) {
        break;
      }
    }
  }
  if (headers_end == NULL) {
    free(buffer);
    return 0;
  }
  line_end = strstr(buffer, "\r\n");
  if (line_end == NULL) {
    free(buffer);
    return 0;
  }
  *line_end = '\0';
  scanned = sscanf(buffer, "%15s %255s", method, path);
  if (scanned != 2) {
    free(buffer);
    return 0;
  }
  method[method_size - 1u] = '\0';
  path[path_size - 1u] = '\0';
  header_size = (size_t)(headers_end + 4 - buffer);
  *out_body = (char *)malloc(content_length + 1u);
  if (*out_body == NULL) {
    free(buffer);
    return 0;
  }
  memcpy(*out_body, buffer + header_size, content_length);
  (*out_body)[content_length] = '\0';
  free(buffer);
  return 1;
}

static int handle_request(acme_mock *mock, int fd) {
  char method[16];
  char path[256];
  char body_order[512];
  char body_auth[512];
  char body[512];
  char location[256];
  char *request_body;
  int written;

  request_body = NULL;
  if (!read_request(fd, method, sizeof(method), path, sizeof(path),
                    &request_body)) {
    return 0;
  }
  if (strcmp(method, "HEAD") == 0 && strcmp(path, "/nonce") == 0) {
    free(request_body);
    return send_head_nonce(fd);
  }
  if (strcmp(method, "GET") == 0 && strcmp(path, "/directory") == 0) {
    written = snprintf(body, sizeof(body),
                       "{\"newNonce\":\"%s/nonce\",\"newAccount\":\"%s/"
                       "account\",\"newOrder\":\"%s/order\","
                       "\"revokeCert\":\"%s/revoke\"}",
                       mock->base_url, mock->base_url, mock->base_url,
                       mock->base_url);
    free(request_body);
    if (written < 0 || (size_t)written >= sizeof(body)) {
      return 0;
    }
    return send_response(fd, 200, "OK", NULL, "application/json", body);
  }
  if (strcmp(method, "POST") == 0 && strcmp(path, "/account") == 0) {
    written = snprintf(location, sizeof(location), "Location: %s/account/1\r\n",
                       mock->base_url);
    free(request_body);
    if (written < 0 || (size_t)written >= sizeof(location)) {
      return 0;
    }
    return send_response(fd, 201, "Created", location, "application/json",
                         "{}");
  }
  if (strcmp(method, "POST") == 0 && strcmp(path, "/order") == 0) {
    written = snprintf(body_order, sizeof(body_order),
                       "{\"status\":\"pending\",\"authorizations\":[\"%s/"
                       "authz/1\"],\"finalize\":\"%s/finalize/1\"}",
                       mock->base_url, mock->base_url);
    if (written >= 0 && (size_t)written < sizeof(body_order)) {
      written = snprintf(location, sizeof(location), "Location: %s/order/1\r\n",
                         mock->base_url);
    }
    free(request_body);
    if (written < 0 || (size_t)written >= sizeof(location)) {
      return 0;
    }
    return send_response(fd, 201, "Created", location, "application/json",
                         body_order);
  }
  if (strcmp(method, "POST") == 0 && strcmp(path, "/authz/1") == 0) {
    written =
        snprintf(body_auth, sizeof(body_auth),
                 "{\"status\":\"%s\",\"challenges\":[{\"type\":"
                 "\"tls-alpn-01\",\"url\":\"%s/challenge/1\","
                 "\"token\":\"token-1\",\"status\":\"%s\"}]}",
                 mock->challenge_posted ? "valid" : "pending", mock->base_url,
                 mock->challenge_posted ? "valid" : "pending");
    free(request_body);
    if (written < 0 || (size_t)written >= sizeof(body_auth)) {
      return 0;
    }
    return send_response(fd, 200, "OK", NULL, "application/json", body_auth);
  }
  if (strcmp(method, "POST") == 0 && strcmp(path, "/challenge/1") == 0) {
    mock->challenge_posted = 1;
    free(request_body);
    return send_response(fd, 200, "OK", NULL, "application/json",
                         "{\"status\":\"valid\"}");
  }
  if (strcmp(method, "POST") == 0 && strcmp(path, "/finalize/1") == 0) {
    if (!finalize_order(mock, request_body)) {
      free(request_body);
      return send_response(fd, 400, "Bad Request", NULL, "application/json",
                           "{\"detail\":\"invalid csr\"}");
    }
    free(request_body);
    return send_response(fd, 200, "OK", NULL, "application/json",
                         "{\"status\":\"processing\"}");
  }
  if (strcmp(method, "POST") == 0 && strcmp(path, "/order/1") == 0) {
    if (mock->cert_ready) {
      written = snprintf(body_order, sizeof(body_order),
                         "{\"status\":\"valid\",\"certificate\":\"%s/cert/1\"}",
                         mock->base_url);
    } else if (mock->challenge_posted) {
      written =
          snprintf(body_order, sizeof(body_order),
                   "{\"status\":\"ready\",\"finalize\":\"%s/finalize/1\"}",
                   mock->base_url);
    } else {
      written = snprintf(body_order, sizeof(body_order),
                         "{\"status\":\"pending\",\"authorizations\":[\"%s/"
                         "authz/1\"],\"finalize\":\"%s/finalize/1\"}",
                         mock->base_url, mock->base_url);
    }
    free(request_body);
    if (written < 0 || (size_t)written >= sizeof(body_order)) {
      return 0;
    }
    return send_response(fd, 200, "OK", NULL, "application/json", body_order);
  }
  if (strcmp(method, "POST") == 0 && strcmp(path, "/cert/1") == 0) {
    free(request_body);
    if (mock->cert_pem == NULL) {
      return send_response(fd, 404, "Not Found", NULL, "text/plain",
                           "certificate not ready\n");
    }
    return send_response(fd, 200, "OK", NULL,
                         "application/pem-certificate-chain", mock->cert_pem);
  }
  free(request_body);
  return send_response(fd, 404, "Not Found", NULL, "text/plain", "not found\n");
}

int main(int argc, char **argv) {
  acme_mock mock;
  int port;
  int server_fd;
  int client_fd;
  int opt;
  struct sockaddr_in addr;

  if (argc != 2) {
    fprintf(stderr, "usage: %s port\n", argv[0]);
    return 2;
  }
  memset(&mock, 0, sizeof(mock));
  port = atoi(argv[1]);
  if (port <= 0 || port > 65535) {
    fprintf(stderr, "invalid port\n");
    return 2;
  }
  if (snprintf(mock.base_url, sizeof(mock.base_url), "http://127.0.0.1:%d",
               port) < 0) {
    return 2;
  }
  if (!init_ca(&mock)) {
    fprintf(stderr, "failed to initialize mock CA\n");
    return 1;
  }
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket");
    return 1;
  }
  opt = 1;
  (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((unsigned short)port);
  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(server_fd, 16) != 0) {
    perror("bind/listen");
    close(server_fd);
    return 1;
  }
  printf("mock acme listening on %s\n", mock.base_url);
  fflush(stdout);
  while (!stop_requested) {
    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("accept");
      break;
    }
    (void)handle_request(&mock, client_fd);
    close(client_fd);
  }
  close(server_fd);
  free(mock.cert_pem);
  X509_free(mock.ca_cert);
  EVP_PKEY_free(mock.ca_key);
  return 0;
}
