#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static size_t live_blocks, allocation_calls, fail_at;
static void *fault_malloc(size_t size) {
  void *memory;
  ++allocation_calls;
  if (fail_at != 0u && allocation_calls == fail_at)
    return NULL;
  memory = malloc(size);
  if (memory != NULL)
    ++live_blocks;
  return memory;
}
static void fault_free(void *memory) {
  if (memory != NULL) {
    if (live_blocks == 0u)
      abort();
    --live_blocks;
  }
  free(memory);
}
#define malloc fault_malloc
#define free fault_free
#include "../../src/vectis_webdav.c"
#undef malloc
#undef free
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "WebDAV allocation line %d: %s\n", __LINE__,             \
              #condition);                                                     \
      return 1;                                                                \
    }                                                                          \
  } while (0)
static int descriptors(void) {
#if defined(__linux__)
  DIR *directory = opendir("/proc/self/fd");
  struct dirent *entry;
  int count = 0;
  if (directory == NULL)
    return -1;
  while ((entry = readdir(directory)) != NULL)
    if (entry->d_name[0] != '.')
      ++count;
  closedir(directory);
  return count;
#else
  return 0;
#endif
}
int main(int argc, char **argv) {
  vectis_webdav_config config;
  vectis_webdav_entry entry;
  vectis_webdav_status status;
  size_t ordinal, total;
  char *root, *disk;
  char destination[64];
  int direct, baseline;
  CHECK(argc == 2);
  root = (char *)malloc(strlen(argv[1]) + 32u);
  disk = (char *)malloc(strlen(argv[1]) + 64u);
  CHECK(root && disk);
  for (direct = 0; direct < 2; ++direct) {
    sprintf(root, "%s-%d-XXXXXX", argv[1], direct);
    CHECK(mkdtemp(root) != NULL);
    sprintf(disk, "%s/disk", root);
    CHECK(mkdir(disk, 0700) == 0);
    vectis_webdav_config_init(&config);
    config.cache_dir = root;
    config.site_id = "faults";
    config.root_dir = direct ? disk : NULL;
    CHECK(vectis_webdav_mkcol(&config, "/source") == VECTIS_WEBDAV_OK);
    CHECK(vectis_webdav_mkcol(&config, "/source/nested") == VECTIS_WEBDAV_OK);
    CHECK(vectis_webdav_put(&config, "/source/nested/file",
                            (const unsigned char *)"payload",
                            7u) == VECTIS_WEBDAV_OK);
    CHECK(live_blocks == 0u);
    allocation_calls = 0u;
    CHECK(vectis_webdav_copy(&config, "/source", "/control", 0) ==
          VECTIS_WEBDAV_OK);
    total = allocation_calls;
    CHECK(vectis_webdav_delete(&config, "/control") == VECTIS_WEBDAV_OK);
    CHECK(live_blocks == 0u);
    baseline = descriptors();
    CHECK(baseline >= 0);
    for (ordinal = 1u; ordinal <= total; ++ordinal) {
      sprintf(destination, "/failed-%lu", (unsigned long)ordinal);
      allocation_calls = 0u;
      fail_at = ordinal;
      status = vectis_webdav_copy(&config, "/source", destination, 0);
      fail_at = 0u;
      CHECK(live_blocks == 0u && descriptors() == baseline);
      CHECK(vectis_webdav_lookup(&config, "/source/nested/file", &entry) ==
            VECTIS_WEBDAV_OK);
      if (status == VECTIS_WEBDAV_OK)
        CHECK(vectis_webdav_delete(&config, destination) == VECTIS_WEBDAV_OK);
      else
        CHECK(vectis_webdav_lookup(&config, destination, &entry) ==
              VECTIS_WEBDAV_NOT_FOUND);
    }
    CHECK(live_blocks == 0u && vectis_webdav_remove_tree(root));
    CHECK(live_blocks == 0u);
    printf("WebDAV %s allocation failures: %lu; heap/FD/source ownership "
           "preserved\n",
           direct ? "direct" : "cached", (unsigned long)total);
  }
  free(root);
  free(disk);
  return 0;
}
