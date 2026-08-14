#include <cpkt/lua_runtime.h>
#include <cpkt/opcua.h>
#include <lua.h>
#include <pthread.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct opcua_server_loop {
  cpkt_opcua_server *server;
  volatile int stop;
  cpkt_opcua_result result;
} opcua_server_loop;

extern int luaopen_opcua(lua_State *lua);

static int open_opcua(void *lua_state) {
  return luaopen_opcua((lua_State *)lua_state);
}

static int fail_result(cpkt_opcua_result result, cpkt_opcua_status status,
                       const char *operation) {
  fprintf(stderr, "%s failed: %s", operation, cpkt_opcua_result_string(result));
  if (status != 0u) {
    fprintf(stderr, " / %s", cpkt_opcua_status_name(status));
  }
  fputc('\n', stderr);
  return 1;
}

static int expect_ok(cpkt_opcua_result result, cpkt_opcua_status status,
                     const char *operation) {
  if (result != CPKT_OPCUA_OK) {
    return fail_result(result, status, operation);
  }
  return 0;
}

static int read_script_file(const char *path, unsigned char **buffer_out,
                            size_t *size_out) {
  FILE *file;
  unsigned char *buffer;
  long file_size;
  size_t read_size;

  *buffer_out = NULL;
  *size_out = 0u;
  file = fopen(path, "rb");
  if (file == NULL) {
    perror(path);
    return 1;
  }
  if (fseek(file, 0L, SEEK_END) != 0) {
    perror("fseek");
    fclose(file);
    return 1;
  }
  file_size = ftell(file);
  if (file_size < 0L) {
    perror("ftell");
    fclose(file);
    return 1;
  }
  if (fseek(file, 0L, SEEK_SET) != 0) {
    perror("fseek");
    fclose(file);
    return 1;
  }
  buffer = (unsigned char *)malloc((size_t)file_size + 1u);
  if (buffer == NULL) {
    fputs("failed to allocate Lua script buffer\n", stderr);
    fclose(file);
    return 1;
  }
  read_size = fread(buffer, 1u, (size_t)file_size, file);
  if (read_size != (size_t)file_size) {
    fputs("failed to read complete Lua script\n", stderr);
    free(buffer);
    fclose(file);
    return 1;
  }
  if (fclose(file) != 0) {
    perror("fclose");
    free(buffer);
    return 1;
  }
  buffer[file_size] = '\0';
  *buffer_out = buffer;
  *size_out = (size_t)file_size;
  return 0;
}

static char *copy_string(const char *value) {
  size_t size;
  char *copy;

  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, size);
  return copy;
}

static int make_temp_output_path(char **path_out) {
  char template_path[] = "/tmp/vectis-opcua-pack-XXXXXX";
  char *path;
  int fd;
  int saved_errno;

  *path_out = NULL;
  fd = mkstemp(template_path);
  if (fd < 0) {
    perror("mkstemp");
    return 1;
  }
  if (close(fd) != 0) {
    saved_errno = errno;
    unlink(template_path);
    errno = saved_errno;
    perror("close");
    return 1;
  }
  if (unlink(template_path) != 0) {
    perror("unlink");
    return 1;
  }
  path = copy_string(template_path);
  if (path == NULL) {
    fputs("failed to allocate packed output path\n", stderr);
    return 1;
  }
  *path_out = path;
  return 0;
}

static int run_child(const char *label, char *const argv[],
                     const char *endpoint_url,
                     unsigned short lua_server_port) {
  pid_t pid;
  int status;
  char port_text[16];

  pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  if (pid == 0) {
    if (endpoint_url != NULL &&
        setenv("OPCUA_ENDPOINT", endpoint_url, 1) != 0) {
      perror("setenv");
      _exit(127);
    }
    if (lua_server_port != 0u) {
      snprintf(port_text, sizeof(port_text), "%u",
               (unsigned int)lua_server_port);
      if (setenv("OPCUA_LUA_SERVER_PORT", port_text, 1) != 0) {
        perror("setenv");
        _exit(127);
      }
    }
    execv(argv[0], argv);
    perror(label);
    _exit(127);
  }
  do {
    if (waitpid(pid, &status, 0) < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("waitpid");
      return 1;
    }
    break;
  } while (1);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    return 0;
  }
  if (WIFEXITED(status)) {
    fprintf(stderr, "%s exited with status %d\n", label, WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    fprintf(stderr, "%s terminated by signal %d\n", label, WTERMSIG(status));
  } else {
    fprintf(stderr, "%s ended unexpectedly\n", label);
  }
  return 1;
}

static int pick_loopback_port(unsigned short *port_out) {
  struct sockaddr_in addr;
  socklen_t addr_len;
  int fd;
  int saved_errno;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return 1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    perror("bind");
    return 1;
  }
  addr_len = (socklen_t)sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) != 0) {
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    perror("getsockname");
    return 1;
  }
  *port_out = ntohs(addr.sin_port);
  close(fd);
  return 0;
}

static void sleep_ms(unsigned long ms) {
  struct timespec ts;

  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

static void *server_loop_main(void *user) {
  opcua_server_loop *loop;
  unsigned short wait_ms;

  loop = (opcua_server_loop *)user;
  loop->result = CPKT_OPCUA_OK;
  while (!loop->stop) {
    wait_ms = 0u;
    loop->result = cpkt_opcua_server_iterate(loop->server, 0, &wait_ms);
    if (loop->result != CPKT_OPCUA_OK) {
      return NULL;
    }
    if (wait_ms > 20u) {
      wait_ms = 20u;
    }
    sleep_ms(wait_ms == 0u ? 1u : wait_ms);
  }
  return NULL;
}

static int run_lua_contract(const char *endpoint_url, const char *script_path,
                            unsigned short lua_server_port) {
  static const unsigned char script[] =
      "local opcua = require(\"opcua\")\n"
      "assert(type(opcua.server) == \"function\")\n"
      "local lua_server = assert(opcua.server({ port = OPCUA_LUA_SERVER_PORT "
      "}))\n"
      "assert(lua_server:set_endpoint({ host = \"127.0.0.1\", port = "
      "OPCUA_LUA_SERVER_PORT }) == true)\n"
      "assert(lua_server:set_application_identity({ application_uri = "
      "\"urn:vectis:lua:opcua\", product_uri = \"urn:vectis\", "
      "application_name = \"Vectis Lua OPC UA\" }) == true)\n"
      "assert(lua_server:set_access_control({ allow_anonymous = true }) == "
      "true)\n"
      "local lua_ns = assert(lua_server:add_namespace(\"urn:vectis:lua:opcua\""
      "))\n"
      "assert(lua_ns >= 1)\n"
      "local lua_node = opcua.node_id_numeric(lua_ns, 8101)\n"
      "assert(lua_server:add_variable({ node_id = lua_node, browse_name = "
      "\"luaOwned\", display_name = \"Lua Owned\", value = "
      "opcua.value_integer(12) }) == true)\n"
      "local lua_initial = assert(lua_server:read(lua_node))\n"
      "assert(lua_initial:get() == 12)\n"
      "assert(lua_server:write(lua_node, opcua.value_integer(44)) == true)\n"
      "local lua_updated = assert(lua_server:read(lua_node))\n"
      "assert(lua_updated:get() == 44)\n"
      "assert(lua_server:startup() == true)\n"
      "assert(type(lua_server:endpoint_url()) == \"string\")\n"
      "assert(type(lua_server:iterate(false)) == \"number\")\n"
      "assert(lua_server:shutdown() == true)\n"
      "assert(lua_server:close() == true)\n"
      "local node = opcua.node_id_numeric(1, 7101)\n"
      "local client = assert(opcua.connect(OPCUA_ENDPOINT))\n"
      "local value = assert(client:read(node))\n"
      "assert(value:type() == opcua.VALUE_INTEGER)\n"
      "assert(value:get() == 42)\n"
      "assert(client:write(node, opcua.value_integer(77)) == true)\n"
      "local updated = assert(client:read(node))\n"
      "assert(updated:get() == 77)\n"
      "assert(client:disconnect() == true)\n"
      "assert(client:close() == true)\n"
      "local manual = assert(opcua.client())\n"
      "assert(manual:connect(OPCUA_ENDPOINT) == true)\n"
      "assert(manual:disconnect() == true)\n"
      "assert(manual:close() == true)\n";
  unsigned char *file_script;
  const unsigned char *script_data;
  size_t script_size;
  const char *chunk_name;
  cpkt_lua_runtime *runtime;
  cpkt_lua_runtime_status lua_status;
  int failed;

  runtime = NULL;
  file_script = NULL;
  script_data = script;
  script_size = sizeof(script) - 1u;
  chunk_name = "opcua_lua_e2e.lua";
  failed = 0;
  if (script_path != NULL) {
    if (read_script_file(script_path, &file_script, &script_size) != 0) {
      return 1;
    }
    script_data = file_script;
    chunk_name = script_path;
  }
  lua_status = cpkt_lua_runtime_new(&runtime);
  if (lua_status != CPKT_LUA_RUNTIME_OK) {
    fprintf(stderr, "lua runtime new failed: %s\n",
            cpkt_lua_runtime_status_string(lua_status));
    free(file_script);
    return 1;
  }
  lua_status = cpkt_lua_runtime_openlibs(runtime);
  if (lua_status == CPKT_LUA_RUNTIME_OK) {
    lua_status =
        cpkt_lua_runtime_register_c_module(runtime, "opcua", open_opcua);
  }
  if (lua_status == CPKT_LUA_RUNTIME_OK) {
    lua_status =
        cpkt_lua_runtime_set_global_string(runtime, "OPCUA_ENDPOINT",
                                           endpoint_url);
  }
  if (lua_status == CPKT_LUA_RUNTIME_OK) {
    lua_status = cpkt_lua_runtime_set_global_integer(
        runtime, "OPCUA_LUA_SERVER_PORT", lua_server_port);
  }
  if (lua_status == CPKT_LUA_RUNTIME_OK) {
    lua_status = cpkt_lua_runtime_run_buffer(
        runtime, script_data, script_size, chunk_name, 0, NULL, 0);
  }
  if (lua_status != CPKT_LUA_RUNTIME_OK) {
    fprintf(stderr, "lua contract failed: %s: %s\n",
            cpkt_lua_runtime_status_string(lua_status),
            cpkt_lua_runtime_error(runtime));
    failed = 1;
  }
  cpkt_lua_runtime_free(runtime);
  free(file_script);
  return failed;
}

static int run_packed_lua_contract(const char *endpoint_url,
                                   const char *script_path,
                                   const char *vectis_bin,
                                   unsigned short lua_server_port) {
  char *output_path;
  char *vectis_path;
  char *script_path_copy;
  char arg_action[] = "-a";
  char arg_pack[] = "pack";
  char arg_script[] = "--script";
  char arg_output[] = "--output";
  char *pack_argv[8];
  char *run_argv[2];
  int failed;

  if (script_path == NULL || vectis_bin == NULL) {
    return 0;
  }
  vectis_path = copy_string(vectis_bin);
  script_path_copy = copy_string(script_path);
  if (vectis_path == NULL || script_path_copy == NULL) {
    fputs("failed to allocate packed OPC UA argv\n", stderr);
    free(vectis_path);
    free(script_path_copy);
    return 1;
  }
  if (make_temp_output_path(&output_path) != 0) {
    free(vectis_path);
    free(script_path_copy);
    return 1;
  }
  pack_argv[0] = vectis_path;
  pack_argv[1] = arg_action;
  pack_argv[2] = arg_pack;
  pack_argv[3] = arg_script;
  pack_argv[4] = script_path_copy;
  pack_argv[5] = arg_output;
  pack_argv[6] = output_path;
  pack_argv[7] = NULL;
  failed = run_child("vectis pack opcua example", pack_argv, NULL, 0u);
  if (!failed) {
    run_argv[0] = output_path;
    run_argv[1] = NULL;
    failed = run_child("packed opcua Lua example", run_argv, endpoint_url,
                       lua_server_port);
  }
  unlink(output_path);
  free(output_path);
  free(vectis_path);
  free(script_path_copy);
  return failed;
}

int main(int argc, char **argv) {
  cpkt_opcua_server *server;
  cpkt_opcua_node_id value_node;
  cpkt_opcua_value value;
  cpkt_opcua_status status;
  opcua_server_loop loop;
  pthread_t thread;
  char endpoint_url[128];
  const char *script_path;
  const char *vectis_bin;
  size_t endpoint_required;
  unsigned short port;
  unsigned short lua_server_port;
  int thread_started;
  int failed;

  server = NULL;
  thread_started = 0;
  failed = 0;
  status = 0u;
  vectis_bin = argc > 2 ? argv[1] : NULL;
  script_path = argc > 2 ? argv[2] : (argc > 1 ? argv[1] : NULL);
  endpoint_required = 0u;

  if (pick_loopback_port(&port) != 0) {
    return 1;
  }
  if (pick_loopback_port(&lua_server_port) != 0) {
    return 1;
  }
  if (expect_ok(cpkt_opcua_server_new(&server, port), status, "server new")) {
    return 1;
  }

  value_node = cpkt_opcua_node_id_numeric(1u, 7101u);
  cpkt_opcua_value_integer(&value, 42);
  failed = expect_ok(cpkt_opcua_server_add_variable(
                         server, value_node, "luaValue", "Lua Value", &value,
                         &status),
                     status, "server add variable");
  if (!failed) {
    failed = expect_ok(cpkt_opcua_server_endpoint_url(
                           server, endpoint_url, sizeof(endpoint_url),
                           &endpoint_required),
                       status, "server endpoint url");
  }
  if (!failed && endpoint_required >= sizeof(endpoint_url)) {
    fprintf(stderr, "server endpoint url exceeded test buffer\n");
    failed = 1;
  }
  if (!failed) {
    failed = expect_ok(cpkt_opcua_server_startup(server, &status), status,
                       "server startup");
  }
  if (!failed) {
    loop.server = server;
    loop.stop = 0;
    loop.result = CPKT_OPCUA_OK;
    if (pthread_create(&thread, NULL, server_loop_main, &loop) != 0) {
      perror("pthread_create");
      failed = 1;
    } else {
      thread_started = 1;
    }
  }
  if (!failed) {
    sleep_ms(50u);
    failed = run_lua_contract(endpoint_url, script_path, lua_server_port);
  }
  if (!failed) {
    failed = run_packed_lua_contract(endpoint_url, script_path, vectis_bin,
                                     lua_server_port);
  }
  if (thread_started) {
    loop.stop = 1;
    pthread_join(thread, NULL);
    if (!failed && loop.result != CPKT_OPCUA_OK) {
      failed = fail_result(loop.result, 0u, "server iterate");
    }
  }
  if (server != NULL) {
    if (!failed) {
      failed =
          expect_ok(cpkt_opcua_server_shutdown(server, &status), status,
                    "server shutdown");
    } else {
      (void)cpkt_opcua_server_shutdown(server, &status);
    }
    cpkt_opcua_server_free(server);
  }
  return failed ? 1 : 0;
}
