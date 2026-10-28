#ifndef VECTIS_CURL_H
#define VECTIS_CURL_H
#include <curl/curl.h>
#include <vectis/vectis.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VECTIS_CURL_BODY_LIMIT 8388608u
#define VECTIS_CURL_HEADER_LIMIT 65536u
#define VECTIS_CURL_CHUNK_LIMIT 65536u
#define VECTIS_CURL_MULTI_LIMIT 64u
typedef struct vectis_curl_session vectis_curl_session;
typedef struct vectis_curl_multi vectis_curl_multi;
typedef struct vectis_curl_progress {
  curl_off_t download_total, downloaded, upload_total, uploaded;
} vectis_curl_progress;
/* Hooks are synchronous on the invoking owner thread and must not longjmp.
 * configure may use public curl options but must not replace engine callbacks,
 * private data or error buffers. Strings/options must remain alive until the
 * session's next perform/reset/close, or multi completion consumption/close.
 * Reader fills <=capacity; zero bytes means EOF. Failure is distinct from EOF.
 * Hooks and their caller-owned context are copied, never transferred. */
typedef struct vectis_curl_request {
  vectis_status (*configure)(CURL *, void *, vectis_error *);
  vectis_status (*read)(void *, void *, size_t, size_t *, vectis_error *);
  vectis_status (*write)(void *, const void *, size_t, vectis_error *);
  vectis_status (*progress)(void *, const vectis_curl_progress *, int *,
                            vectis_error *);
  void *context;
} vectis_curl_request;
typedef struct vectis_curl_result {
  unsigned long id;
  CURLcode code;
  long status;
  int streamed;
  const void *body, *headers;
  size_t body_size, header_size;
  const char *effective_url, *message;
  vectis_error failure;
  void *context;
} vectis_curl_result;
/* Results are borrowed until perform/reset/close or multi consume/close.
 * Transport/callback failure lives in result.code/failure; method failure
 * means the operation could not be initiated/advanced. No implicit retries. */
struct vectis_curl_session {
  vectis_status (*perform)(vectis_curl_session *, const vectis_curl_request *,
                           const vectis_curl_result **, vectis_error *);
  vectis_status (*reset)(vectis_curl_session *, vectis_error *);
  vectis_status (*close)(vectis_curl_session *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
struct vectis_curl_multi {
  vectis_status (*add)(vectis_curl_multi *, const vectis_curl_request *,
                       unsigned long *, vectis_error *);
  vectis_status (*step)(vectis_curl_multi *, size_t *, vectis_error *);
  vectis_status (*poll)(vectis_curl_multi *, int, int *, vectis_error *);
  vectis_status (*completion)(vectis_curl_multi *, size_t,
                              const vectis_curl_result **, vectis_error *);
  vectis_status (*consume)(vectis_curl_multi *, size_t, vectis_error *);
  vectis_status (*close)(vectis_curl_multi *, vectis_error *);
  void *impl;
  vectis_receiver_reserved_fn reserved[VECTIS_RECEIVER_RESERVED_SLOTS];
};
vectis_status vectis_curl_session_new(vectis_curl_session **, vectis_error *);
vectis_status vectis_curl_session_perform(vectis_curl_session *,
                                          const vectis_curl_request *,
                                          const vectis_curl_result **,
                                          vectis_error *);
vectis_status vectis_curl_session_reset(vectis_curl_session *, vectis_error *);
vectis_status vectis_curl_session_close(vectis_curl_session *, vectis_error *);
vectis_status vectis_curl_multi_new(vectis_curl_multi **, vectis_error *);
vectis_status vectis_curl_multi_add(vectis_curl_multi *,
                                    const vectis_curl_request *,
                                    unsigned long *, vectis_error *);
vectis_status vectis_curl_multi_step(vectis_curl_multi *, size_t *,
                                     vectis_error *);
vectis_status vectis_curl_multi_poll(vectis_curl_multi *, int, int *,
                                     vectis_error *);
/* completion offset is zero-based; NULL output means no more queued results.
 * consume acknowledges a prefix only after successful caller-owned copying. */
vectis_status vectis_curl_multi_completion(vectis_curl_multi *, size_t,
                                           const vectis_curl_result **,
                                           vectis_error *);
vectis_status vectis_curl_multi_consume(vectis_curl_multi *, size_t,
                                        vectis_error *);
vectis_status vectis_curl_multi_close(vectis_curl_multi *, vectis_error *);
#ifdef __cplusplus
}
#endif
#endif
