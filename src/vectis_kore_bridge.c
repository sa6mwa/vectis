#include "vectis_internal.h"

#include <lc/lc.h>
#include <kore/acme.h>
#include <kore/kore.h>
#include <kore/http.h>
#if defined(__linux__)
#include <kore/seccomp.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

int vectis_kore_main(int argc, char **argv);
int vectis_kore_route(struct http_request *req);
int vectis_kore_body_chunk(struct http_request *req, const void *data, size_t len);
void vectis_kore_request_free(struct http_request *req);
void kore_parent_configure(int argc, char **argv);
void kore_parent_teardown(void);

extern int skip_chroot;
extern int skip_runas;
extern u_int64_t worker_idle_timeout;
extern u_int32_t worker_max_connections;

static pthread_mutex_t vectis_kore_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t vectis_kore_thread;
static int vectis_kore_thread_active = 0;
static int vectis_kore_dh_loaded = 0;
static vectis_kore_runtime_config vectis_kore_current;

static const char vectis_kore_default_dhparams[] =
    "-----BEGIN DH PARAMETERS-----\n"
    "MIICCAKCAgEA//////////+t+FRYortKmq/cViAnPTzx2LnFg84tNpWp4TZBFGQz\n"
    "+8yTnc4kmz75fS/jY2MMddj2gbICrsRhetPfHtXV/WVhJDP1H18GbtCFY2VVPe0a\n"
    "87VXE15/V8k1mE8McODmi3fipona8+/och3xWKE2rec1MKzKT0g6eXq8CrGCsyT7\n"
    "YdEIqUuyyOP7uWrat2DX9GgdT0Kj3jlN9K5W7edjcrsZCwenyO4KbXCeAvzhzffi\n"
    "7MA0BM0oNC9hkXL+nOmFg/+OTxIy7vKBg8P+OxtMb61zO7X8vC7CIAXFjvGDfRaD\n"
    "ssbzSibBsu/6iGtCOGEfz9zeNVs7ZRkDW7w09N75nAI4YbRvydbmyQd62R0mkff3\n"
    "7lmMsPrBhtkcrv4TCYUTknC0EwyTvEN5RPT9RFLi103TZPLiHnH1S/9croKrnJ32\n"
    "nuhtK8UiNjoNq8Uhl5sN6todv5pC1cRITgq80Gv6U93vPBsg7j/VnXwl5B0rZp4e\n"
    "8W5vUsMWTfT7eTDp5OWIV7asfV9C1p9tGHdjzx1VA0AEh/VbpX4xzHpxNciG77Qx\n"
    "iu1qHgEtnmgyqQdgCpGBMMRtx3j5ca0AOAkpmaMzy4t6Gh25PXFAADwqTs6p+Y0K\n"
    "zAqCkc3OyX3Pjsm1Wn+IpGtNtahR9EGC4caKAH5eZV9q//////////8CAQI=\n"
    "-----END DH PARAMETERS-----\n";

#if defined(__linux__)
static struct sock_filter vectis_kore_dependency_seccomp_filter[] = {
#if defined(SYS_eventfd)
  KORE_SYSCALL_ALLOW(eventfd),
#endif
#if defined(SYS_eventfd2)
  KORE_SYSCALL_ALLOW(eventfd2),
#endif
#if defined(SYS_shutdown)
  KORE_SYSCALL_ALLOW(shutdown),
#endif
};

void kore_seccomp_hook(void) {
  kore_seccomp_filter("vectis-dependencies",
                      vectis_kore_dependency_seccomp_filter,
                      KORE_FILTER_LEN(vectis_kore_dependency_seccomp_filter));
}
#endif

typedef struct vectis_kore_material {
  const char *path;
  const void *memory;
  size_t memory_size;
  lc_source *source;
} vectis_kore_material;

static int vectis_kore_material_present(const vectis_kore_material *material);
static vectis_status vectis_kore_material_bytes(const vectis_kore_material *material,
                                                void **out,
                                                size_t *out_size,
                                                vectis_error *error);

static size_t vectis_kore_environment_size(void) {
  size_t total;
  int i;

  total = 0u;
  if (environ == NULL) {
    return total;
  }
  for (i = 0; environ[i] != NULL; ++i) {
    total += strlen(environ[i]) + 1u;
  }
  return total;
}

static char *vectis_kore_argv_arena(char **argv, const char **args, int argc) {
  char *arena;
  char *cursor;
  size_t total;
  size_t len;
  int i;

  total = vectis_kore_environment_size();
  for (i = 0; i < argc; ++i) {
    total += strlen(args[i]) + 1u;
  }
  total += 1u;

  arena = (char *)calloc(1u, total);
  if (arena == NULL) {
    return NULL;
  }
  cursor = arena;
  for (i = 0; i < argc; ++i) {
    len = strlen(args[i]) + 1u;
    argv[i] = cursor;
    memcpy(cursor, args[i], len);
    cursor += len;
  }
  argv[argc] = NULL;
  return arena;
}

static vectis_http_method vectis_kore_method(u_int8_t method) {
  switch (method) {
  case HTTP_METHOD_GET:
    return VECTIS_HTTP_GET;
  case HTTP_METHOD_POST:
    return VECTIS_HTTP_POST;
  case HTTP_METHOD_PUT:
    return VECTIS_HTTP_PUT;
  case HTTP_METHOD_PATCH:
    return VECTIS_HTTP_PATCH;
  case HTTP_METHOD_DELETE:
    return VECTIS_HTTP_DELETE;
  case HTTP_METHOD_HEAD:
    return VECTIS_HTTP_HEAD;
  case HTTP_METHOD_OPTIONS:
    return VECTIS_HTTP_OPTIONS;
  default:
    return VECTIS_HTTP_ANY;
  }
}

static u_int16_t vectis_kore_seconds_from_ms(long ms) {
  long seconds;

  if (ms <= 0L) {
    return 1u;
  }
  seconds = (ms + 999L) / 1000L;
  if (seconds <= 0L) {
    return 1u;
  }
  if (seconds > 65535L) {
    return 65535u;
  }
  return (u_int16_t)seconds;
}

static u_int16_t vectis_kore_u16_from_size(size_t value) {
  if (value > 65535u) {
    return 65535u;
  }
  return (u_int16_t)value;
}

static u_int32_t vectis_kore_u32_from_size(size_t value) {
  if (value > (size_t)((u_int32_t)-1)) {
    return (u_int32_t)-1;
  }
  return (u_int32_t)value;
}

static char *vectis_kore_strdup(const char *value) {
  char *copy;
  size_t len;

  if (value == NULL) {
    return NULL;
  }
  len = strlen(value);
  copy = (char *)malloc(len + 1u);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, len + 1u);
  return copy;
}

static void vectis_kore_set_errorf(vectis_error *error,
                                   vectis_status code,
                                   const char *fmt,
                                   ...) {
  va_list ap;

  if (error == NULL) {
    return;
  }
  vectis_error_clear(error);
  error->code = code;
  error->source = VECTIS_ERROR_SOURCE_VECTIS;
  va_start(ap, fmt);
  (void)vsnprintf(error->message, sizeof(error->message), fmt, ap);
  va_end(ap);
}

static void vectis_kore_cleanup_config(vectis_kore_runtime_config *config) {
  if (config == NULL) {
    return;
  }
  if (config->runtime_certfile_temporary && config->runtime_certfile != NULL) {
    (void)unlink(config->runtime_certfile);
  }
  if (config->runtime_certkey_temporary && config->runtime_certkey != NULL &&
      config->runtime_certkey != config->runtime_certfile &&
      (config->runtime_certfile == NULL ||
       strcmp(config->runtime_certkey, config->runtime_certfile) != 0)) {
    (void)unlink(config->runtime_certkey);
  }
  if (config->runtime_client_ca_temporary && config->runtime_client_ca_file != NULL) {
    (void)unlink(config->runtime_client_ca_file);
  }
  free(config->runtime_certfile);
  free(config->runtime_certkey);
  free(config->runtime_client_ca_file);
  config->runtime_certfile = NULL;
  config->runtime_certkey = NULL;
  config->runtime_client_ca_file = NULL;
  config->runtime_certfile_temporary = 0;
  config->runtime_certkey_temporary = 0;
  config->runtime_client_ca_temporary = 0;
}

static vectis_status vectis_kore_prepare_acme(vectis_kore_runtime_config *config,
                                              vectis_error *error) {
  if (config->tls_mode != VECTIS_TLS_MODE_ACME) {
    return VECTIS_OK;
  }
  if (config->domain == NULL || config->domain[0] == '\0' ||
      strchr(config->domain, '*') != NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "ACME mode requires a concrete TLS domain");
    return VECTIS_ERR_INVALID;
  }
  if (config->acme_email == NULL || config->acme_email[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID, "ACME mode requires acme_email");
    return VECTIS_ERR_INVALID;
  }
  return VECTIS_OK;
}

static void vectis_kore_cleanup_local_config(vectis_kore_runtime_config *config) {
  vectis_kore_cleanup_config(config);
}

static int vectis_kore_write_all(int fd, const void *data, size_t size) {
  const unsigned char *cursor;
  size_t remaining;
  ssize_t n;

  cursor = (const unsigned char *)data;
  remaining = size;
  while (remaining > 0u) {
    n = write(fd, cursor, remaining);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return 0;
    }
    if (n == 0) {
      return 0;
    }
    cursor += (size_t)n;
    remaining -= (size_t)n;
  }
  return 1;
}

static vectis_status vectis_kore_temp_file_from_bytes(const void *data,
                                                      size_t size,
                                                      char **path_out,
                                                      vectis_error *error) {
  char template_path[] = "/tmp/vectis-kore-XXXXXX";
  int fd;

  if (path_out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "temporary path output is required");
    return VECTIS_ERR_INVALID;
  }
  *path_out = NULL;
  if (data == NULL || size == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "TLS material is empty");
    return VECTIS_ERR_INVALID;
  }

  fd = mkstemp(template_path);
  if (fd < 0) {
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to create temporary TLS material file");
    return VECTIS_ERR_STATE;
  }
  (void)fchmod(fd, S_IRUSR | S_IWUSR);
  if (!vectis_kore_write_all(fd, data, size) || fsync(fd) != 0) {
    (void)close(fd);
    (void)unlink(template_path);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to write temporary TLS material file");
    return VECTIS_ERR_STATE;
  }
  if (close(fd) != 0) {
    (void)unlink(template_path);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to close temporary TLS material file");
    return VECTIS_ERR_STATE;
  }
  *path_out = vectis_kore_strdup(template_path);
  if (*path_out == NULL) {
    (void)unlink(template_path);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy temporary TLS material path");
    return VECTIS_ERR_NOMEM;
  }
  return VECTIS_OK;
}

static vectis_status vectis_kore_read_source(lc_source *source,
                                             void **out,
                                             size_t *out_size,
                                             vectis_error *error) {
  unsigned char chunk[4096];
  unsigned char *buffer;
  unsigned char *grown;
  size_t size;
  size_t capacity;
  size_t nread;
  lc_error lcerr;

  if (source == NULL || out == NULL || out_size == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "TLS source is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  *out_size = 0u;
  buffer = NULL;
  size = 0u;
  capacity = 0u;
  lc_error_init(&lcerr);
  if (source->reset != NULL && source->reset(source, &lcerr) != LC_OK) {
    vectis_kore_set_errorf(error,
                           VECTIS_ERR_STATE,
                           "failed to reset TLS source: %s",
                           lcerr.message != NULL ? lcerr.message : "unknown lockdc error");
    lc_error_cleanup(&lcerr);
    return VECTIS_ERR_STATE;
  }

  for (;;) {
    nread = source->read(source, chunk, sizeof(chunk), &lcerr);
    if (nread == 0u) {
      break;
    }
    if (size + nread < size) {
      free(buffer);
      lc_error_cleanup(&lcerr);
      vectis_set_error(error, VECTIS_ERR_NOMEM, "TLS source is too large");
      return VECTIS_ERR_NOMEM;
    }
    if (size + nread > capacity) {
      capacity = capacity == 0u ? 8192u : capacity * 2u;
      while (capacity < size + nread) {
        capacity *= 2u;
      }
      grown = (unsigned char *)realloc(buffer, capacity);
      if (grown == NULL) {
        free(buffer);
        lc_error_cleanup(&lcerr);
        vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to buffer TLS source");
        return VECTIS_ERR_NOMEM;
      }
      buffer = grown;
    }
    memcpy(buffer + size, chunk, nread);
    size += nread;
  }
  if (source->reset != NULL) {
    (void)source->reset(source, &lcerr);
  }
  lc_error_cleanup(&lcerr);
  if (size == 0u) {
    free(buffer);
    vectis_set_error(error, VECTIS_ERR_INVALID, "TLS source is empty");
    return VECTIS_ERR_INVALID;
  }
  *out = buffer;
  *out_size = size;
  return VECTIS_OK;
}

static vectis_status vectis_kore_material_to_file(const vectis_kore_material *material,
                                                  char **path_out,
                                                  int *temporary_out,
                                                  vectis_error *error) {
  void *source_bytes;
  size_t source_size;
  vectis_status status;

  if (path_out == NULL || temporary_out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "TLS material output is required");
    return VECTIS_ERR_INVALID;
  }
  *path_out = NULL;
  *temporary_out = 0;
  if (material == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "TLS material is required");
    return VECTIS_ERR_INVALID;
  }
  if (material->path != NULL) {
    *path_out = vectis_kore_strdup(material->path);
    if (*path_out == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy TLS material path");
      return VECTIS_ERR_NOMEM;
    }
    return VECTIS_OK;
  }
  if (material->memory != NULL && material->memory_size > 0u) {
    status = vectis_kore_temp_file_from_bytes(material->memory,
                                              material->memory_size,
                                              path_out,
                                              error);
    if (status == VECTIS_OK) {
      *temporary_out = 1;
    }
    return status;
  }
  if (material->source != NULL) {
    source_bytes = NULL;
    source_size = 0u;
    status = vectis_kore_read_source(material->source, &source_bytes, &source_size, error);
    if (status != VECTIS_OK) {
      return status;
    }
    status = vectis_kore_temp_file_from_bytes(source_bytes, source_size, path_out, error);
    free(source_bytes);
    if (status == VECTIS_OK) {
      *temporary_out = 1;
    }
    return status;
  }
  vectis_set_error(error, VECTIS_ERR_INVALID, "TLS material is missing");
  return VECTIS_ERR_INVALID;
}

static vectis_status vectis_kore_certificate_chain_to_file(const vectis_kore_material *certificate,
                                                           const vectis_kore_material *ca_bundle,
                                                           char **path_out,
                                                           int *temporary_out,
                                                           vectis_error *error) {
  void *cert_bytes;
  void *ca_bytes;
  unsigned char *chain;
  size_t cert_size;
  size_t ca_size;
  size_t chain_size;
  vectis_status status;

  if (certificate == NULL || path_out == NULL || temporary_out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "TLS certificate output is required");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_kore_material_present(ca_bundle)) {
    return vectis_kore_material_to_file(certificate, path_out, temporary_out, error);
  }

  cert_bytes = NULL;
  ca_bytes = NULL;
  chain = NULL;
  cert_size = 0u;
  ca_size = 0u;
  status = vectis_kore_material_bytes(certificate, &cert_bytes, &cert_size, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_kore_material_bytes(ca_bundle, &ca_bytes, &ca_size, error);
  if (status != VECTIS_OK) {
    free(cert_bytes);
    return status;
  }
  if (ca_size > ((size_t)-1) - cert_size - 1u) {
    free(cert_bytes);
    free(ca_bytes);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "TLS certificate chain is too large");
    return VECTIS_ERR_NOMEM;
  }
  chain_size = cert_size + ca_size;
  if (cert_size > 0u && ((const unsigned char *)cert_bytes)[cert_size - 1u] != '\n') {
    chain_size++;
  }
  chain = (unsigned char *)malloc(chain_size);
  if (chain == NULL) {
    free(cert_bytes);
    free(ca_bytes);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate TLS certificate chain");
    return VECTIS_ERR_NOMEM;
  }
  memcpy(chain, cert_bytes, cert_size);
  if (chain_size > cert_size + ca_size) {
    chain[cert_size] = '\n';
    memcpy(chain + cert_size + 1u, ca_bytes, ca_size);
  } else {
    memcpy(chain + cert_size, ca_bytes, ca_size);
  }
  status = vectis_kore_temp_file_from_bytes(chain, chain_size, path_out, error);
  if (status == VECTIS_OK) {
    *temporary_out = 1;
  }
  free(chain);
  free(cert_bytes);
  free(ca_bytes);
  return status;
}

static const char *vectis_kore_find_private_key_marker(const char *data,
                                                       size_t size) {
  const char *markers[] = {
      "-----BEGIN PRIVATE KEY-----",
      "-----BEGIN RSA PRIVATE KEY-----",
      "-----BEGIN EC PRIVATE KEY-----"};
  const char *marker;
  size_t marker_size;
  size_t i;
  size_t offset;

  if (data == NULL || size == 0u) {
    return NULL;
  }
  for (i = 0u; i < sizeof(markers) / sizeof(markers[0]); ++i) {
    marker = markers[i];
    marker_size = strlen(marker);
    if (marker_size > size) {
      continue;
    }
    for (offset = 0u; offset <= size - marker_size; ++offset) {
      if (memcmp(data + offset, marker, marker_size) == 0) {
        return data + offset;
      }
    }
  }
  return NULL;
}

static vectis_status vectis_kore_read_path(const char *path,
                                           void **out,
                                           size_t *out_size,
                                           vectis_error *error) {
  FILE *fp;
  long len;
  void *buffer;

  if (path == NULL || out == NULL || out_size == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "TLS material path is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  *out_size = 0u;
  fp = fopen(path, "rb");
  if (fp == NULL) {
    vectis_kore_set_errorf(error, VECTIS_ERR_STATE, "failed to open TLS material path: %s", path);
    return VECTIS_ERR_STATE;
  }
  if (fseek(fp, 0L, SEEK_END) != 0 || (len = ftell(fp)) <= 0L ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    (void)fclose(fp);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to size TLS material path");
    return VECTIS_ERR_STATE;
  }
  buffer = malloc((size_t)len);
  if (buffer == NULL) {
    (void)fclose(fp);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to buffer TLS material path");
    return VECTIS_ERR_NOMEM;
  }
  if (fread(buffer, 1u, (size_t)len, fp) != (size_t)len) {
    free(buffer);
    (void)fclose(fp);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to read TLS material path");
    return VECTIS_ERR_STATE;
  }
  (void)fclose(fp);
  *out = buffer;
  *out_size = (size_t)len;
  return VECTIS_OK;
}

static vectis_status vectis_kore_material_bytes(const vectis_kore_material *material,
                                                void **out,
                                                size_t *out_size,
                                                vectis_error *error) {
  void *copy;

  if (material == NULL || out == NULL || out_size == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "TLS material is required");
    return VECTIS_ERR_INVALID;
  }
  if (material->memory != NULL && material->memory_size > 0u) {
    copy = malloc(material->memory_size);
    if (copy == NULL) {
      vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to copy TLS material");
      return VECTIS_ERR_NOMEM;
    }
    memcpy(copy, material->memory, material->memory_size);
    *out = copy;
    *out_size = material->memory_size;
    return VECTIS_OK;
  }
  if (material->source != NULL) {
    return vectis_kore_read_source(material->source, out, out_size, error);
  }
  return vectis_kore_read_path(material->path, out, out_size, error);
}

static vectis_status vectis_kore_split_bundle(const vectis_kore_material *bundle,
                                              char **certfile,
                                              int *cert_temporary,
                                              char **certkey,
                                              int *key_temporary,
                                              vectis_error *error) {
  void *bundle_bytes;
  size_t bundle_size;
  const char *key_start;
  size_t cert_size;
  vectis_status status;

  bundle_bytes = NULL;
  bundle_size = 0u;
  status = vectis_kore_material_bytes(bundle, &bundle_bytes, &bundle_size, error);
  if (status != VECTIS_OK) {
    return status;
  }
  key_start = vectis_kore_find_private_key_marker((const char *)bundle_bytes, bundle_size);
  if (key_start == NULL || key_start == (const char *)bundle_bytes) {
    free(bundle_bytes);
    vectis_set_error(error,
                     VECTIS_ERR_INVALID,
                     "TLS cert/key bundle must contain certificate data before a private key");
    return VECTIS_ERR_INVALID;
  }
  cert_size = (size_t)(key_start - (const char *)bundle_bytes);
  status = vectis_kore_temp_file_from_bytes(bundle_bytes, cert_size, certfile, error);
  if (status == VECTIS_OK) {
    *cert_temporary = 1;
    status = vectis_kore_temp_file_from_bytes(key_start,
                                              bundle_size - cert_size,
                                              certkey,
                                              error);
  }
  free(bundle_bytes);
  if (status != VECTIS_OK) {
    if (*certfile != NULL) {
      (void)unlink(*certfile);
      free(*certfile);
      *certfile = NULL;
    }
    *cert_temporary = 0;
    return status;
  }
  *key_temporary = 1;
  return VECTIS_OK;
}

static int vectis_kore_material_present(const vectis_kore_material *material) {
  return material != NULL &&
         (material->path != NULL ||
          material->source != NULL ||
          (material->memory != NULL && material->memory_size > 0u));
}

static vectis_status vectis_kore_prepare_tls(vectis_kore_runtime_config *config,
                                             vectis_error *error) {
  vectis_kore_material bundle;
  vectis_kore_material certificate;
  vectis_kore_material private_key;
  vectis_kore_material ca_bundle;
  vectis_kore_material client_ca;
  vectis_status status;

  if (config->tls_mode != VECTIS_TLS_MODE_MANUAL) {
    return VECTIS_OK;
  }

  memset(&bundle, 0, sizeof(bundle));
  memset(&certificate, 0, sizeof(certificate));
  memset(&private_key, 0, sizeof(private_key));
  memset(&ca_bundle, 0, sizeof(ca_bundle));
  memset(&client_ca, 0, sizeof(client_ca));
  bundle.path = config->cert_key_bundle_path;
  bundle.memory = config->cert_key_bundle_pem;
  bundle.memory_size = config->cert_key_bundle_pem_size;
  bundle.source = config->cert_key_bundle_source;
  certificate.path = config->certificate_path;
  certificate.memory = config->certificate_pem;
  certificate.memory_size = config->certificate_pem_size;
  certificate.source = config->certificate_source;
  private_key.path = config->private_key_path;
  private_key.memory = config->private_key_pem;
  private_key.memory_size = config->private_key_pem_size;
  private_key.source = config->private_key_source;
  ca_bundle.path = config->ca_bundle_path;
  ca_bundle.memory = config->ca_bundle_pem;
  ca_bundle.memory_size = config->ca_bundle_pem_size;
  ca_bundle.source = config->ca_bundle_source;
  client_ca.path = config->client_ca_bundle_path;
  client_ca.memory = config->client_ca_bundle_pem;
  client_ca.memory_size = config->client_ca_bundle_pem_size;
  client_ca.source = config->client_ca_bundle_source;

  if (vectis_kore_material_present(&bundle)) {
    status = vectis_kore_split_bundle(&bundle,
                                      &config->runtime_certfile,
                                      &config->runtime_certfile_temporary,
                                      &config->runtime_certkey,
                                      &config->runtime_certkey_temporary,
                                      error);
    if (status != VECTIS_OK) {
      return status;
    }
  } else {
    status = vectis_kore_certificate_chain_to_file(&certificate,
                                                  &ca_bundle,
                                                  &config->runtime_certfile,
                                                  &config->runtime_certfile_temporary,
                                                  error);
    if (status != VECTIS_OK) {
      return status;
    }
    status = vectis_kore_material_to_file(&private_key,
                                          &config->runtime_certkey,
                                          &config->runtime_certkey_temporary,
                                          error);
    if (status != VECTIS_OK) {
      return status;
    }
  }

  if (config->require_client_certificate) {
    status = vectis_kore_material_to_file(&client_ca,
                                          &config->runtime_client_ca_file,
                                          &config->runtime_client_ca_temporary,
                                          error);
    if (status != VECTIS_OK) {
      return status;
    }
  }
  return VECTIS_OK;
}

static vectis_status vectis_kore_load_default_dhparams(vectis_error *error) {
  char *path;
  vectis_status status;

  if (vectis_kore_dh_loaded) {
    return VECTIS_OK;
  }
  path = NULL;
  status = vectis_kore_temp_file_from_bytes(vectis_kore_default_dhparams,
                                            strlen(vectis_kore_default_dhparams),
                                            &path,
                                            error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (!kore_tls_dh_load(path)) {
    (void)unlink(path);
    free(path);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to load Vectis default DH parameters");
    return VECTIS_ERR_STATE;
  }
  (void)unlink(path);
  free(path);
  vectis_kore_dh_loaded = 1;
  return VECTIS_OK;
}

static void vectis_kore_apply_server_config(const vectis_server_config *server,
                                            size_t body_disk_offload_bytes,
                                            int body_disk_offload_configured) {
  (void)body_disk_offload_bytes;
  (void)body_disk_offload_configured;
  worker_max_connections = vectis_kore_u32_from_size(server->max_connections);
  worker_idle_timeout = (u_int64_t)server->idle_timeout_ms;
  http_request_limit = vectis_kore_u32_from_size(server->max_connections);
  http_header_max = vectis_kore_u16_from_size(server->max_request_header_bytes);
  http_body_max = server->max_request_body_bytes;
  http_body_disk_offload = 0u;
  http_header_timeout = vectis_kore_seconds_from_ms(server->request_header_timeout_ms);
  http_body_timeout = vectis_kore_seconds_from_ms(server->request_body_idle_timeout_ms);
  http_response_write_timeout =
      vectis_kore_u32_from_size((size_t)server->response_write_idle_timeout_ms);
  http_body_min_rate = (u_int64_t)server->request_body_min_rate_bytes_per_sec;
  http_body_min_rate_grace = server->request_body_min_rate_grace_ms > 0L ?
      (u_int64_t)server->request_body_min_rate_grace_ms : 0u;
  http_keepalive_time = server->keepalive_disabled ?
      0u : vectis_kore_seconds_from_ms(server->keepalive_timeout_ms);
  http_keepalive_max_requests = server->keepalive_disabled ?
      0u : (u_int32_t)server->keepalive_max_requests;
}

static void vectis_kore_setup_domain_tls(struct kore_domain *domain) {
  vectis_error error;
  void *cert_pem;
  size_t cert_pem_size;

  cert_pem = NULL;
  cert_pem_size = 0u;
  vectis_error_clear(&error);
  if (vectis_kore_read_path(vectis_kore_current.runtime_certfile,
                            &cert_pem,
                            &cert_pem_size,
                            &error) != VECTIS_OK) {
    fatal("failed to read Vectis TLS certificate material: %s", error.message);
  }
  kore_tls_domain_setup(domain, KORE_PEM_CERT_CHAIN, cert_pem, cert_pem_size);
  free(cert_pem);
}

static char *vectis_kore_memdup_cstr(const char *data, size_t len) {
  char *copy;

  copy = (char *)malloc(len + 1u);
  if (copy == NULL) {
    return NULL;
  }
  if (len > 0u) {
    memcpy(copy, data, len);
  }
  copy[len] = '\0';
  return copy;
}

static vectis_status vectis_kore_copy_headers(struct http_request *req,
                                              vectis_request *request,
                                              vectis_error *error) {
  struct http_header *header;
  vectis_status status;

  TAILQ_FOREACH(header, &req->req_headers, list) {
    if (header->header == NULL || header->header[0] == '\0') {
      continue;
    }
    status = vectis_internal_request_add_header(request,
                                                header->header,
                                                header->value != NULL ? header->value : "",
                                                error);
    if (status != VECTIS_OK) {
      return status;
    }
  }
  return VECTIS_OK;
}

static vectis_status vectis_kore_add_query_pair(vectis_request *request,
                                                const char *name,
                                                size_t name_len,
                                                const char *value,
                                                size_t value_len,
                                                vectis_error *error) {
  char *owned_name;
  char *owned_value;
  vectis_status status;

  if (name_len == 0u) {
    return VECTIS_OK;
  }
  owned_name = vectis_kore_memdup_cstr(name, name_len);
  owned_value = vectis_kore_memdup_cstr(value, value_len);
  if (owned_name == NULL || owned_value == NULL) {
    free(owned_name);
    free(owned_value);
    vectis_set_error(error, VECTIS_ERR_NOMEM, "failed to allocate query parameter");
    return VECTIS_ERR_NOMEM;
  }
  if (!http_argument_urldecode(owned_name, 1) ||
      !http_argument_urldecode(owned_value, 1)) {
    free(owned_name);
    free(owned_value);
    vectis_set_error(error, VECTIS_ERR_INVALID, "failed to decode query parameter");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_internal_request_add_query(request, owned_name, owned_value, error);
  free(owned_name);
  free(owned_value);
  return status;
}

static vectis_status vectis_kore_copy_query(struct http_request *req,
                                            vectis_request *request,
                                            vectis_error *error) {
  const char *cursor;
  const char *pair;
  const char *amp;
  const char *eq;
  size_t pair_len;
  size_t name_len;
  size_t value_len;
  vectis_status status;

  if (req->query_string == NULL || req->query_string[0] == '\0') {
    return VECTIS_OK;
  }
  cursor = req->query_string;
  while (*cursor != '\0') {
    pair = cursor;
    amp = strchr(pair, '&');
    pair_len = amp != NULL ? (size_t)(amp - pair) : strlen(pair);
    if (pair_len > 0u) {
      eq = memchr(pair, '=', pair_len);
      if (eq != NULL) {
        name_len = (size_t)(eq - pair);
        value_len = pair_len - name_len - 1u;
        status = vectis_kore_add_query_pair(request,
                                            pair,
                                            name_len,
                                            eq + 1,
                                            value_len,
                                            error);
      } else {
        status = vectis_kore_add_query_pair(request,
                                            pair,
                                            pair_len,
                                            "",
                                            0u,
                                            error);
      }
      if (status != VECTIS_OK) {
        return status;
      }
    }
    if (amp == NULL) {
      break;
    }
    cursor = amp + 1;
  }
  return VECTIS_OK;
}

static vectis_status vectis_kore_copy_request_metadata(struct http_request *req,
                                                       vectis_request *request,
                                                       vectis_error *error) {
  vectis_status status;

  status = vectis_kore_copy_headers(req, request, error);
  if (status != VECTIS_OK) {
    return status;
  }
  return vectis_kore_copy_query(req, request, error);
}

static void *vectis_kore_thread_main(void *userdata) {
  char *argv[5];
  const char *args[4];
  char *arena;
  int argc;

  (void)userdata;
  argc = 0;
  args[argc++] = "vectis-kore";
  args[argc++] = "-f";
  args[argc++] = "-n";
  args[argc++] = "-r";
  arena = vectis_kore_argv_arena(argv, args, argc);
  if (arena == NULL) {
    return NULL;
  }
  skip_chroot = 1;
  skip_runas = 1;
  (void)vectis_kore_main(argc, argv);
  free(arena);
  return NULL;
}

typedef struct vectis_kore_body_state {
  vectis_body_policy policy;
  size_t expected_size;
  size_t total_size;
  unsigned char *memory;
  size_t memory_size;
  size_t memory_capacity;
  FILE *file;
  char *path;
  int error_status;
  int initialized;
  int spooled;
} vectis_kore_body_state;

static int vectis_kore_tmp_template(char *buffer, size_t buffer_size) {
  int n;

  if (buffer == NULL || buffer_size == 0u) {
    return 0;
  }
  n = snprintf(buffer, buffer_size, "/tmp/vectis-kore-body-XXXXXX");
  return n > 0 && (size_t)n < buffer_size;
}

static void vectis_kore_body_state_cleanup(vectis_kore_body_state *state) {
  if (state == NULL) {
    return;
  }
  if (state->file != NULL) {
    (void)fclose(state->file);
    state->file = NULL;
  }
  if (state->path != NULL) {
    (void)unlink(state->path);
    free(state->path);
    state->path = NULL;
  }
  free(state->memory);
  state->memory = NULL;
  state->memory_size = 0u;
  state->memory_capacity = 0u;
  state->total_size = 0u;
  state->initialized = 0;
  state->spooled = 0;
}

void vectis_kore_request_free(struct http_request *req) {
  if (req == NULL || req->hdlr_extra == NULL) {
    return;
  }
  vectis_kore_body_state_cleanup((vectis_kore_body_state *)req->hdlr_extra);
}

static size_t vectis_kore_policy_memory_limit(const vectis_body_policy *policy) {
  if (policy == NULL || policy->memory_buffer_limit_bytes == 0u) {
    return VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  }
  return policy->memory_buffer_limit_bytes;
}

static vectis_kore_body_state *vectis_kore_body_state_get(struct http_request *req) {
  if (req == NULL) {
    return NULL;
  }
  if (req->hdlr_extra == NULL) {
    return (vectis_kore_body_state *)http_state_create(req, sizeof(vectis_kore_body_state));
  }
  return (vectis_kore_body_state *)req->hdlr_extra;
}

static int vectis_kore_body_state_spool(vectis_kore_body_state *state) {
  char tmp_template[PATH_MAX];
  char *path;
  FILE *file;
  int fd;

  if (state == NULL || state->spooled) {
    return state != NULL;
  }
  if (!vectis_kore_tmp_template(tmp_template, sizeof(tmp_template))) {
    return 0;
  }
  fd = mkstemp(tmp_template);
  if (fd < 0) {
    return 0;
  }
  path = strdup(tmp_template);
  if (path == NULL) {
    (void)close(fd);
    (void)unlink(tmp_template);
    return 0;
  }
  file = fdopen(fd, "w+b");
  if (file == NULL) {
    (void)close(fd);
    (void)unlink(tmp_template);
    free(path);
    return 0;
  }
  if (state->memory_size > 0u &&
      fwrite(state->memory, 1u, state->memory_size, file) != state->memory_size) {
    (void)fclose(file);
    (void)unlink(path);
    free(path);
    return 0;
  }
  free(state->memory);
  state->memory = NULL;
  state->memory_size = 0u;
  state->memory_capacity = 0u;
  state->file = file;
  state->path = path;
  state->spooled = 1;
  return 1;
}

static int vectis_kore_body_state_append(vectis_kore_body_state *state,
                                         const void *data,
                                         size_t len) {
  unsigned char *next;
  size_t memory_limit;
  size_t next_capacity;

  if (state == NULL || (data == NULL && len > 0u)) {
    return 0;
  }
  if (len == 0u) {
    return 1;
  }
  if ((size_t)-1 - state->total_size < len) {
    return 0;
  }
  memory_limit = vectis_kore_policy_memory_limit(&state->policy);
  if (!state->spooled && state->memory_size + len > memory_limit) {
    if (state->policy.disk_spool_disabled) {
      return 0;
    }
    if (!vectis_kore_body_state_spool(state)) {
      return 0;
    }
  }
  if (state->spooled) {
    if (fwrite(data, 1u, len, state->file) != len) {
      return 0;
    }
  } else {
    if (state->memory_size + len > state->memory_capacity) {
      next_capacity = state->memory_capacity == 0u ? len : state->memory_capacity;
      while (next_capacity < state->memory_size + len) {
        if (next_capacity > ((size_t)-1 / 2u)) {
          next_capacity = state->memory_size + len;
          break;
        }
        next_capacity *= 2u;
      }
      next = (unsigned char *)realloc(state->memory, next_capacity);
      if (next == NULL) {
        return 0;
      }
      state->memory = next;
      state->memory_capacity = next_capacity;
    }
    memcpy(state->memory + state->memory_size, data, len);
    state->memory_size += len;
  }
  state->total_size += len;
  return 1;
}

static void vectis_kore_reject_body_chunk(struct http_request *req,
                                          vectis_kore_body_state *state,
                                          int status,
                                          const char *message) {
  if (state != NULL) {
    state->error_status = status;
  }
  http_response(req, status, message, message != NULL ? strlen(message) : 0u);
  req->flags |= HTTP_REQUEST_DELETE;
}

int vectis_kore_body_chunk(struct http_request *req, const void *data, size_t len) {
  vectis_kore_body_state *state;
  vectis_error error;
  vectis_app *app;
  vectis_http_method method;
  vectis_status status;

  vectis_error_clear(&error);
  state = vectis_kore_body_state_get(req);
  if (state == NULL) {
    return KORE_RESULT_ERROR;
  }
  if (state->error_status != 0) {
    return KORE_RESULT_OK;
  }
  if (!state->initialized) {
    (void)pthread_mutex_lock(&vectis_kore_mutex);
    app = vectis_kore_current.app;
    (void)pthread_mutex_unlock(&vectis_kore_mutex);
    if (app == NULL || req == NULL || req->path == NULL) {
      vectis_kore_reject_body_chunk(req, state, 503, NULL);
      return KORE_RESULT_OK;
    }
    method = vectis_kore_method(req->method);
    status = vectis_internal_route_body_policy(app, method, req->path, &state->policy, &error);
    if (status != VECTIS_OK) {
      vectis_kore_reject_body_chunk(req, state, 404, NULL);
      return KORE_RESULT_OK;
    }
    state->expected_size = (size_t)req->http_body_length;
    if (state->policy.mode == VECTIS_BODY_NONE && req->http_body_length > 0u) {
      vectis_kore_reject_body_chunk(req,
                                    state,
                                    413,
                                    "request body is not allowed for this route");
      return KORE_RESULT_OK;
    }
    if (req->http_body_length > (u_int64_t)((size_t)-1) ||
        state->expected_size > state->policy.max_bytes) {
      vectis_kore_reject_body_chunk(req,
                                    state,
                                    413,
                                    "request body exceeds route limit");
      return KORE_RESULT_OK;
    }
    state->initialized = 1;
  }
  if (!vectis_kore_body_state_append(state, data, len)) {
    vectis_kore_reject_body_chunk(req, state, 413, "failed to stream request body");
    return KORE_RESULT_OK;
  }
  return KORE_RESULT_OK;
}

static vectis_status vectis_kore_attach_streamed_body(struct http_request *req,
                                                      const vectis_body_policy *policy,
                                                      vectis_request *request,
                                                      int *http_status,
                                                      vectis_error *error) {
  vectis_kore_body_state *state;
  size_t body_size;

  if (http_status != NULL) {
    *http_status = 0;
  }
  if (req == NULL || policy == NULL || request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body state is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (req->http_body_length > (u_int64_t)((size_t)-1)) {
    if (http_status != NULL) {
      *http_status = 413;
    }
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body is too large");
    return VECTIS_ERR_INVALID;
  }
  body_size = (size_t)req->http_body_length;
  if (body_size > 0u && policy->mode == VECTIS_BODY_NONE) {
    if (http_status != NULL) {
      *http_status = 413;
    }
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body is not allowed for this route");
    return VECTIS_ERR_INVALID;
  }
  if (body_size > policy->max_bytes) {
    if (http_status != NULL) {
      *http_status = 413;
    }
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body exceeds route limit");
    return VECTIS_ERR_INVALID;
  }
  if (body_size == 0u) {
    return vectis_internal_request_set_body(request, NULL, 0u, error);
  }
  state = (vectis_kore_body_state *)req->hdlr_extra;
  if (state == NULL || state->error_status != 0 || state->total_size != body_size) {
    if (http_status != NULL) {
      *http_status = state != NULL && state->error_status != 0 ? state->error_status : 400;
    }
    vectis_set_error(error, VECTIS_ERR_INVALID, "request body stream is incomplete");
    return VECTIS_ERR_INVALID;
  }
  if (state->spooled) {
    if (state->file != NULL && fflush(state->file) != 0) {
      vectis_set_error(error, VECTIS_ERR_STATE, "failed to flush streamed request body");
      return VECTIS_ERR_STATE;
    }
    return vectis_internal_request_set_body_path(request, state->path, state->total_size, error);
  }
  return vectis_internal_request_set_body(request, state->memory, state->memory_size, error);
}

static void vectis_kore_send_response(struct http_request *req,
                                      vectis_response *response) {
  vectis_bytes body;
  const char *content_type;
  const char *file_path;
  struct kore_fileref *ref;
  struct stat st;
  struct timespec ts;
  int fd;
  int status;
  size_t i;

  status = vectis_internal_response_status_code(response);
  if (status == 0) {
    status = 204;
  }
  for (i = 0u; i < vectis_internal_response_header_count(response); ++i) {
    http_response_header(req,
                         vectis_internal_response_header_name(response, i),
                         vectis_internal_response_header_value(response, i));
  }
  content_type = vectis_internal_response_content_type(response);
  if (content_type != NULL) {
    http_response_header(req, "content-type", content_type);
  }
  file_path = vectis_internal_response_file_path(response);
  if (file_path != NULL) {
    if (req->owner == NULL || req->owner->owner == NULL ||
        req->owner->owner->server == NULL) {
      http_response(req, 500, NULL, 0);
      return;
    }
    if (!vectis_internal_response_file_temporary(response)) {
      ref = kore_fileref_get(file_path, req->owner->owner->server->tls);
      if (ref != NULL) {
        http_response_fileref(req, status, ref);
        return;
      }
    }
    fd = open(file_path, O_RDONLY);
    if (fd == -1 || fstat(fd, &st) != 0) {
      if (fd != -1) {
        (void)close(fd);
      }
      http_response(req, 404, NULL, 0);
      return;
    }
    ts.tv_sec = st.st_mtime;
    ts.tv_nsec = 0L;
    ref = kore_fileref_create(req->owner->owner->server, file_path, fd, st.st_size, &ts);
    if (ref == NULL) {
      (void)close(fd);
      http_response(req, 500, NULL, 0);
      return;
    }
    if (vectis_internal_response_file_temporary(response)) {
      (void)unlink(file_path);
    }
    http_response_fileref(req, status, ref);
    return;
  }
  body = vectis_internal_response_body(response);
  http_response(req, status, body.data, body.size);
}

int vectis_kore_route(struct http_request *req) {
  vectis_request *request;
  vectis_response *response;
  vectis_error error;
  vectis_app *app;
  vectis_body_policy body_policy;
  vectis_http_method method;
  vectis_status status;
  int error_status;
  int route_matched;

  vectis_error_clear(&error);
  error_status = 0;
  route_matched = 0;
  request = vectis_internal_request_new(&error);
  response = vectis_internal_response_new(&error);

  (void)pthread_mutex_lock(&vectis_kore_mutex);
  app = vectis_kore_current.app;
  (void)pthread_mutex_unlock(&vectis_kore_mutex);

  if (request == NULL || response == NULL) {
    if (request != NULL) {
      vectis_internal_request_free(request);
    }
    if (response != NULL) {
      vectis_internal_response_free(response);
    }
    http_response(req, 500, error.message, strlen(error.message));
    return KORE_RESULT_OK;
  }

  if (app == NULL || req == NULL || req->path == NULL) {
    http_response(req, 503, NULL, 0);
    vectis_internal_request_free(request);
    vectis_internal_response_free(response);
    return KORE_RESULT_OK;
  }
  vectis_internal_request_set_kore(request, req);

  method = vectis_kore_method(req->method);
  vectis_internal_request_set_method(request, method);
  status = vectis_kore_copy_request_metadata(req, request, &error);
  if (status == VECTIS_OK) {
    status = vectis_internal_route_body_policy(app, method, req->path, &body_policy, &error);
    if (status == VECTIS_OK) {
      route_matched = 1;
    }
  }
  if (status == VECTIS_OK) {
    status = vectis_kore_attach_streamed_body(req,
                                              &body_policy,
                                              request,
                                              &error_status,
                                              &error);
  }
  if (status == VECTIS_OK) {
    status = vectis_internal_dispatch_route(app, method, req->path, request, response, &error);
  }

  if (status == VECTIS_OK) {
    vectis_kore_send_response(req, response);
  } else if (error_status != 0) {
    http_response(req, error_status, error.message, strlen(error.message));
  } else if (status == VECTIS_ERR_INVALID) {
    http_response(req, 400, error.message, strlen(error.message));
  } else if (status == VECTIS_ERR_STATE && !route_matched) {
    http_response(req, 404, NULL, 0);
  } else if (status == VECTIS_ERR_NOT_IMPLEMENTED) {
    http_response(req, 501, error.message, strlen(error.message));
  } else {
    http_response(req, 500, error.message, strlen(error.message));
  }

  vectis_internal_response_free(response);
  vectis_internal_request_free(request);
  return KORE_RESULT_OK;
}

void kore_parent_configure(int argc, char **argv) {
  struct kore_server *server;
  struct kore_domain *domain;
  struct kore_route *route;
  char port[16];

  (void)argc;
  (void)argv;

  (void)pthread_mutex_lock(&vectis_kore_mutex);
  if (vectis_kore_current.logger != NULL) {
    kore_log_set_logger(vectis_kore_current.logger);
  }
  vectis_kore_apply_server_config(&vectis_kore_current.server,
                                  vectis_kore_current.body_disk_offload_bytes,
                                  vectis_kore_current.body_disk_offload_configured);
  server = kore_server_create(vectis_kore_current.app_name != NULL ?
                              vectis_kore_current.app_name : "vectis");
  if (vectis_kore_current.tls_mode == VECTIS_TLS_MODE_DISABLED) {
    server->tls = 0;
  }
  (void)snprintf(port, sizeof(port), "%u", (unsigned)vectis_kore_current.port);
  if (!kore_server_bind(server,
                        vectis_kore_current.bind != NULL ? vectis_kore_current.bind : "0.0.0.0",
                        port,
                        NULL)) {
    fatal("failed to bind Vectis Kore listener");
  }
  acme_domains = 0;
  domain = kore_domain_new(vectis_kore_current.domain != NULL ?
                           vectis_kore_current.domain : "*");
  if (server->tls) {
    if (vectis_kore_current.tls_mode == VECTIS_TLS_MODE_ACME) {
      domain->acme = 1;
      kore_free(domain->certfile);
      kore_free(domain->certkey);
      kore_acme_get_paths(domain->domain, &domain->certkey, &domain->certfile);
      acme_domains++;
      kore_free(acme_email);
      acme_email = kore_strdup(vectis_kore_current.acme_email);
      kore_free(acme_provider);
      acme_provider = kore_strdup(vectis_kore_current.acme_directory_url != NULL ?
                                  vectis_kore_current.acme_directory_url :
                                  VECTIS_ACME_DIRECTORY_LETSENCRYPT_PRODUCTION);
    } else {
      domain->certfile = kore_strdup(vectis_kore_current.runtime_certfile);
      domain->certkey = kore_strdup(vectis_kore_current.runtime_certkey);
    }
    if (vectis_kore_current.require_client_certificate &&
        vectis_kore_current.runtime_client_ca_file != NULL) {
      domain->cafile = kore_strdup(vectis_kore_current.runtime_client_ca_file);
    }
    if (vectis_kore_current.tls_mode != VECTIS_TLS_MODE_ACME) {
      vectis_kore_setup_domain_tls(domain);
    }
  }
  if (!kore_domain_attach(domain, server)) {
    fatal("failed to attach Vectis Kore domain");
  }
  route = kore_route_create(domain, "^/.*$", HANDLER_TYPE_DYNAMIC);
  if (route == NULL) {
    fatal("failed to create Vectis Kore route");
  }
  kore_route_callback(route, "vectis_kore_route");
  route->on_body_chunk = kore_runtime_getcall("vectis_kore_body_chunk");
  if (route->on_body_chunk == NULL) {
    fatal("failed to resolve Vectis Kore body chunk callback");
  }
  route->on_free = kore_runtime_getcall("vectis_kore_request_free");
  if (route->on_free == NULL) {
    fatal("failed to resolve Vectis Kore request cleanup callback");
  }
  kore_server_finalize(server);
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
}

void kore_parent_teardown(void) {
  (void)pthread_mutex_lock(&vectis_kore_mutex);
  vectis_kore_cleanup_config(&vectis_kore_current);
  memset(&vectis_kore_current, 0, sizeof(vectis_kore_current));
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
}

vectis_status vectis_internal_kore_start(const vectis_kore_runtime_config *config,
                                         vectis_error *error) {
  vectis_kore_runtime_config prepared;
  vectis_status status;
  int rc;

  if (config == NULL || config->app == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "Kore runtime config is required");
    return VECTIS_ERR_INVALID;
  }
  prepared = *config;
  prepared.runtime_certfile = NULL;
  prepared.runtime_certkey = NULL;
  prepared.runtime_client_ca_file = NULL;
  prepared.runtime_certfile_temporary = 0;
  prepared.runtime_certkey_temporary = 0;
  prepared.runtime_client_ca_temporary = 0;
  status = vectis_kore_prepare_acme(&prepared, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_kore_prepare_tls(&prepared, error);
  if (status != VECTIS_OK) {
    vectis_kore_cleanup_local_config(&prepared);
    return status;
  }
  if (prepared.tls_mode == VECTIS_TLS_MODE_MANUAL) {
    status = vectis_kore_load_default_dhparams(error);
    if (status != VECTIS_OK) {
      vectis_kore_cleanup_local_config(&prepared);
      return status;
    }
  }

  (void)pthread_mutex_lock(&vectis_kore_mutex);
  if (vectis_kore_thread_active) {
    (void)pthread_mutex_unlock(&vectis_kore_mutex);
    vectis_kore_cleanup_local_config(&prepared);
    vectis_set_error(error, VECTIS_ERR_STATE, "Kore runtime is already running");
    return VECTIS_ERR_STATE;
  }
  vectis_kore_current = prepared;
  rc = pthread_create(&vectis_kore_thread, NULL, vectis_kore_thread_main, NULL);
  if (rc != 0) {
    vectis_kore_cleanup_config(&vectis_kore_current);
    memset(&vectis_kore_current, 0, sizeof(vectis_kore_current));
    (void)pthread_mutex_unlock(&vectis_kore_mutex);
    vectis_set_error(error, VECTIS_ERR_STATE, "failed to start Kore runtime thread");
    return VECTIS_ERR_STATE;
  }
  vectis_kore_thread_active = 1;
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_internal_kore_stop(vectis_app *app, vectis_error *error) {
  int active;

  (void)pthread_mutex_lock(&vectis_kore_mutex);
  active = vectis_kore_thread_active && vectis_kore_current.app == app;
  if (active) {
    kore_signal(SIGTERM);
  }
  (void)pthread_mutex_unlock(&vectis_kore_mutex);

  if (!active) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }

  (void)pthread_join(vectis_kore_thread, NULL);
  (void)pthread_mutex_lock(&vectis_kore_mutex);
  vectis_kore_thread_active = 0;
  vectis_kore_cleanup_config(&vectis_kore_current);
  memset(&vectis_kore_current, 0, sizeof(vectis_kore_current));
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
  vectis_error_clear(error);
  return VECTIS_OK;
}
