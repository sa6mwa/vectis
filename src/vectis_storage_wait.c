#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <vectis/storage.h>
static vectis_status failed(vectis_error *error, const char *message) {
  if (error) {
    memset(error, 0, sizeof(*error));
    error->code = VECTIS_ERR_STATE;
    error->source = VECTIS_ERROR_SOURCE_VECTIS;
    strncpy(error->message, message, sizeof(error->message) - 1u);
  }
  return VECTIS_ERR_STATE;
}
vectis_status vectis_storage_wait_blocking(const lc_xa_wait *wait,
                                           vectis_error *error) {
  struct pollfd descriptor;
  struct timespec now;
  int result, timeout;
  int64_t current, remaining;
  uint64_t milliseconds;
  if (!wait || wait->deadline_ms < -1 || (wait->events & ~3u) ||
      (wait->fd >= 0 && !wait->events) ||
      (wait->fd < 0 && wait->deadline_ms < 0))
    return failed(error, "invalid XA wait descriptor");
  descriptor.fd = wait->fd;
  descriptor.events = (short)(((wait->events & 1u) ? POLLIN : 0) |
                              ((wait->events & 2u) ? POLLOUT : 0));
  descriptor.revents = 0;
  for (;;) {
    timeout = -1;
    if (wait->deadline_ms >= 0) {
      if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return failed(error, "cannot read monotonic clock");
      if (now.tv_sec < 0 || (uint64_t)now.tv_sec > (uint64_t)INT64_MAX / 1000u)
        return failed(error, "monotonic clock outside int64 milliseconds");
      milliseconds =
          (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
      if (milliseconds > (uint64_t)INT64_MAX)
        return failed(error, "monotonic clock outside int64 milliseconds");
      current = (int64_t)milliseconds;
      remaining = wait->deadline_ms - current;
      if (remaining <= 0)
        return VECTIS_OK;
      timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
    }
    result = poll(&descriptor, 1, timeout);
    if (result < 0)
      return failed(error, errno == EINTR ? "XA wait interrupted"
                                          : "XA readiness wait failed");
    if (result > 0) {
      if (descriptor.revents & POLLNVAL)
        return failed(error, "XA wait descriptor is closed");
      return VECTIS_OK;
    }
  }
}
