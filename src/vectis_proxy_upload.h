#ifndef VECTIS_PROXY_UPLOAD_H
#define VECTIS_PROXY_UPLOAD_H

#include "vectis_proxy_framing.h"
#include "vectis_proxy_headers.h"

#include <curl/curl.h>
#include <vectis/vectis.h>

/* One bounded request-body chunk and its incremental framing state. No body
 * byte is consumed unless it fits this queue. Request trailers are retained
 * separately under the framer's bounded trailer limit. */
typedef struct vectis_proxy_upload_buffer {
  vectis_proxy_body_framer framer;
  vectis_proxy_headers declared;
  vectis_proxy_headers trailers;
  unsigned char *bytes;
  size_t capacity;
  size_t begin;
  size_t end;
  void (*on_consume)(void *userdata, size_t amount);
  void *consume_userdata;
  int complete;
  int curl_paused;
} vectis_proxy_upload_buffer;

vectis_status
vectis_proxy_upload_init(vectis_proxy_upload_buffer *upload, size_t capacity,
                         int chunked, uint64_t content_length,
                         const vectis_proxy_headers *request_headers,
                         vectis_error *error);
void vectis_proxy_upload_cleanup(vectis_proxy_upload_buffer *upload);

/* consumed reports exactly the prefix accepted from this input span. A
 * PAUSED result leaves the remaining bytes with the caller until resume. */
vectis_proxy_frame_result
vectis_proxy_upload_feed(vectis_proxy_upload_buffer *upload,
                         const unsigned char *data, size_t length,
                         size_t *consumed);

/* libcurl read callback. Returns CURL_READFUNC_PAUSE while waiting for input,
 * and zero only after the framer has completed and the queue has drained. */
size_t vectis_proxy_upload_read(char *buffer, size_t size, size_t count,
                                void *userdata);
int vectis_proxy_upload_full(const vectis_proxy_upload_buffer *upload);
int vectis_proxy_upload_can_resume(const vectis_proxy_upload_buffer *upload);
void vectis_proxy_upload_resumed(vectis_proxy_upload_buffer *upload);

/* Called by libcurl only after upload_read has returned EOF. The returned
 * list is owned and freed by libcurl. */
int vectis_proxy_upload_curl_trailers(struct curl_slist **list, void *userdata);

#endif
