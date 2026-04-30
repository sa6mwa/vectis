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
  vectis_sftp_config config;
  vectis_sftp *sftp;
  vectis_error error;
  const char payload[] = "vectis curl sftp e2e\n";
  const char *local_upload;
  const char *local_download;
  const char *remote_path;

  sftp = NULL;
  vectis_sftp_config_init(&config);
  vectis_error_clear(&error);
  config.url = env_or_default("VECTIS_SFTP_URL", "sftp://127.0.0.1:29222");
  config.username = env_or_default("VECTIS_SFTP_USERNAME", "vectis");
  config.password = env_or_default("VECTIS_SFTP_PASSWORD", "vectispass");
  config.private_key_path = getenv("VECTIS_SFTP_PRIVATE_KEY");
  config.known_hosts_path = getenv("VECTIS_SFTP_KNOWN_HOSTS");
  config.timeout_ms = 10000L;

  local_upload = env_or_default("VECTIS_SFTP_UPLOAD_FILE", "curl-sftp-upload.txt");
  local_download = env_or_default("VECTIS_SFTP_DOWNLOAD_FILE", "curl-sftp-download.txt");
  remote_path = env_or_default("VECTIS_SFTP_REMOTE_FILE", "/config/curl-sftp-upload.txt");

  if (!write_file(local_upload, payload)) {
    fprintf(stderr, "failed to write %s\n", local_upload);
    return 1;
  }

  if (vectis_sftp_new(&config, &sftp, &error) != VECTIS_OK) {
    return print_error("vectis_sftp_new", &error);
  }
  if (sftp->upload_file(sftp, local_upload, remote_path, &error) != VECTIS_OK) {
    sftp->close(sftp);
    return print_error("sftp->upload_file", &error);
  }
  if (sftp->download_file(sftp, remote_path, local_download, &error) != VECTIS_OK) {
    sftp->close(sftp);
    return print_error("sftp->download_file", &error);
  }
  if (!file_equals(local_download, payload)) {
    fprintf(stderr, "downloaded SFTP file did not match uploaded payload\n");
    sftp->close(sftp);
    return 1;
  }
  sftp->close(sftp);
  return 0;
}
