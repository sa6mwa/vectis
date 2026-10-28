#include "vectis_lua_curl.h"
#include <lauxlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vectis/curl.h>
#define CURL_SESSION "vectis.curl.session"
#define CURL_MULTI "vectis.curl.multi"
#define CURL_REQUEST "vectis.curl.request"
typedef struct curl_lua_owner {
  vectis_curl_session *session;
  vectis_curl_multi *multi;
  lua_State *L;
  pid_t process;
  int busy, native_active, violated;
} curl_lua_owner;
typedef struct curl_lua_request {
  curl_lua_owner *owner;
  int slot, failed, event, attached;
  CURL *easy;
  void *read_buffer;
  size_t capacity, read_size;
  const void *write_buffer;
  size_t write_size;
  const vectis_curl_progress *progress;
  int cancel, read_callback, file_failure;
  struct curl_slist *headers;
  FILE *input, *output;
  const char *body;
  size_t body_size, body_offset;
} curl_lua_request;
typedef struct curl_lua_option {
  const char *name;
  CURLoption option;
} curl_lua_option;
static int curl_lua_error(lua_State *L, const vectis_error *error,
                          const char *operation) {
  lua_pushnil(L);
  lua_createtable(L, 0, 8);
  lua_pushinteger(L, error->code);
  lua_setfield(L, -2, "status");
  lua_pushstring(L, vectis_status_string(error->code));
  lua_setfield(L, -2, "status_string");
  lua_pushinteger(L, error->source);
  lua_setfield(L, -2, "source_code");
  lua_pushstring(L, vectis_error_source_string(error->source));
  lua_setfield(L, -2, "source");
  lua_pushinteger(L, error->dependency_code);
  lua_setfield(L, -2, "dependency_code");
  lua_pushstring(L, error->message);
  lua_setfield(L, -2, "message");
  lua_pushstring(L, error->detail);
  lua_setfield(L, -2, "detail");
  lua_pushstring(L, operation);
  lua_setfield(L, -2, "operation");
  return 2;
}
static const char *curl_lua_string(lua_State *L, int index, size_t *size,
                                   int text) {
  const char *value;
  luaL_checktype(L, index, LUA_TSTRING);
  value = lua_tolstring(L, index, size);
  if (text && memchr(value, 0, *size) != NULL)
    luaL_argerror(L, index, "curl option contains NUL");
  return value;
}
static void curl_lua_field(lua_State *L, int index, const char *name) {
  index = lua_absindex(L, index);
  lua_pushstring(L, name);
  lua_rawget(L, index);
}
static int curl_lua_bool(lua_State *L, int index, const char *name,
                         int fallback) {
  int result;
  curl_lua_field(L, index, name);
  if (lua_isnil(L, -1))
    result = fallback;
  else {
    luaL_checktype(L, -1, LUA_TBOOLEAN);
    result = lua_toboolean(L, -1);
  }
  lua_pop(L, 1);
  return result;
}
static void curl_lua_setopt(lua_State *L, CURLcode code) {
  if (code != CURLE_OK)
    luaL_error(L, "curl option: %s", curl_easy_strerror(code));
}
static void curl_lua_request_push(lua_State *L, int owner, int slot) {
  lua_getiuservalue(L, owner, 1);
  if (slot != 0) {
    lua_rawgeti(L, -1, slot);
    lua_remove(L, -2);
  }
}
static void curl_lua_request_cleanup(curl_lua_request *request) {
  request->attached = 0;
  if (request->input != NULL) {
    (void)fclose(request->input);
    request->input = NULL;
  }
  if (request->output != NULL) {
    (void)fclose(request->output);
    request->output = NULL;
  }
  curl_slist_free_all(request->headers);
  request->headers = NULL;
}
static int curl_lua_request_gc(lua_State *L) {
  curl_lua_request *request =
      (curl_lua_request *)luaL_checkudata(L, 1, CURL_REQUEST);
  /* Finalizers may run child-first. The handle closes its native receiver
   * before releasing attached files/header lists; an unattached constructor
   * failure owns no native handle and can be cleaned independently. */
  if (!request->attached)
    curl_lua_request_cleanup(request);
  return 0;
}
static void curl_lua_release_requests(lua_State *L, int multi) {
  int i;
  curl_lua_request *request;
  lua_getiuservalue(L, 1, 1);
  if (multi && lua_istable(L, -1)) {
    for (i = 1; i <= (int)VECTIS_CURL_MULTI_LIMIT; ++i) {
      lua_rawgeti(L, -1, i);
      request = (curl_lua_request *)luaL_testudata(L, -1, CURL_REQUEST);
      if (request != NULL)
        curl_lua_request_cleanup(request);
      lua_pop(L, 1);
    }
  } else {
    request = (curl_lua_request *)luaL_testudata(L, -1, CURL_REQUEST);
    if (request != NULL)
      curl_lua_request_cleanup(request);
  }
  lua_pop(L, 1);
}
static int curl_lua_close(lua_State *L) {
  curl_lua_owner *owner = (curl_lua_owner *)luaL_testudata(L, 1, CURL_SESSION);
  vectis_error error;
  vectis_status status;
  int finalizer = lua_toboolean(L, lua_upvalueindex(1));
  int multi = 0;
  if (owner == NULL)
    owner = (curl_lua_owner *)luaL_checkudata(L, 1, CURL_MULTI);
  multi = owner->multi != NULL;
  if (owner->busy && !owner->native_active) {
    owner->violated = 1;
    if (finalizer)
      return 0;
    return luaL_error(L, "cannot close active curl receiver");
  }
  status = owner->multi != NULL
               ? vectis_curl_multi_close(owner->multi, &error)
               : vectis_curl_session_close(owner->session, &error);
  if (status != VECTIS_OK) {
    if (finalizer)
      return 0;
    return curl_lua_error(L, &error, "close");
  }
  curl_lua_release_requests(L, multi);
  owner->session = NULL;
  owner->multi = NULL;
  lua_pushnil(L);
  lua_setiuservalue(L, 1, 1);
  if (!finalizer)
    lua_pushboolean(L, 1);
  return !finalizer;
}
static int curl_lua_new(lua_State *L) {
  int multi = lua_toboolean(L, lua_upvalueindex(1));
  curl_lua_owner *owner;
  vectis_error error;
  vectis_status status;
  luaL_argcheck(L, lua_isnoneornil(L, 1), 1,
                "curl handle constructor takes no options");
  owner = (curl_lua_owner *)lua_newuserdatauv(L, sizeof(*owner), 1);
  memset(owner, 0, sizeof(*owner));
  owner->process = getpid();
  luaL_setmetatable(L, multi ? CURL_MULTI : CURL_SESSION);
  if (multi) {
    lua_createtable(L, VECTIS_CURL_MULTI_LIMIT, 0);
    lua_setiuservalue(L, -2, 1);
    status = vectis_curl_multi_new(&owner->multi, &error);
  } else
    status = vectis_curl_session_new(&owner->session, &error);
  if (status != VECTIS_OK)
    return curl_lua_error(L, &error, multi ? "multi" : "session");
  return 1;
}
static void curl_lua_snapshot(lua_State *L, int source) {
  int copy, headers;
  source = lua_absindex(L, source);
  luaL_checktype(L, source, LUA_TTABLE);
  lua_newtable(L);
  copy = lua_gettop(L);
  lua_pushnil(L);
  while (lua_next(L, source)) {
    lua_pushvalue(L, -2);
    lua_pushvalue(L, -2);
    lua_rawset(L, copy);
    lua_pop(L, 1);
  }
  curl_lua_field(L, copy, "headers");
  if (!lua_isnil(L, -1)) {
    luaL_checktype(L, -1, LUA_TTABLE);
    headers = lua_gettop(L);
    lua_newtable(L);
    lua_pushnil(L);
    while (lua_next(L, headers)) {
      lua_pushvalue(L, -2);
      lua_pushvalue(L, -2);
      lua_rawset(L, -5);
      lua_pop(L, 1);
    }
    lua_setfield(L, copy, "headers");
  }
  lua_pop(L, 1);
}
static curl_lua_request *curl_lua_request_new(lua_State *L,
                                              curl_lua_owner *owner, int slot) {
  curl_lua_request *request =
      (curl_lua_request *)lua_newuserdatauv(L, sizeof(*request), 2);
  int index = lua_gettop(L);
  memset(request, 0, sizeof(*request));
  request->owner = owner;
  request->slot = slot;
  luaL_setmetatable(L, CURL_REQUEST);
  curl_lua_snapshot(L, 2);
  lua_setiuservalue(L, index, 1);
  return request;
}
static int curl_lua_token(const char *value, size_t size) {
  size_t i;
  unsigned char c;
  if (size == 0u)
    return 0;
  for (i = 0u; i < size; ++i) {
    c = (unsigned char)value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c) != NULL))
      return 0;
  }
  return 1;
}
static int curl_lua_line(const char *value, size_t size) {
  return memchr(value, '\r', size) == NULL && memchr(value, '\n', size) == NULL;
}
static void curl_lua_headers(lua_State *L, curl_lua_request *request,
                             int opts) {
  const char *name, *value, *line;
  size_t name_size, value_size, total = 0u;
  int headers;
  struct curl_slist *grown;
  curl_lua_field(L, opts, "headers");
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  luaL_checktype(L, -1, LUA_TTABLE);
  headers = lua_gettop(L);
  lua_pushnil(L);
  while (lua_next(L, headers)) {
    value = curl_lua_string(L, -1, &value_size, 1);
    luaL_argcheck(L, curl_lua_line(value, value_size), opts,
                  "curl header contains CR/LF");
    if (lua_type(L, -2) == LUA_TSTRING) {
      name = curl_lua_string(L, -2, &name_size, 1);
      luaL_argcheck(L, curl_lua_token(name, name_size), opts,
                    "invalid curl header name");
      lua_pushfstring(L, "%s: %s", name, value);
      line = lua_tostring(L, -1);
      value_size += name_size + 2u;
    } else {
      luaL_argcheck(L, lua_isinteger(L, -2) && lua_tointeger(L, -2) > 0, opts,
                    "header keys must be names or positive indices");
      lua_pushvalue(L, -1);
      line = lua_tostring(L, -1);
    }
    luaL_argcheck(L, value_size <= VECTIS_CURL_HEADER_LIMIT - total, opts,
                  "request headers exceed 64 KiB");
    total += value_size;
    grown = curl_slist_append(request->headers, line);
    if (grown == NULL)
      luaL_error(L, "allocating curl headers");
    request->headers = grown;
    lua_pop(L, 2);
  }
  lua_pop(L, 1);
  curl_lua_setopt(
      L, curl_easy_setopt(request->easy, CURLOPT_HTTPHEADER, request->headers));
}
static int curl_lua_configure_dispatch(lua_State *L) {
  static const curl_lua_option strings[] = {
      {"url", CURLOPT_URL},
      {"protocols", CURLOPT_PROTOCOLS_STR},
      {"ca_file", CURLOPT_CAINFO},
      {"ca_path", CURLOPT_CAPATH},
      {"client_cert", CURLOPT_SSLCERT},
      {"client_key", CURLOPT_SSLKEY},
      {"client_cert_type", CURLOPT_SSLCERTTYPE},
      {"key_password", CURLOPT_KEYPASSWD},
      {"username", CURLOPT_USERNAME},
      {"password", CURLOPT_PASSWORD},
      {"proxy", CURLOPT_PROXY},
      {"proxy_username", CURLOPT_PROXYUSERNAME},
      {"proxy_password", CURLOPT_PROXYPASSWORD},
      {"interface", CURLOPT_INTERFACE},
      {"user_agent", CURLOPT_USERAGENT},
      {"accept_encoding", CURLOPT_ACCEPT_ENCODING},
      {"ssh_private_key", CURLOPT_SSH_PRIVATE_KEYFILE},
      {"ssh_public_key", CURLOPT_SSH_PUBLIC_KEYFILE},
      {"ssh_known_hosts", CURLOPT_SSH_KNOWNHOSTS},
      {NULL, CURLOPT_URL}};
  static const curl_lua_option numbers[] = {
      {"timeout_ms", CURLOPT_TIMEOUT_MS},
      {"connect_timeout_ms", CURLOPT_CONNECTTIMEOUT_MS},
      {"low_speed_limit", CURLOPT_LOW_SPEED_LIMIT},
      {"low_speed_time", CURLOPT_LOW_SPEED_TIME},
      {NULL, CURLOPT_TIMEOUT_MS}};
  static const char *const rest[] = {
      "method",      "headers",     "body",          "upload",
      "upload_path", "body_path",   "download_path", "follow_redirects",
      "http2",       "verify_peer", "verify_host",   "tcp_keepalive",
      "no_signal",   "proxy_type",  "read",          "write",
      "progress",    NULL};
  static const char *const methods[] = {
      "GET",     "HEAD",     "POST",  "PUT",  "PATCH", "DELETE",
      "OPTIONS", "PROPFIND", "MKCOL", "COPY", "MOVE",  NULL};
  curl_lua_request *request = (curl_lua_request *)lua_touserdata(L, 1);
  const char *key, *value, *method = NULL, *upload_path = NULL,
                           *download_path = NULL;
  size_t size;
  int opts, i, j, known, read_callback = 0, write_callback = 0, upload;
  lua_Integer number;
  long proxy_type = -1L;
  struct stat info;
  memset(&info, 0, sizeof(info));
  curl_lua_request_push(L, 2, request->slot);
  lua_getiuservalue(L, -1, 1);
  opts = lua_gettop(L);
  lua_pushnil(L);
  while (lua_next(L, opts)) {
    key = curl_lua_string(L, -2, &size, 1);
    known = 0;
    for (i = 0; strings[i].name != NULL; ++i)
      if (strcmp(key, strings[i].name) == 0)
        known = 1;
    for (i = 0; numbers[i].name != NULL; ++i)
      if (strcmp(key, numbers[i].name) == 0)
        known = 1;
    for (i = 0; rest[i] != NULL; ++i)
      if (strcmp(key, rest[i]) == 0)
        known = 1;
    if (!known)
      luaL_error(L, "unsupported curl receiver option '%s'", key);
    lua_pop(L, 1);
  }
  for (i = 0; strings[i].name != NULL; ++i) {
    curl_lua_field(L, opts, strings[i].name);
    if (!lua_isnil(L, -1)) {
      value = curl_lua_string(L, -1, &size, 1);
      if (i == 0)
        luaL_argcheck(L, size != 0u, opts, "curl url is required");
      curl_lua_setopt(
          L, curl_easy_setopt(request->easy, strings[i].option, value));
    } else if (i == 0)
      luaL_error(L, "curl url is required");
    lua_pop(L, 1);
  }
  for (i = 0; numbers[i].name != NULL; ++i) {
    curl_lua_field(L, opts, numbers[i].name);
    if (!lua_isnil(L, -1)) {
      number = luaL_checkinteger(L, -1);
      luaL_argcheck(
          L, number >= 0 && (lua_Unsigned)number <= (unsigned long)LONG_MAX,
          opts, "curl numeric option out of range");
      curl_lua_setopt(
          L, curl_easy_setopt(request->easy, numbers[i].option, (long)number));
    }
    lua_pop(L, 1);
  }
  curl_lua_setopt(
      L, curl_easy_setopt(request->easy, CURLOPT_NOSIGNAL,
                          (long)curl_lua_bool(L, opts, "no_signal", 1)));
  curl_lua_setopt(
      L, curl_easy_setopt(request->easy, CURLOPT_SSL_VERIFYPEER,
                          (long)curl_lua_bool(L, opts, "verify_peer", 1)));
  curl_lua_setopt(
      L, curl_easy_setopt(request->easy, CURLOPT_SSL_VERIFYHOST,
                          curl_lua_bool(L, opts, "verify_host", 1) ? 2L : 0L));
  curl_lua_setopt(L,
                  curl_easy_setopt(request->easy, CURLOPT_FOLLOWLOCATION,
                                   curl_lua_bool(L, opts, "follow_redirects", 0)
                                       ? CURLFOLLOW_OBEYCODE
                                       : 0L));
  curl_lua_setopt(
      L, curl_easy_setopt(request->easy, CURLOPT_TCP_KEEPALIVE,
                          (long)curl_lua_bool(L, opts, "tcp_keepalive", 0)));
  if (curl_lua_bool(L, opts, "http2", 0))
    curl_lua_setopt(L, curl_easy_setopt(request->easy, CURLOPT_HTTP_VERSION,
                                        (long)CURL_HTTP_VERSION_2TLS));
  curl_lua_field(L, opts, "proxy_type");
  if (!lua_isnil(L, -1)) {
    value = curl_lua_string(L, -1, &size, 1);
    if (strcmp(value, "http") == 0)
      proxy_type = CURLPROXY_HTTP;
    else if (strcmp(value, "https") == 0)
      proxy_type = CURLPROXY_HTTPS;
    else if (strcmp(value, "socks4") == 0)
      proxy_type = CURLPROXY_SOCKS4;
    else if (strcmp(value, "socks4a") == 0)
      proxy_type = CURLPROXY_SOCKS4A;
    else if (strcmp(value, "socks5") == 0)
      proxy_type = CURLPROXY_SOCKS5;
    else if (strcmp(value, "socks5_hostname") == 0)
      proxy_type = CURLPROXY_SOCKS5_HOSTNAME;
    else
      luaL_error(L, "unsupported curl proxy_type '%s'", value);
    curl_lua_setopt(
        L, curl_easy_setopt(request->easy, CURLOPT_PROXYTYPE, proxy_type));
  }
  lua_pop(L, 1);
  curl_lua_headers(L, request, opts);
  for (i = 0; i < 3; ++i) {
    curl_lua_field(L, opts, i == 0 ? "read" : i == 1 ? "write" : "progress");
    if (!lua_isnil(L, -1)) {
      luaL_checktype(L, -1, LUA_TFUNCTION);
      if (i == 0)
        read_callback = 1;
      if (i == 1)
        write_callback = 1;
    }
    lua_pop(L, 1);
  }
  curl_lua_field(L, opts, "method");
  if (!lua_isnil(L, -1)) {
    method = curl_lua_string(L, -1, &size, 1);
    for (j = 0; methods[j] != NULL && strcmp(method, methods[j]) != 0; ++j) {
    }
    if (methods[j] == NULL)
      luaL_error(L, "unsupported curl method '%s'", method);
  }
  curl_lua_field(L, opts, "upload_path");
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    curl_lua_field(L, opts, "body_path");
  }
  if (!lua_isnil(L, -1)) {
    upload_path = curl_lua_string(L, -1, &size, 1);
    luaL_argcheck(L, size != 0u, opts, "empty curl upload path");
  }
  curl_lua_field(L, opts, "download_path");
  if (!lua_isnil(L, -1)) {
    download_path = curl_lua_string(L, -1, &size, 1);
    luaL_argcheck(L, size != 0u, opts, "empty curl download path");
  }
  curl_lua_field(L, opts, "body");
  if (!lua_isnil(L, -1))
    request->body = curl_lua_string(L, -1, &request->body_size, 0);
  luaL_argcheck(
      L,
      !(read_callback && (upload_path != NULL || request->body != NULL)) &&
          !(upload_path != NULL && request->body != NULL),
      opts, "curl request body sources are mutually exclusive");
  luaL_argcheck(L, !(write_callback && download_path != NULL), opts,
                "curl response sinks are mutually exclusive");
  request->read_callback = read_callback;
  upload = curl_lua_bool(L, opts, "upload", 0) || read_callback ||
           upload_path != NULL;
  if (upload_path != NULL) {
    request->input = fopen(upload_path, "rb");
    if (request->input == NULL)
      luaL_error(L, "failed to open curl upload path '%s'", upload_path);
    if (fstat(fileno(request->input), &info) == 0 && S_ISREG(info.st_mode))
      curl_lua_setopt(L,
                      curl_easy_setopt(request->easy, CURLOPT_INFILESIZE_LARGE,
                                       (curl_off_t)info.st_size));
  }
  if (download_path != NULL) {
    request->output = fopen(download_path, "wb");
    if (request->output == NULL)
      luaL_error(L, "failed to open curl download path '%s'", download_path);
  }
  if (upload) {
    curl_lua_setopt(L, curl_easy_setopt(request->easy, CURLOPT_UPLOAD, 1L));
    if (request->body != NULL)
      curl_lua_setopt(L,
                      curl_easy_setopt(request->easy, CURLOPT_INFILESIZE_LARGE,
                                       (curl_off_t)request->body_size));
  } else if (request->body != NULL) {
    curl_lua_setopt(L,
                    curl_easy_setopt(request->easy, CURLOPT_POSTFIELDSIZE_LARGE,
                                     (curl_off_t)request->body_size));
    curl_lua_setopt(
        L, curl_easy_setopt(request->easy, CURLOPT_POSTFIELDS, request->body));
  }
  if (method != NULL) {
    if (strcmp(method, "HEAD") == 0)
      curl_lua_setopt(L, curl_easy_setopt(request->easy, CURLOPT_NOBODY, 1L));
    else if (strcmp(method, "GET") == 0 && !upload && request->body == NULL)
      curl_lua_setopt(L, curl_easy_setopt(request->easy, CURLOPT_HTTPGET, 1L));
    else if (strcmp(method, "POST") == 0 && upload) {
      curl_lua_setopt(L, curl_easy_setopt(request->easy, CURLOPT_UPLOAD, 0L));
      curl_lua_setopt(L, curl_easy_setopt(request->easy, CURLOPT_POST, 1L));
      curl_lua_setopt(
          L, curl_easy_setopt(request->easy, CURLOPT_POSTFIELDSIZE_LARGE,
                              request->input != NULL && S_ISREG(info.st_mode)
                                  ? (curl_off_t)info.st_size
                              : request->body != NULL
                                  ? (curl_off_t)request->body_size
                                  : (curl_off_t)-1));
    } else
      curl_lua_setopt(
          L, curl_easy_setopt(request->easy, CURLOPT_CUSTOMREQUEST, method));
  }
  return 0;
}
static int curl_lua_callback_dispatch(lua_State *L) {
  curl_lua_request *request = (curl_lua_request *)lua_touserdata(L, 1);
  const char *bytes;
  size_t size;
  int opts;
  curl_lua_request_push(L, 2, request->slot);
  lua_getiuservalue(L, -1, 1);
  opts = lua_gettop(L);
  curl_lua_field(L, opts,
                 request->event == 0   ? "read"
                 : request->event == 1 ? "write"
                                       : "progress");
  if (request->event == 0) {
    lua_pushinteger(L, (lua_Integer)request->capacity);
    lua_call(L, 1, 2);
    if (lua_isnil(L, -2)) {
      if (!lua_isnil(L, -1))
        return lua_error(L);
      request->read_size = 0u;
      return 0;
    }
    bytes = curl_lua_string(L, -2, &size, 0);
    if (size == 0u || size > request->capacity)
      return luaL_error(L,
                        "curl reader must return 1..capacity bytes or nil EOF");
    memcpy(request->read_buffer, bytes, size);
    request->read_size = size;
  } else if (request->event == 1) {
    lua_pushlstring(L, (const char *)request->write_buffer,
                    request->write_size);
    lua_call(L, 1, 2);
    if (lua_isnil(L, -2) && !lua_isnil(L, -1))
      return lua_error(L);
    if (!lua_isboolean(L, -2) || !lua_toboolean(L, -2))
      return luaL_error(L, "curl writer must return true or nil,error");
  } else {
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, (lua_Integer)request->progress->download_total);
    lua_setfield(L, -2, "download_total");
    lua_pushinteger(L, (lua_Integer)request->progress->downloaded);
    lua_setfield(L, -2, "downloaded");
    lua_pushinteger(L, (lua_Integer)request->progress->upload_total);
    lua_setfield(L, -2, "upload_total");
    lua_pushinteger(L, (lua_Integer)request->progress->uploaded);
    lua_setfield(L, -2, "uploaded");
    lua_call(L, 1, 1);
    luaL_checktype(L, -1, LUA_TBOOLEAN);
    request->cancel = !lua_toboolean(L, -1);
  }
  return 0;
}
static vectis_status curl_lua_invoke(curl_lua_request *request,
                                     lua_CFunction fn, vectis_error *error) {
  lua_State *L = request->owner->L;
  int base = lua_gettop(L), status;
  if (!lua_checkstack(L, 10)) {
    memset(error, 0, sizeof(*error));
    error->code = VECTIS_ERR_NOMEM;
    error->source = VECTIS_ERROR_SOURCE_CURL;
    strcpy(error->message, "curl callback stack allocation failed");
    return error->code;
  }
  lua_pushcfunction(L, fn);
  lua_pushlightuserdata(L, request);
  lua_pushvalue(L, 1);
  status = lua_pcall(L, 2, 0, 0);
  if (status != LUA_OK) {
    request->failed = 1;
    curl_lua_request_push(L, 1, request->slot);
    lua_pushvalue(L, base + 1);
    lua_setiuservalue(L, -2, 2);
    memset(error, 0, sizeof(*error));
    error->code = status == LUA_ERRMEM ? VECTIS_ERR_NOMEM : VECTIS_ERR_STATE;
    error->source = VECTIS_ERROR_SOURCE_CURL;
    strcpy(error->message, fn == curl_lua_configure_dispatch
                               ? "curl request configuration failed"
                               : "curl Lua callback failed");
  }
  lua_settop(L, base);
  return status == LUA_OK ? VECTIS_OK : error->code;
}
static vectis_status curl_lua_configure(CURL *easy, void *context,
                                        vectis_error *error) {
  curl_lua_request *request = (curl_lua_request *)context;
  request->easy = easy;
  return curl_lua_invoke(request, curl_lua_configure_dispatch, error);
}
static vectis_status curl_lua_read(void *context, void *data, size_t capacity,
                                   size_t *size, vectis_error *error) {
  curl_lua_request *request = (curl_lua_request *)context;
  vectis_status status;
  request->read_buffer = data;
  request->capacity = capacity;
  request->read_size = 0u;
  if (request->input != NULL) {
    *size = fread(data, 1u, capacity, request->input);
    if (ferror(request->input)) {
      memset(error, 0, sizeof(*error));
      error->code = VECTIS_ERR_STATE;
      error->source = VECTIS_ERROR_SOURCE_CURL;
      strcpy(error->message, "curl upload file read failed");
      return error->code;
    }
    return VECTIS_OK;
  }
  if (request->body != NULL) {
    *size = request->body_size - request->body_offset;
    if (*size > capacity)
      *size = capacity;
    memcpy(data, request->body + request->body_offset, *size);
    request->body_offset += *size;
    return VECTIS_OK;
  }
  if (!request->read_callback) {
    *size = 0u;
    return VECTIS_OK;
  }
  request->event = 0;
  status = curl_lua_invoke(request, curl_lua_callback_dispatch, error);
  *size = request->read_size;
  return status;
}
static vectis_status curl_lua_write(void *context, const void *data,
                                    size_t size, vectis_error *error) {
  curl_lua_request *request = (curl_lua_request *)context;
  if (request->output != NULL) {
    if (fwrite(data, 1u, size, request->output) != size) {
      memset(error, 0, sizeof(*error));
      error->code = VECTIS_ERR_STATE;
      error->source = VECTIS_ERROR_SOURCE_CURL;
      strcpy(error->message, "curl download file write failed");
      return error->code;
    }
    return VECTIS_OK;
  }
  request->event = 1;
  request->write_buffer = data;
  request->write_size = size;
  return curl_lua_invoke(request, curl_lua_callback_dispatch, error);
}
static vectis_status curl_lua_progress(void *context,
                                       const vectis_curl_progress *value,
                                       int *cancel, vectis_error *error) {
  curl_lua_request *request = (curl_lua_request *)context;
  vectis_status status;
  request->event = 2;
  request->progress = value;
  request->cancel = 0;
  status = curl_lua_invoke(request, curl_lua_callback_dispatch, error);
  *cancel = request->cancel;
  return status;
}
static void curl_lua_native_request(lua_State *L, curl_lua_request *request,
                                    int index, vectis_curl_request *native) {
  int opts;
  memset(native, 0, sizeof(*native));
  native->configure = curl_lua_configure;
  native->context = request;
  lua_getiuservalue(L, index, 1);
  opts = lua_gettop(L);
  curl_lua_field(L, opts, "read");
  if (!lua_isnil(L, -1))
    native->read = curl_lua_read;
  lua_pop(L, 1);
  curl_lua_field(L, opts, "upload_path");
  if (!lua_isnil(L, -1))
    native->read = curl_lua_read;
  lua_pop(L, 1);
  curl_lua_field(L, opts, "body_path");
  if (!lua_isnil(L, -1))
    native->read = curl_lua_read;
  lua_pop(L, 1);
  if (curl_lua_bool(L, opts, "upload", 0))
    native->read = curl_lua_read;
  curl_lua_field(L, opts, "write");
  if (!lua_isnil(L, -1))
    native->write = curl_lua_write;
  lua_pop(L, 1);
  curl_lua_field(L, opts, "download_path");
  if (!lua_isnil(L, -1))
    native->write = curl_lua_write;
  lua_pop(L, 1);
  curl_lua_field(L, opts, "progress");
  if (!lua_isnil(L, -1))
    native->progress = curl_lua_progress;
  lua_pop(L, 2);
}
static void curl_lua_result(lua_State *L, const vectis_curl_result *result) {
  curl_lua_request *request = (curl_lua_request *)result->context;
  int index;
  lua_createtable(L, 0, 14);
  index = lua_gettop(L);
  lua_pushboolean(L, result->code == CURLE_OK &&
                         result->failure.code == VECTIS_OK);
  lua_setfield(L, index, "ok");
  lua_pushinteger(L, result->code);
  lua_setfield(L, index, "code");
  lua_pushstring(L, curl_easy_strerror(result->code));
  lua_setfield(L, index, "code_name");
  lua_pushinteger(L, result->status);
  lua_setfield(L, index, "status");
  lua_pushinteger(L, (lua_Integer)result->id);
  lua_setfield(L, index, "id");
  if (!result->streamed) {
    lua_pushlstring(L, (const char *)result->body, result->body_size);
    lua_setfield(L, index, "body");
  }
  lua_pushboolean(L, result->streamed);
  lua_setfield(L, index, "streamed");
  lua_pushlstring(L, (const char *)result->headers, result->header_size);
  lua_setfield(L, index, "headers");
  lua_pushstring(L, result->effective_url);
  lua_setfield(L, index, "effective_url");
  lua_pushinteger(L, 1);
  lua_setfield(L, index, "attempts");
  if (result->failure.code != VECTIS_OK) {
    curl_lua_error(L, &result->failure, "transfer");
    lua_setfield(L, index, "failure");
    lua_pop(L, 1);
  }
  if (result->code != CURLE_OK || result->failure.code != VECTIS_OK) {
    lua_pushstring(L, result->failure.code != VECTIS_OK
                          ? result->failure.message
                          : result->message);
    lua_setfield(L, index, "error");
  }
  if (request->file_failure) {
    lua_pushboolean(L, 0);
    lua_setfield(L, index, "ok");
    lua_pushinteger(L, CURLE_WRITE_ERROR);
    lua_setfield(L, index, "code");
    lua_pushstring(L, curl_easy_strerror(CURLE_WRITE_ERROR));
    lua_setfield(L, index, "code_name");
    lua_pushliteral(L, "failed to close curl download file");
    lua_setfield(L, index, "error");
  }
  if (request->failed) {
    curl_lua_request_push(L, 1, request->slot);
    lua_getiuservalue(L, -1, 2);
    lua_pushvalue(L, -1);
    lua_setfield(L, index, "callback_error");
    lua_setfield(L, index, "error");
    lua_pop(L, 1);
  }
}
static int curl_lua_failed_configuration(lua_State *L,
                                         curl_lua_request *request, int index,
                                         const vectis_error *error,
                                         const char *operation) {
  if (request->failed) {
    lua_pushnil(L);
    lua_getiuservalue(L, index, 2);
    return 2;
  }
  return curl_lua_error(L, error, operation);
}
static int curl_lua_perform(lua_State *L) {
  curl_lua_owner *owner = (curl_lua_owner *)luaL_checkudata(L, 1, CURL_SESSION);
  curl_lua_request *request;
  vectis_curl_request native;
  const vectis_curl_result *result;
  vectis_error error;
  vectis_status status;
  int index;
  lua_settop(L, 2);
  luaL_argcheck(L, owner->session != NULL, 1, "curl session is closed");
  if (owner->session->reset(owner->session, &error) != VECTIS_OK)
    return curl_lua_error(L, &error, "reset");
  curl_lua_release_requests(L, 0);
  request = curl_lua_request_new(L, owner, 0);
  index = lua_gettop(L);
  curl_lua_native_request(L, request, index, &native);
  lua_pushvalue(L, index);
  lua_setiuservalue(L, 1, 1);
  request->attached = 1;
  owner->native_active = 1;
  status = owner->session->perform(owner->session, &native, &result, &error);
  owner->native_active = 0;
  if (status != VECTIS_OK) {
    (void)owner->session->reset(owner->session, NULL);
    curl_lua_request_cleanup(request);
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 1);
    return curl_lua_failed_configuration(L, request, index, &error, "perform");
  }
  if (request->output != NULL) {
    int code = fclose(request->output);
    request->output = NULL;
    if (code != 0)
      request->file_failure = 1;
  }
  curl_lua_result(L, result);
  return 1;
}
static int curl_lua_reset(lua_State *L) {
  curl_lua_owner *owner = (curl_lua_owner *)luaL_checkudata(L, 1, CURL_SESSION);
  vectis_error error;
  luaL_argcheck(L, owner->session != NULL, 1, "curl session is closed");
  if (owner->session->reset(owner->session, &error) != VECTIS_OK)
    return curl_lua_error(L, &error, "reset");
  curl_lua_release_requests(L, 0);
  lua_pushnil(L);
  lua_setiuservalue(L, 1, 1);
  lua_pushboolean(L, 1);
  return 1;
}
static int curl_lua_add(lua_State *L) {
  curl_lua_owner *owner = (curl_lua_owner *)luaL_checkudata(L, 1, CURL_MULTI);
  curl_lua_request *request;
  vectis_curl_request native;
  vectis_error error;
  vectis_status status;
  unsigned long id;
  int slot, index, table;
  lua_settop(L, 2);
  luaL_argcheck(L, owner->multi != NULL, 1, "curl multi is closed");
  lua_getiuservalue(L, 1, 1);
  table = lua_gettop(L);
  for (slot = 1; slot <= (int)VECTIS_CURL_MULTI_LIMIT; ++slot) {
    lua_rawgeti(L, table, slot);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    lua_pop(L, 1);
  }
  luaL_argcheck(L, slot <= (int)VECTIS_CURL_MULTI_LIMIT, 1,
                "curl multi capacity exhausted; drain completions");
  request = curl_lua_request_new(L, owner, slot);
  index = lua_gettop(L);
  curl_lua_native_request(L, request, index, &native);
  lua_pushvalue(L, index);
  lua_rawseti(L, table, slot);
  request->attached = 1;
  owner->native_active = 1;
  status = owner->multi->add(owner->multi, &native, &id, &error);
  owner->native_active = 0;
  if (status != VECTIS_OK) {
    curl_lua_request_cleanup(request);
    lua_pushnil(L);
    lua_rawseti(L, table, slot);
    return curl_lua_failed_configuration(L, request, index, &error, "add");
  }
  lua_pushinteger(L, (lua_Integer)id);
  return 1;
}
static int curl_lua_step(lua_State *L) {
  curl_lua_owner *owner = (curl_lua_owner *)luaL_checkudata(L, 1, CURL_MULTI);
  vectis_error error;
  size_t running;
  luaL_argcheck(L, owner->multi != NULL, 1, "curl multi is closed");
  {
    vectis_status status;
    owner->native_active = 1;
    status = owner->multi->step(owner->multi, &running, &error);
    owner->native_active = 0;
    if (status != VECTIS_OK)
      return curl_lua_error(L, &error, "step");
  }
  lua_pushinteger(L, (lua_Integer)running);
  return 1;
}
static int curl_lua_poll(lua_State *L) {
  curl_lua_owner *owner = (curl_lua_owner *)luaL_checkudata(L, 1, CURL_MULTI);
  vectis_error error;
  lua_Integer timeout = luaL_optinteger(L, 2, 0);
  int ready;
  luaL_argcheck(L, owner->multi != NULL, 1, "curl multi is closed");
  luaL_argcheck(L, timeout >= 0 && timeout <= 60000, 2,
                "poll timeout must be 0..60000 ms");
  if (owner->multi->poll(owner->multi, (int)timeout, &ready, &error) !=
      VECTIS_OK)
    return curl_lua_error(L, &error, "poll");
  lua_pushinteger(L, ready);
  return 1;
}
static int curl_lua_completions(lua_State *L) {
  curl_lua_owner *owner = (curl_lua_owner *)luaL_checkudata(L, 1, CURL_MULTI);
  const vectis_curl_result *result;
  vectis_error error;
  size_t count = 0u, i;
  lua_Integer limit;
  int output, slots[VECTIS_CURL_MULTI_LIMIT];
  curl_lua_request *request;
  luaL_argcheck(L, owner->multi != NULL, 1, "curl multi is closed");
  limit = luaL_optinteger(L, 2, VECTIS_CURL_MULTI_LIMIT);
  luaL_argcheck(L, limit >= 1 && limit <= VECTIS_CURL_MULTI_LIMIT, 2,
                "completion limit must be 1..64");
  lua_createtable(L, (int)limit, 0);
  output = lua_gettop(L);
  while (count < (size_t)limit) {
    if (owner->multi->completion(owner->multi, count, &result, &error) !=
        VECTIS_OK)
      return curl_lua_error(L, &error, "completions");
    if (result == NULL)
      break;
    request = (curl_lua_request *)result->context;
    slots[count] = request->slot;
    if (request->output != NULL) {
      int code = fclose(request->output);
      request->output = NULL;
      if (code != 0)
        request->file_failure = 1;
    }
    curl_lua_result(L, result);
    lua_rawseti(L, output, (lua_Integer)++count);
  }
  if (owner->violated)
    return luaL_error(L, "curl result copy attempted receiver reentry/close");
  if (owner->multi->consume(owner->multi, count, &error) != VECTIS_OK)
    return curl_lua_error(L, &error, "consume");
  lua_getiuservalue(L, 1, 1);
  for (i = 0u; i < count; ++i) {
    lua_rawgeti(L, -1, slots[i]);
    curl_lua_request_cleanup(
        (curl_lua_request *)luaL_checkudata(L, -1, CURL_REQUEST));
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_rawseti(L, -2, slots[i]);
  }
  lua_pop(L, 1);
  return 1;
}
static int curl_lua_guard(lua_State *L) {
  curl_lua_owner *owner;
  vectis_error error;
  size_t running;
  int count = lua_gettop(L), status;
  luaL_checkstack(L, 24, "curl method stack");
  owner = (curl_lua_owner *)luaL_testudata(L, 1, CURL_SESSION);
  if (owner == NULL)
    owner = (curl_lua_owner *)luaL_checkudata(L, 1, CURL_MULTI);
  luaL_argcheck(L, owner->process == getpid(), 1,
                "curl handle belongs to another process");
  if (owner->busy) {
    owner->violated = 1;
    if (owner->native_active && owner->multi != NULL)
      (void)owner->multi->step(owner->multi, &running, &error);
    else if (owner->native_active && owner->session != NULL)
      (void)owner->session->reset(owner->session, &error);
    return luaL_error(L, "cannot reenter active curl receiver");
  }
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  owner->busy = 1;
  owner->violated = 0;
  owner->L = L;
  status = lua_pcall(L, count, LUA_MULTRET, 0);
  owner->L = NULL;
  owner->busy = 0;
  if (status != LUA_OK)
    return lua_error(L);
  return lua_gettop(L);
}
static void curl_lua_method(lua_State *L, const char *name, lua_CFunction fn) {
  lua_pushcfunction(L, fn);
  lua_pushcclosure(L, curl_lua_guard, 1);
  lua_setfield(L, -2, name);
}
static void curl_lua_metatable(lua_State *L, const char *name) {
  luaL_newmetatable(L, name);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, curl_lua_close, 1);
  lua_setfield(L, -2, "close");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, curl_lua_close, 1);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "__gc");
  lua_setfield(L, -2, "__close");
}
void vectis_lua_curl_register(lua_State *L) {
  int module = lua_absindex(L, -1);
  curl_lua_metatable(L, CURL_SESSION);
  curl_lua_method(L, "perform", curl_lua_perform);
  curl_lua_method(L, "reset", curl_lua_reset);
  lua_pop(L, 1);
  curl_lua_metatable(L, CURL_MULTI);
  curl_lua_method(L, "add", curl_lua_add);
  curl_lua_method(L, "step", curl_lua_step);
  curl_lua_method(L, "poll", curl_lua_poll);
  curl_lua_method(L, "completions", curl_lua_completions);
  lua_pop(L, 1);
  if (luaL_newmetatable(L, CURL_REQUEST)) {
    lua_pushcfunction(L, curl_lua_request_gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_pop(L, 1);
  lua_pushboolean(L, 0);
  lua_pushcclosure(L, curl_lua_new, 1);
  lua_setfield(L, module, "session");
  lua_pushboolean(L, 1);
  lua_pushcclosure(L, curl_lua_new, 1);
  lua_setfield(L, module, "multi");
}
int luaopen_vectis_curl_handles_core(lua_State *L) {
  lua_newtable(L);
  vectis_lua_curl_register(L);
  return 1;
}
