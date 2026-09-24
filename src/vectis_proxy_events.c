#include "vectis_proxy_events.h"

/* Kore's public header declares C99 flexible-array members. This adapter
 * only calls its event functions; keep this source under Vectis's C89 gate. */
void kore_platform_disable_read(int fd);
void kore_platform_disable_write(int fd);
void kore_platform_event_schedule(int fd, int type, int flags, void *event);

#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif

void vectis_proxy_event_update(int fd, struct kore_event *event, int previous,
                               int desired, int edge_triggered) {
  if (previous == desired) {
    return;
  }
#if defined(__linux__)
  if (desired == 0) {
    if (previous != 0) {
      kore_platform_disable_read(fd);
    }
    return;
  }
  kore_platform_event_schedule(
      fd,
      (desired & VECTIS_PROXY_EVENT_READ ? EPOLLIN : 0) |
          (desired & VECTIS_PROXY_EVENT_WRITE ? EPOLLOUT : 0) | EPOLLRDHUP |
          (edge_triggered ? EPOLLET : 0),
      0, event);
#else
  if ((previous & VECTIS_PROXY_EVENT_READ) != 0 &&
      (desired & VECTIS_PROXY_EVENT_READ) == 0) {
    kore_platform_disable_read(fd);
  }
  if ((previous & VECTIS_PROXY_EVENT_WRITE) != 0 &&
      (desired & VECTIS_PROXY_EVENT_WRITE) == 0) {
    kore_platform_disable_write(fd);
  }
  if ((previous & VECTIS_PROXY_EVENT_READ) == 0 &&
      (desired & VECTIS_PROXY_EVENT_READ) != 0) {
    kore_platform_event_schedule(
        fd, EVFILT_READ, EV_ADD | (edge_triggered ? EV_CLEAR : 0), event);
  }
  if ((previous & VECTIS_PROXY_EVENT_WRITE) == 0 &&
      (desired & VECTIS_PROXY_EVENT_WRITE) != 0) {
    kore_platform_event_schedule(
        fd, EVFILT_WRITE, EV_ADD | (edge_triggered ? EV_CLEAR : 0), event);
  }
#endif
}
