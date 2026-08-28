#include "vectis_internal.h"

#include <vectis/webdav.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define VECTIS_WEBDAV_DEFAULT_MAX_FILE (16u * 1024u * 1024u)
#define VECTIS_WEBDAV_DEFAULT_MAX_TOTAL (128u * 1024u * 1024u)
#define VECTIS_WEBDAV_DEFAULT_MAX_RESOURCES 4096u
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
static const char vectis_webdav_hex[] = "0123456789abcdef";

static int vectis_webdav_remove_tree(const char *path);

static int vectis_webdav_site_valid(const char *site_id) {
  size_t i;

  if (site_id == NULL || site_id[0] == '\0' || strlen(site_id) > 63u) {
    return 0;
  }
  for (i = 0u; site_id[i] != '\0'; ++i) {
    if (!(isalnum((unsigned char)site_id[i]) || site_id[i] == '-' ||
          site_id[i] == '_')) {
      return 0;
    }
  }
  return 1;
}

static int vectis_webdav_config_valid(const vectis_webdav_config *config) {
  return config != NULL && config->cache_dir != NULL &&
         config->cache_dir[0] == '/' &&
         (config->root_dir == NULL || config->root_dir[0] == '/') &&
         vectis_webdav_site_valid(config->site_id) &&
         config->max_file_bytes > 0u && config->max_total_bytes > 0u &&
         config->max_resources > 0u &&
         config->max_file_bytes <= config->max_total_bytes;
}

static int vectis_webdav_direct_root(const vectis_webdav_config *config) {
  return config != NULL && config->root_dir != NULL &&
         config->root_dir[0] == '/';
}

static int vectis_webdav_path_in_root(const char *root, const char *path,
                                      char out[VECTIS_WEBDAV_STORAGE_PATH_MAX]);
static int
vectis_webdav_direct_read_file(const vectis_webdav_config *config,
                               const char *normalized, unsigned char **body,
                               size_t *body_size,
                               char etag[VECTIS_WEBDAV_ETAG_LENGTH + 1u]);
static vectis_webdav_status
vectis_webdav_direct_remove(const vectis_webdav_config *config,
                            const char *normalized);
static vectis_webdav_status
vectis_webdav_direct_mkcol(const vectis_webdav_config *config,
                           const char *normalized);
static vectis_webdav_status vectis_webdav_direct_copy_or_move_fs(
    const vectis_webdav_config *config, const char *normalized_source,
    const char *normalized_destination, int overwrite, int remove_source);
static int vectis_webdav_open_root_fd(const vectis_webdav_config *config,
                                      int create_missing);
static int vectis_webdav_copy_segment(char *out, size_t out_size,
                                      const char **cursor);
static int vectis_webdav_open_child_dir(int parent_fd, const char *name);

static int vectis_webdav_path_append(char *out, size_t out_size,
                                     const char *left, const char *right) {
  int written;

  written = snprintf(out, out_size, "%s/%s", left, right);
  return written >= 0 && (size_t)written < out_size;
}

static int vectis_webdav_base(const vectis_webdav_config *config, char *out,
                              size_t out_size) {
  int written;

  if (!vectis_webdav_config_valid(config)) {
    return 0;
  }
  written = snprintf(out, out_size, "%s/webdav/%s", config->cache_dir,
                     config->site_id);
  return written >= 0 && (size_t)written < out_size;
}

static int vectis_webdav_mkdir_p(const char *path) {
  char copy[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char *cursor;
  size_t length;

  if (path == NULL || path[0] != '/' || strlen(path) >= sizeof(copy)) {
    return 0;
  }
  strcpy(copy, path);
  length = strlen(copy);
  if (length == 0u) {
    return 0;
  }
  for (cursor = copy + 1; *cursor != '\0'; ++cursor) {
    if (*cursor == '/') {
      *cursor = '\0';
      if (mkdir(copy, 0700) == -1 && errno != EEXIST) {
        return 0;
      }
      *cursor = '/';
    }
  }
  if (mkdir(copy, 0700) == -1 && errno != EEXIST) {
    return 0;
  }
  return 1;
}

static int vectis_webdav_parent_dir(const char *path, char *out,
                                    size_t out_size) {
  char *slash;

  if (path == NULL || strlen(path) >= out_size) {
    return 0;
  }
  strcpy(out, path);
  slash = strrchr(out, '/');
  if (slash == NULL || slash == out) {
    return 0;
  }
  *slash = '\0';
  return 1;
}

static int vectis_webdav_prepare(const vectis_webdav_config *config) {
  char base[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char path[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  static const char *const directories[] = {"content", "tombstones"};
  size_t i;
  int root_fd;

  if (!vectis_webdav_base(config, base, sizeof(base)) ||
      !vectis_webdav_mkdir_p(base)) {
    return 0;
  }
  if (vectis_webdav_direct_root(config)) {
    root_fd = vectis_webdav_open_root_fd(config, 1);
    if (root_fd < 0) {
      return 0;
    }
    (void)close(root_fd);
    return 1;
  }
  for (i = 0u; i < sizeof(directories) / sizeof(directories[0]); ++i) {
    if (!vectis_webdav_path_append(path, sizeof(path), base, directories[i]) ||
        !vectis_webdav_mkdir_p(path)) {
      return 0;
    }
  }
  return 1;
}

static int vectis_webdav_disk_path(const vectis_webdav_config *config,
                                   const char *category, const char *path,
                                   char out[VECTIS_WEBDAV_STORAGE_PATH_MAX]) {
  char base[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  const char *relative;
  int written;

  if (!vectis_webdav_base(config, base, sizeof(base)) || category == NULL ||
      !vectis_webdav_path_normalize(path, normalized)) {
    return 0;
  }
  if (strcmp(category, "content") == 0 && vectis_webdav_direct_root(config)) {
    return vectis_webdav_path_in_root(config->root_dir, normalized, out);
  }
  relative = normalized[1] == '\0' ? "" : normalized + 1;
  if (relative[0] == '\0') {
    written =
        snprintf(out, VECTIS_WEBDAV_STORAGE_PATH_MAX, "%s/%s", base, category);
  } else {
    written = snprintf(out, VECTIS_WEBDAV_STORAGE_PATH_MAX, "%s/%s/%s", base,
                       category, relative);
  }
  return written >= 0 && (size_t)written < VECTIS_WEBDAV_STORAGE_PATH_MAX;
}

static int vectis_webdav_file_regular(const char *path, struct stat *st) {
  struct stat local;

  if (path == NULL) {
    return 0;
  }
  if (st == NULL) {
    st = &local;
  }
  return lstat(path, st) == 0 && S_ISREG(st->st_mode);
}

static int vectis_webdav_file_directory(const char *path) {
  struct stat st;

  return path != NULL && lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int vectis_webdav_touch_atomic(const char *path) {
  char parent[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char temporary[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  int fd;
  int written;

  if (!vectis_webdav_parent_dir(path, parent, sizeof(parent)) ||
      !vectis_webdav_mkdir_p(parent)) {
    return 0;
  }
  written =
      snprintf(temporary, sizeof(temporary), "%s/.vectis-tmp-XXXXXX", parent);
  if (written < 0 || (size_t)written >= sizeof(temporary)) {
    return 0;
  }
  fd = mkstemp(temporary);
  if (fd == -1) {
    return 0;
  }
  if (fchmod(fd, 0600) == -1 || fsync(fd) == -1 || close(fd) == -1 ||
      rename(temporary, path) == -1) {
    (void)close(fd);
    (void)unlink(temporary);
    return 0;
  }
  return 1;
}

static int vectis_webdav_write_atomic(const char *path,
                                      const unsigned char *body,
                                      size_t body_size) {
  char parent[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char temporary[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  size_t offset;
  ssize_t written_bytes;
  int fd;
  int written;

  if ((body == NULL && body_size != 0u) ||
      !vectis_webdav_parent_dir(path, parent, sizeof(parent)) ||
      !vectis_webdav_mkdir_p(parent)) {
    return 0;
  }
  written =
      snprintf(temporary, sizeof(temporary), "%s/.vectis-tmp-XXXXXX", parent);
  if (written < 0 || (size_t)written >= sizeof(temporary)) {
    return 0;
  }
  fd = mkstemp(temporary);
  if (fd == -1) {
    return 0;
  }
  if (fchmod(fd, 0600) == -1) {
    (void)close(fd);
    (void)unlink(temporary);
    return 0;
  }
  offset = 0u;
  while (offset < body_size) {
    written_bytes = write(fd, body + offset, body_size - offset);
    if (written_bytes <= 0) {
      (void)close(fd);
      (void)unlink(temporary);
      return 0;
    }
    offset += (size_t)written_bytes;
  }
  if (fsync(fd) == -1 || close(fd) == -1 || rename(temporary, path) == -1) {
    (void)close(fd);
    (void)unlink(temporary);
    return 0;
  }
  return 1;
}

static int vectis_webdav_lock(const vectis_webdav_config *config) {
  char base[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char path[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  struct flock lock;
  int fd;

  if (!vectis_webdav_prepare(config) ||
      !vectis_webdav_base(config, base, sizeof(base)) ||
      !vectis_webdav_path_append(path, sizeof(path), base, ".lock")) {
    return -1;
  }
  fd = open(path, O_CREAT | O_RDWR, 0600);
  if (fd == -1) {
    return -1;
  }
  memset(&lock, 0, sizeof(lock));
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  while (fcntl(fd, F_SETLKW, &lock) == -1) {
    if (errno != EINTR) {
      (void)close(fd);
      return -1;
    }
  }
  return fd;
}

static void vectis_webdav_unlock(int fd) {
  struct flock lock;

  if (fd < 0) {
    return;
  }
  memset(&lock, 0, sizeof(lock));
  lock.l_type = F_UNLCK;
  lock.l_whence = SEEK_SET;
  (void)fcntl(fd, F_SETLK, &lock);
  (void)close(fd);
}

static int vectis_webdav_tombstone_exists(const vectis_webdav_config *config,
                                          const char *path) {
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  char candidate[VECTIS_WEBDAV_PATH_MAX + 1u];
  char disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  size_t i;

  if (!vectis_webdav_path_normalize(path, normalized)) {
    return 0;
  }
  if (vectis_webdav_direct_root(config)) {
    return 0;
  }
  for (i = 1u; normalized[i] != '\0'; ++i) {
    if (normalized[i] != '/' && normalized[i + 1u] != '\0') {
      continue;
    }
    if (i >= sizeof(candidate)) {
      return 0;
    }
    memcpy(candidate, normalized, i + 1u);
    candidate[i + 1u] = '\0';
    if (candidate[i] == '/') {
      candidate[i] = '\0';
    }
    if (candidate[0] != '\0' &&
        vectis_webdav_disk_path(config, "tombstones", candidate, disk) &&
        vectis_webdav_file_regular(disk, NULL)) {
      return 1;
    }
  }
  return 0;
}

static int
vectis_webdav_ancestor_tombstone_exists(const vectis_webdav_config *config,
                                        const char *path) {
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  char candidate[VECTIS_WEBDAV_PATH_MAX + 1u];
  char disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  size_t i;

  if (!vectis_webdav_path_normalize(path, normalized)) {
    return 0;
  }
  if (vectis_webdav_direct_root(config)) {
    return 0;
  }
  for (i = 1u; normalized[i] != '\0'; ++i) {
    if (normalized[i] != '/') {
      continue;
    }
    memcpy(candidate, normalized, i);
    candidate[i] = '\0';
    if (vectis_webdav_disk_path(config, "tombstones", candidate, disk) &&
        vectis_webdav_file_regular(disk, NULL)) {
      return 1;
    }
  }
  return 0;
}

static int
vectis_webdav_path_in_root(const char *root, const char *path,
                           char out[VECTIS_WEBDAV_STORAGE_PATH_MAX]) {
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  int written;

  if (root == NULL || !vectis_webdav_path_normalize(path, normalized)) {
    return 0;
  }
  if (normalized[1] == '\0') {
    written = snprintf(out, VECTIS_WEBDAV_STORAGE_PATH_MAX, "%s", root);
  } else if (strcmp(root, "/") == 0) {
    written = snprintf(out, VECTIS_WEBDAV_STORAGE_PATH_MAX, "%s", normalized);
  } else {
    written =
        snprintf(out, VECTIS_WEBDAV_STORAGE_PATH_MAX, "%s%s", root, normalized);
  }
  return written >= 0 && (size_t)written < VECTIS_WEBDAV_STORAGE_PATH_MAX;
}

static int vectis_webdav_clear_tombstones_in_root(const char *root,
                                                  const char *path) {
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  char disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];

  if (!vectis_webdav_path_normalize(path, normalized) ||
      !vectis_webdav_path_in_root(root, normalized, disk)) {
    return 0;
  }
  return vectis_webdav_remove_tree(disk);
}

static int vectis_webdav_clear_tombstones(const vectis_webdav_config *config,
                                          const char *path) {
  char root[VECTIS_WEBDAV_STORAGE_PATH_MAX];

  if (vectis_webdav_direct_root(config)) {
    return 1;
  }
  return vectis_webdav_disk_path(config, "tombstones", "/", root) &&
         vectis_webdav_clear_tombstones_in_root(root, path);
}

static int vectis_webdav_usage_path(const char *path, uint64_t *total,
                                    uint64_t *resources,
                                    int ignore_unsupported) {
  DIR *directory;
  struct dirent *entry;
  struct stat st;
  char child[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  int written;

  if (total == NULL || resources == NULL) {
    return 0;
  }
  directory = opendir(path);
  if (directory == NULL) {
    return errno == ENOENT;
  }
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(child) ||
        lstat(child, &st) == -1) {
      (void)closedir(directory);
      return 0;
    }
    if (S_ISREG(st.st_mode)) {
      if (st.st_size < 0 || UINT64_MAX - *total < (uint64_t)st.st_size ||
          *resources == UINT64_MAX) {
        (void)closedir(directory);
        return 0;
      }
      *total += (uint64_t)st.st_size;
      (*resources)++;
    } else if (S_ISDIR(st.st_mode)) {
      if (*resources == UINT64_MAX) {
        (void)closedir(directory);
        return 0;
      }
      (*resources)++;
      if (!vectis_webdav_usage_path(child, total, resources,
                                    ignore_unsupported)) {
        (void)closedir(directory);
        return 0;
      }
    } else if (ignore_unsupported) {
      continue;
    } else {
      (void)closedir(directory);
      return 0;
    }
  }
  (void)closedir(directory);
  return 1;
}

static int vectis_webdav_usage(const vectis_webdav_config *config,
                               uint64_t *total, uint64_t *resources) {
  char content[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char tombstones[VECTIS_WEBDAV_STORAGE_PATH_MAX];

  if (total == NULL || resources == NULL ||
      !vectis_webdav_disk_path(config, "content", "/", content)) {
    return 0;
  }
  *total = 0u;
  *resources = 0u;
  if (vectis_webdav_direct_root(config)) {
    return vectis_webdav_usage_path(content, total, resources, 1);
  }
  if (!vectis_webdav_disk_path(config, "tombstones", "/", tombstones)) {
    return 0;
  }
  return vectis_webdav_usage_path(content, total, resources, 0) &&
         vectis_webdav_usage_path(tombstones, total, resources, 0);
}

static int
vectis_webdav_resources_within_limit(const vectis_webdav_config *config,
                                     uint64_t resources,
                                     uint64_t additional_resources) {
  uint64_t limit;

  if (config == NULL) {
    return 0;
  }
  limit = (uint64_t)config->max_resources;
  return resources <= limit && additional_resources <= limit - resources;
}

static int vectis_webdav_add_resources(uint64_t *resources,
                                       uint64_t additional_resources) {
  if (resources == NULL || UINT64_MAX - *resources < additional_resources) {
    return 0;
  }
  *resources += additional_resources;
  return 1;
}

static int vectis_webdav_missing_path_resources(const char *root,
                                                const char *path,
                                                int include_path,
                                                uint64_t *resources) {
  char candidate[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char parent[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  struct stat st;
  size_t root_size;

  if (root == NULL || path == NULL || resources == NULL) {
    return 0;
  }
  root_size = strlen(root);
  if (strncmp(path, root, root_size) != 0 ||
      (path[root_size] != '\0' && path[root_size] != '/')) {
    return 0;
  }
  if (strlen(path) >= sizeof(candidate)) {
    return 0;
  }
  strcpy(candidate, path);
  if (!include_path) {
    if (!vectis_webdav_parent_dir(candidate, parent, sizeof(parent))) {
      return 0;
    }
    strcpy(candidate, parent);
  }
  while (strcmp(candidate, root) != 0) {
    if (lstat(candidate, &st) == 0) {
      if (strcmp(candidate, path) == 0 && include_path) {
        return 1;
      }
      return S_ISDIR(st.st_mode);
    }
    if (errno != ENOENT || *resources == UINT64_MAX) {
      return 0;
    }
    (*resources)++;
    if (!vectis_webdav_parent_dir(candidate, parent, sizeof(parent))) {
      return 0;
    }
    strcpy(candidate, parent);
  }
  return vectis_webdav_file_directory(root);
}

static void vectis_webdav_hash(const unsigned char *data, size_t size,
                               char hex[VECTIS_WEBDAV_ETAG_LENGTH + 1u]) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  static const unsigned char empty[] = "";
  size_t i;

  if (data == NULL) {
    data = empty;
  }
  (void)SHA256(data, size, digest);
  for (i = 0u; i < sizeof(digest); ++i) {
    hex[i * 2u] = vectis_webdav_hex[(digest[i] >> 4u) & 0x0fu];
    hex[i * 2u + 1u] = vectis_webdav_hex[digest[i] & 0x0fu];
  }
  hex[VECTIS_WEBDAV_ETAG_LENGTH] = '\0';
}

static int vectis_webdav_read_file(const char *path, unsigned char **body,
                                   size_t *body_size,
                                   char etag[VECTIS_WEBDAV_ETAG_LENGTH + 1u]) {
  struct stat st;
  unsigned char *out;
  size_t offset;
  ssize_t read_bytes;
  int fd;

  if (body == NULL || body_size == NULL ||
      !vectis_webdav_file_regular(path, &st) || st.st_size < 0 ||
      (uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
    return 0;
  }
  out = NULL;
  if (st.st_size > 0) {
    out = (unsigned char *)malloc((size_t)st.st_size);
    if (out == NULL) {
      return 0;
    }
  }
  fd = open(path, O_RDONLY);
  if (fd == -1) {
    free(out);
    return 0;
  }
  offset = 0u;
  while (offset < (size_t)st.st_size) {
    read_bytes = read(fd, out + offset, (size_t)st.st_size - offset);
    if (read_bytes <= 0) {
      (void)close(fd);
      free(out);
      return 0;
    }
    offset += (size_t)read_bytes;
  }
  if (close(fd) == -1) {
    free(out);
    return 0;
  }
  vectis_webdav_hash(out, (size_t)st.st_size, etag);
  *body = out;
  *body_size = (size_t)st.st_size;
  return 1;
}

static void vectis_webdav_fd_close(int *fd) {
  if (fd != NULL && *fd >= 0) {
    (void)close(*fd);
    *fd = -1;
  }
}

static int vectis_webdav_open_root_fd(const vectis_webdav_config *config,
                                      int create_missing) {
  char segment[VECTIS_WEBDAV_PATH_MAX + 1u];
  const char *cursor;
  int current_fd;
  int next_fd;
  int saved_errno;

  if (!vectis_webdav_direct_root(config)) {
    return -1;
  }
  current_fd = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (current_fd < 0) {
    return -1;
  }
  cursor = config->root_dir;
  while (*cursor == '/') {
    cursor++;
  }
  while (*cursor != '\0') {
    if (!vectis_webdav_copy_segment(segment, sizeof(segment), &cursor)) {
      vectis_webdav_fd_close(&current_fd);
      return -1;
    }
    next_fd = vectis_webdav_open_child_dir(current_fd, segment);
    saved_errno = errno;
    if (next_fd < 0 && create_missing && saved_errno == ENOENT) {
      if (mkdirat(current_fd, segment, 0700) != 0 && errno != EEXIST) {
        vectis_webdav_fd_close(&current_fd);
        return -1;
      }
      next_fd = vectis_webdav_open_child_dir(current_fd, segment);
    }
    if (next_fd < 0) {
      vectis_webdav_fd_close(&current_fd);
      return -1;
    }
    vectis_webdav_fd_close(&current_fd);
    current_fd = next_fd;
  }
  return current_fd;
}

static int vectis_webdav_copy_segment(char *out, size_t out_size,
                                      const char **cursor) {
  const char *start;
  size_t size;

  if (out == NULL || out_size == 0u || cursor == NULL || *cursor == NULL) {
    return 0;
  }
  while (**cursor == '/') {
    (*cursor)++;
  }
  start = *cursor;
  while (**cursor != '\0' && **cursor != '/') {
    (*cursor)++;
  }
  size = (size_t)(*cursor - start);
  if (size == 0u || size >= out_size) {
    return 0;
  }
  memcpy(out, start, size);
  out[size] = '\0';
  return 1;
}

static int vectis_webdav_open_child_dir(int parent_fd, const char *name) {
  struct stat st;
  int fd;

  fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
    (void)close(fd);
    return -1;
  }
  return fd;
}

static int
vectis_webdav_open_parent_fd(const vectis_webdav_config *config,
                             const char *normalized, int create_missing,
                             char leaf[VECTIS_WEBDAV_PATH_MAX + 1u]) {
  char segment[VECTIS_WEBDAV_PATH_MAX + 1u];
  const char *cursor;
  int current_fd;
  int next_fd;

  if (leaf == NULL || normalized == NULL || normalized[0] != '/' ||
      normalized[1] == '\0') {
    return -1;
  }
  current_fd = vectis_webdav_open_root_fd(config, 0);
  if (current_fd < 0) {
    return -1;
  }
  cursor = normalized + 1;
  while (1) {
    if (!vectis_webdav_copy_segment(segment, sizeof(segment), &cursor)) {
      vectis_webdav_fd_close(&current_fd);
      return -1;
    }
    while (*cursor == '/') {
      cursor++;
    }
    if (*cursor == '\0') {
      strcpy(leaf, segment);
      return current_fd;
    }
    next_fd = vectis_webdav_open_child_dir(current_fd, segment);
    if (next_fd < 0 && create_missing && errno == ENOENT) {
      if (mkdirat(current_fd, segment, 0700) != 0) {
        vectis_webdav_fd_close(&current_fd);
        return -1;
      }
      next_fd = vectis_webdav_open_child_dir(current_fd, segment);
    }
    if (next_fd < 0) {
      vectis_webdav_fd_close(&current_fd);
      return -1;
    }
    vectis_webdav_fd_close(&current_fd);
    current_fd = next_fd;
  }
}

static int
vectis_webdav_direct_open_existing(const vectis_webdav_config *config,
                                   const char *normalized, int want_dir,
                                   struct stat *out_st) {
  char leaf[VECTIS_WEBDAV_PATH_MAX + 1u];
  struct stat st;
  int parent_fd;
  int fd;

  if (normalized == NULL || normalized[0] != '/') {
    return -1;
  }
  if (normalized[1] == '\0') {
    fd = vectis_webdav_open_root_fd(config, 0);
  } else {
    parent_fd = vectis_webdav_open_parent_fd(config, normalized, 0, leaf);
    if (parent_fd < 0) {
      return -1;
    }
    fd = openat(parent_fd, leaf,
                O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                    (want_dir ? O_DIRECTORY : 0));
    vectis_webdav_fd_close(&parent_fd);
  }
  if (fd < 0) {
    return -1;
  }
  if (fstat(fd, &st) != 0 || (want_dir && !S_ISDIR(st.st_mode)) ||
      (!want_dir && !S_ISREG(st.st_mode))) {
    vectis_webdav_fd_close(&fd);
    return -1;
  }
  if (out_st != NULL) {
    *out_st = st;
  }
  return fd;
}

static int vectis_webdav_read_fd(int fd, size_t max_bytes, unsigned char **body,
                                 size_t *body_size,
                                 char etag[VECTIS_WEBDAV_ETAG_LENGTH + 1u]) {
  struct stat st;
  unsigned char *out;
  size_t offset;
  ssize_t read_bytes;

  if (fd < 0 || body == NULL || body_size == NULL || fstat(fd, &st) != 0 ||
      !S_ISREG(st.st_mode) || st.st_size < 0 ||
      (uint64_t)st.st_size > (uint64_t)SIZE_MAX ||
      (uint64_t)st.st_size > (uint64_t)max_bytes) {
    return 0;
  }
  out = NULL;
  if (st.st_size > 0) {
    out = (unsigned char *)malloc((size_t)st.st_size);
    if (out == NULL) {
      return 0;
    }
  }
  offset = 0u;
  while (offset < (size_t)st.st_size) {
    read_bytes = read(fd, out + offset, (size_t)st.st_size - offset);
    if (read_bytes <= 0) {
      free(out);
      return 0;
    }
    offset += (size_t)read_bytes;
  }
  vectis_webdav_hash(out, (size_t)st.st_size, etag);
  *body = out;
  *body_size = (size_t)st.st_size;
  return 1;
}

static int
vectis_webdav_direct_read_file(const vectis_webdav_config *config,
                               const char *normalized, unsigned char **body,
                               size_t *body_size,
                               char etag[VECTIS_WEBDAV_ETAG_LENGTH + 1u]) {
  int fd;
  int ok;

  fd = vectis_webdav_direct_open_existing(config, normalized, 0, NULL);
  if (fd < 0) {
    return 0;
  }
  ok = vectis_webdav_read_fd(fd, config->max_file_bytes, body, body_size, etag);
  vectis_webdav_fd_close(&fd);
  return ok;
}

static int vectis_webdav_write_atomic_at(int parent_fd, const char *leaf,
                                         const unsigned char *body,
                                         size_t body_size) {
  char temporary[VECTIS_WEBDAV_PATH_MAX + 1u];
  size_t offset;
  ssize_t written_bytes;
  int fd;
  int i;
  int n;

  if (parent_fd < 0 || leaf == NULL || (body == NULL && body_size != 0u)) {
    return 0;
  }
  fd = -1;
  for (i = 0; i < 100; ++i) {
    n = snprintf(temporary, sizeof(temporary), ".vectis-tmp-%lu-%d",
                 (unsigned long)getpid(), i);
    if (n <= 0 || (size_t)n >= sizeof(temporary)) {
      return 0;
    }
    fd = openat(parent_fd, temporary,
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0 || errno != EEXIST) {
      break;
    }
  }
  if (fd < 0) {
    return 0;
  }
  offset = 0u;
  while (offset < body_size) {
    written_bytes = write(fd, body + offset, body_size - offset);
    if (written_bytes <= 0) {
      (void)close(fd);
      (void)unlinkat(parent_fd, temporary, 0);
      return 0;
    }
    offset += (size_t)written_bytes;
  }
  if (fsync(fd) != 0 || close(fd) != 0 ||
      renameat(parent_fd, temporary, parent_fd, leaf) != 0) {
    (void)close(fd);
    (void)unlinkat(parent_fd, temporary, 0);
    return 0;
  }
  return 1;
}

static int vectis_webdav_direct_remove_tree_at(int parent_fd,
                                               const char *name) {
  DIR *directory;
  struct dirent *item;
  struct stat st;
  int child_fd;
  int scan_fd;

  if (parent_fd < 0 || name == NULL ||
      fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
    return errno == ENOENT;
  }
  if (S_ISREG(st.st_mode)) {
    return unlinkat(parent_fd, name, 0) == 0;
  }
  if (!S_ISDIR(st.st_mode)) {
    return 0;
  }
  child_fd = vectis_webdav_open_child_dir(parent_fd, name);
  if (child_fd < 0) {
    return 0;
  }
  scan_fd = dup(child_fd);
  if (scan_fd < 0) {
    vectis_webdav_fd_close(&child_fd);
    return 0;
  }
  directory = fdopendir(scan_fd);
  if (directory == NULL) {
    vectis_webdav_fd_close(&scan_fd);
    vectis_webdav_fd_close(&child_fd);
    return 0;
  }
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
      continue;
    }
    if (!vectis_webdav_direct_remove_tree_at(child_fd, item->d_name)) {
      (void)closedir(directory);
      vectis_webdav_fd_close(&child_fd);
      return 0;
    }
  }
  (void)closedir(directory);
  vectis_webdav_fd_close(&child_fd);
  return unlinkat(parent_fd, name, AT_REMOVEDIR) == 0;
}

static int
vectis_webdav_direct_reserve_dir_at(int parent_fd, const char *prefix,
                                    char out[VECTIS_WEBDAV_PATH_MAX + 1u]) {
  int i;
  int written;

  if (parent_fd < 0 || prefix == NULL || prefix[0] == '\0' || out == NULL) {
    return 0;
  }
  for (i = 0; i < 1000; ++i) {
    written = snprintf(out, VECTIS_WEBDAV_PATH_MAX + 1u, "%s-%lu-%d", prefix,
                       (unsigned long)getpid(), i);
    if (written <= 0 || (size_t)written >= VECTIS_WEBDAV_PATH_MAX + 1u) {
      return 0;
    }
    if (mkdirat(parent_fd, out, 0700) == 0) {
      return 1;
    }
    if (errno != EEXIST) {
      return 0;
    }
  }
  return 0;
}

static vectis_webdav_status
vectis_webdav_direct_remove(const vectis_webdav_config *config,
                            const char *normalized) {
  char leaf[VECTIS_WEBDAV_PATH_MAX + 1u];
  int parent_fd;
  int ok;

  parent_fd = vectis_webdav_open_parent_fd(config, normalized, 0, leaf);
  if (parent_fd < 0) {
    return errno == ENOENT ? VECTIS_WEBDAV_NOT_FOUND : VECTIS_WEBDAV_IO;
  }
  ok = vectis_webdav_direct_remove_tree_at(parent_fd, leaf);
  vectis_webdav_fd_close(&parent_fd);
  return ok ? VECTIS_WEBDAV_OK : VECTIS_WEBDAV_IO;
}

static vectis_webdav_status
vectis_webdav_direct_mkcol(const vectis_webdav_config *config,
                           const char *normalized) {
  char leaf[VECTIS_WEBDAV_PATH_MAX + 1u];
  struct stat st;
  int parent_fd;
  int exists;
  int ok;

  parent_fd = vectis_webdav_open_parent_fd(config, normalized, 1, leaf);
  if (parent_fd < 0) {
    return VECTIS_WEBDAV_IO;
  }
  exists = fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0;
  if (exists) {
    vectis_webdav_fd_close(&parent_fd);
    return S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) ? VECTIS_WEBDAV_EXISTS
                                                      : VECTIS_WEBDAV_IO;
  }
  if (errno != ENOENT) {
    vectis_webdav_fd_close(&parent_fd);
    return VECTIS_WEBDAV_IO;
  }
  ok = mkdirat(parent_fd, leaf, 0700) == 0;
  vectis_webdav_fd_close(&parent_fd);
  return ok ? VECTIS_WEBDAV_OK : VECTIS_WEBDAV_IO;
}

static int vectis_webdav_direct_copy_file_at(int source_parent_fd,
                                             const char *source_name,
                                             int destination_parent_fd,
                                             const char *destination_name,
                                             size_t max_file_bytes) {
  unsigned char *body;
  char etag[VECTIS_WEBDAV_ETAG_LENGTH + 1u];
  size_t body_size;
  int source_fd;
  int ok;

  body = NULL;
  body_size = 0u;
  source_fd =
      openat(source_parent_fd, source_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (source_fd < 0) {
    return 0;
  }
  ok =
      vectis_webdav_read_fd(source_fd, max_file_bytes, &body, &body_size, etag);
  vectis_webdav_fd_close(&source_fd);
  if (!ok) {
    free(body);
    return 0;
  }
  ok = vectis_webdav_write_atomic_at(destination_parent_fd, destination_name,
                                     body, body_size);
  free(body);
  return ok;
}

static int vectis_webdav_direct_copy_tree_at(int source_parent_fd,
                                             const char *source_name,
                                             int destination_parent_fd,
                                             const char *destination_name,
                                             size_t max_file_bytes) {
  DIR *directory;
  struct dirent *item;
  struct stat st;
  int destination_fd;
  int scan_fd;
  int source_fd;

  if (fstatat(source_parent_fd, source_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
    return 0;
  }
  if (S_ISREG(st.st_mode)) {
    return vectis_webdav_direct_copy_file_at(source_parent_fd, source_name,
                                             destination_parent_fd,
                                             destination_name, max_file_bytes);
  }
  if (!S_ISDIR(st.st_mode) ||
      mkdirat(destination_parent_fd, destination_name, 0700) != 0) {
    return 0;
  }
  source_fd = vectis_webdav_open_child_dir(source_parent_fd, source_name);
  if (source_fd < 0) {
    (void)unlinkat(destination_parent_fd, destination_name, AT_REMOVEDIR);
    return 0;
  }
  destination_fd =
      vectis_webdav_open_child_dir(destination_parent_fd, destination_name);
  if (destination_fd < 0) {
    vectis_webdav_fd_close(&source_fd);
    (void)vectis_webdav_direct_remove_tree_at(destination_parent_fd,
                                              destination_name);
    return 0;
  }
  scan_fd = dup(source_fd);
  if (scan_fd < 0) {
    vectis_webdav_fd_close(&source_fd);
    vectis_webdav_fd_close(&destination_fd);
    (void)vectis_webdav_direct_remove_tree_at(destination_parent_fd,
                                              destination_name);
    return 0;
  }
  directory = fdopendir(scan_fd);
  if (directory == NULL) {
    vectis_webdav_fd_close(&scan_fd);
    vectis_webdav_fd_close(&source_fd);
    vectis_webdav_fd_close(&destination_fd);
    (void)vectis_webdav_direct_remove_tree_at(destination_parent_fd,
                                              destination_name);
    return 0;
  }
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
      continue;
    }
    if (!vectis_webdav_direct_copy_tree_at(source_fd, item->d_name,
                                           destination_fd, item->d_name,
                                           max_file_bytes)) {
      (void)closedir(directory);
      vectis_webdav_fd_close(&source_fd);
      vectis_webdav_fd_close(&destination_fd);
      (void)vectis_webdav_direct_remove_tree_at(destination_parent_fd,
                                                destination_name);
      return 0;
    }
  }
  (void)closedir(directory);
  vectis_webdav_fd_close(&source_fd);
  vectis_webdav_fd_close(&destination_fd);
  return 1;
}

static vectis_webdav_status vectis_webdav_direct_copy_or_move_fs(
    const vectis_webdav_config *config, const char *normalized_source,
    const char *normalized_destination, int overwrite, int remove_source) {
  char backup_leaf[VECTIS_WEBDAV_PATH_MAX + 1u];
  char destination_leaf[VECTIS_WEBDAV_PATH_MAX + 1u];
  char source_leaf[VECTIS_WEBDAV_PATH_MAX + 1u];
  char stage_leaf[VECTIS_WEBDAV_PATH_MAX + 1u];
  char txn_leaf[VECTIS_WEBDAV_PATH_MAX + 1u];
  struct stat st;
  int backup_created;
  int destination_exists;
  int destination_parent_fd;
  int source_parent_fd;
  int staged;
  int txn_fd;
  int ok;

  source_parent_fd =
      vectis_webdav_open_parent_fd(config, normalized_source, 0, source_leaf);
  if (source_parent_fd < 0) {
    return errno == ENOENT ? VECTIS_WEBDAV_NOT_FOUND : VECTIS_WEBDAV_IO;
  }
  if (fstatat(source_parent_fd, source_leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) {
    vectis_webdav_fd_close(&source_parent_fd);
    return errno == ENOENT ? VECTIS_WEBDAV_NOT_FOUND : VECTIS_WEBDAV_IO;
  }
  if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
    vectis_webdav_fd_close(&source_parent_fd);
    return VECTIS_WEBDAV_IO;
  }
  destination_parent_fd = vectis_webdav_open_parent_fd(
      config, normalized_destination, 1, destination_leaf);
  if (destination_parent_fd < 0) {
    vectis_webdav_fd_close(&source_parent_fd);
    return VECTIS_WEBDAV_IO;
  }
  destination_exists = fstatat(destination_parent_fd, destination_leaf, &st,
                               AT_SYMLINK_NOFOLLOW) == 0;
  if (destination_exists) {
    if (!overwrite) {
      vectis_webdav_fd_close(&source_parent_fd);
      vectis_webdav_fd_close(&destination_parent_fd);
      return VECTIS_WEBDAV_EXISTS;
    }
  } else if (errno != ENOENT) {
    vectis_webdav_fd_close(&source_parent_fd);
    vectis_webdav_fd_close(&destination_parent_fd);
    return VECTIS_WEBDAV_IO;
  }
  if (!overwrite && !destination_exists) {
    ok = vectis_webdav_direct_copy_tree_at(
        source_parent_fd, source_leaf, destination_parent_fd, destination_leaf,
        config->max_file_bytes);
    if (ok && remove_source) {
      ok = vectis_webdav_direct_remove_tree_at(source_parent_fd, source_leaf);
      if (!ok) {
        (void)vectis_webdav_direct_remove_tree_at(destination_parent_fd,
                                                  destination_leaf);
      }
    } else if (!ok) {
      (void)vectis_webdav_direct_remove_tree_at(destination_parent_fd,
                                                destination_leaf);
    }
    vectis_webdav_fd_close(&source_parent_fd);
    vectis_webdav_fd_close(&destination_parent_fd);
    return ok ? VECTIS_WEBDAV_OK : VECTIS_WEBDAV_IO;
  }

  if (!vectis_webdav_direct_reserve_dir_at(destination_parent_fd, ".vectis-txn",
                                           txn_leaf)) {
    vectis_webdav_fd_close(&source_parent_fd);
    vectis_webdav_fd_close(&destination_parent_fd);
    return VECTIS_WEBDAV_IO;
  }
  txn_fd = vectis_webdav_open_child_dir(destination_parent_fd, txn_leaf);
  if (txn_fd < 0) {
    (void)vectis_webdav_direct_remove_tree_at(destination_parent_fd, txn_leaf);
    vectis_webdav_fd_close(&source_parent_fd);
    vectis_webdav_fd_close(&destination_parent_fd);
    return VECTIS_WEBDAV_IO;
  }
  (void)snprintf(stage_leaf, sizeof(stage_leaf), "stage");
  (void)snprintf(backup_leaf, sizeof(backup_leaf), "backup");
  backup_created = 0;
  staged = 0;
  ok = vectis_webdav_direct_copy_tree_at(source_parent_fd, source_leaf, txn_fd,
                                         stage_leaf, config->max_file_bytes);
  if (!ok) {
    (void)vectis_webdav_direct_remove_tree_at(destination_parent_fd, txn_leaf);
    vectis_webdav_fd_close(&txn_fd);
    vectis_webdav_fd_close(&source_parent_fd);
    vectis_webdav_fd_close(&destination_parent_fd);
    return VECTIS_WEBDAV_IO;
  }
  staged = 1;
  if (destination_exists) {
    if (renameat(destination_parent_fd, destination_leaf, txn_fd,
                 backup_leaf) != 0) {
      (void)vectis_webdav_direct_remove_tree_at(destination_parent_fd,
                                                txn_leaf);
      vectis_webdav_fd_close(&txn_fd);
      vectis_webdav_fd_close(&source_parent_fd);
      vectis_webdav_fd_close(&destination_parent_fd);
      return VECTIS_WEBDAV_IO;
    }
    backup_created = 1;
  }
  if (renameat(txn_fd, stage_leaf, destination_parent_fd, destination_leaf) !=
      0) {
    if (backup_created) {
      (void)renameat(txn_fd, backup_leaf, destination_parent_fd,
                     destination_leaf);
    }
    (void)vectis_webdav_direct_remove_tree_at(destination_parent_fd, txn_leaf);
    vectis_webdav_fd_close(&txn_fd);
    vectis_webdav_fd_close(&source_parent_fd);
    vectis_webdav_fd_close(&destination_parent_fd);
    return VECTIS_WEBDAV_IO;
  }
  staged = 0;
  if (ok && remove_source) {
    ok = vectis_webdav_direct_remove_tree_at(source_parent_fd, source_leaf);
    if (!ok) {
      (void)vectis_webdav_direct_remove_tree_at(destination_parent_fd,
                                                destination_leaf);
      if (backup_created) {
        (void)renameat(txn_fd, backup_leaf, destination_parent_fd,
                       destination_leaf);
        backup_created = 0;
      }
    }
  }
  if (backup_created) {
    if (ok) {
      (void)vectis_webdav_direct_remove_tree_at(txn_fd, backup_leaf);
    } else {
      (void)renameat(txn_fd, backup_leaf, destination_parent_fd,
                     destination_leaf);
    }
  }
  if (staged) {
    (void)vectis_webdav_direct_remove_tree_at(txn_fd, stage_leaf);
  }
  vectis_webdav_fd_close(&txn_fd);
  (void)vectis_webdav_direct_remove_tree_at(destination_parent_fd, txn_leaf);
  vectis_webdav_fd_close(&source_parent_fd);
  vectis_webdav_fd_close(&destination_parent_fd);
  return ok ? VECTIS_WEBDAV_OK : VECTIS_WEBDAV_IO;
}

void vectis_webdav_config_init(vectis_webdav_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->max_file_bytes = VECTIS_WEBDAV_DEFAULT_MAX_FILE;
  config->max_total_bytes = VECTIS_WEBDAV_DEFAULT_MAX_TOTAL;
  config->max_resources = VECTIS_WEBDAV_DEFAULT_MAX_RESOURCES;
}

int vectis_webdav_path_normalize(const char *path,
                                 char out[VECTIS_WEBDAV_PATH_MAX + 1u]) {
  size_t input;
  size_t output;
  size_t segment_start;

  if (path == NULL || out == NULL || path[0] != '/') {
    return 0;
  }
  input = 1u;
  output = 1u;
  out[0] = '/';
  while (path[input] != '\0') {
    while (path[input] == '/') {
      input++;
    }
    if (path[input] == '\0') {
      break;
    }
    segment_start = input;
    while (path[input] != '\0' && path[input] != '/') {
      unsigned char value;

      value = (unsigned char)path[input];
      if (!(isalnum(value) || value == '.' || value == '_' || value == '-')) {
        return 0;
      }
      input++;
    }
    if (input == segment_start ||
        (input - segment_start == 1u && path[segment_start] == '.') ||
        (input - segment_start == 2u && path[segment_start] == '.' &&
         path[segment_start + 1u] == '.')) {
      return 0;
    }
    if (output > 1u) {
      if (output + 1u >= VECTIS_WEBDAV_PATH_MAX + 1u) {
        return 0;
      }
      out[output++] = '/';
    }
    if (output + input - segment_start >= VECTIS_WEBDAV_PATH_MAX + 1u) {
      return 0;
    }
    memcpy(out + output, path + segment_start, input - segment_start);
    output += input - segment_start;
  }
  out[output] = '\0';
  return 1;
}

const char *vectis_webdav_status_string(vectis_webdav_status status) {
  switch (status) {
  case VECTIS_WEBDAV_OK:
    return "ok";
  case VECTIS_WEBDAV_NOT_FOUND:
    return "not found";
  case VECTIS_WEBDAV_EXISTS:
    return "exists";
  case VECTIS_WEBDAV_INVALID:
    return "invalid";
  case VECTIS_WEBDAV_LIMIT:
    return "limit";
  case VECTIS_WEBDAV_IO:
    return "io";
  case VECTIS_WEBDAV_NOMEM:
    return "out of memory";
  case VECTIS_WEBDAV_CONFLICT:
    return "conflict";
  case VECTIS_WEBDAV_TOMBSTONED:
    return "tombstoned";
  default:
    return "unknown";
  }
}

vectis_status
vectis_webdav_content_dir(const vectis_webdav_config *config,
                          char out[VECTIS_WEBDAV_STORAGE_PATH_MAX],
                          vectis_error *error) {
  char base[VECTIS_WEBDAV_STORAGE_PATH_MAX];

  if (out == NULL) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "WebDAV content dir output is required");
    return VECTIS_ERR_INVALID;
  }
  out[0] = '\0';
  if (vectis_webdav_direct_root(config)) {
    if (strlen(config->root_dir) >= VECTIS_WEBDAV_STORAGE_PATH_MAX) {
      vectis_set_error(error, VECTIS_ERR_INVALID,
                       "WebDAV root_dir is too long");
      return VECTIS_ERR_INVALID;
    }
    strcpy(out, config->root_dir);
    vectis_error_clear(error);
    return VECTIS_OK;
  }
  if (!vectis_webdav_base(config, base, sizeof(base)) ||
      !vectis_webdav_path_append(out, VECTIS_WEBDAV_STORAGE_PATH_MAX, base,
                                 "content")) {
    vectis_set_error(error, VECTIS_ERR_INVALID,
                     "WebDAV storage config is invalid");
    return VECTIS_ERR_INVALID;
  }
  vectis_error_clear(error);
  return VECTIS_OK;
}

vectis_webdav_status vectis_webdav_lookup(const vectis_webdav_config *config,
                                          const char *path,
                                          vectis_webdav_entry *entry) {
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  char disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  struct stat st;

  if (entry == NULL || !vectis_webdav_path_normalize(path, normalized) ||
      !vectis_webdav_config_valid(config)) {
    return VECTIS_WEBDAV_INVALID;
  }
  memset(entry, 0, sizeof(*entry));
  if (vectis_webdav_tombstone_exists(config, normalized)) {
    entry->kind = VECTIS_WEBDAV_ENTRY_TOMBSTONE;
    return VECTIS_WEBDAV_OK;
  }
  if (!vectis_webdav_disk_path(config, "content", normalized, disk)) {
    return VECTIS_WEBDAV_INVALID;
  }
  if (vectis_webdav_direct_root(config)) {
    int fd;

    fd = vectis_webdav_direct_open_existing(config, normalized, 0, &st);
    if (fd >= 0) {
      vectis_webdav_fd_close(&fd);
      entry->kind = VECTIS_WEBDAV_ENTRY_FILE;
      entry->size = (size_t)st.st_size;
      strcpy(entry->storage_path, disk);
      return VECTIS_WEBDAV_OK;
    }
    fd = vectis_webdav_direct_open_existing(config, normalized, 1, NULL);
    if (fd >= 0) {
      vectis_webdav_fd_close(&fd);
      entry->kind = VECTIS_WEBDAV_ENTRY_COLLECTION;
      return VECTIS_WEBDAV_OK;
    }
    return errno == ENOENT ? VECTIS_WEBDAV_NOT_FOUND : VECTIS_WEBDAV_IO;
  }
  if (vectis_webdav_file_regular(disk, &st)) {
    entry->kind = VECTIS_WEBDAV_ENTRY_FILE;
    entry->size = (size_t)st.st_size;
    strcpy(entry->storage_path, disk);
    return VECTIS_WEBDAV_OK;
  }
  if (vectis_webdav_file_directory(disk)) {
    entry->kind = VECTIS_WEBDAV_ENTRY_COLLECTION;
    return VECTIS_WEBDAV_OK;
  }
  return VECTIS_WEBDAV_NOT_FOUND;
}

vectis_webdav_status vectis_webdav_read(const vectis_webdav_config *config,
                                        const char *path, unsigned char **body,
                                        size_t *body_size,
                                        vectis_webdav_entry *entry) {
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  vectis_webdav_status status;
  int lock_fd;

  if (body == NULL || body_size == NULL || entry == NULL ||
      !vectis_webdav_path_normalize(path, normalized)) {
    return VECTIS_WEBDAV_INVALID;
  }
  *body = NULL;
  *body_size = 0u;
  status = vectis_webdav_lookup(config, path, entry);
  if (status != VECTIS_WEBDAV_OK || entry->kind != VECTIS_WEBDAV_ENTRY_FILE) {
    return status;
  }
  lock_fd = vectis_webdav_lock(config);
  if (lock_fd < 0) {
    return VECTIS_WEBDAV_IO;
  }
  status = vectis_webdav_lookup(config, path, entry);
  if (status != VECTIS_WEBDAV_OK || entry->kind != VECTIS_WEBDAV_ENTRY_FILE) {
    vectis_webdav_unlock(lock_fd);
    return status;
  }
  if (vectis_webdav_direct_root(config)
          ? !vectis_webdav_direct_read_file(config, normalized, body, body_size,
                                            entry->etag)
          : !vectis_webdav_read_file(entry->storage_path, body, body_size,
                                     entry->etag)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  entry->size = *body_size;
  vectis_webdav_unlock(lock_fd);
  return VECTIS_WEBDAV_OK;
}

static vectis_webdav_status
vectis_webdav_store_locked(const vectis_webdav_config *config,
                           const char *normalized, const unsigned char *body,
                           size_t body_size) {
  char disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char content_root[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  struct stat old;
  uint64_t usage;
  uint64_t resources;
  uint64_t old_size;
  uint64_t additional_resources;

  if (vectis_webdav_ancestor_tombstone_exists(config, normalized)) {
    return VECTIS_WEBDAV_CONFLICT;
  }
  if (!vectis_webdav_disk_path(config, "content", normalized, disk) ||
      !vectis_webdav_disk_path(config, "content", "/", content_root) ||
      !vectis_webdav_usage(config, &usage, &resources)) {
    return VECTIS_WEBDAV_IO;
  }
  if (vectis_webdav_file_directory(disk)) {
    return VECTIS_WEBDAV_CONFLICT;
  }
  old_size =
      vectis_webdav_file_regular(disk, &old) ? (uint64_t)old.st_size : 0u;
  additional_resources = 0u;
  if (old_size == 0u && !vectis_webdav_file_regular(disk, NULL) &&
      !vectis_webdav_missing_path_resources(content_root, disk, 1,
                                            &additional_resources)) {
    return VECTIS_WEBDAV_IO;
  }
  if (usage < old_size || usage - old_size > config->max_total_bytes ||
      body_size > config->max_total_bytes - (usage - old_size) ||
      !vectis_webdav_resources_within_limit(config, resources,
                                            additional_resources)) {
    return VECTIS_WEBDAV_LIMIT;
  }
  /* Keep a prior tombstone effective until replacement content is durable. */
  if (vectis_webdav_direct_root(config)) {
    char leaf[VECTIS_WEBDAV_PATH_MAX + 1u];
    int parent_fd;
    int ok;

    parent_fd = vectis_webdav_open_parent_fd(config, normalized, 1, leaf);
    if (parent_fd < 0) {
      return VECTIS_WEBDAV_IO;
    }
    ok = vectis_webdav_write_atomic_at(parent_fd, leaf, body, body_size);
    vectis_webdav_fd_close(&parent_fd);
    if (!ok) {
      return VECTIS_WEBDAV_IO;
    }
  } else if (!vectis_webdav_write_atomic(disk, body, body_size) ||
             !vectis_webdav_clear_tombstones(config, normalized)) {
    return VECTIS_WEBDAV_IO;
  }
  return VECTIS_WEBDAV_OK;
}

vectis_webdav_status vectis_webdav_put(const vectis_webdav_config *config,
                                       const char *path,
                                       const unsigned char *body,
                                       size_t body_size) {
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  vectis_webdav_status status;
  int lock_fd;

  if ((body == NULL && body_size != 0u) ||
      !vectis_webdav_config_valid(config) ||
      !vectis_webdav_path_normalize(path, normalized) ||
      normalized[1] == '\0' || body_size > config->max_file_bytes) {
    return VECTIS_WEBDAV_INVALID;
  }
  lock_fd = vectis_webdav_lock(config);
  if (lock_fd < 0) {
    return VECTIS_WEBDAV_IO;
  }
  status = vectis_webdav_store_locked(config, normalized, body, body_size);
  vectis_webdav_unlock(lock_fd);
  return status;
}

static int vectis_webdav_remove_tree(const char *path) {
  DIR *directory;
  struct dirent *item;
  struct stat st;
  char child[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  int written;

  if (path == NULL) {
    return 0;
  }
  if (lstat(path, &st) == -1) {
    return errno == ENOENT;
  }
  if (S_ISREG(st.st_mode)) {
    return unlink(path) == 0;
  }
  if (!S_ISDIR(st.st_mode)) {
    return 0;
  }
  directory = opendir(path);
  if (directory == NULL) {
    return 0;
  }
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
      continue;
    }
    written = snprintf(child, sizeof(child), "%s/%s", path, item->d_name);
    if (written < 0 || (size_t)written >= sizeof(child) ||
        !vectis_webdav_remove_tree(child)) {
      (void)closedir(directory);
      return 0;
    }
  }
  (void)closedir(directory);
  return rmdir(path) == 0;
}

static int vectis_webdav_disk_usage(const char *path, uint64_t *total,
                                    uint64_t *resources) {
  struct stat st;

  if (path == NULL || total == NULL || resources == NULL ||
      lstat(path, &st) == -1) {
    return errno == ENOENT;
  }
  if (S_ISREG(st.st_mode)) {
    if (st.st_size < 0 || UINT64_MAX - *total < (uint64_t)st.st_size ||
        *resources == UINT64_MAX) {
      return 0;
    }
    *total += (uint64_t)st.st_size;
    (*resources)++;
    return 1;
  }
  if (S_ISDIR(st.st_mode)) {
    if (*resources == UINT64_MAX) {
      return 0;
    }
    (*resources)++;
    return vectis_webdav_usage_path(path, total, resources, 0);
  }
  return 0;
}

static int vectis_webdav_copy_file(const char *source,
                                   const char *destination) {
  unsigned char *body;
  char etag[VECTIS_WEBDAV_ETAG_LENGTH + 1u];
  size_t body_size;

  body = NULL;
  body_size = 0u;
  if (!vectis_webdav_read_file(source, &body, &body_size, etag) ||
      !vectis_webdav_write_atomic(destination, body, body_size)) {
    free(body);
    return 0;
  }
  free(body);
  return 1;
}

static int vectis_webdav_copy_tree(const char *source,
                                   const char *destination) {
  DIR *directory;
  struct dirent *item;
  struct stat st;
  char source_child[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char destination_child[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  int written_source;
  int written_destination;

  if (source == NULL || destination == NULL || lstat(source, &st) == -1) {
    return 0;
  }
  if (S_ISREG(st.st_mode)) {
    return vectis_webdav_copy_file(source, destination);
  }
  if (!S_ISDIR(st.st_mode) || mkdir(destination, 0700) == -1) {
    return 0;
  }
  directory = opendir(source);
  if (directory == NULL) {
    (void)rmdir(destination);
    return 0;
  }
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
      continue;
    }
    written_source = snprintf(source_child, sizeof(source_child), "%s/%s",
                              source, item->d_name);
    written_destination = snprintf(destination_child, sizeof(destination_child),
                                   "%s/%s", destination, item->d_name);
    if (written_source < 0 || (size_t)written_source >= sizeof(source_child) ||
        written_destination < 0 ||
        (size_t)written_destination >= sizeof(destination_child) ||
        !vectis_webdav_copy_tree(source_child, destination_child)) {
      (void)closedir(directory);
      (void)vectis_webdav_remove_tree(destination);
      return 0;
    }
  }
  (void)closedir(directory);
  return 1;
}

static int
vectis_webdav_transaction_dir(const vectis_webdav_config *config,
                              char out[VECTIS_WEBDAV_STORAGE_PATH_MAX]) {
  char base[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  int written;

  if (!vectis_webdav_base(config, base, sizeof(base))) {
    return 0;
  }
  written = snprintf(out, VECTIS_WEBDAV_STORAGE_PATH_MAX,
                     "%s/.transaction-XXXXXX", base);
  return written >= 0 && (size_t)written < VECTIS_WEBDAV_STORAGE_PATH_MAX &&
         mkdtemp(out) != NULL;
}

static int
vectis_webdav_transaction_path(const char *transaction, const char *name,
                               char out[VECTIS_WEBDAV_STORAGE_PATH_MAX]) {
  int written;

  if (transaction == NULL || name == NULL) {
    return 0;
  }
  written =
      snprintf(out, VECTIS_WEBDAV_STORAGE_PATH_MAX, "%s/%s", transaction, name);
  return written >= 0 && (size_t)written < VECTIS_WEBDAV_STORAGE_PATH_MAX;
}

static int vectis_webdav_move_to_backup(const char *live, const char *backup,
                                        int *moved) {
  struct stat st;

  if (live == NULL || backup == NULL || moved == NULL) {
    return 0;
  }
  *moved = 0;
  if (lstat(live, &st) == -1) {
    return errno == ENOENT;
  }
  if (rename(live, backup) == -1) {
    return 0;
  }
  *moved = 1;
  return 1;
}

static void vectis_webdav_restore_backup(const char *live, const char *backup,
                                         int moved) {
  if (live == NULL || backup == NULL) {
    return;
  }
  (void)vectis_webdav_remove_tree(live);
  if (moved) {
    (void)rename(backup, live);
  }
}

static int vectis_webdav_create_destination_parent(
    const char *content_root, const char *destination,
    char created[VECTIS_WEBDAV_STORAGE_PATH_MAX]) {
  char candidate[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char parent[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  struct stat st;

  if (content_root == NULL || destination == NULL || created == NULL ||
      !vectis_webdav_parent_dir(destination, candidate, sizeof(candidate))) {
    return 0;
  }
  created[0] = '\0';
  while (strcmp(candidate, content_root) != 0) {
    if (lstat(candidate, &st) == 0) {
      if (!S_ISDIR(st.st_mode)) {
        return 0;
      }
      break;
    }
    if (errno != ENOENT) {
      return 0;
    }
    strcpy(created, candidate);
    if (!vectis_webdav_parent_dir(candidate, parent, sizeof(parent))) {
      return 0;
    }
    strcpy(candidate, parent);
  }
  if (strcmp(candidate, content_root) == 0 &&
      !vectis_webdav_file_directory(content_root)) {
    return 0;
  }
  return created[0] == '\0' || vectis_webdav_mkdir_p(created);
}

vectis_webdav_status vectis_webdav_delete(const vectis_webdav_config *config,
                                          const char *path) {
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  char content[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char tombstone[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char tombstone_root[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  uint64_t usage;
  uint64_t resources;
  uint64_t additional_resources;
  vectis_webdav_status status;
  int lock_fd;

  if (!vectis_webdav_path_normalize(path, normalized) ||
      normalized[1] == '\0') {
    return VECTIS_WEBDAV_INVALID;
  }
  lock_fd = vectis_webdav_lock(config);
  if (lock_fd < 0) {
    return VECTIS_WEBDAV_IO;
  }
  if (vectis_webdav_ancestor_tombstone_exists(config, normalized)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_CONFLICT;
  }
  if (!vectis_webdav_disk_path(config, "content", normalized, content) ||
      !vectis_webdav_usage(config, &usage, &resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  if (vectis_webdav_direct_root(config)) {
    (void)usage;
    (void)resources;
    status = vectis_webdav_direct_remove(config, normalized);
    if (status != VECTIS_WEBDAV_OK) {
      vectis_webdav_unlock(lock_fd);
      return status;
    }
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_OK;
  }
  if (!vectis_webdav_disk_path(config, "tombstones", normalized, tombstone) ||
      !vectis_webdav_disk_path(config, "tombstones", "/", tombstone_root)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  additional_resources = 0u;
  if (!vectis_webdav_missing_path_resources(tombstone_root, tombstone, 1,
                                            &additional_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  if (!vectis_webdav_resources_within_limit(config, resources,
                                            additional_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_LIMIT;
  }
  if (!vectis_webdav_clear_tombstones(config, normalized) ||
      !vectis_webdav_touch_atomic(tombstone) ||
      !vectis_webdav_remove_tree(content)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  vectis_webdav_unlock(lock_fd);
  return VECTIS_WEBDAV_OK;
}

vectis_webdav_status vectis_webdav_mkcol(const vectis_webdav_config *config,
                                         const char *path) {
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  char disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char content_root[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  uint64_t usage;
  uint64_t resources;
  uint64_t additional_resources;
  vectis_webdav_status status;
  int lock_fd;

  if (!vectis_webdav_path_normalize(path, normalized) ||
      normalized[1] == '\0') {
    return VECTIS_WEBDAV_INVALID;
  }
  lock_fd = vectis_webdav_lock(config);
  if (lock_fd < 0) {
    return VECTIS_WEBDAV_IO;
  }
  if (vectis_webdav_ancestor_tombstone_exists(config, normalized)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_CONFLICT;
  }
  if (!vectis_webdav_disk_path(config, "content", normalized, disk) ||
      !vectis_webdav_disk_path(config, "content", "/", content_root) ||
      !vectis_webdav_usage(config, &usage, &resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  if (vectis_webdav_file_regular(disk, NULL) ||
      vectis_webdav_file_directory(disk)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_EXISTS;
  }
  additional_resources = 0u;
  if (!vectis_webdav_missing_path_resources(content_root, disk, 1,
                                            &additional_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  if (!vectis_webdav_resources_within_limit(config, resources,
                                            additional_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_LIMIT;
  }
  if (vectis_webdav_direct_root(config)) {
    status = vectis_webdav_direct_mkcol(config, normalized);
    if (status != VECTIS_WEBDAV_OK) {
      vectis_webdav_unlock(lock_fd);
      return status;
    }
  } else if (!vectis_webdav_mkdir_p(disk) ||
             !vectis_webdav_clear_tombstones(config, normalized)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  vectis_webdav_unlock(lock_fd);
  return VECTIS_WEBDAV_OK;
}

static vectis_webdav_status
vectis_webdav_copy_or_move_direct(const vectis_webdav_config *config,
                                  const char *source, const char *destination,
                                  int overwrite, int remove_source,
                                  int destination_embedded_exists) {
  vectis_webdav_entry source_entry;
  vectis_webdav_entry destination_entry;
  char normalized_source[VECTIS_WEBDAV_PATH_MAX + 1u];
  char normalized_destination[VECTIS_WEBDAV_PATH_MAX + 1u];
  char source_disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char destination_disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char content_root[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  uint64_t usage;
  uint64_t resources;
  uint64_t source_usage;
  uint64_t destination_usage;
  uint64_t source_resources;
  uint64_t destination_resources;
  uint64_t additional_resources;
  vectis_webdav_status status;
  int lock_fd;

  if (!vectis_webdav_config_valid(config) ||
      !vectis_webdav_direct_root(config) ||
      !vectis_webdav_path_normalize(source, normalized_source) ||
      !vectis_webdav_path_normalize(destination, normalized_destination) ||
      normalized_source[1] == '\0' || normalized_destination[1] == '\0' ||
      strcmp(normalized_source, normalized_destination) == 0 ||
      (strncmp(normalized_source, normalized_destination,
               strlen(normalized_destination)) == 0 &&
       normalized_source[strlen(normalized_destination)] == '/')) {
    return VECTIS_WEBDAV_INVALID;
  }
  lock_fd = vectis_webdav_lock(config);
  if (lock_fd < 0) {
    return VECTIS_WEBDAV_IO;
  }
  status = vectis_webdav_lookup(config, normalized_source, &source_entry);
  if (status != VECTIS_WEBDAV_OK ||
      (source_entry.kind != VECTIS_WEBDAV_ENTRY_FILE &&
       source_entry.kind != VECTIS_WEBDAV_ENTRY_COLLECTION)) {
    vectis_webdav_unlock(lock_fd);
    return status == VECTIS_WEBDAV_OK ? VECTIS_WEBDAV_TOMBSTONED : status;
  }
  if (source_entry.kind == VECTIS_WEBDAV_ENTRY_COLLECTION &&
      strncmp(normalized_destination, normalized_source,
              strlen(normalized_source)) == 0 &&
      normalized_destination[strlen(normalized_source)] == '/') {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_INVALID;
  }
  status =
      vectis_webdav_lookup(config, normalized_destination, &destination_entry);
  if (!overwrite &&
      ((status == VECTIS_WEBDAV_OK &&
        destination_entry.kind != VECTIS_WEBDAV_ENTRY_TOMBSTONE) ||
       (status == VECTIS_WEBDAV_NOT_FOUND && destination_embedded_exists))) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_EXISTS;
  }
  if (status == VECTIS_WEBDAV_OK &&
      destination_entry.kind == VECTIS_WEBDAV_ENTRY_COLLECTION &&
      source_entry.kind == VECTIS_WEBDAV_ENTRY_FILE) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_CONFLICT;
  }
  if (status != VECTIS_WEBDAV_OK && status != VECTIS_WEBDAV_NOT_FOUND) {
    vectis_webdav_unlock(lock_fd);
    return status;
  }
  if (!vectis_webdav_disk_path(config, "content", normalized_source,
                               source_disk) ||
      !vectis_webdav_disk_path(config, "content", normalized_destination,
                               destination_disk) ||
      !vectis_webdav_disk_path(config, "content", "/", content_root) ||
      !vectis_webdav_usage(config, &usage, &resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  source_usage = 0u;
  destination_usage = 0u;
  source_resources = 0u;
  destination_resources = 0u;
  if (!vectis_webdav_disk_usage(source_disk, &source_usage,
                                &source_resources) ||
      !vectis_webdav_disk_usage(destination_disk, &destination_usage,
                                &destination_resources) ||
      usage < destination_usage ||
      (!remove_source && (usage - destination_usage > config->max_total_bytes ||
                          source_usage > config->max_total_bytes -
                                             (usage - destination_usage)))) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_LIMIT;
  }
  if (resources < destination_resources) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  resources -= destination_resources;
  additional_resources = 0u;
  if (!vectis_webdav_missing_path_resources(content_root, destination_disk, 0,
                                            &additional_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  if (!remove_source &&
      !vectis_webdav_add_resources(&additional_resources, source_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_LIMIT;
  }
  if (!vectis_webdav_resources_within_limit(config, resources,
                                            additional_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_LIMIT;
  }
  status = vectis_webdav_direct_copy_or_move_fs(config, normalized_source,
                                                normalized_destination,
                                                overwrite, remove_source);
  vectis_webdav_unlock(lock_fd);
  return status;
}

static vectis_webdav_status vectis_webdav_copy_or_move(
    const vectis_webdav_config *config, const char *source,
    const char *destination, int overwrite, int remove_source,
    int embedded_source, const unsigned char *embedded_body,
    size_t embedded_size, int destination_embedded_exists) {
  vectis_webdav_entry source_entry;
  vectis_webdav_entry destination_entry;
  char normalized_source[VECTIS_WEBDAV_PATH_MAX + 1u];
  char normalized_destination[VECTIS_WEBDAV_PATH_MAX + 1u];
  char source_disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char destination_disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char content_root[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char tombstone_root[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char source_tombstone[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char destination_tombstone[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char transaction[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char staged_content[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char staged_tombstones[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char staged_source_tombstone[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char staged_destination_tombstone[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char staged_destination_tombstone_parent[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char backup_destination[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char backup_source[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char backup_tombstones[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char created_destination_parent[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  struct stat source_tombstone_stat;
  uint64_t usage;
  uint64_t resources;
  uint64_t source_usage;
  uint64_t destination_usage;
  uint64_t source_resources;
  uint64_t destination_resources;
  uint64_t source_tombstone_usage;
  uint64_t source_tombstone_resources;
  uint64_t destination_tombstone_usage;
  uint64_t destination_tombstone_resources;
  uint64_t additional_resources;
  vectis_webdav_status status;
  int destination_installed;
  int destination_moved;
  int source_moved;
  int source_tombstone_present;
  int tombstones_moved;
  int lock_fd;

  if (!vectis_webdav_config_valid(config) ||
      (embedded_source && ((embedded_body == NULL && embedded_size != 0u) ||
                           embedded_size > config->max_file_bytes)) ||
      !vectis_webdav_path_normalize(source, normalized_source) ||
      !vectis_webdav_path_normalize(destination, normalized_destination) ||
      normalized_source[1] == '\0' || normalized_destination[1] == '\0' ||
      strcmp(normalized_source, normalized_destination) == 0 ||
      (strncmp(normalized_source, normalized_destination,
               strlen(normalized_destination)) == 0 &&
       normalized_source[strlen(normalized_destination)] == '/')) {
    return VECTIS_WEBDAV_INVALID;
  }
  if (vectis_webdav_direct_root(config)) {
    if (embedded_source) {
      return VECTIS_WEBDAV_INVALID;
    }
    return vectis_webdav_copy_or_move_direct(config, source, destination,
                                             overwrite, remove_source,
                                             destination_embedded_exists);
  }
  lock_fd = vectis_webdav_lock(config);
  if (lock_fd < 0) {
    return VECTIS_WEBDAV_IO;
  }
  if (vectis_webdav_ancestor_tombstone_exists(config, normalized_source)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_TOMBSTONED;
  }
  if (vectis_webdav_ancestor_tombstone_exists(config, normalized_destination)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_CONFLICT;
  }
  status = vectis_webdav_lookup(config, normalized_source, &source_entry);
  if (embedded_source) {
    if (status == VECTIS_WEBDAV_OK &&
        source_entry.kind == VECTIS_WEBDAV_ENTRY_TOMBSTONE) {
      vectis_webdav_unlock(lock_fd);
      return VECTIS_WEBDAV_TOMBSTONED;
    }
    if (status != VECTIS_WEBDAV_NOT_FOUND) {
      vectis_webdav_unlock(lock_fd);
      return status == VECTIS_WEBDAV_OK ? VECTIS_WEBDAV_EXISTS : status;
    }
    memset(&source_entry, 0, sizeof(source_entry));
    source_entry.kind = VECTIS_WEBDAV_ENTRY_FILE;
  } else if (status != VECTIS_WEBDAV_OK ||
             (source_entry.kind != VECTIS_WEBDAV_ENTRY_FILE &&
              source_entry.kind != VECTIS_WEBDAV_ENTRY_COLLECTION)) {
    vectis_webdav_unlock(lock_fd);
    return status == VECTIS_WEBDAV_OK ? VECTIS_WEBDAV_TOMBSTONED : status;
  }
  if (source_entry.kind == VECTIS_WEBDAV_ENTRY_COLLECTION &&
      strncmp(normalized_destination, normalized_source,
              strlen(normalized_source)) == 0 &&
      normalized_destination[strlen(normalized_source)] == '/') {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_INVALID;
  }
  status =
      vectis_webdav_lookup(config, normalized_destination, &destination_entry);
  if (!overwrite &&
      ((status == VECTIS_WEBDAV_OK &&
        destination_entry.kind != VECTIS_WEBDAV_ENTRY_TOMBSTONE) ||
       (status == VECTIS_WEBDAV_NOT_FOUND && destination_embedded_exists))) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_EXISTS;
  }
  if (status == VECTIS_WEBDAV_OK &&
      destination_entry.kind == VECTIS_WEBDAV_ENTRY_COLLECTION &&
      source_entry.kind == VECTIS_WEBDAV_ENTRY_FILE) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_CONFLICT;
  }
  if (status != VECTIS_WEBDAV_OK && status != VECTIS_WEBDAV_NOT_FOUND) {
    vectis_webdav_unlock(lock_fd);
    return status;
  }
  if (!vectis_webdav_disk_path(config, "content", normalized_source,
                               source_disk) ||
      !vectis_webdav_disk_path(config, "content", normalized_destination,
                               destination_disk) ||
      !vectis_webdav_disk_path(config, "content", "/", content_root) ||
      !vectis_webdav_disk_path(config, "tombstones", "/", tombstone_root) ||
      !vectis_webdav_disk_path(config, "tombstones", normalized_source,
                               source_tombstone) ||
      !vectis_webdav_disk_path(config, "tombstones", normalized_destination,
                               destination_tombstone) ||
      !vectis_webdav_usage(config, &usage, &resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  source_usage = embedded_source ? embedded_size : 0u;
  destination_usage = 0u;
  source_resources = embedded_source ? 1u : 0u;
  destination_resources = 0u;
  source_tombstone_usage = 0u;
  source_tombstone_resources = 0u;
  destination_tombstone_usage = 0u;
  destination_tombstone_resources = 0u;
  if ((!embedded_source && !vectis_webdav_disk_usage(source_disk, &source_usage,
                                                     &source_resources)) ||
      !vectis_webdav_disk_usage(destination_disk, &destination_usage,
                                &destination_resources) ||
      usage < destination_usage ||
      ((embedded_source || !remove_source) &&
       (usage - destination_usage > config->max_total_bytes ||
        source_usage >
            config->max_total_bytes - (usage - destination_usage)))) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_LIMIT;
  }
  if (resources < destination_resources) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  resources -= destination_resources;
  additional_resources = 0u;
  if (!vectis_webdav_missing_path_resources(content_root, destination_disk, 0,
                                            &additional_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  if (source_entry.kind == VECTIS_WEBDAV_ENTRY_COLLECTION &&
      !vectis_webdav_disk_usage(source_tombstone, &source_tombstone_usage,
                                &source_tombstone_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  if (!vectis_webdav_disk_usage(destination_tombstone,
                                &destination_tombstone_usage,
                                &destination_tombstone_resources) ||
      resources < destination_tombstone_resources) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  resources -= destination_tombstone_resources;
  if ((embedded_source || !remove_source) &&
      !vectis_webdav_add_resources(&additional_resources, source_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_LIMIT;
  }
  if (!remove_source && source_entry.kind == VECTIS_WEBDAV_ENTRY_COLLECTION &&
      !vectis_webdav_add_resources(&additional_resources,
                                   source_tombstone_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_LIMIT;
  }
  if (source_tombstone_resources != 0u &&
      !vectis_webdav_missing_path_resources(
          tombstone_root, destination_tombstone, 0, &additional_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_IO;
  }
  /* A moved tombstone subtree stays counted at its destination. */
  if (remove_source) {
    if (source_tombstone_resources != 0u) {
      if (!vectis_webdav_add_resources(&additional_resources, 1u)) {
        vectis_webdav_unlock(lock_fd);
        return VECTIS_WEBDAV_LIMIT;
      }
    } else if (!vectis_webdav_missing_path_resources(tombstone_root,
                                                     source_tombstone, 1,
                                                     &additional_resources)) {
      vectis_webdav_unlock(lock_fd);
      return VECTIS_WEBDAV_IO;
    }
  }
  if (!vectis_webdav_resources_within_limit(config, resources,
                                            additional_resources)) {
    vectis_webdav_unlock(lock_fd);
    return VECTIS_WEBDAV_LIMIT;
  }
  transaction[0] = '\0';
  created_destination_parent[0] = '\0';
  destination_installed = 0;
  destination_moved = 0;
  source_moved = 0;
  tombstones_moved = 0;
  if (!vectis_webdav_transaction_dir(config, transaction) ||
      !vectis_webdav_transaction_path(transaction, "content", staged_content) ||
      !vectis_webdav_transaction_path(transaction, "tombstones",
                                      staged_tombstones) ||
      !vectis_webdav_transaction_path(transaction, "old-destination",
                                      backup_destination) ||
      !vectis_webdav_transaction_path(transaction, "old-source",
                                      backup_source) ||
      !vectis_webdav_transaction_path(transaction, "old-tombstones",
                                      backup_tombstones) ||
      !(embedded_source
            ? vectis_webdav_write_atomic(staged_content, embedded_body,
                                         embedded_size)
            : vectis_webdav_copy_tree(source_disk, staged_content)) ||
      !vectis_webdav_copy_tree(tombstone_root, staged_tombstones) ||
      !vectis_webdav_path_in_root(staged_tombstones, normalized_source,
                                  staged_source_tombstone) ||
      !vectis_webdav_path_in_root(staged_tombstones, normalized_destination,
                                  staged_destination_tombstone)) {
    status = VECTIS_WEBDAV_IO;
    goto cleanup;
  }
  source_tombstone_present = 0;
  if (source_entry.kind == VECTIS_WEBDAV_ENTRY_COLLECTION) {
    if (lstat(staged_source_tombstone, &source_tombstone_stat) == 0) {
      if (!S_ISREG(source_tombstone_stat.st_mode) &&
          !S_ISDIR(source_tombstone_stat.st_mode)) {
        status = VECTIS_WEBDAV_IO;
        goto cleanup;
      }
      source_tombstone_present = 1;
    } else if (errno != ENOENT) {
      status = VECTIS_WEBDAV_IO;
      goto cleanup;
    }
  }
  if (!vectis_webdav_clear_tombstones_in_root(staged_tombstones,
                                              normalized_destination) ||
      (source_tombstone_present &&
       (!vectis_webdav_parent_dir(
            staged_destination_tombstone, staged_destination_tombstone_parent,
            sizeof(staged_destination_tombstone_parent)) ||
        !vectis_webdav_mkdir_p(staged_destination_tombstone_parent) ||
        !vectis_webdav_copy_tree(staged_source_tombstone,
                                 staged_destination_tombstone))) ||
      (remove_source &&
       (!vectis_webdav_clear_tombstones_in_root(staged_tombstones,
                                                normalized_source) ||
        !vectis_webdav_touch_atomic(staged_source_tombstone)))) {
    status = VECTIS_WEBDAV_IO;
    goto cleanup;
  }
  if (!vectis_webdav_create_destination_parent(content_root, destination_disk,
                                               created_destination_parent)) {
    status = VECTIS_WEBDAV_IO;
    goto cleanup;
  }
  if (!vectis_webdav_move_to_backup(destination_disk, backup_destination,
                                    &destination_moved) ||
      (remove_source && !embedded_source &&
       !vectis_webdav_move_to_backup(source_disk, backup_source,
                                     &source_moved)) ||
      rename(staged_content, destination_disk) == -1) {
    status = VECTIS_WEBDAV_IO;
    goto rollback_content;
  }
  destination_installed = 1;
  if (!vectis_webdav_move_to_backup(tombstone_root, backup_tombstones,
                                    &tombstones_moved) ||
      rename(staged_tombstones, tombstone_root) == -1) {
    status = VECTIS_WEBDAV_IO;
    goto rollback_tombstones;
  }
  (void)vectis_webdav_remove_tree(transaction);
  vectis_webdav_unlock(lock_fd);
  return VECTIS_WEBDAV_OK;

rollback_tombstones:
  if (tombstones_moved) {
    vectis_webdav_restore_backup(tombstone_root, backup_tombstones, 1);
  }
rollback_content:
  if (destination_installed || destination_moved) {
    vectis_webdav_restore_backup(destination_disk, backup_destination,
                                 destination_moved);
  }
  if (source_moved) {
    vectis_webdav_restore_backup(source_disk, backup_source, 1);
  }
cleanup:
  if (created_destination_parent[0] != '\0') {
    (void)vectis_webdav_remove_tree(created_destination_parent);
  }
  if (transaction[0] != '\0') {
    (void)vectis_webdav_remove_tree(transaction);
  }
  vectis_webdav_unlock(lock_fd);
  return status;
}

vectis_webdav_status vectis_webdav_copy(const vectis_webdav_config *config,
                                        const char *source,
                                        const char *destination,
                                        int overwrite) {
  return vectis_webdav_copy_or_move(config, source, destination, overwrite, 0,
                                    0, NULL, 0u, 0);
}

vectis_webdav_status vectis_webdav_move(const vectis_webdav_config *config,
                                        const char *source,
                                        const char *destination,
                                        int overwrite) {
  return vectis_webdav_copy_or_move(config, source, destination, overwrite, 1,
                                    0, NULL, 0u, 0);
}

static int vectis_webdav_list_path(const char *base_path, const char *name,
                                   char out[VECTIS_WEBDAV_PATH_MAX + 1u]) {
  int written;

  if (strcmp(base_path, "/") == 0) {
    written = snprintf(out, VECTIS_WEBDAV_PATH_MAX + 1u, "/%s", name);
  } else {
    written =
        snprintf(out, VECTIS_WEBDAV_PATH_MAX + 1u, "%s/%s", base_path, name);
  }
  return written >= 0 && (size_t)written < VECTIS_WEBDAV_PATH_MAX + 1u;
}

vectis_webdav_status vectis_webdav_list(const vectis_webdav_config *config,
                                        const char *path,
                                        vectis_webdav_list_callback callback,
                                        void *userdata) {
  char normalized[VECTIS_WEBDAV_PATH_MAX + 1u];
  char disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char child_path[VECTIS_WEBDAV_PATH_MAX + 1u];
  char child_disk[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  DIR *directory;
  struct dirent *item;
  struct stat st;
  int written;

  if (callback == NULL || !vectis_webdav_path_normalize(path, normalized) ||
      !vectis_webdav_disk_path(config, "content", normalized, disk)) {
    return VECTIS_WEBDAV_INVALID;
  }
  if (vectis_webdav_direct_root(config)) {
    int dir_fd;
    int scan_fd;

    dir_fd = vectis_webdav_direct_open_existing(config, normalized, 1, NULL);
    if (dir_fd < 0) {
      return errno == ENOENT ? VECTIS_WEBDAV_NOT_FOUND : VECTIS_WEBDAV_IO;
    }
    scan_fd = dup(dir_fd);
    if (scan_fd < 0) {
      vectis_webdav_fd_close(&dir_fd);
      return VECTIS_WEBDAV_IO;
    }
    directory = fdopendir(scan_fd);
    if (directory == NULL) {
      vectis_webdav_fd_close(&scan_fd);
      vectis_webdav_fd_close(&dir_fd);
      return VECTIS_WEBDAV_IO;
    }
    while ((item = readdir(directory)) != NULL) {
      if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0 ||
          item->d_name[0] == '.') {
        continue;
      }
      if (!vectis_webdav_list_path(normalized, item->d_name, child_path)) {
        (void)closedir(directory);
        vectis_webdav_fd_close(&dir_fd);
        return VECTIS_WEBDAV_IO;
      }
      if (fstatat(dir_fd, item->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        (void)closedir(directory);
        vectis_webdav_fd_close(&dir_fd);
        return VECTIS_WEBDAV_IO;
      }
      if (S_ISREG(st.st_mode)) {
        if (!callback(child_path, VECTIS_WEBDAV_ENTRY_FILE, (size_t)st.st_size,
                      userdata)) {
          break;
        }
      } else if (S_ISDIR(st.st_mode) &&
                 !callback(child_path, VECTIS_WEBDAV_ENTRY_COLLECTION, 0u,
                           userdata)) {
        break;
      }
    }
    (void)closedir(directory);
    vectis_webdav_fd_close(&dir_fd);
    return VECTIS_WEBDAV_OK;
  }
  directory = opendir(disk);
  if (directory == NULL) {
    return errno == ENOENT ? VECTIS_WEBDAV_NOT_FOUND : VECTIS_WEBDAV_IO;
  }
  while ((item = readdir(directory)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0 ||
        item->d_name[0] == '.') {
      continue;
    }
    if (!vectis_webdav_list_path(normalized, item->d_name, child_path) ||
        vectis_webdav_tombstone_exists(config, child_path)) {
      continue;
    }
    written =
        snprintf(child_disk, sizeof(child_disk), "%s/%s", disk, item->d_name);
    if (written < 0 || (size_t)written >= sizeof(child_disk) ||
        lstat(child_disk, &st) == -1) {
      (void)closedir(directory);
      return VECTIS_WEBDAV_IO;
    }
    if (S_ISREG(st.st_mode)) {
      if (!callback(child_path, VECTIS_WEBDAV_ENTRY_FILE, (size_t)st.st_size,
                    userdata)) {
        break;
      }
    } else if (S_ISDIR(st.st_mode) &&
               !callback(child_path, VECTIS_WEBDAV_ENTRY_COLLECTION, 0u,
                         userdata)) {
      break;
    }
  }
  (void)closedir(directory);
  return VECTIS_WEBDAV_OK;
}
