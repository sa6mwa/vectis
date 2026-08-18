#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vectis/vectis.h>

static const char *env_or_default(const char *name, const char *fallback) {
  const char *value;

  value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  return value;
}

static unsigned short env_port_or_default(const char *name,
                                          unsigned short fallback) {
  const char *value;
  long port;

  value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return fallback;
  }
  port = strtol(value, NULL, 10);
  if (port <= 0L || port > 65535L) {
    return fallback;
  }
  return (unsigned short)port;
}

static int print_error(const char *operation, const vectis_error *error) {
  fprintf(stderr, "%s failed", operation);
  if (error != NULL && error->message[0] != '\0') {
    fprintf(stderr, ": %s", error->message);
  }
  if (error != NULL && error->detail[0] != '\0') {
    fprintf(stderr, " (%s)", error->detail);
  }
  fprintf(stderr, "\n");
  return 1;
}

static int read_file(const char *path, char **out, size_t *out_size) {
  FILE *fp;
  long length;
  char *buffer;

  *out = NULL;
  *out_size = 0u;
  fp = fopen(path, "rb");
  if (fp == NULL) {
    return 0;
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    (void)fclose(fp);
    return 0;
  }
  length = ftell(fp);
  if (length <= 0L || fseek(fp, 0L, SEEK_SET) != 0) {
    (void)fclose(fp);
    return 0;
  }
  buffer = (char *)malloc((size_t)length);
  if (buffer == NULL) {
    (void)fclose(fp);
    return 0;
  }
  if (fread(buffer, 1u, (size_t)length, fp) != (size_t)length) {
    (void)fclose(fp);
    free(buffer);
    return 0;
  }
  (void)fclose(fp);
  *out = buffer;
  *out_size = (size_t)length;
  return 1;
}

int main(void) {
  vectis_ssh_config config;
  vectis_ssh *ssh;
  vectis_ssh_exec_result result = {0};
  vectis_error error;
  const char *command;
  const char *memory_key_path;
  const char *password_env;
  char *memory_key;
  size_t memory_key_size;
  const char expected[] = "vectis-ssh-ok";

  ssh = NULL;
  memory_key = NULL;
  memory_key_size = 0u;
  vectis_ssh_config_init(&config);
  vectis_error_clear(&error);

  config.host = env_or_default("VECTIS_SSH_HOST", "127.0.0.1");
  config.port = env_port_or_default("VECTIS_SSH_PORT", 29222u);
  config.username = env_or_default("VECTIS_SSH_USERNAME", "vectis");
  password_env = getenv("VECTIS_SSH_PASSWORD");
  config.password = password_env != NULL && password_env[0] != '\0'
                        ? password_env
                        : "vectispass";
  config.private_key_path = getenv("VECTIS_SSH_PRIVATE_KEY");
  memory_key_path = getenv("VECTIS_SSH_PRIVATE_KEY_MEMORY_FILE");
  if (memory_key_path != NULL && memory_key_path[0] != '\0') {
    if (!read_file(memory_key_path, &memory_key, &memory_key_size)) {
      fprintf(stderr, "failed to read memory private key: %s\n",
              memory_key_path);
      return 1;
    }
    config.private_key = vectis_source_from_memory(memory_key, memory_key_size);
    config.private_key_path = NULL;
    if (password_env == NULL || password_env[0] == '\0') {
      config.password = NULL;
    }
  }
  config.known_hosts_path = getenv("VECTIS_SSH_KNOWN_HOSTS");
  config.host_key_sha256 = getenv("VECTIS_SSH_HOST_KEY_SHA256");
  config.timeout_ms = 10000L;
  command = env_or_default("VECTIS_SSH_COMMAND", "printf vectis-ssh-ok");

  if (vectis_ssh_new(&config, &ssh, &error) != VECTIS_OK) {
    free(memory_key);
    return print_error("vectis_ssh_new", &error);
  }
  if (ssh->exec(ssh, command, &result, &error) != VECTIS_OK) {
    ssh->close(ssh);
    free(memory_key);
    return print_error("ssh->exec", &error);
  }
  if (result.exit_status != 0) {
    fprintf(stderr, "SSH command exited with status %d\n", result.exit_status);
    vectis_ssh_exec_result_cleanup(&result);
    ssh->close(ssh);
    free(memory_key);
    return 1;
  }
  if (strcmp(command, "printf vectis-ssh-ok") == 0 &&
      (result.stdout_size != strlen(expected) ||
       memcmp(result.stdout_data, expected, strlen(expected)) != 0)) {
    fprintf(stderr, "SSH command returned unexpected stdout\n");
    vectis_ssh_exec_result_cleanup(&result);
    ssh->close(ssh);
    free(memory_key);
    return 1;
  }
  vectis_ssh_exec_result_cleanup(&result);
  ssh->close(ssh);
  free(memory_key);
  return 0;
}
