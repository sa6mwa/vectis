#include <assert.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <kore/http.h>
#include <kore/kore.h>
#pragma GCC diagnostic pop

static unsigned disconnects;

static void on_disconnect(struct connection *connection) {
  (void)connection;
  ++disconnects;
}

int main(void) {
  struct connection connection;
  struct netbuf pending;

  memset(&connection, 0, sizeof(connection));
  memset(&pending, 0, sizeof(pending));
  TAILQ_INIT(&connections);
  TAILQ_INIT(&disconnected);
  TAILQ_INIT(&connection.send_queue);
  TAILQ_INSERT_TAIL(&connections, &connection, list);
  TAILQ_INSERT_TAIL(&connection.send_queue, &pending, list);
  connection.state = CONN_STATE_ESTABLISHED;
  connection.proto = CONN_PROTO_HTTP;
  connection.http_response_count = 1u;
  connection.http_send_start = 101u;
  connection.disconnect = on_disconnect;
  http_response_write_timeout = 15u;

  /* Kore samples time before callbacks that can queue a response. */
  kore_connection_check_timeout(100u);
  assert(disconnects == 0u);
  assert(connection.state == CONN_STATE_ESTABLISHED);

  kore_connection_check_timeout(115u);
  assert(disconnects == 0u);
  kore_connection_check_timeout(116u);
  assert(disconnects == 1u);
  assert(connection.state == CONN_STATE_DISCONNECTING);
  return 0;
}
