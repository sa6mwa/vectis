#include "vectis_acme_state.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *contents) {
  FILE *file;

  file = fopen(path, "wb");
  assert(file != NULL);
  assert(fwrite(contents, 1u, strlen(contents), file) == strlen(contents));
  assert(fclose(file) == 0);
  assert(chmod(path, 0600) == 0);
}

static void assert_file(const char *path, const char *contents) {
  FILE *file;
  char buffer[256];
  size_t size;
  struct stat st;

  file = fopen(path, "rb");
  assert(file != NULL);
  size = fread(buffer, 1u, sizeof(buffer) - 1u, file);
  assert(fclose(file) == 0);
  buffer[size] = '\0';
  assert(strcmp(buffer, contents) == 0);
  assert(stat(path, &st) == 0);
  assert((st.st_mode & 0777) == 0600);
}

static void remove_tree(const char *path) {
  DIR *dir;
  struct dirent *entry;
  char child[4096];
  struct stat st;

  if (lstat(path, &st) != 0) {
    return;
  }
  if (!S_ISDIR(st.st_mode)) {
    assert(unlink(path) == 0);
    return;
  }
  dir = opendir(path);
  assert(dir != NULL);
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    assert(snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) > 0);
    remove_tree(child);
  }
  assert(closedir(dir) == 0);
  assert(rmdir(path) == 0);
}

int main(void) {
  char root[] = "/tmp/vectis-acme-state-XXXXXX";
  char endpoint[4096];
  char storage[4096];
  char runtime_one[4096];
  char runtime_two[4096];
  char certificates_one[4096];
  char certificates_two[4096];
  char domain_one[4096];
  char domain_two[4096];
  char account_one[4096];
  char chain_one[4096];
  char key_one[4096];
  char account_two[4096];
  char chain_two[4096];
  char key_two[4096];
  const char *domains[1];
  vectis_acme_state_config config;
  vectis_error error;
  int hydrated;

  assert(mkdtemp(root) != NULL);
  assert(snprintf(storage, sizeof(storage), "%s/storage", root) > 0);
  assert(snprintf(endpoint, sizeof(endpoint), "pouch://%s", storage) > 0);
  assert(snprintf(runtime_one, sizeof(runtime_one), "%s/runtime-one", root) > 0);
  assert(snprintf(runtime_two, sizeof(runtime_two), "%s/runtime-two", root) > 0);
  assert(mkdir(runtime_one, 0700) == 0);
  assert(mkdir(runtime_two, 0700) == 0);
  assert(snprintf(certificates_one, sizeof(certificates_one), "%s/certificates",
                  runtime_one) > 0);
  assert(snprintf(certificates_two, sizeof(certificates_two), "%s/certificates",
                  runtime_two) > 0);
  assert(mkdir(certificates_one, 0700) == 0);
  assert(snprintf(domain_one, sizeof(domain_one), "%s/example.test",
                  certificates_one) > 0);
  assert(mkdir(domain_one, 0700) == 0);
  assert(snprintf(account_one, sizeof(account_one), "%s/account-key.pem",
                  runtime_one) > 0);
  assert(snprintf(chain_one, sizeof(chain_one), "%s/fullchain.pem", domain_one) > 0);
  assert(snprintf(key_one, sizeof(key_one), "%s/key.pem", domain_one) > 0);
  write_file(account_one, "account-key");
  write_file(chain_one, "certificate-chain");
  write_file(key_one, "certificate-key");

  domains[0] = "example.test";
  memset(&config, 0, sizeof(config));
  config.endpoint = endpoint;
  config.namespace_name = "vectis.acme";
  config.key = "round-trip";
  config.owner = "test";
  config.runtime_dir = runtime_one;
  config.domains = domains;
  config.domain_count = 1u;
  config.timeout_ms = 30000L;
  assert(vectis_acme_state_persist(&config, &error) == VECTIS_OK);

  config.runtime_dir = runtime_two;
  hydrated = 0;
  assert(vectis_acme_state_hydrate(&config, &hydrated, &error) == VECTIS_OK);
  assert(hydrated != 0);
  assert(snprintf(account_two, sizeof(account_two), "%s/account-key.pem",
                  runtime_two) > 0);
  assert(snprintf(domain_two, sizeof(domain_two), "%s/example.test",
                  certificates_two) > 0);
  assert(snprintf(chain_two, sizeof(chain_two), "%s/fullchain.pem", domain_two) > 0);
  assert(snprintf(key_two, sizeof(key_two), "%s/key.pem", domain_two) > 0);
  assert_file(account_two, "account-key");
  assert_file(chain_two, "certificate-chain");
  assert_file(key_two, "certificate-key");

  remove_tree(root);
  return 0;
}
