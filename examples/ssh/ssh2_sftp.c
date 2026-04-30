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

static int write_file(const char *path, const char *bytes) {
  FILE *file;
  size_t size;

  file = fopen(path, "wb");
  if (file == NULL) {
    return 0;
  }
  size = strlen(bytes);
  if (fwrite(bytes, 1u, size, file) != size) {
    (void)fclose(file);
    return 0;
  }
  return fclose(file) == 0;
}

static int file_equals(const char *path, const char *expected) {
  FILE *file;
  char buffer[128];
  size_t n;
  size_t expected_size;

  file = fopen(path, "rb");
  if (file == NULL) {
    return 0;
  }
  n = fread(buffer, 1u, sizeof(buffer), file);
  if (ferror(file)) {
    (void)fclose(file);
    return 0;
  }
  (void)fclose(file);
  expected_size = strlen(expected);
  return n == expected_size && memcmp(buffer, expected, expected_size) == 0;
}

int main(void) {
  vectis_ssh_config config;
  vectis_ssh *ssh;
  vectis_error error;
  const char payload[] = "vectis libssh2 sftp e2e\n";
  const char *local_upload;
  const char *local_download;
  const char *remote_path;

  ssh = NULL;
  vectis_ssh_config_init(&config);
  vectis_error_clear(&error);
  config.host = env_or_default("VECTIS_SSH_HOST", "127.0.0.1");
  config.port = env_port_or_default("VECTIS_SSH_PORT", 29222u);
  config.username = env_or_default("VECTIS_SSH_USERNAME", "vectis");
  config.password = env_or_default("VECTIS_SSH_PASSWORD", "vectispass");
  config.private_key_path = getenv("VECTIS_SSH_PRIVATE_KEY");
  config.known_hosts_path = getenv("VECTIS_SSH_KNOWN_HOSTS");
  config.timeout_ms = 30000L;

  local_upload = env_or_default("VECTIS_SSH_SFTP_UPLOAD_FILE", "ssh-sftp-upload.txt");
  local_download = env_or_default("VECTIS_SSH_SFTP_DOWNLOAD_FILE", "ssh-sftp-download.txt");
  remote_path = env_or_default("VECTIS_SSH_SFTP_REMOTE_FILE", "/config/ssh-sftp-upload.txt");

  if (!write_file(local_upload, payload)) {
    fprintf(stderr, "failed to write %s\n", local_upload);
    return 1;
  }
  if (vectis_ssh_new(&config, &ssh, &error) != VECTIS_OK) {
    return print_error("vectis_ssh_new", &error);
  }
  if (ssh->sftp_upload_file(ssh, local_upload, remote_path, &error) != VECTIS_OK) {
    ssh->close(ssh);
    return print_error("ssh->sftp_upload_file", &error);
  }
  if (ssh->sftp_download_file(ssh, remote_path, local_download, &error) != VECTIS_OK) {
    ssh->close(ssh);
    return print_error("ssh->sftp_download_file", &error);
  }
  if (!file_equals(local_download, payload)) {
    fprintf(stderr, "downloaded SSH SFTP file did not match uploaded payload\n");
    ssh->close(ssh);
    return 1;
  }
  ssh->close(ssh);
  return 0;
}
