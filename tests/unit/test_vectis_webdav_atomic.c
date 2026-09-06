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
  assert(rmdir(root) == 0);
  return 0;
}
