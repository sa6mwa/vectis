#include <arpa/inet.h>
#include <assert.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <vectis/proxy.h>
#include <vectis/vectis.h>

static vectis_status ordinary(vectis_app *app, vectis_request *request,
                              vectis_response *response, void *userdata,
                              vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ordinary", error);
}

static vectis_status proxy_reply(const vectis_proxy_inbound *inbound,
                                 vectis_proxy_local_response *response,
                                 void *userdata, vectis_error *error) {
  const char *path;
  const char *id;

  (void)userdata;
  path = vectis_proxy_inbound_path(inbound);
  if (strncmp(path, "/overlap/raw/", 13u) == 0) {
    id = vectis_proxy_inbound_path_param(inbound, "id");
    assert(id != NULL && strcmp(id, "a%2Fb") == 0);
  }
  return vectis_proxy_local_respond(response, 200, "proxy", 5u, error);
}

static vectis_status upload_open(vectis_app *app, vectis_request *request,
                                 void *userdata, void **state,
                                 vectis_error *error) {
  size_t *count;

  (void)app;
  (void)request;
  (void)userdata;
  count = (size_t *)calloc(1u, sizeof(*count));
  if (count == NULL)
    return VECTIS_ERR_NOMEM;
  *state = count;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status upload_write(vectis_app *app, vectis_request *request,
                                  const void *data, size_t size, void *state,
                                  void *userdata, vectis_error *error) {
  static const char expected[] = "data";
  size_t *count;

  (void)app;
  (void)request;
  (void)userdata;
  count = (size_t *)state;
  if (*count > 4u || size > 4u - *count ||
      memcmp(data, expected + *count, size) != 0)
    return VECTIS_ERR_INVALID;
  *count += size;
  vectis_error_clear(error);
  return VECTIS_OK;
}

static vectis_status upload_finish(vectis_app *app, vectis_request *request,
                                   vectis_response *response, void *state,
                                   void *userdata, vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  assert(*(size_t *)state == 4u);
  return vectis_response_text(response, 200, "text/plain", "upload", error);
}

static void upload_close(vectis_app *app, vectis_request *request, void *state,
                         void *userdata) {
  (void)app;
  (void)request;
  (void)userdata;
  free(state);
}

static void ws_message(vectis_app *app, vectis_websocket *websocket,
                       vectis_websocket_opcode opcode, const void *data,
                       size_t size, void *userdata) {
  (void)app;
  (void)websocket;
  (void)opcode;
  (void)data;
  (void)size;
  (void)userdata;
}

static unsigned short available_port(void) {
  struct sockaddr_in address;
  socklen_t length;
  unsigned short port;
  int fd;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  length = sizeof(address);
  assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
  port = ntohs(address.sin_port);
  assert(close(fd) == 0);
  return port;
}

static int connect_app(unsigned short port) {
  struct sockaddr_in address;
  struct timeval timeout;
  int attempt;
  int fd;

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  for (attempt = 0; attempt < 100; ++attempt) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
      break;
    assert(close(fd) == 0);
    usleep(10000u);
  }
  assert(attempt < 100);
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  return fd;
}

static void request_once(unsigned short port, const char *wire, int status,
                         const char *body) {
  char response[4096];
  const char *end;
  size_t used;
  size_t offset;
  ssize_t amount;
  int fd;
  int got_status;

  fd = connect_app(port);
  for (offset = 0u; offset < strlen(wire); offset += (size_t)amount) {
    amount = send(fd, wire + offset, strlen(wire) - offset, 0);
    assert(amount > 0);
  }
  used = 0u;
  response[0] = '\0';
  for (;;) {
    assert(used < sizeof(response) - 1u);
    amount = recv(fd, response + used, sizeof(response) - 1u - used, 0);
    assert(amount >= 0);
    if (amount == 0)
      break;
    used += (size_t)amount;
    response[used] = '\0';
    end = strstr(response, "\r\n\r\n");
    if (end != NULL && (body == NULL || strstr(end + 4u, body) != NULL))
      break;
  }
  assert(sscanf(response, "HTTP/1.1 %d", &got_status) == 1);
  assert(got_status == status);
  end = strstr(response, "\r\n\r\n");
  assert(end != NULL);
  if (body != NULL && strstr(end + 4u, body) == NULL)
    fprintf(stderr, "overlap request %.80s expected %s; response %.160s\n",
            wire, body, response);
  if (body != NULL)
    assert(strstr(end + 4u, body) != NULL);
  assert(close(fd) == 0);
}

static void request_ws(unsigned short port) {
  static const char wire[] = "GET /overlap/ws HTTP/1.1\r\nHost: localhost\r\n"
                             "Connection: Upgrade\r\nUpgrade: websocket\r\n"
                             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                             "Sec-WebSocket-Version: 13\r\n\r\n";
  char response[1024];
  size_t used;
  ssize_t amount;
  int fd;

  fd = connect_app(port);
  assert(send(fd, wire, sizeof(wire) - 1u, 0) == (ssize_t)(sizeof(wire) - 1u));
  used = 0u;
  do {
    amount = recv(fd, response + used, sizeof(response) - 1u - used, 0);
    assert(amount > 0);
    used += (size_t)amount;
    response[used] = '\0';
    assert(used < sizeof(response) - 1u);
  } while (strstr(response, "\r\n\r\n") == NULL);
  assert(strstr(response, "HTTP/1.1 101 ") == response);
  assert(close(fd) == 0);
}

static void register_proxy(vectis_app *app, const char *path,
                           vectis_route_path_kind kind,
                           vectis_http_methods methods, vectis_error *error) {
  vectis_proxy_route_config config;

  vectis_proxy_route_config_init(&config);
  config.path = path;
  config.path_kind = kind;
  config.methods = methods;
  config.target = "http://127.0.0.1:1";
  config.preflight = proxy_reply;
  assert(app->proxy_route(app, &config, error) == VECTIS_OK);
}

static void run_order(int proxy_first, const char *root) {
  vectis_app_config config;
  vectis_static_directory_config mount;
  vectis_upload_route_config upload;
  vectis_websocket_route_config websocket;
  vectis_route_config route;
  vectis_app *app;
  vectis_error error;
  unsigned short port;

  port = available_port();
  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = port;
  config.server.worker_count = 1u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route =
      vectis_route(VECTIS_HTTP_GET, "^/overlap/ordinary/.*$", ordinary, NULL);
  route.path_kind = VECTIS_ROUTE_PATH_REGEX;
  if (proxy_first)
    register_proxy(app, "/overlap/ordinary/a", VECTIS_ROUTE_PATH_LITERAL,
                   VECTIS_HTTP_METHODS_GET, &error);
  assert(app->route(app, &route, &error) == VECTIS_OK);
  if (!proxy_first)
    register_proxy(app, "/overlap/ordinary/a", VECTIS_ROUTE_PATH_LITERAL,
                   VECTIS_HTTP_METHODS_GET, &error);
  route = vectis_route(VECTIS_HTTP_GET, "/overlap/regex/a", ordinary, NULL);
  if (proxy_first)
    register_proxy(app, "^/overlap/regex/.*$", VECTIS_ROUTE_PATH_REGEX,
                   VECTIS_HTTP_METHODS_GET, &error);
  assert(app->route(app, &route, &error) == VECTIS_OK);
  if (!proxy_first)
    register_proxy(app, "^/overlap/regex/.*$", VECTIS_ROUTE_PATH_REGEX,
                   VECTIS_HTTP_METHODS_GET, &error);
  vectis_static_directory_config_init(&mount);
  mount.path_prefix = "/overlap/static/";
  mount.root_dir = root;
  if (proxy_first)
    register_proxy(app, "^/overlap/static/.*$", VECTIS_ROUTE_PATH_REGEX,
                   VECTIS_HTTP_METHODS_GET, &error);
  assert(app->static_directory(app, &mount, &error) == VECTIS_OK);
  if (!proxy_first)
    register_proxy(app, "^/overlap/static/.*$", VECTIS_ROUTE_PATH_REGEX,
                   VECTIS_HTTP_METHODS_GET, &error);
  upload = vectis_stream_upload_route(VECTIS_HTTP_POST, "/overlap/upload/a",
                                      upload_open, upload_write, upload_finish,
                                      upload_close, NULL);
  upload.body.max_bytes = 4u;
  if (proxy_first)
    register_proxy(app, "^/overlap/upload/.*$", VECTIS_ROUTE_PATH_REGEX,
                   VECTIS_HTTP_METHODS_POST, &error);
  assert(app->upload_stream(app, &upload, &error) == VECTIS_OK);
  if (!proxy_first)
    register_proxy(app, "^/overlap/upload/.*$", VECTIS_ROUTE_PATH_REGEX,
                   VECTIS_HTTP_METHODS_POST, &error);
  websocket = vectis_websocket_route("/overlap/ws", ws_message, NULL);
  if (proxy_first)
    register_proxy(app, "^/overlap/ws$", VECTIS_ROUTE_PATH_REGEX,
                   VECTIS_HTTP_METHODS_GET, &error);
  assert(app->websocket(app, &websocket, &error) == VECTIS_OK);
  if (!proxy_first)
    register_proxy(app, "^/overlap/ws$", VECTIS_ROUTE_PATH_REGEX,
                   VECTIS_HTTP_METHODS_GET, &error);
  route = vectis_route(VECTIS_HTTP_GET, "^/overlap/raw/.*$", ordinary, NULL);
  route.path_kind = VECTIS_ROUTE_PATH_REGEX;
  assert(app->route(app, &route, &error) == VECTIS_OK);
  register_proxy(app, "/overlap/raw/:id", VECTIS_ROUTE_PATH_PARAMS,
                 VECTIS_HTTP_METHODS_GET, &error);
  assert(app->start(app, &error) == VECTIS_OK);

  request_once(port,
               "GET /overlap/ordinary/a HTTP/1.1\r\nHost: localhost\r\n"
               "Connection: close\r\n\r\n",
               200, proxy_first ? "proxy" : "ordinary");
  request_once(port,
               "GET /overlap/regex/a HTTP/1.1\r\nHost: localhost\r\n"
               "Connection: close\r\n\r\n",
               200, proxy_first ? "proxy" : "ordinary");
  request_once(port,
               "GET /overlap/static/file.txt HTTP/1.1\r\nHost: localhost\r\n"
               "Connection: close\r\n\r\n",
               200, proxy_first ? "proxy" : "static-winner");
  request_once(port,
               "GET /overlap/static/a%2Fb HTTP/1.1\r\nHost: localhost\r\n"
               "Connection: close\r\n\r\n",
               200, "proxy");
  request_once(port,
               "POST /overlap/static/file.txt HTTP/1.1\r\n"
               "Host: localhost\r\nContent-Length: 0\r\n"
               "Connection: close\r\n\r\n",
               405, NULL);
  request_once(port,
               "POST /overlap/upload/a HTTP/1.1\r\nHost: localhost\r\n"
               "Content-Length: 4\r\nConnection: close\r\n\r\ndata",
               200, proxy_first ? "proxy" : "upload");
  request_once(port,
               "POST /overlap/upload/a%2Fb HTTP/1.1\r\nHost: localhost\r\n"
               "Content-Length: 4\r\nConnection: close\r\n\r\ndata",
               200, "proxy");
  request_ws(port);
  request_once(port,
               "GET /overlap/ws HTTP/1.1\r\nHost: localhost\r\n"
               "Connection: close\r\n\r\n",
               400, NULL);
  request_once(port,
               "GET /overlap/ws HTTP/1.1\r\nHost: localhost\r\n"
               "Connection: Upgrade\r\nUpgrade: h2c\r\n"
               "Connection: close\r\n\r\n",
               400, NULL);
  request_once(port,
               "GET /overlap/raw/a%2Fb HTTP/1.1\r\nHost: localhost\r\n"
               "Connection: close\r\n\r\n",
               200, "proxy");
  assert(vectis_stop(app, &error) == VECTIS_OK);
  app->close(app);
}

int main(void) {
  char root[] = "proxy-overlap.XXXXXX";
  char absolute_root[PATH_MAX];
  char path[PATH_MAX];
  char cwd[PATH_MAX];
  FILE *file;
  int length;

  assert(getcwd(cwd, sizeof(cwd)) != NULL);
  assert(mkdtemp(root) != NULL);
  length = snprintf(absolute_root, sizeof(absolute_root), "%s/%s", cwd, root);
  assert(length > 0 && (size_t)length < sizeof(absolute_root));
  length = snprintf(path, sizeof(path), "%s/file.txt", root);
  assert(length > 0 && (size_t)length < sizeof(path));
  file = fopen(path, "wb");
  assert(file != NULL);
  assert(fwrite("static-winner", 1u, 13u, file) == 13u);
  assert(fclose(file) == 0);
  run_order(0, absolute_root);
  run_order(1, absolute_root);
  assert(unlink(path) == 0);
  assert(rmdir(root) == 0);
  return 0;
}
