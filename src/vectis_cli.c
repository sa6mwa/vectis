#include "vectis_cli.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <openssl/sha.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vectis/vectis.h>

#define VECTIS_PACK_FOOTER_SIZE 128u
#define VECTIS_PACK_MAGIC "VECTIS_PACK_V1"
#define VECTIS_PACK_MAGIC_SIZE 14u

static void vectis_cli_usage(FILE *stream) {
  fputs("usage: vectis [--version] [--help] script.lua [args...]\n"
        "       vectis pack --script script.lua --output output [--lockd-bundle bundle.pem]\n",
        stream);
}

static void vectis_pack_write_u64(unsigned char *out, unsigned long long value) {
  int i;

  for (i = 0; i < 8; ++i) {
    out[i] = (unsigned char)((value >> (8 * i)) & 0xffu);
  }
}

static unsigned long long vectis_pack_read_u64(const unsigned char *in) {
  unsigned long long value;
  int i;

  value = 0u;
  for (i = 0; i < 8; ++i) {
    value |= ((unsigned long long)in[i]) << (8 * i);
  }
  return value;
}

static int vectis_read_all(const char *path, unsigned char **out, size_t *out_size) {
  FILE *fp;
  long length;
  unsigned char *buffer;
  size_t nread;

  if (path == NULL || out == NULL || out_size == NULL) {
    return -1;
  }
  *out = NULL;
  *out_size = 0u;
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return -1;
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    (void)fclose(fp);
    return -1;
  }
  length = ftell(fp);
  if (length < 0L || fseek(fp, 0L, SEEK_SET) != 0) {
    (void)fclose(fp);
    return -1;
  }
  buffer = NULL;
  if (length > 0L) {
    buffer = (unsigned char *)malloc((size_t)length);
    if (buffer == NULL) {
      (void)fclose(fp);
      return -1;
    }
    nread = fread(buffer, 1u, (size_t)length, fp);
    if (nread != (size_t)length) {
      free(buffer);
      (void)fclose(fp);
      return -1;
    }
  }
  if (fclose(fp) != 0) {
    free(buffer);
    return -1;
  }
  *out = buffer;
  *out_size = (size_t)length;
  return 0;
}

static int vectis_write_all(FILE *fp, const void *data, size_t size) {
  if (size == 0u) {
    return 0;
  }
  return fwrite(data, 1u, size, fp) == size ? 0 : -1;
}

static void vectis_pack_make_footer(unsigned char *footer,
                                    unsigned long long script_size,
                                    const unsigned char *script_sha,
                                    unsigned long long bundle_size,
                                    const unsigned char *bundle_sha) {
  memset(footer, 0, VECTIS_PACK_FOOTER_SIZE);
  memcpy(footer, VECTIS_PACK_MAGIC, VECTIS_PACK_MAGIC_SIZE);
  vectis_pack_write_u64(footer + 16u, script_size);
  vectis_pack_write_u64(footer + 24u, bundle_size);
  memcpy(footer + 32u, script_sha, SHA256_DIGEST_LENGTH);
  if (bundle_sha != NULL) {
    memcpy(footer + 64u, bundle_sha, SHA256_DIGEST_LENGTH);
  }
}

static int vectis_pack_footer_valid(const unsigned char *footer) {
  return memcmp(footer, VECTIS_PACK_MAGIC, VECTIS_PACK_MAGIC_SIZE) == 0;
}

static int vectis_self_path(const char *argv0, char *path, size_t path_size) {
#ifdef __linux__
  ssize_t nread;

  nread = readlink("/proc/self/exe", path, path_size - 1u);
  if (nread > 0 && (size_t)nread < path_size) {
    path[nread] = '\0';
    return 0;
  }
#endif
  if (argv0 == NULL || strlen(argv0) + 1u > path_size) {
    return -1;
  }
  memcpy(path, argv0, strlen(argv0) + 1u);
  return 0;
}

static int vectis_pack_command(int argc, char **argv) {
  const char *script_path;
  const char *output_path;
  const char *bundle_path;
  unsigned char *self;
  unsigned char *script;
  unsigned char *bundle;
  size_t self_size;
  size_t script_size;
  size_t bundle_size;
  unsigned char script_sha[SHA256_DIGEST_LENGTH];
  unsigned char bundle_sha[SHA256_DIGEST_LENGTH];
  unsigned char footer[VECTIS_PACK_FOOTER_SIZE];
  char self_path[4096];
  FILE *out;
  int i;

  script_path = NULL;
  output_path = NULL;
  bundle_path = NULL;
  for (i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
      script_path = argv[++i];
    } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      output_path = argv[++i];
    } else if (strcmp(argv[i], "--lockd-bundle") == 0 && i + 1 < argc) {
      bundle_path = argv[++i];
    } else {
      fprintf(stderr, "vectis: unknown pack argument: %s\n", argv[i]);
      return 64;
    }
  }
  if (script_path == NULL || output_path == NULL) {
    fputs("vectis: pack requires --script and --output\n", stderr);
    return 64;
  }
  if (vectis_self_path(argv[0], self_path, sizeof(self_path)) != 0) {
    fputs("vectis: failed to resolve current executable path\n", stderr);
    return 1;
  }
  self = NULL;
  script = NULL;
  bundle = NULL;
  bundle_size = 0u;
  if (vectis_read_all(self_path, &self, &self_size) != 0 ||
      vectis_read_all(script_path, &script, &script_size) != 0) {
    fprintf(stderr, "vectis: failed to read pack input: %s\n", strerror(errno));
    free(self);
    free(script);
    return 1;
  }
  if (bundle_path != NULL && vectis_read_all(bundle_path, &bundle, &bundle_size) != 0) {
    fprintf(stderr, "vectis: failed to read lockd bundle: %s\n", strerror(errno));
    free(self);
    free(script);
    return 1;
  }
  SHA256(script, script_size, script_sha);
  memset(bundle_sha, 0, sizeof(bundle_sha));
  if (bundle != NULL) {
    SHA256(bundle, bundle_size, bundle_sha);
  }
  vectis_pack_make_footer(footer,
                          (unsigned long long)script_size,
                          script_sha,
                          (unsigned long long)bundle_size,
                          bundle != NULL ? bundle_sha : NULL);
  out = fopen(output_path, "wb");
  if (out == NULL) {
    fprintf(stderr, "vectis: failed to create packed output: %s\n", output_path);
    free(bundle);
    free(script);
    free(self);
    return 1;
  }
  if (vectis_write_all(out, self, self_size) != 0 ||
      vectis_write_all(out, script, script_size) != 0 ||
      vectis_write_all(out, bundle, bundle_size) != 0 ||
      vectis_write_all(out, footer, sizeof(footer)) != 0 ||
      fclose(out) != 0) {
    fprintf(stderr, "vectis: failed to write packed output: %s\n", output_path);
    free(bundle);
    free(script);
    free(self);
    return 1;
  }
  if (chmod(output_path, 0755) != 0) {
    fprintf(stderr, "vectis: failed to chmod packed output: %s\n", output_path);
    free(bundle);
    free(script);
    free(self);
    return 1;
  }
  free(bundle);
  free(script);
  free(self);
  return 0;
}

static int vectis_lua_status_string(lua_State *lua) {
  lua_Integer status;
  const char *name;

  status = luaL_checkinteger(lua, 1);
  name = vectis_status_string((vectis_status)status);
  if (name == NULL) {
    lua_pushnil(lua);
  } else {
    lua_pushstring(lua, name);
  }
  return 1;
}

static int luaopen_vectis(lua_State *lua) {
  lua_newtable(lua);
  lua_pushliteral(lua, "0.0.0");
  lua_setfield(lua, -2, "version");
  lua_pushinteger(lua, VECTIS_OK);
  lua_setfield(lua, -2, "OK");
  lua_pushinteger(lua, VECTIS_ERR_INVALID);
  lua_setfield(lua, -2, "ERR_INVALID");
  lua_pushcfunction(lua, vectis_lua_status_string);
  lua_setfield(lua, -2, "status_string");
  return 1;
}

static void vectis_lua_preload(lua_State *lua) {
  lua_getglobal(lua, "package");
  lua_getfield(lua, -1, "preload");
  lua_pushcfunction(lua, luaopen_vectis);
  lua_setfield(lua, -2, "vectis");
  lua_pop(lua, 2);
}

static void vectis_lua_set_arg(lua_State *lua,
                               int argc,
                               char **argv,
                               int script_index,
                               const char *script_name) {
  int i;

  lua_newtable(lua);
  lua_pushstring(lua, script_name != NULL ? script_name : argv[script_index]);
  lua_rawseti(lua, -2, 0);
  if (script_index >= 0) {
    for (i = script_index + 1; i < argc; ++i) {
      lua_pushstring(lua, argv[i]);
      lua_rawseti(lua, -2, i - script_index);
    }
  }
  lua_setglobal(lua, "arg");
}

static int vectis_lua_run_loaded(lua_State *lua, int status) {
  if (status == LUA_OK) {
    status = lua_pcall(lua, 0, LUA_MULTRET, 0);
  }
  if (status != LUA_OK) {
    fprintf(stderr, "vectis: %s\n", lua_tostring(lua, -1));
    lua_close(lua);
    return 1;
  }
  lua_close(lua);
  return 0;
}

static int vectis_lua_run_buffer(const char *script_name,
                                 const unsigned char *script,
                                 size_t script_size,
                                 int argc,
                                 char **argv) {
  lua_State *lua;
  const unsigned char *load_script;
  size_t load_size;
  int status;

  lua = luaL_newstate();
  if (lua == NULL) {
    fputs("vectis: failed to allocate Lua state\n", stderr);
    return 70;
  }
  luaL_openlibs(lua);
  vectis_lua_preload(lua);
  vectis_lua_set_arg(lua, argc, argv, -1, script_name);

  load_script = script;
  load_size = script_size;
  if (load_size >= 2u && load_script[0] == '#' && load_script[1] == '!') {
    while (load_size > 0u && *load_script != '\n') {
      load_script++;
      load_size--;
    }
    if (load_size > 0u) {
      load_script++;
      load_size--;
    }
  }
  status = luaL_loadbuffer(lua, (const char *)load_script, load_size, script_name);
  return vectis_lua_run_loaded(lua, status);
}

static int vectis_lua_run_script(int argc, char **argv, int script_index) {
  lua_State *lua;
  int status;

  lua = luaL_newstate();
  if (lua == NULL) {
    fputs("vectis: failed to allocate Lua state\n", stderr);
    return 70;
  }
  luaL_openlibs(lua);
  vectis_lua_preload(lua);
  vectis_lua_set_arg(lua, argc, argv, script_index, NULL);

  status = luaL_loadfile(lua, argv[script_index]);
  return vectis_lua_run_loaded(lua, status);
}

static int vectis_lua_run_embedded(int argc, char **argv) {
  unsigned char *self;
  unsigned char *script;
  unsigned char footer[VECTIS_PACK_FOOTER_SIZE];
  unsigned char actual_sha[SHA256_DIGEST_LENGTH];
  size_t self_size;
  size_t script_size;
  size_t bundle_size;
  size_t script_offset;
  char self_path[4096];
  int rc;

  if (vectis_self_path(argv[0], self_path, sizeof(self_path)) != 0) {
    return -1;
  }
  self = NULL;
  if (vectis_read_all(self_path, &self, &self_size) != 0) {
    return -1;
  }
  if (self_size < VECTIS_PACK_FOOTER_SIZE) {
    free(self);
    return -1;
  }
  memcpy(footer, self + self_size - VECTIS_PACK_FOOTER_SIZE, VECTIS_PACK_FOOTER_SIZE);
  if (!vectis_pack_footer_valid(footer)) {
    free(self);
    return -1;
  }
  script_size = (size_t)vectis_pack_read_u64(footer + 16u);
  bundle_size = (size_t)vectis_pack_read_u64(footer + 24u);
  if (script_size == 0u ||
      script_size > self_size ||
      bundle_size > self_size ||
      script_size + bundle_size + VECTIS_PACK_FOOTER_SIZE > self_size) {
    free(self);
    fputs("vectis: embedded payload is invalid\n", stderr);
    return 1;
  }
  script_offset = self_size - VECTIS_PACK_FOOTER_SIZE - bundle_size - script_size;
  script = self + script_offset;
  SHA256(script, script_size, actual_sha);
  if (memcmp(actual_sha, footer + 32u, SHA256_DIGEST_LENGTH) != 0) {
    free(self);
    fputs("vectis: embedded Lua script hash mismatch\n", stderr);
    return 1;
  }
  rc = vectis_lua_run_buffer(argv[0], script, script_size, argc, argv);
  free(self);
  return rc;
}

int vectis_cli_main(int argc, char **argv) {
  int rc;

  if (argc > 1 && strcmp(argv[1], "--help") == 0) {
    vectis_cli_usage(stdout);
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "--version") == 0) {
    puts("vectis 0.0.0");
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "pack") == 0) {
    return vectis_pack_command(argc, argv);
  }

  if (argc > 1) {
    return vectis_lua_run_script(argc, argv, 1);
  }
  rc = vectis_lua_run_embedded(argc, argv);
  if (rc >= 0) {
    return rc;
  }
  vectis_cli_usage(stderr);
  return 64;
}
