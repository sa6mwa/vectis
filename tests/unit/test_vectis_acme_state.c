#include "vectis_acme_state.h"

#include <assert.h>
#include <dirent.h>
#include <lc/lc.h>
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

static void assert_private_file(const char *path) {
  struct stat st;

  assert(stat(path, &st) == 0);
  assert(S_ISREG(st.st_mode));
  assert((st.st_mode & 0777) == 0600);
  assert(st.st_size > 0);
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

static void generate_key_file(const char *path) {
  char *key;
  lc_error error;

  key = NULL;
  lc_error_init(&error);
  assert(lc_pouch_crypto_generate_key_file(path, 0, &key, &error) == LC_OK);
  assert(key != NULL);
  lc_pouch_crypto_key_string_free(key);
  lc_error_cleanup(&error);
}

int main(void) {
  char root[] = "/tmp/vectis-acme-state-XXXXXX";
  char endpoint[4096];
  char storage[4096];
  char default_storage[4096];
  char default_endpoint[4096];
  char default_config[4096];
  char default_key[4096];
  char key_file[4096];
  char wrong_key_file[4096];
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
  assert(snprintf(default_storage, sizeof(default_storage), "%s/default-storage", root) > 0);
  assert(snprintf(default_endpoint, sizeof(default_endpoint), "pouch://%s", default_storage) > 0);
  assert(snprintf(default_config, sizeof(default_config), "%s/default-config", root) > 0);
  assert(snprintf(default_key, sizeof(default_key), "%s/liblockdc/pouch.key", default_config) > 0);
  assert(snprintf(key_file, sizeof(key_file), "%s/pouch.key", root) > 0);
  assert(snprintf(wrong_key_file, sizeof(wrong_key_file), "%s/wrong.key", root) > 0);
  generate_key_file(key_file);
  generate_key_file(wrong_key_file);
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
  config.pouch_crypto_key_file = key_file;
  config.pouch_crypto_generate_key_file = 0;
  config.pouch_crypto_generate_key_file_set = 1;
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

  assert(setenv("XDG_CONFIG_HOME", default_config, 1) == 0);
  config.endpoint = default_endpoint;
  config.key = "default-key";
  config.runtime_dir = runtime_two;
  config.pouch_crypto_key_file = NULL;
  config.pouch_crypto_generate_key_file = 0;
  config.pouch_crypto_generate_key_file_set = 0;
  assert(vectis_acme_state_persist(&config, &error) == VECTIS_OK);
  assert_private_file(default_key);

  config.endpoint = endpoint;
  config.key = "round-trip";
  config.pouch_crypto_key_file = wrong_key_file;
  hydrated = 0;
  assert(vectis_acme_state_hydrate(&config, &hydrated, &error) ==
         VECTIS_ERR_STATE);

  remove_tree(root);
  return 0;
}
