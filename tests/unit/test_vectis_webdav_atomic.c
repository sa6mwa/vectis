#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int armed;
static int close_calls;
static int replacement_fd;
static int fail_close;
static int fail_sync;

static int test_close(int fd) {
  int result;
  if (!armed)
    return close(fd);
  ++close_calls;
  result = close(fd);
  if (close_calls == 1) {
    replacement_fd = open("/dev/null", O_RDONLY);
    assert(replacement_fd >= 0);
    if (replacement_fd != fd) {
      assert(dup2(replacement_fd, fd) == fd);
      assert(close(replacement_fd) == 0);
      replacement_fd = fd;
    }
  }
  if (fail_close) {
    errno = EIO;
    return -1;
  }
  return result;
}

static int test_fsync(int fd) {
  if (armed && fail_sync) {
    errno = EIO;
    return -1;
  }
  return fsync(fd);
}

/* Compile the storage implementation with syscall substitution confined to
 * this test. Descriptor reuse is deterministic, without thread scheduling. */
#define close test_close
#define fsync test_fsync
#include "../../src/vectis_webdav.c"
#undef fsync
#undef close

typedef struct test_preflight {
  const vectis_webdav_config *config;
  int allow;
  int calls;
} test_preflight;

static int check_mutation_lock(void *context) {
  test_preflight *check = context;
  char base[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  char path[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  int fd;
  assert(vectis_webdav_base(check->config, base, sizeof(base)));
  assert(vectis_webdav_path_append(path, sizeof(path), base, ".lock"));
  fd = open(path, O_RDWR);
  assert(fd >= 0);
  assert(flock(fd, LOCK_EX | LOCK_NB) == -1);
  assert(errno == EWOULDBLOCK || errno == EAGAIN);
  assert(close(fd) == 0);
  ++check->calls;
  return check->allow;
}

static int collect_affected(const char *path, vectis_webdav_entry_kind kind,
                            size_t size, void *context) {
  unsigned int *mask = context;
  const char *name = strrchr(path, '/') + 1;
  (void)size;
  if (strcmp(name, "visible") == 0)
    *mask |= 1u;
  else if (strcmp(name, ".vectis-tmp-file") == 0)
    *mask |= 2u;
  else if (strcmp(name, ".vectis-txn-dir") == 0) {
    assert(kind == VECTIS_WEBDAV_ENTRY_COLLECTION);
    *mask |= 4u;
  } else if (strcmp(name, "masked") == 0) {
    assert(kind == VECTIS_WEBDAV_ENTRY_COLLECTION);
    *mask |= 8u;
  } else if (strcmp(name, "link") == 0) {
    assert(kind == VECTIS_WEBDAV_ENTRY_FILE);
    *mask |= 16u;
  } else if (strcmp(name, "fifo") == 0) {
    assert(kind == VECTIS_WEBDAV_ENTRY_FILE);
    *mask |= 32u;
  } else
    assert(0);
  return 1;
}

static void test_affected_enumeration(const vectis_webdav_config *config,
                                      int direct) {
  char path[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  unsigned int mask;
  int lock_fd;
  assert(vectis_webdav_mkcol(config, "/enumerate") == VECTIS_WEBDAV_OK);
  assert(vectis_webdav_mkcol(config, "/enumerate/.vectis-txn-dir") ==
         VECTIS_WEBDAV_OK);
  assert(vectis_webdav_mkcol(config, "/enumerate/masked") == VECTIS_WEBDAV_OK);
  assert(vectis_webdav_put(config, "/enumerate/visible",
                           (const unsigned char *)"x", 1u) == VECTIS_WEBDAV_OK);
  assert(vectis_webdav_put(config, "/enumerate/.vectis-tmp-file",
                           (const unsigned char *)"x", 1u) == VECTIS_WEBDAV_OK);
  assert(vectis_webdav_disk_path(config, "content", "/enumerate/link", path));
  assert(symlink("masked", path) == 0);
  assert(vectis_webdav_disk_path(config, "content", "/enumerate/fifo", path));
  assert(mkfifo(path, 0600) == 0);
  if (!direct) {
    assert(vectis_webdav_disk_path(config, "tombstones", "/enumerate/masked",
                                   path));
    assert(vectis_webdav_touch_atomic(path));
  }
  mask = 0u;
  assert(vectis_webdav_list(config, "/enumerate", collect_affected, &mask) ==
         VECTIS_WEBDAV_OK);
  assert(mask == (direct ? 9u : 1u));
  lock_fd = vectis_webdav_lock(config);
  assert(lock_fd >= 0);
  mask = 0u;
  assert(vectis_internal_webdav_list_affected(config, "/enumerate",
                                              collect_affected,
                                              &mask) == VECTIS_WEBDAV_OK);
  assert(mask == 63u);
  vectis_webdav_unlock(lock_fd);
  assert(vectis_webdav_disk_path(config, "content", "/enumerate/link", path));
  assert(unlink(path) == 0);
  assert(vectis_webdav_disk_path(config, "content", "/enumerate/fifo", path));
  assert(unlink(path) == 0);
}

static void test_authorization_preflight(const char *root) {
  vectis_webdav_config config;
  vectis_webdav_entry entry;
  test_preflight check;
  char direct[VECTIS_WEBDAV_STORAGE_PATH_MAX];
  int mode;
  int operation;
  int created;
  vectis_webdav_status status;
  assert(snprintf(direct, sizeof(direct), "%s/direct", root) > 0);
  assert(mkdir(direct, 0700) == 0);
  for (mode = 0; mode < 2; ++mode) {
    vectis_webdav_config_init(&config);
    config.cache_dir = root;
    config.site_id = mode == 0 ? "managed" : "direct";
    config.root_dir = mode == 0 ? NULL : direct;
    check.config = &config;
    check.allow = 0;
    check.calls = 0;
    assert(vectis_webdav_put(&config, "/source",
                             (const unsigned char *)"source",
                             6u) == VECTIS_WEBDAV_OK);
    assert(vectis_webdav_put(&config, "/target", (const unsigned char *)"keep",
                             4u) == VECTIS_WEBDAV_OK);
    for (operation = 0; operation < 3; ++operation) {
      if (operation == 0) {
        status = vectis_internal_webdav_delete_authorized(
            &config, "/source", NULL, NULL, check_mutation_lock, &check);
      } else {
        created = 99;
        status = vectis_internal_webdav_transfer_authorized(
            &config, "/source", "/target", 1, operation == 2, 0, NULL, NULL,
            check_mutation_lock, &check, &created);
        assert(created == 0);
      }
      assert(status == VECTIS_WEBDAV_INVALID);
      assert(check.calls == operation + 1);
      assert(vectis_webdav_lookup(&config, "/source", &entry) ==
                 VECTIS_WEBDAV_OK &&
             entry.size == 6u);
      assert(vectis_webdav_lookup(&config, "/target", &entry) ==
                 VECTIS_WEBDAV_OK &&
             entry.size == 4u);
    }
    check.allow = 1;
    assert(vectis_internal_webdav_transfer_authorized(
               &config, "/source", "/target", 1, 1, 0, NULL, NULL,
               check_mutation_lock, &check, &created) == VECTIS_WEBDAV_OK);
    assert(created == 0);
    assert(vectis_internal_webdav_delete_authorized(
               &config, "/target", NULL, NULL, check_mutation_lock, &check) ==
           VECTIS_WEBDAV_OK);
    test_affected_enumeration(&config, mode);
  }
}

int main(void) {
  char root[4096];
  char cwd[2048];
  char target[8192];
  char file[8192];
  const unsigned char payload[] = "preserved";
  DIR *directory;
  struct dirent *entry;
  int parent_fd;
  int operation;
  int failure;
  int result;
  int count;

  assert(getcwd(cwd, sizeof(cwd)) != NULL);
  assert(snprintf(root, sizeof(root), "%s/webdav-atomic.XXXXXX", cwd) > 0);
  assert(mkdtemp(root) != NULL);
  assert(snprintf(target, sizeof(target), "%s/target", root) > 0);
  parent_fd = open(root, O_RDONLY | O_DIRECTORY);
  assert(parent_fd >= 0);
  for (operation = 0; operation < 3; ++operation) {
    /* Success, rename failure, close failure, and pre-close sync failure. */
    for (failure = 0; failure < 4; ++failure) {
      if (failure == 1)
        assert(mkdir(target, 0700) == 0);
      fail_close = failure == 2;
      fail_sync = failure == 3;
      close_calls = 0;
      replacement_fd = -1;
      armed = 1;
      if (operation == 0) {
        result = vectis_webdav_touch_atomic(target);
      } else if (operation == 1) {
        result = vectis_webdav_write_atomic(target, payload, sizeof(payload));
      } else {
        result = vectis_webdav_write_atomic_at(parent_fd, "target", payload,
                                               sizeof(payload));
      }
      armed = 0;
      assert(result == (failure == 0));
      assert(close_calls == 1);
      assert(replacement_fd >= 0);
      assert(fcntl(replacement_fd, F_GETFD) != -1);
      assert(close(replacement_fd) == 0);
      if (failure == 0)
        assert(unlink(target) == 0);
      if (failure == 1)
        assert(rmdir(target) == 0);
      directory = opendir(root);
      assert(directory != NULL);
      count = 0;
      while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
          assert(snprintf(file, sizeof(file), "%s/%s", root, entry->d_name) >
                 0);
          fprintf(stderr, "unexpected temporary file: %s\n", file);
          ++count;
        }
      }
      assert(closedir(directory) == 0);
      assert(count == 0);
    }
  }
  assert(close(parent_fd) == 0);
  test_authorization_preflight(root);
  assert(vectis_webdav_remove_tree(root));
  return 0;
}
