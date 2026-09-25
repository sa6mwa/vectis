#include "vectis_kore_proxy_local.h"

#include "vectis_internal.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <kore/http.h>
#include <kore/kore.h>
#pragma GCC diagnostic pop

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int vectis_kore_proxy_drop_unwritten(struct connection *connection) {
  struct netbuf *buffer;

  if (connection == NULL || connection->snb != NULL ||
      TAILQ_EMPTY(&connection->send_queue))
    return 0;
  TAILQ_FOREACH(buffer, &connection->send_queue, list) {
    if (buffer->s_off != 0u ||
        (buffer->flags & (NETBUF_MUST_RESEND | NETBUF_IS_STREAM)) != 0)
      return 0;
  }
  while ((buffer = TAILQ_FIRST(&connection->send_queue)) != NULL)
    net_remove_netbuf(connection, buffer);
  return 1;
}

int vectis_kore_proxy_local_send(struct http_request *request, vectis_app *app,
                                 const vectis_proxy_local_response *response) {
  struct connection *connection;
  const char *reason;
  char *head;
  size_t capacity;
  size_t used;
  size_t i;
  int count;

  if (request == NULL || response == NULL || response->status == 0)
    return 0;
  capacity = 128u + response->headers.bytes + response->headers.count * 4u;
  head = (char *)malloc(capacity);
  if (head == NULL)
    return 0;
  reason = response->status == 504 ? "Gateway Timeout"
                                   : http_status_text(response->status);
  count = snprintf(head, capacity, "HTTP/1.1 %d %s\r\nConnection: close\r\n",
                   response->status, reason);
  if (count < 0 || (size_t)count >= capacity)
    goto failed;
  used = (size_t)count;
  for (i = 0u; i < response->headers.count; ++i) {
    count = snprintf(head + used, capacity - used, "%s: %s\r\n",
                     response->headers.fields[i].name,
                     response->headers.fields[i].value);
    if (count < 0 || (size_t)count >= capacity - used)
      goto failed;
    used += (size_t)count;
  }
  if (response->status != 204 && response->status != 304) {
    count = snprintf(head + used, capacity - used, "Content-Length: %lu\r\n",
                     (unsigned long)response->body_length);
    if (count < 0 || (size_t)count >= capacity - used)
      goto failed;
    used += (size_t)count;
  }
  if (capacity - used < 2u)
    goto failed;
  memcpy(head + used, "\r\n", 2u);
  used += 2u;
  connection = request->owner;
  connection->flags |= CONN_CLOSE_EMPTY;
  net_send_queue(connection, head, used);
  if (request->method != HTTP_METHOD_HEAD && response->body_length != 0u)
    net_send_queue(connection, response->body, response->body_length);
  request->status = (u_int16_t)response->status;
  connection->http_response_count++;
  vectis_internal_metrics_note_http_status(app, response->status);
  free(head);
  return 1;
failed:
  free(head);
  return 0;
}
