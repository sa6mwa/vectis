#ifndef VECTIS_PROXY_HEADERS_H
#define VECTIS_PROXY_HEADERS_H

#include <stddef.h>
#include <stdint.h>

#define VECTIS_PROXY_HEADER_COUNT_LIMIT 100u
#define VECTIS_PROXY_HEADER_BLOCK_LIMIT 65536u

typedef enum vectis_proxy_header_status {
  VECTIS_PROXY_HEADER_OK = 0,
  VECTIS_PROXY_HEADER_INVALID = 1,
  VECTIS_PROXY_HEADER_LIMIT = 2,
  VECTIS_PROXY_HEADER_NOMEM = 3,
  VECTIS_PROXY_HEADER_UNSUPPORTED_UPGRADE = 4
} vectis_proxy_header_status;

typedef struct vectis_proxy_header {
  char *name;
  char *value;
} vectis_proxy_header;

/* A bounded, owned copy of metadata. Bodies never enter this container. */
typedef struct vectis_proxy_headers {
  vectis_proxy_header fields[VECTIS_PROXY_HEADER_COUNT_LIMIT];
  size_t count;
  size_t bytes;
} vectis_proxy_headers;

typedef struct vectis_proxy_request_head {
  const char *host; /* Borrowed from the input headers. */
  uint64_t content_length;
  int has_content_length;
  int chunked;
  int expect_continue;
  int websocket_upgrade;
  int has_trailers;
} vectis_proxy_request_head;

void vectis_proxy_headers_init(vectis_proxy_headers *headers);
void vectis_proxy_headers_cleanup(vectis_proxy_headers *headers);
vectis_proxy_header_status
vectis_proxy_headers_add(vectis_proxy_headers *headers, const char *name,
                         const char *value);

/* Validate request framing and upgrade metadata before any upstream connect.
 * The returned head borrows from headers until headers_cleanup(). */
vectis_proxy_header_status
vectis_proxy_request_head_parse(const vectis_proxy_headers *headers,
                                vectis_proxy_request_head *head,
                                const char **reason);

/* Copy end-to-end fields into an initially empty destination. Host, framing,
 * forwarding, and connection-nominated fields are rebuilt by the transport. */
vectis_proxy_header_status
vectis_proxy_headers_sanitize_request(const vectis_proxy_headers *source,
                                      vectis_proxy_headers *destination);

/* Copy upstream end-to-end response fields into an empty destination.
 * The response writer reconstructs framing and Connection fields. */
vectis_proxy_header_status
vectis_proxy_headers_sanitize_response(const vectis_proxy_headers *source,
                                       vectis_proxy_headers *destination);

/* Only a declared, otherwise permitted request trailer may be forwarded. */
int vectis_proxy_request_trailer_declared(const vectis_proxy_headers *headers,
                                          const char *name);

#endif
