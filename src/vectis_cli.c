#include "vectis_cli.h"

#include <cpkt/lua_runtime.h>
#include <errno.h>
#include <lauxlib.h>
#include <lua.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vectis/auth.h>
#include <vectis/totp_qr.h>
#include <vectis/vectis.h>
#include <vectis/vectis_version.h>

#include "vectis_libmdf_lua_init.h"
#include "vectis_lockdc_lua_init.h"
#include "vectis_pslog_lua_init.h"

#define VECTIS_PACK_FOOTER_SIZE 128u
#define VECTIS_PACK_MAGIC "VECTIS_PACK_V1"
#define VECTIS_PACK_MAGIC_SIZE 14u

typedef struct vectis_lua_runtime_context {
  const unsigned char *embedded_lockd_bundle;
  size_t embedded_lockd_bundle_size;
} vectis_lua_runtime_context;

typedef struct vectis_lua_totp {
  vectis_totp value;
} vectis_lua_totp;

typedef struct vectis_lua_qr {
  vectis_qr value;
} vectis_lua_qr;

#define VECTIS_LUA_TOTP "vectis.totp"
#define VECTIS_LUA_QR "vectis.qr"

extern int luaopen_lonejson_core(lua_State *lua);
extern int luaopen_lockdc_core(lua_State *lua);
extern int luaopen_cai(lua_State *lua);
extern int luaopen_libmdf_core(lua_State *lua);
extern int luaopen_pslog_core(lua_State *lua);
extern int luaopen_softline(lua_State *lua);

static const char vectis_lonejson_lua_init[] =
    "local core = require(\"lonejson.core\")\n"
    "local M = {}\n"
    "local function field(kind, opts)\n"
    "  opts = opts or {}\n"
    "  opts.kind = kind\n"
    "  return opts\n"
    "end\n"
    "function M.field(name, spec)\n"
    "  spec = spec or {}\n"
    "  spec.name = name\n"
    "  return spec\n"
    "end\n"
    "function M.string(opts) return field(\"string\", opts) end\n"
    "function M.spooled_text(opts) return field(\"spooled_text\", opts) end\n"
    "function M.spooled_bytes(opts) return field(\"spooled_bytes\", opts) end\n"
    "function M.json_value(opts) return field(\"json_value\", opts) end\n"
    "function M.i64(opts) return field(\"i64\", opts) end\n"
    "function M.u64(opts) return field(\"u64\", opts) end\n"
    "function M.f64(opts) return field(\"f64\", opts) end\n"
    "function M.boolean(opts) return field(\"boolean\", opts) end\n"
    "M.bool = M.boolean\n"
    "function M.object(opts) return field(\"object\", opts) end\n"
    "function M.string_array(opts) return field(\"string_array\", opts) end\n"
    "function M.i64_array(opts) return field(\"i64_array\", opts) end\n"
    "function M.u64_array(opts) return field(\"u64_array\", opts) end\n"
    "function M.f64_array(opts) return field(\"f64_array\", opts) end\n"
    "function M.boolean_array(opts) return field(\"boolean_array\", opts) end\n"
    "function M.object_array(opts) return field(\"object_array\", opts) end\n"
    "function M.json_array(value)\n"
    "  value = value or {}\n"
    "  return setmetatable(value, { __lonejson_json_kind = \"array\" })\n"
    "end\n"
    "function M.json_object(value)\n"
    "  value = value or {}\n"
    "  return setmetatable(value, { __lonejson_json_kind = \"object\" })\n"
    "end\n"
    "function M.schema(name, fields) return core.compile_schema(name, fields) "
    "end\n"
    "function M.chunks(spool, chunk_size)\n"
    "  spool:rewind()\n"
    "  return function() return spool:read(chunk_size or 4096) end\n"
    "end\n"
    "M.array_rewrite_string = core.array_rewrite_string\n"
    "M.array_rewrite_path = core.array_rewrite_path\n"
    "M.encode_json = core.encode_json\n"
    "M.encode_json_to_sink = core.encode_json_to_sink\n"
    "M.encode_value = core.encode_json\n"
    "M.encode_value_to_sink = core.encode_json_to_sink\n"
    "M.decode_json = core.decode_json\n"
    "M.decode_value = core.decode_json\n"
    "M.core = core\n"
    "M.json_null = core.json_null()\n"
    "return M\n";

static void vectis_cli_usage(FILE *stream) {
  fputs("usage: vectis [--version] [--help] [-x] script.lua [args...]\n"
        "       vectis pack --script script.lua --output output "
        "[--lockd-bundle bundle.pem]\n"
        "       vectis -a credentials [--store credentials.json] "
        "(--init | --issue --subject user [--purpose name] "
        "[--basic] [--bearer] | --verify authorization | "
        "--revoke client_id)\n"
        "       vectis -a users [--store credentials.json] "
        "(--add username [--password value] [--totp] | "
        "--login username --password value [--totp-code code] | "
        "--webdav-key username --password value [--totp-code code])\n",
        stream);
}

static void vectis_pack_write_u64(unsigned char *out,
                                  unsigned long long value) {
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

static int vectis_read_all(const char *path, unsigned char **out,
                           size_t *out_size) {
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

static const char *vectis_cli_auth_mode_name(unsigned mode) {
  if ((mode & VECTIS_AUTH_MODE_BASIC) != 0u) {
    return "basic";
  }
  if ((mode & VECTIS_AUTH_MODE_BEARER) != 0u) {
    return "bearer";
  }
  return "default";
}

static int vectis_cli_credentials_default_path(char *out, size_t out_size) {
  const char *config_dir;
  const char *home;
  int written;

  config_dir = getenv("VECTIS_CONFIG_DIR");
  if (config_dir != NULL && config_dir[0] != '\0') {
    written = snprintf(out, out_size, "%s/credentials.json", config_dir);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
  }
  config_dir = getenv("XDG_CONFIG_HOME");
  if (config_dir != NULL && config_dir[0] != '\0') {
    written = snprintf(out, out_size, "%s/vectis/credentials.json", config_dir);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
  }
  home = getenv("HOME");
  if (home == NULL || home[0] == '\0') {
    return -1;
  }
  written = snprintf(out, out_size, "%s/.config/vectis/credentials.json", home);
  return written > 0 && (size_t)written < out_size ? 0 : -1;
}

static int vectis_cli_auth_status(vectis_status status,
                                  const vectis_error *error) {
  const char *message;

  message = error != NULL && error->message[0] != '\0'
                ? error->message
                : vectis_status_string(status);
  fprintf(stderr, "vectis: %s\n", message != NULL ? message : "auth failed");
  return status == VECTIS_ERR_NOMEM ? 70 : 1;
}

static int vectis_cli_credentials_command(int argc, char **argv, int index) {
  vectis_auth_store_config store;
  vectis_auth_issue_config issue;
  vectis_auth_issued_credential credential;
  vectis_auth_result result;
  vectis_error error;
  vectis_status status;
  char default_path[4096];
  const char *action;
  const char *authorization;
  const char *revoke_client_id;
  unsigned explicit_modes;

  if (vectis_cli_credentials_default_path(default_path, sizeof(default_path)) !=
      0) {
    fputs("vectis: unable to resolve default credentials path\n", stderr);
    return 1;
  }
  vectis_auth_store_config_init(&store);
  store.credentials_path = default_path;
  vectis_auth_issue_config_init(&issue);
  action = NULL;
  authorization = NULL;
  revoke_client_id = NULL;
  explicit_modes = 0u;
  while (index < argc) {
    if (strcmp(argv[index], "--store") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --store requires a path\n", stderr);
        return 64;
      }
      store.credentials_path = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--init") == 0) {
      action = "init";
      index++;
    } else if (strcmp(argv[index], "--issue") == 0) {
      action = "issue";
      index++;
    } else if (strcmp(argv[index], "--verify") == 0 ||
               strcmp(argv[index], "--authorization") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --verify requires an Authorization header value\n",
              stderr);
        return 64;
      }
      action = "verify";
      authorization = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--revoke") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --revoke requires a client id\n", stderr);
        return 64;
      }
      action = "revoke";
      revoke_client_id = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--subject") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --subject requires a value\n", stderr);
        return 64;
      }
      issue.subject = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--purpose") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --purpose requires a value\n", stderr);
        return 64;
      }
      issue.purpose = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--basic") == 0) {
      explicit_modes |= VECTIS_AUTH_MODE_BASIC;
      index++;
    } else if (strcmp(argv[index], "--bearer") == 0) {
      explicit_modes |= VECTIS_AUTH_MODE_BEARER;
      index++;
    } else {
      fprintf(stderr, "vectis: unknown credentials option: %s\n", argv[index]);
      return 64;
    }
  }
  vectis_error_clear(&error);
  if (action == NULL) {
    fputs("vectis: credentials requires --init, --issue, --verify, or "
          "--revoke\n",
          stderr);
    return 64;
  }
  if (strcmp(action, "init") == 0) {
    status = vectis_auth_store_init(&store, &error);
    if (status != VECTIS_OK) {
      return vectis_cli_auth_status(status, &error);
    }
    printf("initialized=%s\n", store.credentials_path);
    return 0;
  }
  if (strcmp(action, "issue") == 0) {
    if (explicit_modes != 0u) {
      issue.auth_modes = explicit_modes;
    }
    vectis_auth_issued_credential_init(&credential);
    status = vectis_auth_issue_credential(&store, &issue, &credential, &error);
    if (status != VECTIS_OK) {
      return vectis_cli_auth_status(status, &error);
    }
    if (credential.client_id != NULL) {
      printf("client_id=%s\n", credential.client_id);
    }
    if (credential.client_secret != NULL) {
      printf("client_secret=%s\n", credential.client_secret);
    }
    if (credential.api_key != NULL) {
      printf("api_key=%s\n", credential.api_key);
    }
    if (credential.claim_json != NULL) {
      printf("claim_json=%s\n", credential.claim_json);
    }
    vectis_auth_issued_credential_cleanup(&credential);
    return 0;
  }
  if (strcmp(action, "verify") == 0) {
    vectis_auth_result_init(&result);
    status = vectis_auth_verify_authorization(
        &store, authorization,
        explicit_modes != 0u ? explicit_modes : VECTIS_AUTH_MODE_DEFAULT,
        &result, &error);
    if (status != VECTIS_OK) {
      return vectis_cli_auth_status(status, &error);
    }
    printf("authenticated=%s\n", result.authenticated ? "true" : "false");
    printf("auth_mode=%s\n", vectis_cli_auth_mode_name(result.auth_mode));
    if (result.client_id != NULL) {
      printf("client_id=%s\n", result.client_id);
    }
    if (result.claim_json != NULL) {
      printf("claim_json=%s\n", result.claim_json);
    }
    vectis_auth_result_cleanup(&result);
    return 0;
  }
  status = vectis_auth_revoke_client(&store, revoke_client_id, &error);
  if (status != VECTIS_OK) {
    return vectis_cli_auth_status(status, &error);
  }
  printf("revoked=%s\n", revoke_client_id);
  return 0;
}

static int vectis_cli_users_time_arg(const char *value, uint64_t *out) {
  char *end;
  unsigned long long parsed;

  if (value == NULL || out == NULL) {
    return -1;
  }
  errno = 0;
  end = NULL;
  parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || end == NULL || *end != '\0') {
    return -1;
  }
  *out = (uint64_t)parsed;
  return 0;
}

static int
vectis_cli_print_credential(const vectis_auth_issued_credential *credential) {
  if (credential->client_id != NULL) {
    printf("client_id=%s\n", credential->client_id);
  }
  if (credential->client_secret != NULL) {
    printf("client_secret=%s\n", credential->client_secret);
  }
  if (credential->api_key != NULL) {
    printf("api_key=%s\n", credential->api_key);
  }
  if (credential->claim_json != NULL) {
    printf("claim_json=%s\n", credential->claim_json);
  }
  return 0;
}

static int vectis_cli_users_command(int argc, char **argv, int index) {
  vectis_auth_store_config store;
  vectis_auth_user_config user;
  vectis_auth_user_enrollment enrollment;
  vectis_auth_login_config login;
  vectis_auth_result result;
  vectis_auth_issued_credential credential;
  vectis_error error;
  vectis_status status;
  char default_path[4096];
  const char *action;

  if (vectis_cli_credentials_default_path(default_path, sizeof(default_path)) !=
      0) {
    fputs("vectis: unable to resolve default credentials path\n", stderr);
    return 1;
  }
  vectis_auth_store_config_init(&store);
  store.credentials_path = default_path;
  vectis_auth_user_config_init(&user);
  vectis_auth_login_config_init(&login);
  action = NULL;
  while (index < argc) {
    if (strcmp(argv[index], "--store") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --store requires a path\n", stderr);
        return 64;
      }
      store.credentials_path = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--add") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --add requires a username\n", stderr);
        return 64;
      }
      action = "add";
      user.username = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--login") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --login requires a username\n", stderr);
        return 64;
      }
      action = "login";
      login.username = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--webdav-key") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --webdav-key requires a username\n", stderr);
        return 64;
      }
      action = "webdav-key";
      login.username = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--password") == 0 ||
               strcmp(argv[index], "-p") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --password requires a value\n", stderr);
        return 64;
      }
      user.password = argv[index + 1];
      login.password = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--totp") == 0) {
      user.enable_totp = 1;
      index++;
    } else if (strcmp(argv[index], "--totp-secret") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --totp-secret requires a value\n", stderr);
        return 64;
      }
      user.enable_totp = 1;
      user.totp_secret = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--totp-code") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --totp-code requires a value\n", stderr);
        return 64;
      }
      login.totp_code = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--issuer") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --issuer requires a value\n", stderr);
        return 64;
      }
      user.totp_issuer = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--label") == 0) {
      if (index + 1 >= argc) {
        fputs("vectis: --label requires a value\n", stderr);
        return 64;
      }
      user.totp_label = argv[index + 1];
      index += 2;
    } else if (strcmp(argv[index], "--time") == 0) {
      if (index + 1 >= argc || vectis_cli_users_time_arg(
                                   argv[index + 1], &login.unix_seconds) != 0) {
        fputs("vectis: --time requires a non-negative integer\n", stderr);
        return 64;
      }
      index += 2;
    } else if (strcmp(argv[index], "--window") == 0) {
      uint64_t parsed;

      if (index + 1 >= argc ||
          vectis_cli_users_time_arg(argv[index + 1], &parsed) != 0 ||
          parsed > 10u) {
        fputs("vectis: --window requires an integer from 0 to 10\n", stderr);
        return 64;
      }
      login.totp_window = (unsigned int)parsed;
      index += 2;
    } else {
      fprintf(stderr, "vectis: unknown users option: %s\n", argv[index]);
      return 64;
    }
  }
  vectis_error_clear(&error);
  if (action == NULL) {
    fputs("vectis: users requires --add, --login, or --webdav-key\n", stderr);
    return 64;
  }
  if (strcmp(action, "add") == 0) {
    vectis_auth_user_enrollment_init(&enrollment);
    status = vectis_auth_user_add_or_update(&store, &user, &enrollment, &error);
    if (status != VECTIS_OK) {
      return vectis_cli_auth_status(status, &error);
    }
    printf("username=%s\n", enrollment.username);
    if (enrollment.generated_password != NULL) {
      printf("password=%s\n", enrollment.generated_password);
    }
    if (enrollment.totp_secret != NULL) {
      printf("totp_secret=%s\n", enrollment.totp_secret);
    }
    if (enrollment.totp_uri != NULL) {
      printf("totp_uri=%s\n", enrollment.totp_uri);
    }
    if (enrollment.totp_qr_ansi != NULL) {
      fputs("totp_qr:\n", stdout);
      fputs(enrollment.totp_qr_ansi, stdout);
    }
    vectis_auth_user_enrollment_cleanup(&enrollment);
    return 0;
  }
  if (strcmp(action, "login") == 0) {
    vectis_auth_result_init(&result);
    status = vectis_auth_user_login(&store, &login, &result, &error);
    if (status != VECTIS_OK) {
      return vectis_cli_auth_status(status, &error);
    }
    printf("authenticated=%s\n", result.authenticated ? "true" : "false");
    if (result.claim_json != NULL) {
      printf("claim_json=%s\n", result.claim_json);
    }
    vectis_auth_result_cleanup(&result);
    return 0;
  }
  vectis_auth_issued_credential_init(&credential);
  status = vectis_auth_issue_webdav_key_for_login(&store, &login, &credential,
                                                  &error);
  if (status != VECTIS_OK) {
    return vectis_cli_auth_status(status, &error);
  }
  (void)vectis_cli_print_credential(&credential);
  vectis_auth_issued_credential_cleanup(&credential);
  return 0;
}

static int vectis_admin_command(int argc, char **argv, int index) {
  const char *operation;

  if (index >= argc) {
    fputs("vectis: -a/--admin-operation requires an operation\n", stderr);
    return 64;
  }
  operation = argv[index];
  index++;
  if (strcmp(operation, "credentials") == 0) {
    return vectis_cli_credentials_command(argc, argv, index);
  }
  if (strcmp(operation, "users") == 0) {
    return vectis_cli_users_command(argc, argv, index);
  }
  fprintf(stderr, "vectis: unknown admin operation: %s\n", operation);
  return 64;
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
  if (bundle_path != NULL &&
      vectis_read_all(bundle_path, &bundle, &bundle_size) != 0) {
    fprintf(stderr, "vectis: failed to read lockd bundle: %s\n",
            strerror(errno));
    free(self);
    free(script);
    return 1;
  }
  SHA256(script, script_size, script_sha);
  memset(bundle_sha, 0, sizeof(bundle_sha));
  if (bundle != NULL) {
    SHA256(bundle, bundle_size, bundle_sha);
  }
  vectis_pack_make_footer(footer, (unsigned long long)script_size, script_sha,
                          (unsigned long long)bundle_size,
                          bundle != NULL ? bundle_sha : NULL);
  out = fopen(output_path, "wb");
  if (out == NULL) {
    fprintf(stderr, "vectis: failed to create packed output: %s\n",
            output_path);
    free(bundle);
    free(script);
    free(self);
    return 1;
  }
  if (vectis_write_all(out, self, self_size) != 0 ||
      vectis_write_all(out, script, script_size) != 0 ||
      vectis_write_all(out, bundle, bundle_size) != 0 ||
      vectis_write_all(out, footer, sizeof(footer)) != 0 || fclose(out) != 0) {
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

static int vectis_lua_has_embedded_lockd_bundle(lua_State *lua) {
  vectis_lua_runtime_context *context;

  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  lua_pushboolean(lua, context != NULL &&
                           context->embedded_lockd_bundle != NULL &&
                           context->embedded_lockd_bundle_size > 0u);
  return 1;
}

static int vectis_lua_embedded_lockd_bundle_size(lua_State *lua) {
  vectis_lua_runtime_context *context;

  context = (vectis_lua_runtime_context *)cpkt_lua_runtime_context_from_state(
      (void *)lua);
  lua_pushinteger(lua, context != NULL
                           ? (lua_Integer)context->embedded_lockd_bundle_size
                           : (lua_Integer)0);
  return 1;
}

static int vectis_lua_push_error(lua_State *lua, vectis_status status,
                                 const vectis_error *error) {
  const char *message;

  message = error != NULL && error->message[0] != '\0'
                ? error->message
                : vectis_status_string(status);
  lua_pushnil(lua);
  lua_newtable(lua);
  lua_pushinteger(lua, (lua_Integer)status);
  lua_setfield(lua, -2, "status");
  lua_pushstring(lua, vectis_status_string(status));
  lua_setfield(lua, -2, "status_string");
  lua_pushstring(lua, message != NULL ? message : "vectis error");
  lua_setfield(lua, -2, "message");
  return 2;
}

static const char *vectis_lua_table_string(lua_State *lua, int index,
                                           const char *field) {
  const char *value;

  lua_getfield(lua, index, field);
  value = lua_isnil(lua, -1) ? NULL : luaL_checkstring(lua, -1);
  lua_pop(lua, 1);
  return value;
}

static size_t vectis_lua_table_size(lua_State *lua, int index,
                                    const char *field, size_t fallback) {
  size_t value;

  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    value = fallback;
  } else {
    value = (size_t)luaL_checkinteger(lua, -1);
  }
  lua_pop(lua, 1);
  return value;
}

static int vectis_lua_table_bool(lua_State *lua, int index, const char *field,
                                 int fallback) {
  int value;

  lua_getfield(lua, index, field);
  if (lua_isnil(lua, -1)) {
    value = fallback;
  } else {
    value = lua_toboolean(lua, -1);
  }
  lua_pop(lua, 1);
  return value;
}

static unsigned vectis_lua_auth_mode_value(const char *mode) {
  if (mode == NULL || strcmp(mode, "default") == 0) {
    return VECTIS_AUTH_MODE_DEFAULT;
  }
  if (strcmp(mode, "basic") == 0) {
    return VECTIS_AUTH_MODE_BASIC;
  }
  if (strcmp(mode, "bearer") == 0) {
    return VECTIS_AUTH_MODE_BEARER;
  }
  return VECTIS_AUTH_MODE_DEFAULT;
}

static unsigned vectis_lua_auth_modes_at(lua_State *lua, int index,
                                         unsigned fallback) {
  unsigned modes;
  size_t count;
  size_t i;

  if (lua_isnil(lua, index)) {
    return fallback;
  }
  if (lua_istable(lua, index)) {
    modes = 0u;
    count = lua_rawlen(lua, index);
    for (i = 1u; i <= count; ++i) {
      lua_rawgeti(lua, index, (lua_Integer)i);
      if (lua_isnumber(lua, -1)) {
        modes |= (unsigned)lua_tointeger(lua, -1);
      } else {
        modes |= vectis_lua_auth_mode_value(luaL_checkstring(lua, -1));
      }
      lua_pop(lua, 1);
    }
    return modes != 0u ? modes : fallback;
  }
  if (lua_isnumber(lua, index)) {
    return (unsigned)lua_tointeger(lua, index);
  }
  return vectis_lua_auth_mode_value(luaL_checkstring(lua, index));
}

static unsigned vectis_lua_auth_modes_field(lua_State *lua, int index,
                                            const char *field,
                                            unsigned fallback) {
  unsigned modes;

  lua_getfield(lua, index, field);
  modes = vectis_lua_auth_modes_at(lua, -1, fallback);
  lua_pop(lua, 1);
  return modes;
}

static const char *vectis_lua_auth_mode_name(unsigned mode) {
  if ((mode & VECTIS_AUTH_MODE_BASIC) != 0u) {
    return "basic";
  }
  if ((mode & VECTIS_AUTH_MODE_BEARER) != 0u) {
    return "bearer";
  }
  return "default";
}

static void vectis_lua_auth_store_config(lua_State *lua, int index,
                                         vectis_auth_store_config *config) {
  const char *path;

  vectis_auth_store_config_init(config);
  index = lua_absindex(lua, index);
  path = vectis_lua_table_string(lua, index, "credentials_path");
  if (path == NULL) {
    path = vectis_lua_table_string(lua, index, "path");
  }
  config->credentials_path = path;
  config->max_store_bytes = vectis_lua_table_size(
      lua, index, "max_store_bytes", VECTIS_AUTH_DEFAULT_MAX_STORE_BYTES);
}

static void vectis_lua_auth_push_result(lua_State *lua,
                                        const vectis_auth_result *result) {
  lua_newtable(lua);
  lua_pushboolean(lua, result != NULL && result->authenticated);
  lua_setfield(lua, -2, "authenticated");
  lua_pushstring(lua, result != NULL
                          ? vectis_lua_auth_mode_name(result->auth_mode)
                          : "default");
  lua_setfield(lua, -2, "auth_mode");
  if (result != NULL && result->client_id != NULL) {
    lua_pushstring(lua, result->client_id);
    lua_setfield(lua, -2, "client_id");
  }
  if (result != NULL && result->claim_json != NULL) {
    lua_pushstring(lua, result->claim_json);
    lua_setfield(lua, -2, "claim_json");
  }
}

static void vectis_lua_auth_push_issued_credential(
    lua_State *lua, const vectis_auth_issued_credential *credential) {
  lua_newtable(lua);
  if (credential != NULL && credential->client_id != NULL) {
    lua_pushstring(lua, credential->client_id);
    lua_setfield(lua, -2, "client_id");
  }
  if (credential != NULL && credential->client_secret != NULL) {
    lua_pushstring(lua, credential->client_secret);
    lua_setfield(lua, -2, "client_secret");
  }
  if (credential != NULL && credential->api_key != NULL) {
    lua_pushstring(lua, credential->api_key);
    lua_setfield(lua, -2, "api_key");
  }
  if (credential != NULL && credential->claim_json != NULL) {
    lua_pushstring(lua, credential->claim_json);
    lua_setfield(lua, -2, "claim_json");
  }
}

static void vectis_lua_auth_push_provider_response(
    lua_State *lua, const vectis_auth_provider_response *response) {
  lua_newtable(lua);
  switch (response != NULL ? response->action : VECTIS_AUTH_DENY) {
  case VECTIS_AUTH_ALLOW:
    lua_pushliteral(lua, "allow");
    break;
  case VECTIS_AUTH_REQUIRED:
    lua_pushliteral(lua, "required");
    break;
  case VECTIS_AUTH_REDIRECT:
    lua_pushliteral(lua, "redirect");
    break;
  case VECTIS_AUTH_DENY:
  default:
    lua_pushliteral(lua, "deny");
    break;
  }
  lua_setfield(lua, -2, "action");
  lua_pushinteger(lua, response != NULL ? (lua_Integer)response->status_code
                                        : (lua_Integer)0);
  lua_setfield(lua, -2, "status_code");
  if (response != NULL && response->location != NULL) {
    lua_pushstring(lua, response->location);
    lua_setfield(lua, -2, "location");
  }
  if (response != NULL && response->www_authenticate[0] != '\0') {
    lua_pushstring(lua, response->www_authenticate);
    lua_setfield(lua, -2, "www_authenticate");
  }
  if (response != NULL && response->principal[0] != '\0') {
    lua_pushstring(lua, response->principal);
    lua_setfield(lua, -2, "principal");
  }
  if (response != NULL) {
    vectis_lua_auth_push_result(lua, &response->result);
    lua_setfield(lua, -2, "result");
  }
}

static int vectis_lua_auth_store_init(lua_State *lua) {
  vectis_auth_store_config config;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &config);
  status = vectis_auth_store_init(&config, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static int vectis_lua_auth_issue(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_auth_issue_config issue;
  vectis_auth_issued_credential credential;
  vectis_error error;
  vectis_status status;
  const char *purpose;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  vectis_auth_issue_config_init(&issue);
  issue.subject = vectis_lua_table_string(lua, 1, "subject");
  purpose = vectis_lua_table_string(lua, 1, "purpose");
  if (purpose != NULL) {
    issue.purpose = purpose;
  }
  issue.auth_modes =
      vectis_lua_auth_modes_field(lua, 1, "modes", VECTIS_AUTH_MODE_BEARER);
  issue.max_record_bytes =
      vectis_lua_table_size(lua, 1, "max_record_bytes", 0u);
  vectis_auth_issued_credential_init(&credential);
  status = vectis_auth_issue_credential(&store, &issue, &credential, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_auth_push_issued_credential(lua, &credential);
  vectis_auth_issued_credential_cleanup(&credential);
  return 1;
}

static int vectis_lua_auth_verify(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_auth_result result;
  vectis_error error;
  vectis_status status;
  const char *authorization;
  unsigned modes;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  authorization = vectis_lua_table_string(lua, 1, "authorization");
  modes = vectis_lua_auth_modes_field(lua, 1, "allowed_modes",
                                      VECTIS_AUTH_MODE_DEFAULT);
  vectis_auth_result_init(&result);
  status = vectis_auth_verify_authorization(&store, authorization, modes,
                                            &result, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_auth_push_result(lua, &result);
  vectis_auth_result_cleanup(&result);
  return 1;
}

static int vectis_lua_auth_revoke(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_error error;
  vectis_status status;
  const char *client_id;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  client_id = vectis_lua_table_string(lua, 1, "client_id");
  status = vectis_auth_revoke_client(&store, client_id, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_pushboolean(lua, 1);
  return 1;
}

static void vectis_lua_auth_user_config(lua_State *lua, int index,
                                        vectis_auth_user_config *config) {
  const char *issuer;

  vectis_auth_user_config_init(config);
  config->username = vectis_lua_table_string(lua, index, "username");
  config->password = vectis_lua_table_string(lua, index, "password");
  config->enable_totp = vectis_lua_table_bool(lua, index, "totp", 0);
  config->totp_secret = vectis_lua_table_string(lua, index, "totp_secret");
  if (config->totp_secret != NULL) {
    config->enable_totp = 1;
  }
  config->totp_label = vectis_lua_table_string(lua, index, "totp_label");
  issuer = vectis_lua_table_string(lua, index, "totp_issuer");
  if (issuer == NULL) {
    issuer = vectis_lua_table_string(lua, index, "issuer");
  }
  if (issuer != NULL) {
    config->totp_issuer = issuer;
  }
}

static void vectis_lua_auth_login_config(lua_State *lua, int index,
                                         vectis_auth_login_config *config) {
  vectis_auth_login_config_init(config);
  config->username = vectis_lua_table_string(lua, index, "username");
  config->password = vectis_lua_table_string(lua, index, "password");
  config->totp_code = vectis_lua_table_string(lua, index, "totp_code");
  config->unix_seconds =
      (uint64_t)vectis_lua_table_size(lua, index, "time", 0u);
  config->totp_window =
      (unsigned int)vectis_lua_table_size(lua, index, "window", 1u);
}

static int vectis_lua_auth_user_add(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_auth_user_config user;
  vectis_auth_user_enrollment enrollment;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  vectis_lua_auth_user_config(lua, 1, &user);
  vectis_auth_user_enrollment_init(&enrollment);
  status = vectis_auth_user_add_or_update(&store, &user, &enrollment, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_newtable(lua);
  if (enrollment.username != NULL) {
    lua_pushstring(lua, enrollment.username);
    lua_setfield(lua, -2, "username");
  }
  if (enrollment.generated_password != NULL) {
    lua_pushstring(lua, enrollment.generated_password);
    lua_setfield(lua, -2, "password");
  }
  if (enrollment.totp_secret != NULL) {
    lua_pushstring(lua, enrollment.totp_secret);
    lua_setfield(lua, -2, "totp_secret");
  }
  if (enrollment.totp_uri != NULL) {
    lua_pushstring(lua, enrollment.totp_uri);
    lua_setfield(lua, -2, "totp_uri");
  }
  if (enrollment.totp_qr_ansi != NULL) {
    lua_pushstring(lua, enrollment.totp_qr_ansi);
    lua_setfield(lua, -2, "totp_qr");
  }
  vectis_auth_user_enrollment_cleanup(&enrollment);
  return 1;
}

static int vectis_lua_auth_user_login(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_auth_login_config login;
  vectis_auth_result result;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  vectis_lua_auth_login_config(lua, 1, &login);
  vectis_auth_result_init(&result);
  status = vectis_auth_user_login(&store, &login, &result, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_auth_push_result(lua, &result);
  vectis_auth_result_cleanup(&result);
  return 1;
}

static int vectis_lua_auth_webdav_key(lua_State *lua) {
  vectis_auth_store_config store;
  vectis_auth_login_config login;
  vectis_auth_issued_credential credential;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_lua_auth_store_config(lua, 1, &store);
  vectis_lua_auth_login_config(lua, 1, &login);
  vectis_auth_issued_credential_init(&credential);
  status = vectis_auth_issue_webdav_key_for_login(&store, &login, &credential,
                                                  &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_auth_push_issued_credential(lua, &credential);
  vectis_auth_issued_credential_cleanup(&credential);
  return 1;
}

static int vectis_lua_auth_oidc_authorization(lua_State *lua) {
  vectis_auth_oidc_authorization_config config;
  vectis_auth_oidc_authorization authorization;
  vectis_error error;
  vectis_status status;

  luaL_checktype(lua, 1, LUA_TTABLE);
  vectis_error_clear(&error);
  vectis_auth_oidc_authorization_config_init(&config);
  config.authorization_endpoint =
      vectis_lua_table_string(lua, 1, "authorization_endpoint");
  config.client_id = vectis_lua_table_string(lua, 1, "client_id");
  config.redirect_uri = vectis_lua_table_string(lua, 1, "redirect_uri");
  config.scope = vectis_lua_table_string(lua, 1, "scope");
  config.state = vectis_lua_table_string(lua, 1, "state");
  config.nonce = vectis_lua_table_string(lua, 1, "nonce");
  config.code_verifier = vectis_lua_table_string(lua, 1, "code_verifier");
  config.code_challenge = vectis_lua_table_string(lua, 1, "code_challenge");
  config.audience = vectis_lua_table_string(lua, 1, "audience");
  config.resource = vectis_lua_table_string(lua, 1, "resource");
  config.verifier_bytes = vectis_lua_table_size(lua, 1, "verifier_bytes", 0u);
  config.max_url_bytes = vectis_lua_table_size(lua, 1, "max_url_bytes", 0u);
  vectis_auth_oidc_authorization_init(&authorization);
  status =
      vectis_auth_oidc_authorization_start(&config, &authorization, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  lua_newtable(lua);
  if (authorization.authorization_url != NULL) {
    lua_pushstring(lua, authorization.authorization_url);
    lua_setfield(lua, -2, "authorization_url");
  }
  if (authorization.code_verifier != NULL) {
    lua_pushstring(lua, authorization.code_verifier);
    lua_setfield(lua, -2, "code_verifier");
  }
  if (authorization.code_challenge != NULL) {
    lua_pushstring(lua, authorization.code_challenge);
    lua_setfield(lua, -2, "code_challenge");
  }
  if (authorization.state != NULL) {
    lua_pushstring(lua, authorization.state);
    lua_setfield(lua, -2, "state");
  }
  if (authorization.nonce != NULL) {
    lua_pushstring(lua, authorization.nonce);
    lua_setfield(lua, -2, "nonce");
  }
  vectis_auth_oidc_authorization_cleanup(&authorization);
  return 1;
}

static int vectis_lua_auth_native_provider_authenticate(lua_State *lua) {
  vectis_auth_native_provider_config config;
  vectis_auth_provider provider;
  vectis_auth_provider_request request;
  vectis_auth_provider_response response;
  vectis_error error;
  vectis_status status;
  int request_index;

  luaL_checktype(lua, 1, LUA_TTABLE);
  request_index = lua_gettop(lua) >= 2 && !lua_isnil(lua, 2) ? 2 : 0;
  vectis_error_clear(&error);
  vectis_auth_native_provider_config_init(&config);
  vectis_lua_auth_store_config(lua, 1, &config.store);
  config.purpose = vectis_lua_table_string(lua, 1, "purpose");
  config.realm = vectis_lua_table_string(lua, 1, "realm");
  config.allowed_auth_modes = vectis_lua_auth_modes_field(
      lua, 1, "allowed_modes", VECTIS_AUTH_MODE_DEFAULT);
  if (config.realm == NULL) {
    config.realm = "vectis";
  }
  vectis_auth_provider_init(&provider);
  status = vectis_auth_provider_from_native_store(&provider, &config, &error);
  if (status != VECTIS_OK) {
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_auth_provider_request_init(&request);
  if (request_index != 0) {
    luaL_checktype(lua, request_index, LUA_TTABLE);
    request.authorization =
        vectis_lua_table_string(lua, request_index, "authorization");
    request.purpose = vectis_lua_table_string(lua, request_index, "purpose");
    request.resource = vectis_lua_table_string(lua, request_index, "resource");
    request.allowed_auth_modes = vectis_lua_auth_modes_field(
        lua, request_index, "allowed_modes", VECTIS_AUTH_MODE_DEFAULT);
  }
  vectis_auth_provider_response_init(&response);
  status =
      vectis_auth_provider_authenticate(&provider, &request, &response, &error);
  if (status != VECTIS_OK) {
    vectis_auth_provider_response_cleanup(&response);
    return vectis_lua_push_error(lua, status, &error);
  }
  vectis_lua_auth_push_provider_response(lua, &response);
  vectis_auth_provider_response_cleanup(&response);
  return 1;
}

static int vectis_lua_auth_provider_native(lua_State *lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  lua_newtable(lua);
  lua_pushliteral(lua, "native");
  lua_setfield(lua, -2, "kind");
  lua_pushvalue(lua, 1);
  lua_setfield(lua, -2, "config");
  lua_getfield(lua, 1, "credentials_path");
  lua_setfield(lua, -2, "credentials_path");
  lua_getfield(lua, 1, "path");
  lua_setfield(lua, -2, "path");
  lua_getfield(lua, 1, "max_store_bytes");
  lua_setfield(lua, -2, "max_store_bytes");
  lua_getfield(lua, 1, "purpose");
  lua_setfield(lua, -2, "purpose");
  lua_getfield(lua, 1, "realm");
  lua_setfield(lua, -2, "realm");
  lua_getfield(lua, 1, "allowed_modes");
  lua_setfield(lua, -2, "allowed_modes");
  lua_pushcfunction(lua, vectis_lua_auth_native_provider_authenticate);
  lua_setfield(lua, -2, "authenticate");
  return 1;
}

static int vectis_lua_auth_callback_provider_authenticate(lua_State *lua) {
  int base;

  luaL_checktype(lua, 1, LUA_TTABLE);
  if (lua_gettop(lua) < 2) {
    lua_newtable(lua);
  } else {
    luaL_checktype(lua, 2, LUA_TTABLE);
  }
  base = lua_gettop(lua);
  lua_getfield(lua, 1, "callback");
  luaL_checktype(lua, -1, LUA_TFUNCTION);
  lua_pushvalue(lua, 2);
  lua_call(lua, 1, LUA_MULTRET);
  return lua_gettop(lua) - base;
}

static int vectis_lua_auth_provider_callback(lua_State *lua) {
  luaL_checktype(lua, 1, LUA_TFUNCTION);
  lua_newtable(lua);
  lua_pushliteral(lua, "callback");
  lua_setfield(lua, -2, "kind");
  lua_pushvalue(lua, 1);
  lua_setfield(lua, -2, "callback");
  lua_pushcfunction(lua, vectis_lua_auth_callback_provider_authenticate);
  lua_setfield(lua, -2, "authenticate");
  return 1;
}

static vectis_lua_totp *vectis_lua_check_totp(lua_State *lua, int index) {
  return (vectis_lua_totp *)luaL_checkudata(lua, index, VECTIS_LUA_TOTP);
}

static vectis_lua_qr *vectis_lua_check_qr(lua_State *lua, int index) {
  return (vectis_lua_qr *)luaL_checkudata(lua, index, VECTIS_LUA_QR);
}

static uint64_t vectis_lua_totp_time(lua_State *lua, int index) {
  lua_Integer value;

  if (lua_isnoneornil(lua, index)) {
    return (uint64_t)time(NULL);
  }
  value = luaL_checkinteger(lua, index);
  if (value < 0) {
    luaL_error(lua, "TOTP unix time must not be negative");
  }
  return (uint64_t)value;
}

static int vectis_lua_totp_new(lua_State *lua) {
  vectis_lua_totp *totp;
  vectis_totp_qr_status status;
  const char *secret;

  secret = luaL_checkstring(lua, 1);
  totp = (vectis_lua_totp *)lua_newuserdata(lua, sizeof(*totp));
  status = vectis_totp_init(&totp->value, secret);
  if (status != VECTIS_TOTP_QR_OK) {
    return luaL_error(lua, "TOTP secret is invalid: %s",
                      vectis_totp_qr_status_string(status));
  }
  luaL_getmetatable(lua, VECTIS_LUA_TOTP);
  lua_setmetatable(lua, -2);
  return 1;
}

static int vectis_lua_totp_secret(lua_State *lua) {
  vectis_lua_totp *totp;

  totp = vectis_lua_check_totp(lua, 1);
  lua_pushstring(lua, totp->value.secret);
  return 1;
}

static int vectis_lua_totp_generate(lua_State *lua) {
  vectis_lua_totp *totp;
  vectis_totp_qr_status status;
  char code[VECTIS_TOTP_CODE_LENGTH + 1u];

  totp = vectis_lua_check_totp(lua, 1);
  status =
      vectis_totp_generate(&totp->value, vectis_lua_totp_time(lua, 2), code);
  if (status != VECTIS_TOTP_QR_OK) {
    return luaL_error(lua, "TOTP generation failed: %s",
                      vectis_totp_qr_status_string(status));
  }
  lua_pushstring(lua, code);
  return 1;
}

static int vectis_lua_totp_validate(lua_State *lua) {
  vectis_lua_totp *totp;
  lua_Integer window;
  const char *code;

  totp = vectis_lua_check_totp(lua, 1);
  code = luaL_checkstring(lua, 2);
  window = lua_isnoneornil(lua, 4) ? 1 : luaL_checkinteger(lua, 4);
  if (window < 0 || window > 10) {
    return luaL_error(lua, "TOTP validation window must be between 0 and 10");
  }
  lua_pushboolean(lua, vectis_totp_validate(&totp->value, code,
                                            vectis_lua_totp_time(lua, 3),
                                            (unsigned int)window));
  return 1;
}

static int vectis_lua_qr_ansi_value(lua_State *lua, const vectis_qr *qr) {
  vectis_totp_qr_status status;
  char *rendered;
  size_t rendered_len;

  rendered = NULL;
  rendered_len = 0u;
  status = vectis_qr_render_ansi(qr, &rendered, &rendered_len);
  if (status != VECTIS_TOTP_QR_OK) {
    return luaL_error(lua, "terminal QR rendering failed: %s",
                      vectis_totp_qr_status_string(status));
  }
  lua_pushlstring(lua, rendered, rendered_len);
  vectis_totp_qr_free(rendered);
  return 1;
}

static int vectis_lua_totp_uri(lua_State *lua) {
  vectis_lua_totp *totp;
  vectis_totp_qr_status status;
  const char *label;
  const char *issuer;
  char *uri;

  totp = vectis_lua_check_totp(lua, 1);
  label = luaL_checkstring(lua, 2);
  issuer = luaL_checkstring(lua, 3);
  uri = NULL;
  status = vectis_totp_uri(&totp->value, label, issuer, &uri);
  if (status != VECTIS_TOTP_QR_OK) {
    return luaL_error(lua, "TOTP URI generation failed: %s",
                      vectis_totp_qr_status_string(status));
  }
  lua_pushstring(lua, uri);
  vectis_totp_qr_free(uri);
  return 1;
}

static int vectis_lua_totp_qr(lua_State *lua) {
  vectis_lua_totp *totp;
  vectis_totp_qr_status status;
  const char *label;
  const char *issuer;
  vectis_qr qr;

  totp = vectis_lua_check_totp(lua, 1);
  label = luaL_checkstring(lua, 2);
  issuer = luaL_checkstring(lua, 3);
  status = vectis_totp_enrollment_qr(&totp->value, label, issuer, &qr);
  if (status != VECTIS_TOTP_QR_OK) {
    return luaL_error(lua, "TOTP QR generation failed: %s",
                      vectis_totp_qr_status_string(status));
  }
  return vectis_lua_qr_ansi_value(lua, &qr);
}

static int vectis_lua_qr_new(lua_State *lua) {
  vectis_lua_qr *qr;
  vectis_totp_qr_status status;
  const char *text;
  size_t text_len;

  text = luaL_checkstring(lua, 1);
  text_len = strlen(text);
  qr = (vectis_lua_qr *)lua_newuserdata(lua, sizeof(*qr));
  status = vectis_qr_encode(&qr->value, (const unsigned char *)text, text_len);
  if (status != VECTIS_TOTP_QR_OK) {
    return luaL_error(lua, "QR text is invalid: %s",
                      vectis_totp_qr_status_string(status));
  }
  luaL_getmetatable(lua, VECTIS_LUA_QR);
  lua_setmetatable(lua, -2);
  return 1;
}

static int vectis_lua_qr_ansi(lua_State *lua) {
  vectis_lua_qr *qr;

  qr = vectis_lua_check_qr(lua, 1);
  return vectis_lua_qr_ansi_value(lua, &qr->value);
}

static int vectis_lua_qr_size(lua_State *lua) {
  vectis_lua_qr *qr;

  qr = vectis_lua_check_qr(lua, 1);
  lua_pushinteger(lua, (lua_Integer)vectis_qr_size(&qr->value));
  return 1;
}

static void vectis_lua_register_totp_qr(lua_State *lua) {
  if (luaL_newmetatable(lua, VECTIS_LUA_TOTP)) {
    lua_newtable(lua);
    lua_pushcfunction(lua, vectis_lua_totp_secret);
    lua_setfield(lua, -2, "secret");
    lua_pushcfunction(lua, vectis_lua_totp_generate);
    lua_setfield(lua, -2, "generate");
    lua_pushcfunction(lua, vectis_lua_totp_validate);
    lua_setfield(lua, -2, "validate");
    lua_pushcfunction(lua, vectis_lua_totp_uri);
    lua_setfield(lua, -2, "uri");
    lua_pushcfunction(lua, vectis_lua_totp_qr);
    lua_setfield(lua, -2, "qr");
    lua_setfield(lua, -2, "__index");
  }
  lua_pop(lua, 1);
  if (luaL_newmetatable(lua, VECTIS_LUA_QR)) {
    lua_newtable(lua);
    lua_pushcfunction(lua, vectis_lua_qr_ansi);
    lua_setfield(lua, -2, "ansi");
    lua_pushcfunction(lua, vectis_lua_qr_size);
    lua_setfield(lua, -2, "size");
    lua_setfield(lua, -2, "__index");
  }
  lua_pop(lua, 1);
}

static void vectis_lua_push_auth_table(lua_State *lua) {
  lua_newtable(lua);
  lua_pushinteger(lua, VECTIS_AUTH_MODE_BASIC);
  lua_setfield(lua, -2, "BASIC");
  lua_pushinteger(lua, VECTIS_AUTH_MODE_BEARER);
  lua_setfield(lua, -2, "BEARER");
  lua_pushcfunction(lua, vectis_lua_auth_store_init);
  lua_setfield(lua, -2, "store_init");
  lua_pushcfunction(lua, vectis_lua_auth_issue);
  lua_setfield(lua, -2, "issue");
  lua_pushcfunction(lua, vectis_lua_auth_verify);
  lua_setfield(lua, -2, "verify");
  lua_pushcfunction(lua, vectis_lua_auth_revoke);
  lua_setfield(lua, -2, "revoke");
  lua_pushcfunction(lua, vectis_lua_auth_user_add);
  lua_setfield(lua, -2, "user_add");
  lua_pushcfunction(lua, vectis_lua_auth_user_login);
  lua_setfield(lua, -2, "user_login");
  lua_pushcfunction(lua, vectis_lua_auth_webdav_key);
  lua_setfield(lua, -2, "webdav_key");
  lua_pushcfunction(lua, vectis_lua_auth_oidc_authorization);
  lua_setfield(lua, -2, "oidc_authorization");
  lua_pushcfunction(lua, vectis_lua_auth_provider_native);
  lua_setfield(lua, -2, "provider_native");
  lua_pushcfunction(lua, vectis_lua_auth_provider_callback);
  lua_setfield(lua, -2, "provider_callback");
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_lua_totp_new);
  lua_setfield(lua, -2, "new");
  lua_setfield(lua, -2, "totp");
  lua_newtable(lua);
  lua_pushcfunction(lua, vectis_lua_qr_new);
  lua_setfield(lua, -2, "new");
  lua_setfield(lua, -2, "qr");
}

static int luaopen_vectis(lua_State *lua) {
  vectis_lua_register_totp_qr(lua);
  lua_newtable(lua);
  lua_pushliteral(lua, VECTIS_VERSION);
  lua_setfield(lua, -2, "version");
  lua_pushinteger(lua, VECTIS_OK);
  lua_setfield(lua, -2, "OK");
  lua_pushinteger(lua, VECTIS_ERR_INVALID);
  lua_setfield(lua, -2, "ERR_INVALID");
  lua_pushinteger(lua, VECTIS_ERR_TIMEOUT);
  lua_setfield(lua, -2, "ERR_TIMEOUT");
  lua_pushcfunction(lua, vectis_lua_status_string);
  lua_setfield(lua, -2, "status_string");
  lua_pushcfunction(lua, vectis_lua_has_embedded_lockd_bundle);
  lua_setfield(lua, -2, "has_embedded_lockd_bundle");
  lua_pushcfunction(lua, vectis_lua_embedded_lockd_bundle_size);
  lua_setfield(lua, -2, "embedded_lockd_bundle_size");
  vectis_lua_push_auth_table(lua);
  lua_setfield(lua, -2, "auth");
  return 1;
}

static int vectis_luaopen_vectis(void *lua_state) {
  return luaopen_vectis((lua_State *)lua_state);
}

static int vectis_luaopen_lockdc_core(void *lua_state) {
  return luaopen_lockdc_core((lua_State *)lua_state);
}

static int vectis_luaopen_lonejson_core(void *lua_state) {
  return luaopen_lonejson_core((lua_State *)lua_state);
}

static int vectis_luaopen_cai(void *lua_state) {
  return luaopen_cai((lua_State *)lua_state);
}

static int vectis_luaopen_libmdf_core(void *lua_state) {
  return luaopen_libmdf_core((lua_State *)lua_state);
}

static int vectis_luaopen_pslog_core(void *lua_state) {
  return luaopen_pslog_core((lua_State *)lua_state);
}

static int vectis_luaopen_softline(void *lua_state) {
  return luaopen_softline((lua_State *)lua_state);
}

static int vectis_lua_report_status(cpkt_lua_runtime *runtime,
                                    cpkt_lua_runtime_status status) {
  const char *message;

  if (status == CPKT_LUA_RUNTIME_OK) {
    return 0;
  }
  message = runtime != NULL ? cpkt_lua_runtime_error(runtime) : NULL;
  if (message == NULL || message[0] == '\0') {
    message = cpkt_lua_runtime_status_string(status);
  }
  fprintf(stderr, "vectis: %s\n", message);
  if (status == CPKT_LUA_RUNTIME_ERR_ALLOC) {
    return 70;
  }
  return 1;
}

static cpkt_lua_runtime_status
vectis_lua_register_modules(cpkt_lua_runtime *runtime) {
  cpkt_lua_runtime_status status;

  status = cpkt_lua_runtime_register_c_module(runtime, "vectis",
                                              vectis_luaopen_vectis);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_c_module(runtime, "lockdc.core",
                                              vectis_luaopen_lockdc_core);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "lockdc", vectis_lockdc_lua_init, sizeof(vectis_lockdc_lua_init),
      "lockdc.init");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_c_module(runtime, "lonejson.core",
                                              vectis_luaopen_lonejson_core);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "lonejson", (const unsigned char *)vectis_lonejson_lua_init,
      sizeof(vectis_lonejson_lua_init) - 1u, "lonejson.init");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status =
      cpkt_lua_runtime_register_c_module(runtime, "cai", vectis_luaopen_cai);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_c_module(runtime, "pslog.core",
                                              vectis_luaopen_pslog_core);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "pslog", vectis_pslog_lua_init, sizeof(vectis_pslog_lua_init),
      "pslog.init");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_c_module(runtime, "libmdf.core",
                                              vectis_luaopen_libmdf_core);
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  status = cpkt_lua_runtime_register_lua_module(
      runtime, "libmdf", vectis_libmdf_lua_init, sizeof(vectis_libmdf_lua_init),
      "libmdf.init");
  if (status != CPKT_LUA_RUNTIME_OK) {
    return status;
  }
  return cpkt_lua_runtime_register_c_module(runtime, "softline",
                                            vectis_luaopen_softline);
}

static int vectis_lua_prepare_runtime(cpkt_lua_runtime **out,
                                      vectis_lua_runtime_context *context) {
  cpkt_lua_runtime *runtime;
  cpkt_lua_runtime_status status;
  int rc;

  runtime = NULL;
  status = cpkt_lua_runtime_new(&runtime);
  if (status == CPKT_LUA_RUNTIME_OK) {
    cpkt_lua_runtime_set_context(runtime, context);
    status = cpkt_lua_runtime_openlibs(runtime);
  }
  if (status == CPKT_LUA_RUNTIME_OK) {
    status = vectis_lua_register_modules(runtime);
  }
  if (status != CPKT_LUA_RUNTIME_OK) {
    rc = vectis_lua_report_status(runtime, status);
    cpkt_lua_runtime_free(runtime);
    return rc;
  }
  *out = runtime;
  return 0;
}

static int
vectis_lua_run_buffer(const char *script_name, const unsigned char *script,
                      size_t script_size, const unsigned char *lockd_bundle,
                      size_t lockd_bundle_size, int argc, char **argv) {
  cpkt_lua_runtime *runtime;
  vectis_lua_runtime_context context;
  const unsigned char *load_script;
  size_t load_size;
  cpkt_lua_runtime_status status;
  int rc;

  context.embedded_lockd_bundle = lockd_bundle;
  context.embedded_lockd_bundle_size = lockd_bundle_size;
  rc = vectis_lua_prepare_runtime(&runtime, &context);
  if (rc != 0) {
    return rc;
  }

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
  status = cpkt_lua_runtime_run_buffer(
      runtime, load_script, load_size, script_name, argc > 0 ? argc - 1 : 0,
      argc > 0 ? (const char *const *)(argv + 1) : NULL, 0);
  rc = vectis_lua_report_status(runtime, status);
  cpkt_lua_runtime_free(runtime);
  return rc;
}

static int vectis_lua_run_script(int argc, char **argv, int script_index) {
  cpkt_lua_runtime *runtime;
  vectis_lua_runtime_context context;
  cpkt_lua_runtime_status status;
  int rc;

  memset(&context, 0, sizeof(context));
  rc = vectis_lua_prepare_runtime(&runtime, &context);
  if (rc != 0) {
    return rc;
  }

  status = cpkt_lua_runtime_run_file(
      runtime, argv[script_index], argc - script_index - 1,
      (const char *const *)(argv + script_index + 1), 0);
  rc = vectis_lua_report_status(runtime, status);
  cpkt_lua_runtime_free(runtime);
  return rc;
}

static int vectis_lua_run_embedded(int argc, char **argv) {
  unsigned char *self;
  unsigned char *script;
  unsigned char *bundle;
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
  memcpy(footer, self + self_size - VECTIS_PACK_FOOTER_SIZE,
         VECTIS_PACK_FOOTER_SIZE);
  if (!vectis_pack_footer_valid(footer)) {
    free(self);
    return -1;
  }
  script_size = (size_t)vectis_pack_read_u64(footer + 16u);
  bundle_size = (size_t)vectis_pack_read_u64(footer + 24u);
  if (script_size == 0u || script_size > self_size || bundle_size > self_size ||
      script_size + bundle_size + VECTIS_PACK_FOOTER_SIZE > self_size) {
    free(self);
    fputs("vectis: embedded payload is invalid\n", stderr);
    return 1;
  }
  script_offset =
      self_size - VECTIS_PACK_FOOTER_SIZE - bundle_size - script_size;
  script = self + script_offset;
  bundle = script + script_size;
  SHA256(script, script_size, actual_sha);
  if (memcmp(actual_sha, footer + 32u, SHA256_DIGEST_LENGTH) != 0) {
    free(self);
    fputs("vectis: embedded Lua script hash mismatch\n", stderr);
    return 1;
  }
  if (bundle_size > 0u) {
    SHA256(bundle, bundle_size, actual_sha);
    if (memcmp(actual_sha, footer + 64u, SHA256_DIGEST_LENGTH) != 0) {
      free(self);
      fputs("vectis: embedded lockd bundle hash mismatch\n", stderr);
      return 1;
    }
  }
  rc = vectis_lua_run_buffer(argv[0], script, script_size,
                             bundle_size > 0u ? bundle : NULL, bundle_size,
                             argc, argv);
  free(self);
  return rc;
}

int vectis_cli_main(int argc, char **argv) {
  int rc;

  rc = vectis_lua_run_embedded(argc, argv);
  if (rc >= 0) {
    return rc;
  }

  if (argc > 1 && strcmp(argv[1], "--help") == 0) {
    vectis_cli_usage(stdout);
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "--version") == 0) {
    puts("vectis " VECTIS_VERSION);
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "pack") == 0) {
    return vectis_pack_command(argc, argv);
  }
  if (argc > 1 && (strcmp(argv[1], "-a") == 0 ||
                   strcmp(argv[1], "--admin-operation") == 0)) {
    return vectis_admin_command(argc, argv, 2);
  }
  if (argc > 1 && strcmp(argv[1], "-x") == 0) {
    fputs("vectis: Lua execution tracing (-x) is not supported by the "
          "current Lua runtime facade\n",
          stderr);
    return 64;
  }

  if (argc > 1) {
    return vectis_lua_run_script(argc, argv, 1);
  }
  vectis_cli_usage(stderr);
  return 64;
}
