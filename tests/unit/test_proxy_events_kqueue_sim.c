#include "vectis_proxy_events.h"

#include <assert.h>
#include <stddef.h>
#include <sys/event.h>

struct kore_event {
  int marker;
};

typedef struct test_event_call {
  int fd;
  int type;
  int flags;
  void *event;
} test_event_call;

static test_event_call calls[12];
static unsigned call_count;

void kore_platform_event_schedule(int fd, int type, int flags, void *event);
void kore_platform_disable_read(int fd);
void kore_platform_disable_write(int fd);

void kore_platform_event_schedule(int fd, int type, int flags, void *event) {
  test_event_call *call;

  assert(call_count < sizeof(calls) / sizeof(calls[0]));
  call = &calls[call_count++];
  call->fd = fd;
  call->type = type;
  call->flags = flags;
  call->event = event;
}

void kore_platform_disable_read(int fd) {
  kore_platform_event_schedule(fd, EVFILT_READ, EV_DELETE, NULL);
}

void kore_platform_disable_write(int fd) {
  kore_platform_event_schedule(fd, EVFILT_WRITE, EV_DELETE, NULL);
}

static void expect_call(unsigned index, int type, int flags, void *event) {
  assert(index < call_count);
  assert(calls[index].fd == 42);
  assert(calls[index].type == type);
  assert(calls[index].flags == flags);
  assert(calls[index].event == event);
}

int main(void) {
  struct kore_event event;

  event.marker = 1;
  vectis_proxy_event_takeover(42);
  assert(call_count == 2u);
  expect_call(0u, EVFILT_READ, EV_DELETE, NULL);
  expect_call(1u, EVFILT_WRITE, EV_DELETE, NULL);

  vectis_proxy_event_update(42, &event, 0, VECTIS_PROXY_EVENT_READ, 1);
  assert(call_count == 3u);
  expect_call(2u, EVFILT_READ, EV_ADD | EV_CLEAR, &event);

  vectis_proxy_event_update(42, &event, VECTIS_PROXY_EVENT_READ,
                            VECTIS_PROXY_EVENT_READ | VECTIS_PROXY_EVENT_WRITE,
                            1);
  assert(call_count == 4u);
  expect_call(3u, EVFILT_WRITE, EV_ADD | EV_CLEAR, &event);

  vectis_proxy_event_update(42, &event,
                            VECTIS_PROXY_EVENT_READ | VECTIS_PROXY_EVENT_WRITE,
                            VECTIS_PROXY_EVENT_WRITE, 1);
  assert(call_count == 5u);
  expect_call(4u, EVFILT_READ, EV_DELETE, NULL);

  vectis_proxy_event_update(42, &event, VECTIS_PROXY_EVENT_WRITE, 0, 1);
  assert(call_count == 6u);
  expect_call(5u, EVFILT_WRITE, EV_DELETE, NULL);

  vectis_proxy_event_update(
      42, &event, 0, VECTIS_PROXY_EVENT_READ | VECTIS_PROXY_EVENT_WRITE, 0);
  assert(call_count == 8u);
  expect_call(6u, EVFILT_READ, EV_ADD, &event);
  expect_call(7u, EVFILT_WRITE, EV_ADD, &event);

  vectis_proxy_event_update(
      42, &event, VECTIS_PROXY_EVENT_READ | VECTIS_PROXY_EVENT_WRITE, 0, 0);
  assert(call_count == 10u);
  expect_call(8u, EVFILT_READ, EV_DELETE, NULL);
  expect_call(9u, EVFILT_WRITE, EV_DELETE, NULL);

  vectis_proxy_event_update(42, &event, 0, 0, 0);
  assert(call_count == 10u);
  return 0;
}
