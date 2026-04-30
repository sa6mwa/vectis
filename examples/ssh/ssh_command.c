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

static unsigned short env_port_or_default(const char *name, unsigned short fallback) {
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

int main(void) {
  vectis_ssh_config config;
  vectis_ssh *ssh;
  vectis_ssh_exec_result result = {0};
  vectis_error error;
  const char *command;
  const char expected[] = "vectis-ssh-ok";

  ssh = NULL;
  vectis_ssh_config_init(&config);
  vectis_error_clear(&error);

  config.host = env_or_default("VECTIS_SSH_HOST", "127.0.0.1");
  config.port = env_port_or_default("VECTIS_SSH_PORT", 29222u);
  config.username = env_or_default("VECTIS_SSH_USERNAME", "vectis");
  config.password = env_or_default("VECTIS_SSH_PASSWORD", "vectispass");
  config.private_key_path = getenv("VECTIS_SSH_PRIVATE_KEY");
  config.known_hosts_path = getenv("VECTIS_SSH_KNOWN_HOSTS");
  config.timeout_ms = 10000L;
  command = env_or_default("VECTIS_SSH_COMMAND", "printf vectis-ssh-ok");

  if (vectis_ssh_new(&config, &ssh, &error) != VECTIS_OK) {
    return print_error("vectis_ssh_new", &error);
  }
  if (ssh->exec(ssh, command, &result, &error) != VECTIS_OK) {
    ssh->close(ssh);
    return print_error("ssh->exec", &error);
  }
  if (result.exit_status != 0) {
    fprintf(stderr, "SSH command exited with status %d\n", result.exit_status);
    vectis_ssh_exec_result_cleanup(&result);
    ssh->close(ssh);
    return 1;
  }
  if (strcmp(command, "printf vectis-ssh-ok") == 0 &&
      (result.stdout_size != strlen(expected) ||
       memcmp(result.stdout_data, expected, strlen(expected)) != 0)) {
    fprintf(stderr, "SSH command returned unexpected stdout\n");
    vectis_ssh_exec_result_cleanup(&result);
    ssh->close(ssh);
    return 1;
  }
  vectis_ssh_exec_result_cleanup(&result);
  ssh->close(ssh);
  return 0;
}
