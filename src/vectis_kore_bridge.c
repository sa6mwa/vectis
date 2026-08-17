#include "vectis_internal.h"

#include <kore/acme.h>
#include <kore/http.h>
#include <kore/kore.h>
#include <lc/lc.h>
#if defined(__linux__)
#include <kore/seccomp.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "vectis_kore_hooks.h"

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

extern char **environ;

int vectis_kore_main(int argc, char **argv);
int vectis_kore_route(struct http_request *req);
int vectis_kore_body_chunk(struct http_request *req, const void *data,
                           size_t len);
void vectis_kore_request_free(struct http_request *req);
void vectis_kore_ws_connect(struct connection *connection);
void vectis_kore_ws_message(struct connection *connection, u_int8_t opcode,
                            void *data, size_t len);
void vectis_kore_ws_disconnect(struct connection *connection);
void kore_parent_configure(int argc, char **argv);
void kore_parent_teardown(void);
void vectis_kore_parent_timers(void);

#if defined(KORE_VECTIS_STATIC_RUNTIME)
const struct kore_vectis_runtime_symbol kore_vectis_runtime_symbols[] = {
    {"kore_parent_configure", (void *)kore_parent_configure},
    {"kore_parent_teardown", (void *)kore_parent_teardown},
    {"vectis_kore_route", (void *)vectis_kore_route},
    {"vectis_kore_body_chunk", (void *)vectis_kore_body_chunk},
    {"vectis_kore_request_free", (void *)vectis_kore_request_free},
    {"vectis_kore_ws_connect", (void *)vectis_kore_ws_connect},
    {"vectis_kore_ws_message", (void *)vectis_kore_ws_message},
    {"vectis_kore_ws_disconnect", (void *)vectis_kore_ws_disconnect}};
const size_t kore_vectis_runtime_symbol_count =
    sizeof(kore_vectis_runtime_symbols) /
    sizeof(kore_vectis_runtime_symbols[0]);
#endif

extern int skip_chroot;
extern int skip_runas;
extern int kore_quit;
extern volatile sig_atomic_t sig_recv;
extern u_int64_t worker_idle_timeout;
extern u_int8_t worker_count;
extern u_int8_t worker_set_affinity;
extern u_int32_t worker_accept_threshold;
extern u_int32_t worker_max_connections;
extern u_int32_t worker_rlimit_nofiles;
extern u_int32_t worker_shutdown_timeout_ms;
extern int worker_policy;
extern char *http_body_disk_path;
extern u_int32_t http_request_ms;
extern u_int64_t http_hsts_enable;
extern u_int32_t kore_socket_backlog;
extern u_int64_t kore_websocket_maxframe;
extern u_int64_t kore_websocket_timeout;
extern void http_server_version(const char *version);
extern int http_pretty_error;

static pthread_mutex_t vectis_kore_mutex = PTHREAD_MUTEX_INITIALIZER;
static int vectis_kore_runtime_active = 0;
static int vectis_kore_dh_loaded = 0;
static vectis_kore_runtime_config vectis_kore_current;
static char *vectis_kore_keymgr_root = NULL;
static char *vectis_kore_acme_root = NULL;
static char *vectis_kore_body_disk_path = NULL;

typedef struct vectis_kore_autoblock_entry {
  int used;
  char ip[64];
  unsigned long long window_start_ms;
  unsigned int tcp_stalls;
  unsigned int tls_failures;
  unsigned int status_counts[VECTIS_AUTOBLOCK_MAX_STATUS_RULES];
  unsigned int event_counts[VECTIS_AUTOBLOCK_MAX_EVENT_RULES];
  unsigned long long blocked_until_ms;
} vectis_kore_autoblock_entry;

typedef struct vectis_kore_autoblock_shared_state {
  pthread_mutex_t mutex;
  vectis_kore_autoblock_entry entries[1];
} vectis_kore_autoblock_shared_state;

static vectis_kore_autoblock_shared_state *vectis_kore_autoblock_shared = NULL;
static size_t vectis_kore_autoblock_shared_size = 0u;
static vectis_kore_autoblock_entry *vectis_kore_autoblock_entries = NULL;
static unsigned int vectis_kore_autoblock_capacity = 0u;

static void vectis_kore_wake_listener(void) {
  struct sockaddr_in addr;
  int fd;
  unsigned short port;

  port = vectis_kore_current.port;
  if (port == 0u) {
    return;
  }
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    return;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1) {
    (void)connect(fd, (struct sockaddr *)&addr, sizeof(addr));
  }
  (void)close(fd);
}

static void vectis_kore_reset_runtime_state(void) {
  kore_quit = KORE_QUIT_NONE;
  sig_recv = 0;
  optind = 1;
  worker_count = 0u;
}

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
#if defined(SYS_gettimeofday)
    KORE_SYSCALL_ALLOW(gettimeofday),
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

struct vectis_websocket {
  struct connection *connection;
};

typedef struct vectis_kore_websocket_state {
  vectis_app *app;
  vectis_internal_websocket_match match;
} vectis_kore_websocket_state;

static int vectis_kore_material_present(const vectis_kore_material *material);
static vectis_status
vectis_kore_material_bytes(const vectis_kore_material *material, void **out,
                           size_t *out_size, vectis_error *error);
static vectis_status
vectis_kore_preflight_listener(const vectis_kore_runtime_config *config,
                               vectis_error *error);
static int vectis_kore_preflight_sleep_ms(long delay_ms);

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

static vectis_http_method vectis_kore_method(int method) {
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
  case HTTP_METHOD_PROPFIND:
    return VECTIS_HTTP_PROPFIND;
  case HTTP_METHOD_MKCOL:
    return VECTIS_HTTP_MKCOL;
  case HTTP_METHOD_COPY:
    return VECTIS_HTTP_COPY;
  case HTTP_METHOD_MOVE:
    return VECTIS_HTTP_MOVE;
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

static unsigned long long vectis_kore_autoblock_now(void) {
  struct timeval tv;

  if (gettimeofday(&tv, NULL) != 0) {
    return 0ULL;
  }
  return (unsigned long long)tv.tv_sec * 1000ULL +
         (unsigned long long)(tv.tv_usec / 1000L);
}

static int vectis_kore_autoblock_enabled(void) {
  return vectis_kore_current.server.autoblock.enabled &&
         vectis_kore_current.server.autoblock.window_seconds > 0u &&
         vectis_kore_current.server.autoblock.block_seconds > 0u &&
         vectis_kore_current.server.autoblock.max_entries > 0u &&
         vectis_kore_autoblock_shared != NULL &&
         vectis_kore_autoblock_entries != NULL;
}

static void vectis_kore_autoblock_unmap(void) {
  if (vectis_kore_autoblock_shared != NULL &&
      vectis_kore_autoblock_shared_size > 0u) {
    (void)munmap(vectis_kore_autoblock_shared,
                 vectis_kore_autoblock_shared_size);
  }
  vectis_kore_autoblock_shared = NULL;
  vectis_kore_autoblock_shared_size = 0u;
  vectis_kore_autoblock_entries = NULL;
  vectis_kore_autoblock_capacity = 0u;
}

static int vectis_kore_autoblock_map(unsigned int capacity) {
  pthread_mutexattr_t attr;
  size_t size;
  int ok;

  if (capacity == 0u) {
    return 0;
  }
  size = sizeof(*vectis_kore_autoblock_shared) +
         ((size_t)capacity - 1u) * sizeof(vectis_kore_autoblock_entry);
  vectis_kore_autoblock_shared = (vectis_kore_autoblock_shared_state *)mmap(
      NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
  if (vectis_kore_autoblock_shared == MAP_FAILED) {
    vectis_kore_autoblock_shared = NULL;
    return 0;
  }
  memset(vectis_kore_autoblock_shared, 0, size);
  vectis_kore_autoblock_shared_size = size;
  ok = pthread_mutexattr_init(&attr) == 0;
  if (ok) {
    ok = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0;
  }
  if (ok) {
    ok = pthread_mutex_init(&vectis_kore_autoblock_shared->mutex, &attr) == 0;
  }
  (void)pthread_mutexattr_destroy(&attr);
  if (!ok) {
    vectis_kore_autoblock_unmap();
    return 0;
  }
  vectis_kore_autoblock_entries = vectis_kore_autoblock_shared->entries;
  vectis_kore_autoblock_capacity = capacity;
  return 1;
}

static int vectis_kore_autoblock_lock(void) {
  if (vectis_kore_autoblock_shared == NULL) {
    return 0;
  }
  return pthread_mutex_lock(&vectis_kore_autoblock_shared->mutex) == 0;
}

static void vectis_kore_autoblock_unlock(void) {
  if (vectis_kore_autoblock_shared != NULL) {
    (void)pthread_mutex_unlock(&vectis_kore_autoblock_shared->mutex);
  }
}

static void
vectis_kore_autoblock_reset_entry(vectis_kore_autoblock_entry *entry,
                                  const char *ip, unsigned long long now) {
  memset(entry, 0, sizeof(*entry));
  entry->used = 1;
  (void)snprintf(entry->ip, sizeof(entry->ip), "%s", ip);
  entry->window_start_ms = now;
}

static int vectis_kore_autoblock_ip_safe(const char *ip) {
  size_t i;

  if (ip == NULL || ip[0] == '\0' || strlen(ip) >= 64u) {
    return 0;
  }
  for (i = 0u; ip[i] != '\0'; ++i) {
    if (!((ip[i] >= '0' && ip[i] <= '9') || (ip[i] >= 'a' && ip[i] <= 'f') ||
          (ip[i] >= 'A' && ip[i] <= 'F') || ip[i] == '.' || ip[i] == ':' ||
          ip[i] == '[' || ip[i] == ']')) {
      return 0;
    }
  }
  return 1;
}

static int vectis_kore_autoblock_copy_ip(const char *value, char *out,
                                         size_t out_size) {
  size_t len;

  if (!vectis_kore_autoblock_ip_safe(value) || out == NULL || out_size == 0u) {
    return 0;
  }
  len = strlen(value);
  if (len >= out_size) {
    return 0;
  }
  memcpy(out, value, len + 1u);
  return 1;
}

static int vectis_kore_autoblock_peer_ip(struct connection *connection,
                                         char *out, size_t out_size) {
  const char *ip;

  if (connection == NULL) {
    return 0;
  }
  ip = kore_connection_ip(connection);
  return vectis_kore_autoblock_copy_ip(ip, out, out_size);
}

static int vectis_kore_autoblock_trusted_proxy(const char *ip) {
  const char *trusted;
  size_t i;

  if (ip == NULL || !vectis_kore_current.server.autoblock.proxy_enabled) {
    return 0;
  }
  for (i = 0u; i < vectis_kore_current.server.autoblock.trusted_proxy_count;
       ++i) {
    trusted = vectis_kore_current.server.autoblock.trusted_proxies[i];
    if (trusted != NULL && strcmp(trusted, ip) == 0) {
      return 1;
    }
  }
  return 0;
}

static int vectis_kore_autoblock_header_first_ip(const char *value, char *out,
                                                 size_t out_size) {
  char ip[64];
  size_t i;

  if (value == NULL) {
    return 0;
  }
  while (*value == ' ' || *value == '\t') {
    value++;
  }
  for (i = 0u; value[i] != '\0' && value[i] != ','; ++i) {
    if (i + 1u >= sizeof(ip)) {
      return 0;
    }
    ip[i] = value[i];
  }
  while (i > 0u && (ip[i - 1u] == ' ' || ip[i - 1u] == '\t')) {
    i--;
  }
  ip[i] = '\0';
  return vectis_kore_autoblock_copy_ip(ip, out, out_size);
}

static int vectis_kore_autoblock_request_ip(struct http_request *request,
                                            char *out, size_t out_size) {
  const char *header;
  char peer[64];

  if (request == NULL ||
      !vectis_kore_autoblock_peer_ip(request->owner, peer, sizeof(peer))) {
    return 0;
  }
  if (vectis_kore_autoblock_trusted_proxy(peer)) {
    if (http_request_header(request, "x-forwarded-for", &header) &&
        vectis_kore_autoblock_header_first_ip(header, out, out_size)) {
      return 1;
    }
    if (http_request_header(request, "x-real-ip", &header) &&
        vectis_kore_autoblock_header_first_ip(header, out, out_size)) {
      return 1;
    }
  }
  return vectis_kore_autoblock_copy_ip(peer, out, out_size);
}

static vectis_kore_autoblock_entry *
vectis_kore_autoblock_find_entry(const char *ip, unsigned long long now,
                                 int create) {
  vectis_kore_autoblock_entry *entry;
  vectis_kore_autoblock_entry *candidate;
  unsigned long long oldest;
  unsigned int i;

  if (ip == NULL || vectis_kore_autoblock_entries == NULL ||
      vectis_kore_autoblock_capacity == 0u) {
    return NULL;
  }
  candidate = NULL;
  oldest = (unsigned long long)-1;
  for (i = 0u; i < vectis_kore_autoblock_capacity; ++i) {
    entry = &vectis_kore_autoblock_entries[i];
    if (entry->used && strcmp(entry->ip, ip) == 0) {
      return entry;
    }
    if (!entry->used) {
      candidate = entry;
      break;
    }
    if (entry->blocked_until_ms <= now && entry->window_start_ms < oldest) {
      oldest = entry->window_start_ms;
      candidate = entry;
    }
  }
  if (!create || candidate == NULL) {
    return NULL;
  }
  vectis_kore_autoblock_reset_entry(candidate, ip, now);
  return candidate;
}

static void
vectis_kore_autoblock_note_counter(vectis_kore_autoblock_entry *entry,
                                   const char *ip, unsigned int *counter,
                                   unsigned int threshold, const char *reason) {
  unsigned long long now;
  unsigned long long window_ms;
  unsigned long long block_ms;

  if (!vectis_kore_autoblock_enabled() || entry == NULL || ip == NULL ||
      counter == NULL || threshold == 0u) {
    return;
  }
  now = vectis_kore_autoblock_now();
  window_ms =
      (unsigned long long)vectis_kore_current.server.autoblock.window_seconds *
      1000ULL;
  block_ms =
      (unsigned long long)vectis_kore_current.server.autoblock.block_seconds *
      1000ULL;
  if (now == 0ULL) {
    return;
  }
  if (now - entry->window_start_ms > window_ms) {
    vectis_kore_autoblock_reset_entry(entry, ip, now);
  }
  (*counter)++;
  if (*counter >= threshold) {
    entry->blocked_until_ms = now + block_ms;
    kore_log(LOG_NOTICE, "vectis autoblock ip=%s reason=%s threshold=%u", ip,
             reason != NULL ? reason : "rule", threshold);
  }
}

static int vectis_kore_autoblock_is_blocked_ip(const char *ip) {
  vectis_kore_autoblock_entry *entry;
  unsigned long long now;

  if (!vectis_kore_autoblock_enabled()) {
    return 0;
  }
  now = vectis_kore_autoblock_now();
  entry = vectis_kore_autoblock_find_entry(ip, now, 0);
  return entry != NULL && entry->blocked_until_ms > now;
}

int vectis_kore_autoblock_accept(struct connection *connection) {
  char ip[64];
  int blocked;

  if (!vectis_kore_autoblock_enabled()) {
    return 1;
  }
  if (!vectis_kore_autoblock_peer_ip(connection, ip, sizeof(ip))) {
    return 1;
  }
  if (!vectis_kore_autoblock_lock()) {
    return 1;
  }
  blocked = vectis_kore_autoblock_is_blocked_ip(ip);
  vectis_kore_autoblock_unlock();
  if (blocked) {
    kore_log(LOG_NOTICE, "vectis autoblock dropping accepted peer ip=%s", ip);
  }
  return !blocked;
}

void vectis_kore_autoblock_tcp_stall(struct connection *connection) {
  vectis_kore_autoblock_entry *entry;
  char ip[64];

  if (!vectis_kore_autoblock_enabled() ||
      !vectis_kore_autoblock_peer_ip(connection, ip, sizeof(ip)) ||
      !vectis_kore_autoblock_lock()) {
    return;
  }
  entry = vectis_kore_autoblock_find_entry(ip, vectis_kore_autoblock_now(), 1);
  if (entry != NULL) {
    vectis_kore_autoblock_note_counter(
        entry, ip, &entry->tcp_stalls,
        vectis_kore_current.server.autoblock.tcp_stall_threshold, "tcp_stall");
  }
  vectis_kore_autoblock_unlock();
}

void vectis_kore_autoblock_tls_failure(struct connection *connection) {
  vectis_kore_autoblock_entry *entry;
  char ip[64];

  if (!vectis_kore_autoblock_enabled() ||
      !vectis_kore_autoblock_peer_ip(connection, ip, sizeof(ip)) ||
      !vectis_kore_autoblock_lock()) {
    return;
  }
  entry = vectis_kore_autoblock_find_entry(ip, vectis_kore_autoblock_now(), 1);
  if (entry != NULL) {
    vectis_kore_autoblock_note_counter(
        entry, ip, &entry->tls_failures,
        vectis_kore_current.server.autoblock.tls_failure_threshold,
        "tls_failure");
  }
  vectis_kore_autoblock_unlock();
}

int vectis_kore_autoblock_request_allowed(struct http_request *request) {
  char ip[64];
  int blocked;

  if (!vectis_kore_autoblock_enabled()) {
    return 1;
  }
  if (!vectis_kore_autoblock_request_ip(request, ip, sizeof(ip))) {
    return 1;
  }
  if (!vectis_kore_autoblock_lock()) {
    return 1;
  }
  blocked = vectis_kore_autoblock_is_blocked_ip(ip);
  vectis_kore_autoblock_unlock();
  if (blocked) {
    kore_log(LOG_NOTICE, "vectis autoblock dropping request ip=%s", ip);
  }
  return !blocked;
}

void vectis_kore_autoblock_http_status(struct http_request *request,
                                       int status) {
  vectis_kore_autoblock_entry *entry;
  char ip[64];
  char reason[32];
  size_t i;

  if (!vectis_kore_autoblock_enabled() ||
      !vectis_kore_autoblock_request_ip(request, ip, sizeof(ip)) ||
      !vectis_kore_autoblock_lock()) {
    return;
  }
  for (i = 0u; i < vectis_kore_current.server.autoblock.status_rule_count;
       ++i) {
    if ((int)vectis_kore_current.server.autoblock.status_rules[i].status ==
        status) {
      entry =
          vectis_kore_autoblock_find_entry(ip, vectis_kore_autoblock_now(), 1);
      if (entry != NULL) {
        (void)snprintf(reason, sizeof(reason), "status_%d", status);
        vectis_kore_autoblock_note_counter(
            entry, ip, &entry->status_counts[i],
            vectis_kore_current.server.autoblock.status_rules[i].threshold,
            reason);
      }
    }
  }
  vectis_kore_autoblock_unlock();
}

void vectis_kore_autoblock_connection_status(struct connection *connection,
                                             int status) {
  vectis_kore_autoblock_entry *entry;
  char ip[64];
  char reason[32];
  size_t i;

  if (!vectis_kore_autoblock_enabled() ||
      !vectis_kore_autoblock_peer_ip(connection, ip, sizeof(ip)) ||
      !vectis_kore_autoblock_lock()) {
    return;
  }
  for (i = 0u; i < vectis_kore_current.server.autoblock.status_rule_count;
       ++i) {
    if ((int)vectis_kore_current.server.autoblock.status_rules[i].status ==
        status) {
      entry =
          vectis_kore_autoblock_find_entry(ip, vectis_kore_autoblock_now(), 1);
      if (entry != NULL) {
        (void)snprintf(reason, sizeof(reason), "status_%d", status);
        vectis_kore_autoblock_note_counter(
            entry, ip, &entry->status_counts[i],
            vectis_kore_current.server.autoblock.status_rules[i].threshold,
            reason);
      }
    }
  }
  vectis_kore_autoblock_unlock();
}

void vectis_kore_autoblock_request_event(struct http_request *request,
                                         const char *name) {
  vectis_kore_autoblock_entry *entry;
  char ip[64];
  size_t i;

  if (!vectis_kore_autoblock_enabled() || name == NULL ||
      !vectis_kore_autoblock_request_ip(request, ip, sizeof(ip)) ||
      !vectis_kore_autoblock_lock()) {
    return;
  }
  for (i = 0u; i < vectis_kore_current.server.autoblock.event_rule_count; ++i) {
    if (vectis_kore_current.server.autoblock.event_rules[i].name != NULL &&
        strcmp(vectis_kore_current.server.autoblock.event_rules[i].name,
               name) == 0) {
      entry =
          vectis_kore_autoblock_find_entry(ip, vectis_kore_autoblock_now(), 1);
      if (entry != NULL) {
        vectis_kore_autoblock_note_counter(
            entry, ip, &entry->event_counts[i],
            vectis_kore_current.server.autoblock.event_rules[i].threshold,
            name);
      }
    }
  }
  vectis_kore_autoblock_unlock();
}

#if defined(VECTIS_BUILD_FUZZERS)
void vectis_internal_kore_fuzzer_set_app(vectis_app *app) {
  (void)pthread_mutex_lock(&vectis_kore_mutex);
  vectis_kore_current.app = app;
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
}
#endif

static void vectis_kore_set_errorf(vectis_error *error, vectis_status code,
                                   const char *fmt, ...) {
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
  if (config->control_fd >= 0) {
    (void)close(config->control_fd);
    config->control_fd = -1;
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
  if (config->runtime_client_ca_temporary &&
      config->runtime_client_ca_file != NULL) {
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

static int vectis_kore_mkdir_p(const char *path) {
  char *copy;
  char *cursor;
  int ok;

  if (path == NULL || path[0] == '\0') {
    return 0;
  }
  copy = (char *)malloc(strlen(path) + 1u);
  if (copy == NULL) {
    return 0;
  }
  (void)strcpy(copy, path);
  ok = 1;
  cursor = copy;
  if (cursor[0] == '/') {
    cursor++;
  }
  for (; *cursor != '\0'; ++cursor) {
    if (*cursor != '/') {
      continue;
    }
    *cursor = '\0';
    if (copy[0] != '\0' && mkdir(copy, 0700) != 0 && errno != EEXIST) {
      ok = 0;
      *cursor = '/';
      break;
    }
    *cursor = '/';
  }
  if (ok && mkdir(copy, 0700) != 0 && errno != EEXIST) {
    ok = 0;
  }
  free(copy);
  return ok;
}

static vectis_status vectis_kore_prepare_body_spool_dir(const char *path,
                                                        vectis_error *error) {
  struct stat st;
  char detail[256];

  if (!vectis_kore_mkdir_p(path)) {
    snprintf(detail, sizeof(detail),
             "failed to create Vectis request body spool directory: %s",
             path != NULL ? path : "(null)");
    vectis_set_error(error, VECTIS_ERR_STATE, detail);
    return VECTIS_ERR_STATE;
  }
  if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
      (st.st_mode & 0777u) != 0700u || access(path, W_OK | X_OK) != 0) {
    snprintf(detail, sizeof(detail),
             "Vectis request body spool directory must be an owner-only "
             "writable directory: %s",
             path != NULL ? path : "(null)");
    vectis_set_error(error, VECTIS_ERR_STATE, detail);
    return VECTIS_ERR_STATE;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status
vectis_kore_preflight_body_spool(const vectis_kore_runtime_config *config,
                                 vectis_error *error) {
  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "Kore runtime config is required");
    return VECTIS_ERR_INVALID;
  }
  if (!config->body_disk_offload_configured ||
      config->body_disk_offload_bytes == 0u ||
      config->server.request_body_spool_dir == NULL ||
      config->server.request_body_spool_dir[0] == '\0') {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  return vectis_kore_prepare_body_spool_dir(
      config->server.request_body_spool_dir, error);
}

static vectis_status
vectis_kore_preflight_access_log(const vectis_kore_runtime_config *config,
                                 vectis_error *error) {
  const char *path;
  int fd;
  int saved_errno;

  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "Kore runtime config is required");
    return VECTIS_ERR_INVALID;
  }
  path = config->server.access_log_path;
  if (path == NULL || path[0] == '\0') {
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  fd = open(path, O_CREAT | O_APPEND | O_WRONLY, 0644);
  if (fd == -1) {
    saved_errno = errno;
    vectis_kore_set_errorf(error, VECTIS_ERR_STATE,
                           "failed to open Kore access_log_path '%s': %s", path,
                           strerror(saved_errno));
    return VECTIS_ERR_STATE;
  }
  if (close(fd) != 0) {
    saved_errno = errno;
    vectis_kore_set_errorf(error, VECTIS_ERR_STATE,
                           "failed to close Kore access_log_path '%s': %s",
                           path, strerror(saved_errno));
    return VECTIS_ERR_STATE;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status
vectis_kore_prepare_acme(vectis_kore_runtime_config *config,
                         vectis_error *error) {
  if (config->tls_mode != VECTIS_TLS_MODE_ACME) {
    return VECTIS_OK;
  }
  if (config->domain_count == 0u || config->domains == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "ACME mode requires tls.domains");
    return VECTIS_ERR_INVALID;
  }
  if (config->acme_email == NULL || config->acme_email[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "ACME mode requires acme_email");
    return VECTIS_ERR_INVALID;
  }
  if (config->acme_state_dir == NULL || config->acme_state_dir[0] == '\0') {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "ACME mode requires acme_state_dir");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_kore_mkdir_p(config->acme_state_dir)) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to create ACME state directory");
    return VECTIS_ERR_STATE;
  }
  return VECTIS_OK;
}

static void vectis_kore_clear_installed_acme_roots(void) {
  if (vectis_kore_keymgr_root != NULL &&
      keymgr_privsep.root == vectis_kore_keymgr_root) {
    kore_free(keymgr_privsep.root);
    keymgr_privsep.root = NULL;
  }
  if (vectis_kore_acme_root != NULL &&
      acme_privsep.root == vectis_kore_acme_root) {
    kore_free(acme_privsep.root);
    acme_privsep.root = NULL;
  }
  vectis_kore_keymgr_root = NULL;
  vectis_kore_acme_root = NULL;
}

static void vectis_kore_clear_installed_body_disk_path(void) {
  if (vectis_kore_body_disk_path != NULL &&
      http_body_disk_path == vectis_kore_body_disk_path) {
    kore_free(http_body_disk_path);
    http_body_disk_path = NULL;
  }
  vectis_kore_body_disk_path = NULL;
}

static void
vectis_kore_cleanup_local_config(vectis_kore_runtime_config *config) {
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

static void vectis_kore_notify_ready(void) {
  int fd;

  fd = vectis_kore_current.control_fd;
  if (fd < 0) {
    return;
  }
  (void)vectis_internal_runtime_control_write(fd, VECTIS_RUNTIME_CONTROL_READY,
                                              NULL, 0u, NULL);
}

static int vectis_kore_app_ready(void) {
  int *ready;

  ready = vectis_kore_current.app_ready;
  if (ready == NULL) {
    return 1;
  }
  return __sync_fetch_and_add(ready, 0) != 0;
}

static void vectis_kore_request_controlled_shutdown(void) {
  kore_quit = KORE_QUIT_NORMAL;
  kore_signal(SIGTERM);
  vectis_kore_wake_listener();
}

static void vectis_kore_control_timer(void *arg, u_int64_t now) {
  vectis_runtime_control_type type;
  vectis_mutable_bytes payload;
  struct timeval timeout;
  fd_set readfds;
  int fd;
  int selected;

  (void)arg;
  (void)now;
  fd = vectis_kore_current.control_fd;
  if (fd < 0 || kore_quit != KORE_QUIT_NONE) {
    return;
  }

  FD_ZERO(&readfds);
  FD_SET(fd, &readfds);
  timeout.tv_sec = 0;
  timeout.tv_usec = 0;
  selected = select(fd + 1, &readfds, NULL, NULL, &timeout);
  if (selected == 0 || (selected < 0 && errno == EINTR)) {
    return;
  }
  if (selected < 0) {
    vectis_kore_request_controlled_shutdown();
    return;
  }

  memset(&payload, 0, sizeof(payload));
  if (vectis_internal_runtime_control_read(fd, &type, &payload, NULL) !=
      VECTIS_OK) {
    vectis_mutable_bytes_cleanup(&payload);
    vectis_kore_request_controlled_shutdown();
    return;
  }
  vectis_mutable_bytes_cleanup(&payload);
  if (type == VECTIS_RUNTIME_CONTROL_STOP) {
    vectis_kore_request_controlled_shutdown();
  }
}

void vectis_kore_parent_timers(void) {
  if (vectis_kore_current.control_fd >= 0) {
    (void)kore_timer_add(vectis_kore_control_timer, 100, NULL, 0);
  }
}

static vectis_status vectis_kore_temp_file_from_bytes(const void *data,
                                                      size_t size,
                                                      char **path_out,
                                                      vectis_error *error) {
  char template_path[] = "/tmp/vectis-kore-XXXXXX";
  int fd;

  if (path_out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "temporary path output is required");
    return VECTIS_ERR_INVALID;
  }
  *path_out = NULL;
  if (data == NULL || size == 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "TLS material is empty");
    return VECTIS_ERR_INVALID;
  }

  fd = mkstemp(template_path);
  if (fd < 0) {
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to create temporary TLS material file");
    return VECTIS_ERR_STATE;
  }
  (void)fchmod(fd, S_IRUSR | S_IWUSR);
  if (!vectis_kore_write_all(fd, data, size) || fsync(fd) != 0) {
    (void)close(fd);
    (void)unlink(template_path);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to write temporary TLS material file");
    return VECTIS_ERR_STATE;
  }
  if (close(fd) != 0) {
    (void)unlink(template_path);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to close temporary TLS material file");
    return VECTIS_ERR_STATE;
  }
  *path_out = vectis_kore_strdup(template_path);
  if (*path_out == NULL) {
    (void)unlink(template_path);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to copy temporary TLS material path");
    return VECTIS_ERR_NOMEM;
  }
  return VECTIS_OK;
}

static vectis_status vectis_kore_read_source(lc_source *source, void **out,
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
    vectis_kore_set_errorf(
        error, VECTIS_ERR_STATE, "failed to reset TLS source: %s",
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
        vectis_set_error(error, VECTIS_ERR_NOMEM,
                         "failed to buffer TLS source");
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

static vectis_status
vectis_kore_material_to_file(const vectis_kore_material *material,
                             char **path_out, int *temporary_out,
                             vectis_error *error) {
  void *source_bytes;
  size_t source_size;
  vectis_status status;

  if (path_out == NULL || temporary_out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "TLS material output is required");
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
      vectis_set_error(error, VECTIS_ERR_NOMEM,
                       "failed to copy TLS material path");
      return VECTIS_ERR_NOMEM;
    }
    return VECTIS_OK;
  }
  if (material->memory != NULL && material->memory_size > 0u) {
    status = vectis_kore_temp_file_from_bytes(
        material->memory, material->memory_size, path_out, error);
    if (status == VECTIS_OK) {
      *temporary_out = 1;
    }
    return status;
  }
  if (material->source != NULL) {
    source_bytes = NULL;
    source_size = 0u;
    status = vectis_kore_read_source(material->source, &source_bytes,
                                     &source_size, error);
    if (status != VECTIS_OK) {
      return status;
    }
    status = vectis_kore_temp_file_from_bytes(source_bytes, source_size,
                                              path_out, error);
    free(source_bytes);
    if (status == VECTIS_OK) {
      *temporary_out = 1;
    }
    return status;
  }
  vectis_set_error(error, VECTIS_ERR_INVALID, "TLS material is missing");
  return VECTIS_ERR_INVALID;
}

static vectis_status
vectis_kore_certificate_chain_to_file(const vectis_kore_material *certificate,
                                      const vectis_kore_material *ca_bundle,
                                      char **path_out, int *temporary_out,
                                      vectis_error *error) {
  void *cert_bytes;
  void *ca_bytes;
  unsigned char *chain;
  size_t cert_size;
  size_t ca_size;
  size_t chain_size;
  vectis_status status;

  if (certificate == NULL || path_out == NULL || temporary_out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "TLS certificate output is required");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_kore_material_present(ca_bundle)) {
    return vectis_kore_material_to_file(certificate, path_out, temporary_out,
                                        error);
  }

  cert_bytes = NULL;
  ca_bytes = NULL;
  chain = NULL;
  cert_size = 0u;
  ca_size = 0u;
  status =
      vectis_kore_material_bytes(certificate, &cert_bytes, &cert_size, error);
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
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "TLS certificate chain is too large");
    return VECTIS_ERR_NOMEM;
  }
  chain_size = cert_size + ca_size;
  if (cert_size > 0u &&
      ((const unsigned char *)cert_bytes)[cert_size - 1u] != '\n') {
    chain_size++;
  }
  chain = (unsigned char *)malloc(chain_size);
  if (chain == NULL) {
    free(cert_bytes);
    free(ca_bytes);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate TLS certificate chain");
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
  const char *markers[] = {"-----BEGIN PRIVATE KEY-----",
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

static vectis_status vectis_kore_read_path(const char *path, void **out,
                                           size_t *out_size,
                                           vectis_error *error) {
  FILE *fp;
  long len;
  void *buffer;

  if (path == NULL || out == NULL || out_size == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "TLS material path is required");
    return VECTIS_ERR_INVALID;
  }
  *out = NULL;
  *out_size = 0u;
  fp = fopen(path, "rb");
  if (fp == NULL) {
    vectis_kore_set_errorf(error, VECTIS_ERR_STATE,
                           "failed to open TLS material path: %s", path);
    return VECTIS_ERR_STATE;
  }
  if (fseek(fp, 0L, SEEK_END) != 0 || (len = ftell(fp)) <= 0L ||
      fseek(fp, 0L, SEEK_SET) != 0) {
    (void)fclose(fp);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to size TLS material path");
    return VECTIS_ERR_STATE;
  }
  buffer = malloc((size_t)len);
  if (buffer == NULL) {
    (void)fclose(fp);
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to buffer TLS material path");
    return VECTIS_ERR_NOMEM;
  }
  if (fread(buffer, 1u, (size_t)len, fp) != (size_t)len) {
    free(buffer);
    (void)fclose(fp);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to read TLS material path");
    return VECTIS_ERR_STATE;
  }
  (void)fclose(fp);
  *out = buffer;
  *out_size = (size_t)len;
  return VECTIS_OK;
}

static vectis_status
vectis_kore_material_bytes(const vectis_kore_material *material, void **out,
                           size_t *out_size, vectis_error *error) {
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

static vectis_status
vectis_kore_split_bundle(const vectis_kore_material *bundle, char **certfile,
                         int *cert_temporary, char **certkey,
                         int *key_temporary, vectis_error *error) {
  void *bundle_bytes;
  size_t bundle_size;
  const char *key_start;
  size_t cert_size;
  vectis_status status;

  bundle_bytes = NULL;
  bundle_size = 0u;
  status =
      vectis_kore_material_bytes(bundle, &bundle_bytes, &bundle_size, error);
  if (status != VECTIS_OK) {
    return status;
  }
  key_start = vectis_kore_find_private_key_marker((const char *)bundle_bytes,
                                                  bundle_size);
  if (key_start == NULL || key_start == (const char *)bundle_bytes) {
    free(bundle_bytes);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "TLS cert/key bundle must contain certificate data before "
                     "a private key");
    return VECTIS_ERR_INVALID;
  }
  cert_size = (size_t)(key_start - (const char *)bundle_bytes);
  status = vectis_kore_temp_file_from_bytes(bundle_bytes, cert_size, certfile,
                                            error);
  if (status == VECTIS_OK) {
    *cert_temporary = 1;
    status = vectis_kore_temp_file_from_bytes(
        key_start, bundle_size - cert_size, certkey, error);
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
         (material->path != NULL || material->source != NULL ||
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
    status = vectis_kore_split_bundle(
        &bundle, &config->runtime_certfile, &config->runtime_certfile_temporary,
        &config->runtime_certkey, &config->runtime_certkey_temporary, error);
    if (status != VECTIS_OK) {
      return status;
    }
  } else {
    status = vectis_kore_certificate_chain_to_file(
        &certificate, &ca_bundle, &config->runtime_certfile,
        &config->runtime_certfile_temporary, error);
    if (status != VECTIS_OK) {
      return status;
    }
    status =
        vectis_kore_material_to_file(&private_key, &config->runtime_certkey,
                                     &config->runtime_certkey_temporary, error);
    if (status != VECTIS_OK) {
      return status;
    }
  }

  if (config->require_client_certificate) {
    status = vectis_kore_material_to_file(
        &client_ca, &config->runtime_client_ca_file,
        &config->runtime_client_ca_temporary, error);
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
  status = vectis_kore_temp_file_from_bytes(
      vectis_kore_default_dhparams, strlen(vectis_kore_default_dhparams), &path,
      error);
  if (status != VECTIS_OK) {
    return status;
  }
  if (!kore_tls_dh_load(path)) {
    (void)unlink(path);
    free(path);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to load Vectis default DH parameters");
    return VECTIS_ERR_STATE;
  }
  (void)unlink(path);
  free(path);
  vectis_kore_dh_loaded = 1;
  return VECTIS_OK;
}

static int vectis_kore_tls_version(vectis_tls_version version) {
  switch (version) {
  case VECTIS_TLS_VERSION_1_2:
    return KORE_TLS_VERSION_1_2;
  case VECTIS_TLS_VERSION_1_3:
    return KORE_TLS_VERSION_1_3;
  case VECTIS_TLS_VERSION_DEFAULT:
  case VECTIS_TLS_VERSION_BOTH:
  default:
    return KORE_TLS_VERSION_BOTH;
  }
}

static void
vectis_kore_apply_tls_config(const vectis_kore_runtime_config *config) {
  if (config == NULL || config->tls_mode == VECTIS_TLS_MODE_DISABLED) {
    return;
  }
  kore_tls_version_set(vectis_kore_tls_version(config->tls_version));
  if (config->tls_cipher_list != NULL &&
      !kore_tls_ciphersuite_set(config->tls_cipher_list)) {
    fatal("failed to configure Vectis TLS cipher list");
  }
}

static void vectis_kore_apply_server_config(const vectis_server_config *server,
                                            size_t body_disk_offload_bytes,
                                            int body_disk_offload_configured) {
  worker_count = (u_int8_t)server->worker_count;
  worker_accept_threshold =
      vectis_kore_u32_from_size(server->worker_accept_threshold);
  worker_max_connections = vectis_kore_u32_from_size(server->max_connections);
  worker_rlimit_nofiles =
      vectis_kore_u32_from_size(server->worker_rlimit_nofiles);
  worker_idle_timeout = (u_int64_t)server->idle_timeout_ms;
  worker_set_affinity = server->worker_set_affinity_disabled ? 0u : 1u;
  worker_shutdown_timeout_ms =
      vectis_kore_u32_from_size((size_t)server->worker_shutdown_timeout_ms);
  worker_policy = server->worker_death_policy == VECTIS_WORKER_DEATH_TERMINATE
                      ? KORE_WORKER_POLICY_TERMINATE
                      : KORE_WORKER_POLICY_RESTART;
  http_request_limit = vectis_kore_u32_from_size(server->request_limit);
  http_header_max = server->max_request_header_bytes;
  http_body_max = server->max_request_body_bytes;
  http_body_disk_offload =
      body_disk_offload_configured ? (u_int64_t)body_disk_offload_bytes : 0u;
  vectis_kore_clear_installed_body_disk_path();
  if (http_body_disk_offload > 0u && server->request_body_spool_dir != NULL &&
      server->request_body_spool_dir[0] != '\0') {
    vectis_kore_body_disk_path = kore_strdup(server->request_body_spool_dir);
    if (vectis_kore_body_disk_path == NULL) {
      fatal("failed to copy Vectis request body spool directory");
    }
    if (vectis_kore_prepare_body_spool_dir(vectis_kore_body_disk_path, NULL) !=
        VECTIS_OK) {
      fatal("failed to create Vectis request body spool directory: %s",
            vectis_kore_body_disk_path);
    }
    http_body_disk_path = vectis_kore_body_disk_path;
  }
  http_header_timeout =
      vectis_kore_seconds_from_ms(server->request_header_timeout_ms);
  http_body_timeout =
      vectis_kore_seconds_from_ms(server->request_body_idle_timeout_ms);
  http_response_write_timeout =
      vectis_kore_u32_from_size((size_t)server->response_write_idle_timeout_ms);
  http_body_min_rate = (u_int64_t)server->request_body_min_rate_bytes_per_sec;
  http_body_min_rate_grace =
      server->request_body_min_rate_grace_ms > 0L
          ? (u_int64_t)server->request_body_min_rate_grace_ms
          : 0u;
  http_keepalive_time =
      server->keepalive_disabled
          ? 0u
          : vectis_kore_seconds_from_ms(server->keepalive_timeout_ms);
  http_keepalive_max_requests = server->keepalive_disabled
                                    ? 0u
                                    : (u_int32_t)server->keepalive_max_requests;
  kore_socket_backlog = vectis_kore_u32_from_size(server->socket_backlog);
  http_request_ms = (u_int32_t)server->request_process_budget_ms;
  http_hsts_enable = (u_int64_t)server->hsts_max_age_seconds;
  kore_websocket_maxframe = (u_int64_t)server->websocket_max_frame_bytes;
  kore_websocket_timeout = (u_int64_t)server->websocket_timeout_ms;
  http_server_version(server->server_header);
  http_pretty_error = server->pretty_error_pages ? 1 : 0;
}

static int vectis_kore_preflight_sleep_ms(long delay_ms) {
  struct timespec request;
  struct timespec remaining;

  if (delay_ms <= 0L) {
    return 0;
  }
  request.tv_sec = delay_ms / 1000L;
  request.tv_nsec = (delay_ms % 1000L) * 1000000L;
  while (nanosleep(&request, &remaining) != 0) {
    if (errno != EINTR) {
      return errno != 0 ? errno : EINVAL;
    }
    request = remaining;
  }
  return 0;
}

static vectis_status
vectis_kore_preflight_listener(const vectis_kore_runtime_config *config,
                               vectis_error *error) {
  struct addrinfo hints;
  struct addrinfo *results;
  struct addrinfo *item;
  const char *bind_addr;
  char service[16];
  char detail[128];
  int gai;
  int fd;
  int option;
  int last_errno;
  int written;
  long remaining_ms;
  long sleep_ms;

  if (config == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "Kore runtime config is required");
    return VECTIS_ERR_INVALID;
  }
  bind_addr = config->bind != NULL ? config->bind : "0.0.0.0";
  written = snprintf(service, sizeof(service), "%u", (unsigned)config->port);
  if (written <= 0 || (size_t)written >= sizeof(service)) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "Kore listener port is invalid");
    return VECTIS_ERR_INVALID;
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  results = NULL;
  gai = getaddrinfo(bind_addr, service, &hints, &results);
  if (gai != 0) {
    snprintf(detail, sizeof(detail),
             "Kore listener bind address is invalid: %s", gai_strerror(gai));
    vectis_set_error(error, VECTIS_ERR_INVALID, detail);
    return VECTIS_ERR_INVALID;
  }

  remaining_ms = 500L;
  for (;;) {
    last_errno = 0;
    for (item = results; item != NULL; item = item->ai_next) {
      fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
      if (fd < 0) {
        last_errno = errno;
        continue;
      }
      option = 1;
      (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &option,
                       (socklen_t)sizeof(option));
      if (bind(fd, item->ai_addr, item->ai_addrlen) == 0) {
        (void)close(fd);
        freeaddrinfo(results);
        vectis_error_clear(error);
        return VECTIS_OK;
      }
      last_errno = errno;
      (void)close(fd);
    }
    if (last_errno != EADDRINUSE || remaining_ms <= 0L) {
      break;
    }
    sleep_ms = remaining_ms < 25L ? remaining_ms : 25L;
    last_errno = vectis_kore_preflight_sleep_ms(sleep_ms);
    if (last_errno != 0) {
      break;
    }
    remaining_ms -= sleep_ms;
  }
  freeaddrinfo(results);

  if (last_errno == EADDRINUSE) {
    snprintf(detail, sizeof(detail), "Kore listener %s:%u is already in use",
             bind_addr, (unsigned)config->port);
    vectis_set_error(error, VECTIS_ERR_CONFLICT, detail);
    return VECTIS_ERR_CONFLICT;
  }
  snprintf(detail, sizeof(detail), "Kore listener %s:%u is unavailable: %s",
           bind_addr, (unsigned)config->port,
           last_errno != 0 ? strerror(last_errno) : "no usable address");
  vectis_set_error(error, VECTIS_ERR_STATE, detail);
  return VECTIS_ERR_STATE;
}

static void vectis_kore_setup_domain_tls(struct kore_domain *domain) {
  vectis_error error;
  void *cert_pem;
  size_t cert_pem_size;

  cert_pem = NULL;
  cert_pem_size = 0u;
  vectis_error_clear(&error);
  if (vectis_kore_read_path(vectis_kore_current.runtime_certfile, &cert_pem,
                            &cert_pem_size, &error) != VECTIS_OK) {
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
    status = vectis_internal_request_add_header(
        request, header->header, header->value != NULL ? header->value : "",
        error);
    if (status != VECTIS_OK) {
      return status;
    }
  }
  return VECTIS_OK;
}

static vectis_status
vectis_kore_add_query_pair(vectis_request *request, const char *name,
                           size_t name_len, const char *value, size_t value_len,
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
    vectis_set_error(error, VECTIS_ERR_NOMEM,
                     "failed to allocate query parameter");
    return VECTIS_ERR_NOMEM;
  }
  if (!http_argument_urldecode(owned_name, 1) ||
      !http_argument_urldecode(owned_value, 1)) {
    free(owned_name);
    free(owned_value);
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "failed to decode query parameter");
    return VECTIS_ERR_INVALID;
  }
  status = vectis_internal_request_add_query(request, owned_name, owned_value,
                                             error);
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
        status = vectis_kore_add_query_pair(request, pair, name_len, eq + 1,
                                            value_len, error);
      } else {
        status =
            vectis_kore_add_query_pair(request, pair, pair_len, "", 0u, error);
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

static int vectis_kore_run_main(void) {
  char *argv[5];
  const char *args[4];
  char *arena;
  int result;
  int argc;

  argc = 0;
  args[argc++] = "vectis-kore";
  args[argc++] = "-f";
  args[argc++] = "-n";
  args[argc++] = "-r";
  arena = vectis_kore_argv_arena(argv, args, argc);
  if (arena == NULL) {
    return KORE_QUIT_FATAL;
  }
  skip_chroot = 1;
  skip_runas = 1;
  result = vectis_kore_main(argc, argv);
  free(arena);
  return result;
}

typedef struct vectis_kore_body_state {
  vectis_body_policy policy;
  vectis_upload_stream_runtime stream;
  vectis_request *request;
  size_t expected_size;
  size_t total_size;
  unsigned char *memory;
  size_t memory_size;
  size_t memory_capacity;
  FILE *file;
  char *path;
  int error_status;
  int initialized;
  int streaming;
  int spooled;
} vectis_kore_body_state;

static int vectis_kore_tmp_template(char *buffer, size_t buffer_size) {
  int n;

  if (buffer == NULL || buffer_size == 0u || http_body_disk_path == NULL ||
      http_body_disk_path[0] == '\0') {
    return 0;
  }
  n = snprintf(buffer, buffer_size, "%s/vectis-kore-body-XXXXXX",
               http_body_disk_path);
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
  if (state->streaming) {
    vectis_app *app;

    (void)pthread_mutex_lock(&vectis_kore_mutex);
    app = vectis_kore_current.app;
    (void)pthread_mutex_unlock(&vectis_kore_mutex);
    vectis_internal_upload_stream_close(app, state->request, &state->stream);
  }
  if (state->request != NULL) {
    vectis_internal_request_free(state->request);
    state->request = NULL;
  }
  free(state->memory);
  state->memory = NULL;
  state->memory_size = 0u;
  state->memory_capacity = 0u;
  state->total_size = 0u;
  state->error_status = 0;
  state->initialized = 0;
  state->streaming = 0;
  state->spooled = 0;
}

void vectis_kore_request_free(struct http_request *req) {
  if (req == NULL || req->hdlr_extra == NULL) {
    return;
  }
  vectis_kore_body_state_cleanup((vectis_kore_body_state *)req->hdlr_extra);
}

static size_t
vectis_kore_policy_memory_limit(const vectis_body_policy *policy) {
  if (policy == NULL || policy->memory_buffer_limit_bytes == 0u) {
    return VECTIS_BODY_DEFAULT_UPLOAD_MEMORY_LIMIT_BYTES;
  }
  return policy->memory_buffer_limit_bytes;
}

static vectis_kore_body_state *
vectis_kore_body_state_get(struct http_request *req) {
  if (req == NULL) {
    return NULL;
  }
  if (req->hdlr_extra == NULL) {
    return (vectis_kore_body_state *)http_state_create(
        req, sizeof(vectis_kore_body_state));
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
  if (state->memory_size > 0u && fwrite(state->memory, 1u, state->memory_size,
                                        file) != state->memory_size) {
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
                                         const void *data, size_t len) {
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
      next_capacity =
          state->memory_capacity == 0u ? len : state->memory_capacity;
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
                                          int status, const char *message) {
  vectis_app *app;

  if (state != NULL) {
    state->error_status = status;
  }
  (void)pthread_mutex_lock(&vectis_kore_mutex);
  app = vectis_kore_current.app;
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
  vectis_internal_metrics_note_body_reject(app, status);
  http_response(req, status, message, message != NULL ? strlen(message) : 0u);
  req->flags |= HTTP_REQUEST_DELETE;
}

static vectis_status
vectis_kore_open_upload_stream(struct http_request *req,
                               vectis_kore_body_state *state, vectis_app *app,
                               vectis_http_method method, vectis_error *error) {
  vectis_status status;

  if (state->request == NULL) {
    state->request = vectis_internal_request_new(error);
    if (state->request == NULL) {
      return error != NULL ? error->code : VECTIS_ERR_NOMEM;
    }
    vectis_internal_request_set_kore(state->request, req);
    vectis_internal_request_set_method(state->request, method);
    status = vectis_kore_copy_request_metadata(req, state->request, error);
    if (status != VECTIS_OK) {
      return status;
    }
  }

  status = vectis_internal_upload_stream_open(
      app, method, req->path, state->request, &state->stream, error);
  if (status == VECTIS_OK) {
    state->streaming = 1;
  }
  return status;
}

int vectis_kore_body_chunk(struct http_request *req, const void *data,
                           size_t len) {
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
  (void)pthread_mutex_lock(&vectis_kore_mutex);
  app = vectis_kore_current.app;
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
  if (app == NULL || req == NULL || req->path == NULL) {
    vectis_kore_reject_body_chunk(req, state, 503, NULL);
    return KORE_RESULT_OK;
  }
  method = vectis_kore_method(req->method);
  if (!state->initialized) {
    status = vectis_internal_route_body_policy(app, method, req->path,
                                               &state->policy, &error);
    if (status != VECTIS_OK) {
      vectis_kore_reject_body_chunk(req, state, 404, NULL);
      return KORE_RESULT_OK;
    }
    state->expected_size = (size_t)req->http_body_length;
    if (state->policy.mode == VECTIS_BODY_NONE && req->http_body_length > 0u) {
      vectis_kore_reject_body_chunk(
          req, state, 413, "request body is not allowed for this route");
      return KORE_RESULT_OK;
    }
    if (req->http_body_length > (u_int64_t)((size_t)-1) ||
        state->expected_size > state->policy.max_bytes) {
      vectis_kore_reject_body_chunk(req, state, 413,
                                    "request body exceeds route limit");
      return KORE_RESULT_OK;
    }
    state->initialized = 1;
  }
  if (state->policy.mode == VECTIS_BODY_STREAMING_UPLOAD) {
    if (!state->streaming) {
      status = vectis_kore_open_upload_stream(req, state, app, method, &error);
      if (status != VECTIS_OK && status != VECTIS_ERR_STATE) {
        vectis_kore_reject_body_chunk(req, state,
                                      status == VECTIS_ERR_INVALID ? 400 : 500,
                                      error.message);
        return KORE_RESULT_OK;
      }
    }
    if (state->streaming) {
      status = vectis_internal_upload_stream_write(
          app, state->request, &state->stream, data, len, &error);
      if (status != VECTIS_OK) {
        vectis_kore_reject_body_chunk(req, state,
                                      status == VECTIS_ERR_INVALID ? 400 : 500,
                                      error.message);
        return KORE_RESULT_OK;
      }
      state->total_size += len;
      return KORE_RESULT_OK;
    }
  }
  if (!vectis_kore_body_state_append(state, data, len)) {
    vectis_kore_reject_body_chunk(req, state, 413,
                                  "failed to stream request body");
    return KORE_RESULT_OK;
  }
  return KORE_RESULT_OK;
}

static vectis_status vectis_kore_attach_streamed_body(
    struct http_request *req, const vectis_body_policy *policy,
    vectis_request *request, int *http_status, vectis_error *error) {
  vectis_kore_body_state *state;
  size_t body_size;

  if (http_status != NULL) {
    *http_status = 0;
  }
  if (req == NULL || policy == NULL || request == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body state is invalid");
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
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body is not allowed for this route");
    return VECTIS_ERR_INVALID;
  }
  if (body_size > policy->max_bytes) {
    if (http_status != NULL) {
      *http_status = 413;
    }
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body exceeds route limit");
    return VECTIS_ERR_INVALID;
  }
  if (body_size == 0u) {
    return vectis_internal_request_set_body(request, NULL, 0u, error);
  }
  state = (vectis_kore_body_state *)req->hdlr_extra;
  if (state == NULL || state->error_status != 0 ||
      state->total_size != body_size) {
    if (http_status != NULL) {
      *http_status =
          state != NULL && state->error_status != 0 ? state->error_status : 400;
    }
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "request body stream is incomplete");
    return VECTIS_ERR_INVALID;
  }
  if (state->spooled) {
    if (state->file != NULL && fflush(state->file) != 0) {
      vectis_set_error(error, VECTIS_ERR_STATE,
                       "failed to flush streamed request body");
      return VECTIS_ERR_STATE;
    }
    return vectis_internal_request_set_body_path(request, state->path,
                                                 state->total_size, error);
  }
  return vectis_internal_request_set_body(request, state->memory,
                                          state->memory_size, error);
}

typedef struct vectis_kore_response_stream {
  struct connection *connection;
  struct lc_source *source;
  int remove;
} vectis_kore_response_stream;

static void
vectis_kore_response_stream_free(vectis_kore_response_stream *stream) {
  if (stream == NULL) {
    return;
  }
  if (stream->source != NULL) {
    lc_source_close(stream->source);
    stream->source = NULL;
  }
  free(stream);
}

static int
vectis_kore_response_stream_next(vectis_kore_response_stream *stream);

static int vectis_kore_response_stream_chunk_sent(struct netbuf *nb) {
  vectis_kore_response_stream *stream;
  struct connection *connection;
  int ret;

  stream = nb != NULL ? (vectis_kore_response_stream *)nb->extra : NULL;
  free(nb != NULL ? nb->buf : NULL);
  if (nb != NULL) {
    nb->buf = NULL;
  }
  if (stream == NULL || stream->connection == NULL || stream->remove) {
    ret = KORE_RESULT_ERROR;
  } else {
    ret = vectis_kore_response_stream_next(stream);
  }

  if (ret != KORE_RESULT_RETRY) {
    connection = stream != NULL ? stream->connection : NULL;
    if (connection != NULL) {
      if (connection->hdlr_extra == stream) {
        connection->hdlr_extra = NULL;
      }
      connection->disconnect = NULL;
      connection->flags &= ~CONN_IS_BUSY;
      if (ret == KORE_RESULT_OK && !stream->remove) {
        http_start_recv(connection);
        net_send_queue(connection, "0\r\n\r\n", 5u);
      } else if (!stream->remove) {
        kore_connection_disconnect(connection);
      }
    }
    vectis_kore_response_stream_free(stream);
  } else {
    ret = KORE_RESULT_OK;
  }
  return ret;
}

static int
vectis_kore_response_stream_next(vectis_kore_response_stream *stream) {
  unsigned char buffer[8192];
  char prefix[32];
  char *packet;
  struct netbuf *nb;
  lc_error lcerr;
  size_t nread;
  size_t prefix_len;
  size_t packet_size;
  int written;

  if (stream == NULL || stream->connection == NULL || stream->source == NULL ||
      stream->remove) {
    return KORE_RESULT_ERROR;
  }

  lc_error_init(&lcerr);
  nread = stream->source->read(stream->source, buffer, sizeof(buffer), &lcerr);
  if (nread == 0u) {
    if (lcerr.code != 0) {
      lc_error_cleanup(&lcerr);
      return KORE_RESULT_ERROR;
    }
    lc_error_cleanup(&lcerr);
    return KORE_RESULT_OK;
  }
  lc_error_cleanup(&lcerr);

  written =
      snprintf(prefix, sizeof(prefix), "%llx\r\n", (unsigned long long)nread);
  if (written <= 0 || (size_t)written >= sizeof(prefix)) {
    return KORE_RESULT_ERROR;
  }
  prefix_len = (size_t)written;
  packet_size = prefix_len + nread + 2u;
  packet = (char *)malloc(packet_size);
  if (packet == NULL) {
    return KORE_RESULT_ERROR;
  }
  memcpy(packet, prefix, prefix_len);
  memcpy(packet + prefix_len, buffer, nread);
  packet[prefix_len + nread] = '\r';
  packet[prefix_len + nread + 1u] = '\n';

  net_send_stream(stream->connection, packet, packet_size,
                  vectis_kore_response_stream_chunk_sent, &nb);
  nb->extra = stream;
  return KORE_RESULT_RETRY;
}

static void vectis_kore_response_stream_disconnect(struct connection *c) {
  vectis_kore_response_stream *stream;

  stream = c != NULL ? (vectis_kore_response_stream *)c->hdlr_extra : NULL;
  if (stream != NULL) {
    stream->remove = 1;
  }
  if (c != NULL) {
    c->hdlr_extra = NULL;
  }
}

static int vectis_kore_send_stream_response(struct http_request *req,
                                            int status,
                                            struct lc_source *source,
                                            int *emitted_status) {
  vectis_kore_response_stream *stream;
  lc_error lcerr;

  if (emitted_status != NULL) {
    *emitted_status = 500;
  }
  if (req == NULL || source == NULL || req->owner == NULL) {
    if (source != NULL) {
      lc_source_close(source);
    }
    return 0;
  }

  if (req->method == HTTP_METHOD_HEAD) {
    lc_source_close(source);
    http_response(req, status, NULL, 0);
    if (emitted_status != NULL) {
      *emitted_status = status;
    }
    return 1;
  }

  if (source->reset != NULL) {
    lc_error_init(&lcerr);
    if (source->reset(source, &lcerr) != LC_OK) {
      lc_error_cleanup(&lcerr);
      lc_source_close(source);
      http_response(req, 500, NULL, 0);
      if (emitted_status != NULL) {
        *emitted_status = 500;
      }
      return 1;
    }
    lc_error_cleanup(&lcerr);
  }

  stream = (vectis_kore_response_stream *)calloc(1u, sizeof(*stream));
  if (stream == NULL) {
    lc_source_close(source);
    http_response(req, 500, NULL, 0);
    if (emitted_status != NULL) {
      *emitted_status = 500;
    }
    return 1;
  }
  stream->connection = req->owner;
  stream->source = source;
  stream->connection->hdlr_extra = stream;
  stream->connection->flags |= CONN_IS_BUSY;
  stream->connection->disconnect = vectis_kore_response_stream_disconnect;

  req->flags |= HTTP_REQUEST_NO_CONTENT_LENGTH;
  http_response_header(req, "transfer-encoding", "chunked");
  http_response(req, status, NULL, 0);
  if (emitted_status != NULL) {
    *emitted_status = status;
  }
  {
    int stream_result;

    stream_result = vectis_kore_response_stream_next(stream);
    if (stream_result != KORE_RESULT_RETRY) {
      stream->connection->hdlr_extra = NULL;
      stream->connection->disconnect = NULL;
      stream->connection->flags &= ~CONN_IS_BUSY;
      if (stream_result == KORE_RESULT_OK) {
        net_send_queue(stream->connection, "0\r\n\r\n", 5u);
      } else {
        kore_connection_disconnect(stream->connection);
      }
      vectis_kore_response_stream_free(stream);
    }
  }
  return 1;
}

static void vectis_kore_send_response(vectis_app *app, struct http_request *req,
                                      vectis_response *response) {
  vectis_bytes body;
  const char *content_type;
  const char *file_path;
  struct lc_source *stream_source;
  struct kore_fileref *ref;
  struct stat st;
  struct timespec ts;
  int fd;
  int status;
  int emitted_status;
  size_t i;

  status = vectis_internal_response_status_code(response);
  if (status == 0) {
    status = 204;
  }
  for (i = 0u; i < vectis_internal_response_header_count(response); ++i) {
    http_response_header(req, vectis_internal_response_header_name(response, i),
                         vectis_internal_response_header_value(response, i));
  }
  content_type = vectis_internal_response_content_type(response);
  if (content_type != NULL) {
    http_response_header(req, "content-type", content_type);
  }
  stream_source = vectis_internal_response_take_stream_source(response);
  if (stream_source != NULL) {
    emitted_status = 500;
    if (!vectis_kore_send_stream_response(req, status, stream_source,
                                          &emitted_status)) {
      vectis_internal_metrics_note_http_status(app, 500);
      http_response(req, 500, NULL, 0);
    } else {
      vectis_internal_metrics_note_http_status(app, emitted_status);
    }
    return;
  }
  file_path = vectis_internal_response_file_path(response);
  if (file_path != NULL) {
    if (req->owner == NULL || req->owner->owner == NULL ||
        req->owner->owner->server == NULL) {
      vectis_internal_metrics_note_http_status(app, 500);
      http_response(req, 500, NULL, 0);
      return;
    }
    if (!vectis_internal_response_file_temporary(response)) {
      ref = kore_fileref_get(file_path, req->owner->owner->server->tls);
      if (ref != NULL) {
        vectis_internal_metrics_note_http_status(app, status);
        http_response_fileref(req, status, ref);
        return;
      }
    }
    fd = open(file_path, O_RDONLY);
    if (fd == -1 || fstat(fd, &st) != 0) {
      if (fd != -1) {
        (void)close(fd);
      }
      vectis_internal_metrics_note_http_status(app, 404);
      http_response(req, 404, NULL, 0);
      return;
    }
    ts.tv_sec = st.st_mtime;
    ts.tv_nsec = 0L;
    ref = kore_fileref_create(req->owner->owner->server, file_path, fd,
                              st.st_size, &ts);
    if (ref == NULL) {
      (void)close(fd);
      vectis_internal_metrics_note_http_status(app, 500);
      http_response(req, 500, NULL, 0);
      return;
    }
    if (vectis_internal_response_file_temporary(response)) {
      (void)unlink(file_path);
    }
    vectis_internal_metrics_note_http_status(app, status);
    http_response_fileref(req, status, ref);
    return;
  }
  body = vectis_internal_response_body(response);
  vectis_internal_metrics_note_http_status(app, status);
  http_response(req, status, body.data, body.size);
}

static int vectis_kore_websocket_opcode_valid(vectis_websocket_opcode opcode) {
  return opcode == VECTIS_WEBSOCKET_CONTINUATION ||
         opcode == VECTIS_WEBSOCKET_TEXT || opcode == VECTIS_WEBSOCKET_BINARY ||
         opcode == VECTIS_WEBSOCKET_CLOSE || opcode == VECTIS_WEBSOCKET_PING ||
         opcode == VECTIS_WEBSOCKET_PONG;
}

vectis_status vectis_websocket_send(vectis_websocket *websocket,
                                    vectis_websocket_opcode opcode,
                                    const void *data, size_t size,
                                    vectis_error *error) {
  if (websocket == NULL || websocket->connection == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "websocket connection is required");
    return VECTIS_ERR_INVALID;
  }
  if (!vectis_kore_websocket_opcode_valid(opcode)) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "websocket opcode is invalid");
    return VECTIS_ERR_INVALID;
  }
  if (data == NULL && size > 0u) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "websocket payload is required");
    return VECTIS_ERR_INVALID;
  }
  kore_websocket_send(websocket->connection, (u_int8_t)opcode, data, size);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_websocket_send_text(vectis_websocket *websocket,
                                         const char *text,
                                         vectis_error *error) {
  if (text == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID, "websocket text is required");
    return VECTIS_ERR_INVALID;
  }
  return vectis_websocket_send(websocket, VECTIS_WEBSOCKET_TEXT, text,
                               strlen(text), error);
}

vectis_status vectis_websocket_send_binary(vectis_websocket *websocket,
                                           const void *data, size_t size,
                                           vectis_error *error) {
  return vectis_websocket_send(websocket, VECTIS_WEBSOCKET_BINARY, data, size,
                               error);
}

vectis_status vectis_websocket_close(vectis_websocket *websocket,
                                     vectis_error *error) {
  return vectis_websocket_send(websocket, VECTIS_WEBSOCKET_CLOSE, NULL, 0u,
                               error);
}

void vectis_kore_ws_connect(struct connection *connection) {
  vectis_kore_websocket_state *state;
  vectis_websocket websocket;

  state = connection != NULL
              ? (vectis_kore_websocket_state *)connection->hdlr_extra
              : NULL;
  if (state == NULL || state->match.connect == NULL) {
    return;
  }
  websocket.connection = connection;
  state->match.connect(state->app, &websocket, state->match.userdata);
}

void vectis_kore_ws_message(struct connection *connection, u_int8_t opcode,
                            void *data, size_t len) {
  vectis_kore_websocket_state *state;
  vectis_websocket websocket;

  state = connection != NULL
              ? (vectis_kore_websocket_state *)connection->hdlr_extra
              : NULL;
  if (state == NULL || state->match.message == NULL) {
    return;
  }
  websocket.connection = connection;
  state->match.message(state->app, &websocket, (vectis_websocket_opcode)opcode,
                       data, len, state->match.userdata);
}

void vectis_kore_ws_disconnect(struct connection *connection) {
  vectis_kore_websocket_state *state;
  vectis_websocket websocket;

  state = connection != NULL
              ? (vectis_kore_websocket_state *)connection->hdlr_extra
              : NULL;
  if (state == NULL) {
    return;
  }
  websocket.connection = connection;
  if (state->match.disconnect != NULL) {
    state->match.disconnect(state->app, &websocket, state->match.userdata);
  }
  connection->hdlr_extra = NULL;
  kore_free(state);
}

int vectis_kore_route(struct http_request *req) {
  vectis_request *request;
  vectis_response *response;
  vectis_error error;
  vectis_app *app;
  vectis_kore_body_state *body_state;
  vectis_body_policy body_policy;
  vectis_internal_websocket_match websocket_match;
  vectis_kore_websocket_state *websocket_state;
  vectis_http_method method;
  vectis_status status;
  int error_status;
  int route_matched;

  vectis_error_clear(&error);
  error_status = 0;
  route_matched = 0;
  body_state = (vectis_kore_body_state *)req->hdlr_extra;
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
    vectis_internal_metrics_note_http_status(app, 500);
    http_response(req, 500, error.message, strlen(error.message));
    return KORE_RESULT_OK;
  }

  if (app == NULL || req == NULL || req->path == NULL) {
    vectis_internal_metrics_note_http_status(app, 503);
    http_response(req, 503, NULL, 0);
    vectis_internal_request_free(request);
    vectis_internal_response_free(response);
    return KORE_RESULT_OK;
  }
  if (!vectis_kore_app_ready()) {
    vectis_internal_metrics_note_http_status(app, 503);
    http_response(req, 503, "vectis app is starting\n", 23u);
    vectis_internal_request_free(request);
    vectis_internal_response_free(response);
    return KORE_RESULT_OK;
  }
  vectis_internal_request_set_kore(request, req);

  method = vectis_kore_method(req->method);
  if (body_state != NULL && body_state->streaming) {
    vectis_internal_request_free(request);
    request = body_state->request;
    body_state->request = NULL;
    status = vectis_internal_upload_stream_finish(app, request, response,
                                                  &body_state->stream, &error);
    route_matched = 1;
    if (status == VECTIS_OK) {
      vectis_kore_send_response(app, req, response);
    } else if (status == VECTIS_ERR_INVALID) {
      vectis_internal_metrics_note_http_status(app, 400);
      http_response(req, 400, error.message, strlen(error.message));
    } else if (status == VECTIS_ERR_NOT_IMPLEMENTED) {
      vectis_internal_metrics_note_http_status(app, 501);
      http_response(req, 501, error.message, strlen(error.message));
    } else {
      vectis_internal_metrics_note_http_status(app, 500);
      http_response(req, 500, error.message, strlen(error.message));
    }
    vectis_internal_upload_stream_close(app, request, &body_state->stream);
    vectis_internal_response_free(response);
    vectis_internal_request_free(request);
    vectis_kore_body_state_cleanup(body_state);
    return KORE_RESULT_OK;
  }
  vectis_internal_request_set_method(request, method);
  status = vectis_kore_copy_request_metadata(req, request, &error);
  if (status == VECTIS_OK) {
    status = vectis_internal_match_websocket(app, method, req->path, request,
                                             &websocket_match, &error);
    if (status == VECTIS_OK) {
      websocket_state = (vectis_kore_websocket_state *)kore_calloc(
          1u, sizeof(*websocket_state));
      websocket_state->app = app;
      websocket_state->match = websocket_match;
      req->owner->hdlr_extra = websocket_state;
      vectis_internal_metrics_note_http_status(app, 101);
      kore_websocket_handshake(req, "vectis_kore_ws_connect",
                               "vectis_kore_ws_message",
                               "vectis_kore_ws_disconnect");
      vectis_internal_request_free(request);
      vectis_internal_response_free(response);
      return KORE_RESULT_OK;
    }
    if (status != VECTIS_ERR_STATE) {
      if (status == VECTIS_ERR_INVALID) {
        vectis_internal_metrics_note_http_status(app, 400);
        http_response(req, 400, error.message, strlen(error.message));
      } else {
        vectis_internal_metrics_note_http_status(app, 500);
        http_response(req, 500, error.message, strlen(error.message));
      }
      vectis_internal_request_free(request);
      vectis_internal_response_free(response);
      return KORE_RESULT_OK;
    }
    status = VECTIS_OK;
    vectis_error_clear(&error);
  }
  if (status == VECTIS_OK) {
    status = vectis_internal_route_body_policy(app, method, req->path,
                                               &body_policy, &error);
    if (status == VECTIS_OK) {
      route_matched = 1;
    }
  }
  if (status == VECTIS_OK && body_policy.mode == VECTIS_BODY_STREAMING_UPLOAD &&
      req->http_body_length == 0u) {
    vectis_upload_stream_runtime stream;

    memset(&stream, 0, sizeof(stream));
    status = vectis_internal_upload_stream_open(app, method, req->path, request,
                                                &stream, &error);
    if (status == VECTIS_OK) {
      status = vectis_internal_upload_stream_finish(app, request, response,
                                                    &stream, &error);
      if (status == VECTIS_OK) {
        vectis_kore_send_response(app, req, response);
      } else if (status == VECTIS_ERR_INVALID) {
        vectis_internal_metrics_note_http_status(app, 400);
        http_response(req, 400, error.message, strlen(error.message));
      } else if (status == VECTIS_ERR_NOT_IMPLEMENTED) {
        vectis_internal_metrics_note_http_status(app, 501);
        http_response(req, 501, error.message, strlen(error.message));
      } else {
        vectis_internal_metrics_note_http_status(app, 500);
        http_response(req, 500, error.message, strlen(error.message));
      }
      vectis_internal_upload_stream_close(app, request, &stream);
      vectis_internal_request_free(request);
      vectis_internal_response_free(response);
      return KORE_RESULT_OK;
    }
    if (status != VECTIS_ERR_STATE) {
      if (status == VECTIS_ERR_INVALID) {
        vectis_internal_metrics_note_http_status(app, 400);
        http_response(req, 400, error.message, strlen(error.message));
      } else {
        vectis_internal_metrics_note_http_status(app, 500);
        http_response(req, 500, error.message, strlen(error.message));
      }
      vectis_internal_request_free(request);
      vectis_internal_response_free(response);
      return KORE_RESULT_OK;
    }
    status = VECTIS_OK;
    vectis_error_clear(&error);
  }
  if (status == VECTIS_OK) {
    status = vectis_kore_attach_streamed_body(req, &body_policy, request,
                                              &error_status, &error);
  }
  if (status == VECTIS_OK) {
    status = vectis_internal_dispatch_route(app, method, req->path, request,
                                            response, &error);
  }

  if (status == VECTIS_OK) {
    vectis_kore_send_response(app, req, response);
  } else if (error_status != 0) {
    vectis_internal_metrics_note_http_status(app, error_status);
    http_response(req, error_status, error.message, strlen(error.message));
  } else if (status == VECTIS_ERR_INVALID) {
    vectis_internal_metrics_note_http_status(app, 400);
    http_response(req, 400, error.message, strlen(error.message));
  } else if (status == VECTIS_ERR_STATE && !route_matched) {
    vectis_internal_metrics_note_http_status(app, 404);
    http_response(req, 404, NULL, 0);
  } else if (status == VECTIS_ERR_NOT_IMPLEMENTED) {
    vectis_internal_metrics_note_http_status(app, 501);
    http_response(req, 501, error.message, strlen(error.message));
  } else {
    vectis_internal_metrics_note_http_status(app, 500);
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
  size_t domain_count;
  size_t i;
  const char *domain_name;

  (void)argc;
  (void)argv;

  (void)pthread_mutex_lock(&vectis_kore_mutex);
  if (vectis_kore_current.logger != NULL) {
    kore_log_set_logger(vectis_kore_current.logger);
  }
  vectis_kore_apply_server_config(
      &vectis_kore_current.server, vectis_kore_current.body_disk_offload_bytes,
      vectis_kore_current.body_disk_offload_configured);
  vectis_kore_apply_tls_config(&vectis_kore_current);
  vectis_kore_clear_installed_acme_roots();
  if (vectis_kore_current.tls_mode == VECTIS_TLS_MODE_ACME &&
      vectis_kore_current.acme_state_dir != NULL) {
    keymgr_privsep.root = kore_strdup(vectis_kore_current.acme_state_dir);
    acme_privsep.root = kore_strdup(vectis_kore_current.acme_state_dir);
    vectis_kore_keymgr_root = keymgr_privsep.root;
    vectis_kore_acme_root = acme_privsep.root;
  }
  server = kore_server_create(vectis_kore_current.app_name != NULL
                                  ? vectis_kore_current.app_name
                                  : "vectis");
  if (vectis_kore_current.tls_mode == VECTIS_TLS_MODE_DISABLED) {
    server->tls = 0;
  }
  (void)snprintf(port, sizeof(port), "%u", (unsigned)vectis_kore_current.port);
  if (!kore_server_bind(server,
                        vectis_kore_current.bind != NULL
                            ? vectis_kore_current.bind
                            : "0.0.0.0",
                        port, NULL)) {
    fatal("failed to bind Vectis Kore listener");
  }
  acme_domains = 0;
  if (server->tls && vectis_kore_current.tls_mode == VECTIS_TLS_MODE_ACME) {
    kore_free(acme_email);
    acme_email = kore_strdup(vectis_kore_current.acme_email);
    kore_free(acme_provider);
    acme_provider =
        kore_strdup(vectis_kore_current.acme_directory_url != NULL
                        ? vectis_kore_current.acme_directory_url
                        : VECTIS_ACME_DIRECTORY_LETSENCRYPT_PRODUCTION);
  }
  domain_count =
      server->tls && vectis_kore_current.tls_mode == VECTIS_TLS_MODE_ACME
          ? vectis_kore_current.domain_count
          : 1u;
  for (i = 0u; i < domain_count; ++i) {
    domain_name =
        server->tls && vectis_kore_current.tls_mode == VECTIS_TLS_MODE_ACME
            ? vectis_kore_current.domains[i]
            : (vectis_kore_current.domain != NULL ? vectis_kore_current.domain
                                                  : "*");
    domain = kore_domain_new(domain_name);
    if (server->tls) {
      if (vectis_kore_current.tls_mode == VECTIS_TLS_MODE_ACME) {
        domain->acme = 1;
        kore_free(domain->certfile);
        kore_free(domain->certkey);
        kore_acme_get_paths(domain->domain, &domain->certkey,
                            &domain->certfile);
        acme_domains++;
      } else {
        domain->certfile = kore_strdup(vectis_kore_current.runtime_certfile);
        domain->certkey = kore_strdup(vectis_kore_current.runtime_certkey);
      }
      if (vectis_kore_current.require_client_certificate &&
          vectis_kore_current.runtime_client_ca_file != NULL) {
        domain->cafile =
            kore_strdup(vectis_kore_current.runtime_client_ca_file);
      }
      if (vectis_kore_current.tls_mode != VECTIS_TLS_MODE_ACME) {
        vectis_kore_setup_domain_tls(domain);
      }
    }
    if (vectis_kore_current.server.access_log_path != NULL &&
        vectis_kore_current.server.access_log_path[0] != '\0') {
      domain->accesslog = open(vectis_kore_current.server.access_log_path,
                               O_CREAT | O_APPEND | O_WRONLY, 0644);
      if (domain->accesslog == -1) {
        fatal("failed to open Vectis Kore access log");
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
  }
  kore_server_finalize(server);
  vectis_kore_notify_ready();
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
}

void kore_parent_teardown(void) {
  (void)pthread_mutex_lock(&vectis_kore_mutex);
  vectis_kore_autoblock_unmap();
  vectis_kore_clear_installed_acme_roots();
  vectis_kore_clear_installed_body_disk_path();
  vectis_kore_cleanup_config(&vectis_kore_current);
  memset(&vectis_kore_current, 0, sizeof(vectis_kore_current));
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
}

vectis_status vectis_internal_kore_run(const vectis_kore_runtime_config *config,
                                       vectis_error *error) {
  vectis_kore_runtime_config prepared;
  vectis_status status;
  int result;

  if (config == NULL || config->app == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "Kore runtime config is required");
    return VECTIS_ERR_INVALID;
  }
  prepared = *config;
  prepared.runtime_certfile = NULL;
  prepared.runtime_certkey = NULL;
  prepared.runtime_client_ca_file = NULL;
  prepared.runtime_certfile_temporary = 0;
  prepared.runtime_certkey_temporary = 0;
  prepared.runtime_client_ca_temporary = 0;
  status = vectis_kore_preflight_listener(&prepared, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_kore_preflight_body_spool(&prepared, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_kore_preflight_access_log(&prepared, error);
  if (status != VECTIS_OK) {
    return status;
  }
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
  if (prepared.server.autoblock.enabled &&
      !vectis_kore_autoblock_map(prepared.server.autoblock.max_entries)) {
    vectis_kore_cleanup_local_config(&prepared);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "failed to create Kore autoblock state");
    return VECTIS_ERR_STATE;
  }

  (void)pthread_mutex_lock(&vectis_kore_mutex);
  if (vectis_kore_runtime_active) {
    (void)pthread_mutex_unlock(&vectis_kore_mutex);
    vectis_kore_autoblock_unmap();
    vectis_kore_cleanup_local_config(&prepared);
    vectis_set_error(error, VECTIS_ERR_STATE,
                     "Kore runtime is already running");
    return VECTIS_ERR_STATE;
  }
  vectis_kore_reset_runtime_state();
  vectis_kore_current = prepared;
  vectis_kore_runtime_active = 1;
  (void)pthread_mutex_unlock(&vectis_kore_mutex);

  result = vectis_kore_run_main();

  (void)pthread_mutex_lock(&vectis_kore_mutex);
  vectis_kore_runtime_active = 0;
  if (vectis_kore_current.app != NULL) {
    vectis_kore_autoblock_unmap();
    vectis_kore_clear_installed_body_disk_path();
    vectis_kore_cleanup_config(&vectis_kore_current);
    memset(&vectis_kore_current, 0, sizeof(vectis_kore_current));
  }
  (void)pthread_mutex_unlock(&vectis_kore_mutex);
  if (result == KORE_QUIT_FATAL) {
    vectis_set_error(error, VECTIS_ERR_STATE, "Kore runtime failed");
    return VECTIS_ERR_STATE;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status
vectis_internal_kore_validate(const vectis_kore_runtime_config *config,
                              vectis_error *error) {
  vectis_kore_runtime_config prepared;
  vectis_status status;

  if (config == NULL || config->app == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "Kore runtime config is required");
    return VECTIS_ERR_INVALID;
  }
  prepared = *config;
  prepared.runtime_certfile = NULL;
  prepared.runtime_certkey = NULL;
  prepared.runtime_client_ca_file = NULL;
  prepared.runtime_certfile_temporary = 0;
  prepared.runtime_certkey_temporary = 0;
  prepared.runtime_client_ca_temporary = 0;
  status = vectis_kore_preflight_listener(&prepared, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_kore_preflight_body_spool(&prepared, error);
  if (status != VECTIS_OK) {
    return status;
  }
  status = vectis_kore_preflight_access_log(&prepared, error);
  if (status != VECTIS_OK) {
    return status;
  }
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
  vectis_kore_cleanup_local_config(&prepared);
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_status vectis_internal_kore_stop(vectis_app *app, vectis_error *error) {
  int active;

  (void)pthread_mutex_lock(&vectis_kore_mutex);
  active = vectis_kore_runtime_active && vectis_kore_current.app == app;
  if (active) {
    kore_quit = KORE_QUIT_NORMAL;
    kore_signal(SIGTERM);
    vectis_kore_wake_listener();
  }
  (void)pthread_mutex_unlock(&vectis_kore_mutex);

  if (!active) {
    vectis_error_clear(error);
    return VECTIS_OK;
  }

  vectis_error_clear(error);
  return VECTIS_OK;
}

int vectis_internal_kore_signal_requested(void) {
  return sig_recv == SIGINT || sig_recv == SIGTERM || sig_recv == SIGQUIT;
}

int vectis_internal_kore_signal_number(void) {
  if (vectis_internal_kore_signal_requested()) {
    return (int)sig_recv;
  }
  return 0;
}
