#include "vectis_kore_proxy_local.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <kore/http.h>
#include <kore/kore.h>
#pragma GCC diagnostic pop

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static size_t queue_count(struct connection *connection) {
  struct netbuf *buffer;
  size_t count;

  count = 0u;
  TAILQ_FOREACH(buffer, &connection->send_queue, list) { count++; }
  return count;
}

int main(void) {
  static const char provisional[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  static const char gateway[] = "bad gateway";
  struct connection connection;
  struct http_request request;
  struct netbuf *buffer;
  vectis_proxy_local_response local;
  vectis_error error;
  char *large;

  kore_mem_init();
  net_init();
  memset(&connection, 0, sizeof(connection));
  TAILQ_INIT(&connection.send_queue);
  assert(vectis_kore_proxy_drop_unwritten(&connection) == 0);

  large = (char *)malloc(NETBUF_SEND_PAYLOAD_MAX);
  assert(large != NULL);
  memset(large, 'x', NETBUF_SEND_PAYLOAD_MAX);
  net_send_queue(&connection, provisional, sizeof(provisional) - 1u);
  net_send_queue(&connection, large, NETBUF_SEND_PAYLOAD_MAX);
  assert(queue_count(&connection) == 2u);
  assert(vectis_kore_proxy_drop_unwritten(&connection) == 1);
  assert(queue_count(&connection) == 0u);
  free(large);

  net_send_queue(&connection, provisional, sizeof(provisional) - 1u);
  buffer = TAILQ_FIRST(&connection.send_queue);
  assert(buffer != NULL);
  connection.snb = buffer;
  assert(vectis_kore_proxy_drop_unwritten(&connection) == 0);
  assert(queue_count(&connection) == 1u);
  connection.snb = NULL;
  buffer->s_off = 1u;
  assert(vectis_kore_proxy_drop_unwritten(&connection) == 0);
  buffer->s_off = 0u;
  buffer->flags |= NETBUF_MUST_RESEND;
  assert(vectis_kore_proxy_drop_unwritten(&connection) == 0);
  buffer->flags &= ~NETBUF_MUST_RESEND;
  buffer->flags |= NETBUF_IS_STREAM;
  assert(vectis_kore_proxy_drop_unwritten(&connection) == 0);
  buffer->flags &= ~NETBUF_IS_STREAM;
  assert(vectis_kore_proxy_drop_unwritten(&connection) == 1);

  memset(&request, 0, sizeof(request));
  request.owner = &connection;
  request.method = HTTP_METHOD_GET;
  vectis_proxy_local_init(&local);
  assert(vectis_proxy_local_respond(&local, 502, gateway, sizeof(gateway) - 1u,
                                    &error) == VECTIS_OK);
  assert(vectis_kore_proxy_local_send(&request, NULL, &local) == 1);
  assert(queue_count(&connection) == 1u);
  buffer = TAILQ_FIRST(&connection.send_queue);
  assert(buffer != NULL);
  assert(buffer->b_len > sizeof(gateway) - 1u);
  assert(memcmp(buffer->buf, "HTTP/1.1 502 Bad Gateway\r\n", 26u) == 0);
  assert(memcmp(buffer->buf + buffer->b_len - sizeof(gateway) + 1u, gateway,
                sizeof(gateway) - 1u) == 0);
  assert(request.status == 502);
  assert(connection.http_response_count == 1u);
  assert((connection.flags & CONN_CLOSE_EMPTY) != 0);
  assert(vectis_kore_proxy_drop_unwritten(&connection) == 1);
  vectis_proxy_local_cleanup(&local);

  net_cleanup();
  kore_mem_cleanup();
  return 0;
}
