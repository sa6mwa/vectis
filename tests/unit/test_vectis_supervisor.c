#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vectis/supervisor.h>

static int capture_poll_timeout, captured_poll_timeout;
static int fixture_poll(struct pollfd *fds, nfds_t count, int timeout) {
  if (capture_poll_timeout) {
    captured_poll_timeout = timeout;
    return 0;
  }
  return poll(fds, count, timeout);
}
/* Instrument the actual wake syscall, without changing production code. */
static pthread_mutex_t wake_handshake = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake_changed = PTHREAD_COND_INITIALIZER;
static int pause_next_send, send_entered, release_send;
static ssize_t fixture_send(int fd, const void *buffer, size_t size,
                            int flags) {
  assert(pthread_mutex_lock(&wake_handshake) == 0);
  if (pause_next_send) {
    pause_next_send = 0;
    send_entered = 1;
    assert(pthread_cond_signal(&wake_changed) == 0);
    while (!release_send)
      assert(pthread_cond_wait(&wake_changed, &wake_handshake) == 0);
  }
  assert(pthread_mutex_unlock(&wake_handshake) == 0);
  return send(fd, buffer, size, flags);
}
#define send fixture_send
#define poll fixture_poll
#include "../../src/vectis_supervisor.c"
#undef poll
#undef send

typedef struct observed {
  int starts, stops, messages, signals, timers;
  int fail_start, fail_stop;
  vectis_supervisor *scope;
  pid_t owner;
} observed;
static vectis_status handler(void *context,
                             const vectis_supervisor_event *event,
                             vectis_error *error) {
  observed *seen = context;
  (void)error;
  assert(getpid() == seen->owner);
  if (event->type == VECTIS_SUPERVISOR_START) {
    ++seen->starts;
    if (seen->scope != NULL && seen->starts == 1)
      assert(seen->scope->start(seen->scope, NULL) == VECTIS_ERR_STATE);
    if (seen->fail_start)
      return VECTIS_ERR_INVALID;
  } else if (event->type == VECTIS_SUPERVISOR_STOP) {
    ++seen->stops;
    if (seen->fail_stop)
      return VECTIS_ERR_TIMEOUT;
  } else if (event->type == VECTIS_SUPERVISOR_MESSAGE)
    ++seen->messages;
  else if (event->type == VECTIS_SUPERVISOR_SIGNAL)
    ++seen->signals;
  else
    ++seen->timers;
  return VECTIS_OK;
}
static int fd_count(void) {
  DIR *dir = opendir("/proc/self/fd");
  struct dirent *entry;
  int count = 0;
  if (dir == NULL)
    dir = opendir("/dev/fd");
  assert(dir != NULL);
  while ((entry = readdir(dir)) != NULL)
    if (entry->d_name[0] != '.')
      ++count;
  (void)closedir(dir);
  return count;
}
typedef struct contention_fixture {
  supervisor_impl scope;
  vectis_supervisor_host host;
  pthread_mutex_t handshake;
  pthread_cond_t changed;
  int entered, leave;
} contention_fixture;
static void *hold_inbox(void *context) {
  contention_fixture *fixture = context;
  assert(pthread_mutex_lock(&fixture->scope.channel.shared->mutex) == 0);
  (void)pthread_mutex_lock(&fixture->handshake);
  fixture->entered = 1;
  (void)pthread_cond_signal(&fixture->changed);
  while (!fixture->leave)
    (void)pthread_cond_wait(&fixture->changed, &fixture->handshake);
  (void)pthread_mutex_unlock(&fixture->handshake);
  assert(pthread_mutex_unlock(&fixture->scope.channel.shared->mutex) == 0);
  /* Stop retains closed storage until this producer is quiesced. */
  assert(vectis_supervisor_channel_send(&fixture->scope.channel, 1u, "x", 1u,
                                        NULL) == VECTIS_ERR_STATE);
  return NULL;
}
static void *foreign_receive(void *context) {
  vectis_supervisor_channel *channel = context;
  vectis_supervisor_event event;
  char buffer[4];
  assert(vectis_supervisor_channel_receive(channel, &event, buffer,
                                           sizeof(buffer),
                                           NULL) == VECTIS_ERR_STATE);
  return NULL;
}
static void test_contended_inbox(void) {
  contention_fixture fixture;
  pthread_t producer;
  vectis_supervisor_event event;
  char buffer[4];
  pid_t child;
  int status;
  memset(&fixture, 0, sizeof(fixture));
  fixture.host.owner = getpid();
  fixture.host.thread = pthread_self();
  fixture.host.running = 1;
  fixture.scope.host = &fixture.host;
  fixture.scope.pub.impl = &fixture.scope;
  fixture.scope.state = VECTIS_SUPERVISOR_RUNNING;
  fixture.scope.channel.capacity = 1u;
  fixture.scope.channel.frame_size = 4u;
  fixture.scope.channel.stride = sizeof(inbox_frame) * 2u;
  fixture.scope.channel.read_fd = fixture.scope.channel.write_fd = -1;
  assert(channel_open(&fixture.scope.channel, NULL) == VECTIS_OK);
  assert(pthread_create(&producer, NULL, foreign_receive,
                        &fixture.scope.channel) == 0);
  assert(pthread_join(producer, NULL) == 0);
  assert(pthread_mutex_init(&fixture.handshake, NULL) == 0);
  assert(pthread_cond_init(&fixture.changed, NULL) == 0);
  assert(pthread_create(&producer, NULL, hold_inbox, &fixture) == 0);
  (void)pthread_mutex_lock(&fixture.handshake);
  while (!fixture.entered)
    (void)pthread_cond_wait(&fixture.changed, &fixture.handshake);
  (void)pthread_mutex_unlock(&fixture.handshake);
  assert(vectis_supervisor_channel_send(&fixture.scope.channel, 1u, "x", 1u,
                                        NULL) == VECTIS_ERR_CONFLICT);
  assert(vectis_supervisor_channel_receive(&fixture.scope.channel, &event,
                                           buffer, sizeof(buffer),
                                           NULL) == VECTIS_ERR_TIMEOUT);
  assert(vectis_supervisor_stop(&fixture.scope.pub, 0L, NULL) == VECTIS_OK);
  assert(fixture.scope.channel.shared != NULL);
  assert(vectis_supervisor_channel_send(&fixture.scope.channel, 1u, "x", 1u,
                                        NULL) == VECTIS_ERR_STATE);
  assert(vectis_supervisor_start(&fixture.scope.pub, NULL) ==
         VECTIS_ERR_CONFLICT);
  assert(fixture.scope.last_error.code == VECTIS_ERR_CONFLICT);
  (void)pthread_mutex_lock(&fixture.handshake);
  fixture.leave = 1;
  (void)pthread_cond_signal(&fixture.changed);
  (void)pthread_mutex_unlock(&fixture.handshake);
  assert(pthread_join(producer, NULL) == 0);
  (void)pthread_cond_destroy(&fixture.changed);
  (void)pthread_mutex_destroy(&fixture.handshake);
  __atomic_store_n(&fixture.scope.channel.shared->closed, 0, __ATOMIC_RELEASE);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    assert(pthread_mutex_lock(&fixture.scope.channel.shared->mutex) == 0);
    raise(SIGSTOP);
    (void)pthread_mutex_unlock(&fixture.scope.channel.shared->mutex);
    _exit(0);
  }
  assert(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));
  fixture.scope.state = VECTIS_SUPERVISOR_RUNNING;
  assert(vectis_supervisor_channel_receive(&fixture.scope.channel, &event,
                                           buffer, sizeof(buffer),
                                           NULL) == VECTIS_ERR_TIMEOUT);
  assert(vectis_supervisor_stop(&fixture.scope.pub, 0L, NULL) == VECTIS_OK);
  assert(kill(child, SIGCONT) == 0);
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
  channel_dispose(&fixture.scope.channel);
}
static void *paused_signal(void *context) {
  assert(vectis_supervisor_channel_signal(context, 11u, NULL) == VECTIS_OK);
  return NULL;
}
static void test_coalesced_wake(void) {
  vectis_supervisor_channel channel;
  vectis_supervisor_event event;
  pthread_t producer;
  struct pollfd fd;
  memset(&channel, 0, sizeof(channel));
  channel.capacity = 1u;
  channel.frame_size = 4u;
  channel.stride = sizeof(inbox_frame) * 2u;
  channel.read_fd = channel.write_fd = -1;
  channel.signals[0] = 11u;
  channel.signal_count = 1u;
  assert(channel_open(&channel, NULL) == VECTIS_OK);
  pause_next_send = 1;
  assert(pthread_create(&producer, NULL, paused_signal, &channel) == 0);
  assert(pthread_mutex_lock(&wake_handshake) == 0);
  while (!send_entered)
    assert(pthread_cond_wait(&wake_changed, &wake_handshake) == 0);
  assert(pthread_mutex_unlock(&wake_handshake) == 0);
  /* B must arm the owner FD while A remains paused before its syscall. */
  assert(vectis_supervisor_channel_signal(&channel, 11u, NULL) == VECTIS_OK);
  fd.fd = channel.read_fd;
  fd.events = POLLIN;
  fd.revents = 0;
  assert(poll(&fd, 1u, 0) == 1 && (fd.revents & POLLIN));
  assert(vectis_supervisor_channel_receive(&channel, &event, NULL, 0u, NULL) ==
         VECTIS_OK);
  assert(event.type == VECTIS_SUPERVISOR_SIGNAL && event.kind == 11u);
  assert(pthread_mutex_lock(&wake_handshake) == 0);
  release_send = 1;
  assert(pthread_cond_signal(&wake_changed) == 0);
  assert(pthread_mutex_unlock(&wake_handshake) == 0);
  assert(pthread_join(producer, NULL) == 0);
  /* A's late token is harmless: no duplicate retained signal. */
  assert(poll(&fd, 1u, 0) == 1);
  assert(vectis_supervisor_channel_receive(&channel, &event, NULL, 0u, NULL) ==
         VECTIS_ERR_TIMEOUT);
  assert(poll(&fd, 1u, 0) == 0);
  __atomic_store_n(&channel.shared->closed, 1, __ATOMIC_RELEASE);
  assert(vectis_supervisor_channel_signal(&channel, 11u, NULL) ==
         VECTIS_ERR_STATE);
  channel_dispose(&channel);
}
int main(int argc, char **argv) {
  vectis_app_config app_config;
  vectis_supervisor_config config;
  vectis_app *app;
  vectis_supervisor *scope, *second, *bad;
  vectis_supervisor_host *host;
  vectis_supervisor_channel *channel;
  vectis_supervisor_event event;
  vectis_error error;
  observed seen;
  struct pollfd pollfd;
  uint32_t signals[2];
  char buffer[8], original[4] = "abc";
  pid_t child;
  int status, before, after, i, fd;
  if (argc == 4 && strcmp(argv[1], "exec-fd") == 0) {
    assert(fcntl(atoi(argv[2]), F_GETFD) == -1);
    assert(fd_count() <= atoi(argv[3]));
    return 0;
  }
  test_coalesced_wake();
  test_contended_inbox();
  before = fd_count();
  memset(&seen, 0, sizeof(seen));
  seen.owner = getpid();
  vectis_app_config_init(&app_config);
  app_config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  assert(vectis_app_supervisor_host(app, &host, &error) == VECTIS_OK);
  vectis_supervisor_config_init(&config);
  config.name = "one";
  config.capacity = 2u;
  config.max_frame_size = 4u;
  signals[0] = 11u;
  signals[1] = 12u;
  config.signal_kinds = signals;
  config.signal_kind_count = 2u;
  config.handler = handler;
  config.context = &seen;
  assert(vectis_supervisor_new(app, &config, &scope, &error) == VECTIS_OK);
  seen.scope = scope;
  assert(vectis_supervisor_new(app, &config, &bad, &error) ==
         VECTIS_ERR_CONFLICT);
  config.name = "too-large";
  config.capacity = 4096u;
  config.max_frame_size = 1048576u;
  assert(vectis_supervisor_new(app, &config, &bad, &error) ==
         VECTIS_ERR_INVALID);
  config.capacity = 2u;
  config.max_frame_size = 4u;
  config.name = "two";
  signals[1] = 11u;
  assert(vectis_supervisor_new(app, &config, &bad, &error) ==
         VECTIS_ERR_INVALID);
  signals[1] = 12u;
  assert(vectis_supervisor_new(app, &config, &second, &error) == VECTIS_OK);
  assert(scope->channel(scope, &channel, &error) == VECTIS_OK);
  fd = vectis_supervisor_channel_fd(channel);
  assert(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
  assert(vectis_supervisor_channel_send_frame(channel, 1u, 33u, 7u, original,
                                              4u, &error) == VECTIS_OK);
  original[0] = 'X';
  assert(vectis_supervisor_channel_send(channel, 2u, "def", 3u, &error) ==
         VECTIS_OK);
  assert(vectis_supervisor_channel_send(channel, 3u, "ghi", 3u, &error) ==
         VECTIS_ERR_CONFLICT);
  assert(vectis_supervisor_channel_send(channel, 1u, "large", 5u, &error) ==
         VECTIS_ERR_INVALID);
  assert(vectis_supervisor_channel_signal(channel, 13u, &error) ==
         VECTIS_ERR_INVALID);
  for (i = 0; i < 10000; ++i)
    assert(vectis_supervisor_channel_signal(channel, 11u, &error) == VECTIS_OK);
  assert(vectis_supervisor_channel_signal(channel, 12u, &error) == VECTIS_OK);
  assert(vectis_supervisor_channel_receive(channel, &event, buffer, 2u,
                                           &error) == VECTIS_ERR_INVALID);
  assert(vectis_supervisor_channel_receive(
             channel, &event, buffer, sizeof(buffer), &error) == VECTIS_OK);
  assert(event.correlation_id == 33u && event.flags == 7u && event.kind == 1u &&
         memcmp(buffer, "abc", 4u) == 0);
  assert(vectis_supervisor_channel_receive(
             channel, &event, buffer, sizeof(buffer), &error) == VECTIS_OK &&
         event.kind == 11u);
  assert(vectis_supervisor_channel_receive(
             channel, &event, buffer, sizeof(buffer), &error) == VECTIS_OK &&
         event.kind == 2u);
  assert(vectis_supervisor_channel_receive(
             channel, &event, buffer, sizeof(buffer), &error) == VECTIS_OK &&
         event.kind == 12u);
  assert(vectis_supervisor_channel_receive(channel, &event, buffer,
                                           sizeof(buffer),
                                           &error) == VECTIS_ERR_TIMEOUT);
  pollfd.fd = fd;
  pollfd.events = POLLIN;
  pollfd.revents = 0;
  /* A signal storm may leave redundant datagrams, but never retained events.
   * Receive remains bounded and drains those harmless tokens incrementally. */
  for (i = 0; i < 10000 && poll(&pollfd, 1u, 0) == 1; ++i)
    assert(vectis_supervisor_channel_receive(channel, &event, buffer,
                                             sizeof(buffer),
                                             &error) == VECTIS_ERR_TIMEOUT);
  assert(i < 10000 && poll(&pollfd, 1u, 0) == 0);
  /* A constantly replenished ring cannot starve a coalesced repair signal. */
  for (i = 0; i < 100; ++i) {
    assert(vectis_supervisor_channel_send(channel, 1u, "a", 1u, &error) ==
           VECTIS_OK);
    assert(vectis_supervisor_channel_send(channel, 2u, "b", 1u, &error) ==
           VECTIS_OK);
    assert(vectis_supervisor_channel_signal(channel, 11u, &error) == VECTIS_OK);
    assert(vectis_supervisor_channel_receive(
               channel, &event, buffer, sizeof(buffer), &error) == VECTIS_OK);
    status = (int)event.kind;
    assert(status == 1 || status == 11);
    assert(vectis_supervisor_channel_receive(
               channel, &event, buffer, sizeof(buffer), &error) == VECTIS_OK &&
           event.kind == (status == 1 ? 11u : 1u));
    assert(vectis_supervisor_channel_receive(
               channel, &event, buffer, sizeof(buffer), &error) == VECTIS_OK &&
           event.kind == 2u);
  }
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    vectis_supervisor_host_after_fork_producer(host);
    assert(vectis_supervisor_channel_fd(channel) == -1);
    assert(vectis_supervisor_channel_receive(channel, &event, buffer,
                                             sizeof(buffer),
                                             &error) == VECTIS_ERR_STATE);
    assert(vectis_supervisor_channel_send(channel, 99u, "fork", 4u, &error) ==
           VECTIS_OK);
    assert(vectis_supervisor_channel_signal(channel, 12u, &error) == VECTIS_OK);
    _exit(0);
  }
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0);
  assert(poll(&pollfd, 1u, 0) == 1);
  assert(vectis_supervisor_channel_receive(
             channel, &event, buffer, sizeof(buffer), &error) == VECTIS_OK);
  status = (int)event.kind;
  assert(status == 99 || status == 12);
  if (status == 99)
    assert(memcmp(buffer, "fork", 4u) == 0);
  assert(vectis_supervisor_channel_receive(
             channel, &event, buffer, sizeof(buffer), &error) == VECTIS_OK &&
         event.kind == (status == 99 ? 12u : 99u));
  if (status == 12)
    assert(memcmp(buffer, "fork", 4u) == 0);
  child = fork();
  assert(child >= 0);
  if (child == 0) {
    char number[32], baseline[32];
    (void)snprintf(number, sizeof(number), "%d", fd);
    (void)snprintf(baseline, sizeof(baseline), "%d", before);
    execl(argv[0], argv[0], "exec-fd", number, baseline, (char *)NULL);
    _exit(1);
  }
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0);
  assert(scope->start(scope, &error) == VECTIS_OK && seen.starts == 0);
  assert(scope->wait(scope, 0L, &error) == VECTIS_ERR_STATE);
  assert(vectis_supervisor_timer(scope, 55u, 1000000L, &error) == VECTIS_OK);
  assert(vectis_supervisor_timer_enable(scope, 99u, 0, &error) ==
         VECTIS_ERR_INVALID);
  assert(vectis_supervisor_timer_enable(scope, 55u, 0, &error) == VECTIS_OK);
  assert(app->start(app, &error) == VECTIS_OK && seen.starts == 2);
  assert(vectis_supervisor_host_pump(host, -2L, &error) == VECTIS_ERR_INVALID);
  config.name = "late";
  assert(vectis_supervisor_new(app, &config, &bad, &error) == VECTIS_ERR_STATE);
  assert(vectis_supervisor_channel_send(channel, 1u, "ok", 2u, &error) ==
         VECTIS_OK);
  assert(vectis_supervisor_host_pump(host, 0L, &error) == VECTIS_OK &&
         seen.messages == 1);
  assert(second->stop(second, 0L, &error) == VECTIS_OK && seen.stops == 1);
  assert(second->wait(second, 0L, &error) == VECTIS_OK);
  assert(scope->state(scope) == VECTIS_SUPERVISOR_RUNNING);
  {
    supervisor_impl *internal = scope->impl;
    int timers_before = seen.timers;
    internal->timers->due.tv_sec = 0;
    assert(vectis_supervisor_host_pump(host, 0L, &error) == VECTIS_OK);
    /* PROBE is counted by the fixture, disabled TIMER must not be delivered. */
    assert(seen.timers == timers_before + 1);
    capture_poll_timeout = 1;
    assert(vectis_supervisor_host_poll(host, -1, 500L, NULL, &error) ==
           VECTIS_OK);
    assert(captured_poll_timeout ==
           500); /* disabled due timer adds no deadline */
    capture_poll_timeout = 0;
    assert(vectis_supervisor_timer_enable(scope, 55u, 1, &error) == VECTIS_OK);
    assert(remaining(&internal->timers->due) > 0L);
  }
  assert(app->stop(app, &error) == VECTIS_OK && seen.stops == 2);
  assert(vectis_supervisor_channel_send(channel, 1u, "ok", 2u, &error) ==
         VECTIS_ERR_STATE);
  assert(app->start(app, &error) == VECTIS_OK && seen.starts == 3);
  assert(vectis_supervisor_channel_send(channel, 1u, "ok", 2u, &error) ==
         VECTIS_OK);
  assert(vectis_supervisor_host_pump(host, 0L, &error) == VECTIS_OK &&
         seen.messages == 2);
  assert(app->stop(app, &error) == VECTIS_OK);
  app->close(app);
  memset(&seen, 0, sizeof(seen));
  seen.owner = getpid();
  seen.fail_start = 1;
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  config.name = "startup-fault";
  assert(vectis_supervisor_new(app, &config, &scope, &error) == VECTIS_OK);
  seen.scope = scope;
  assert(app->start(app, &error) == VECTIS_ERR_INVALID);
  assert(seen.starts == 1 && seen.stops == 1);
  assert(vectis_supervisor_last_error(scope, &error) == VECTIS_OK &&
         error.code != VECTIS_OK);
  app->close(app);
  assert(seen.stops == 1);
  memset(&seen, 0, sizeof(seen));
  seen.owner = getpid();
  seen.fail_stop = 1;
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  config.name = "shutdown-fault";
  config.stop_on_failure = 1;
  assert(vectis_supervisor_new(app, &config, &scope, &error) == VECTIS_OK);
  seen.scope = scope;
  assert(app->start(app, &error) == VECTIS_OK);
  assert(app->stop(app, &error) == VECTIS_ERR_TIMEOUT);
  assert(error.code == VECTIS_ERR_TIMEOUT && error.message[0] != '\0');
  assert(scope->state(scope) == VECTIS_SUPERVISOR_FAILED);
  assert(seen.starts == 1 && seen.stops == 1);
  assert(vectis_supervisor_last_error(scope, &error) == VECTIS_OK &&
         error.code != VECTIS_OK);
  app->close(app);
  assert(seen.stops == 1);
  /* A direct lifecycle caller may reuse an error from an earlier operation.
   * A handler returning failure without setting it must not inherit that data.
   */
  memset(&seen, 0, sizeof(seen));
  seen.owner = getpid();
  seen.fail_stop = 1;
  app = vectis_app_new(&app_config, &error);
  assert(app != NULL);
  config.name = "stale-diagnostic";
  assert(vectis_supervisor_new(app, &config, &scope, &error) == VECTIS_OK);
  seen.scope = scope;
  assert(app->start(app, &error) == VECTIS_OK);
  error.code = VECTIS_ERR_INVALID;
  strcpy(error.message, "unrelated earlier diagnostic");
  assert(scope->stop(scope, 0L, &error) == VECTIS_ERR_TIMEOUT);
  assert(error.code == VECTIS_ERR_TIMEOUT);
  assert(strcmp(error.message, "unrelated earlier diagnostic") != 0);
  assert(vectis_supervisor_last_error(scope, &error) == VECTIS_OK &&
         error.code == VECTIS_ERR_TIMEOUT);
  app->close(app);
  assert(seen.stops == 1);
  after = fd_count();
  assert(before < 0 || before == after);
  puts("supervisor IPC bounds/fork/lifecycle ok");
  return 0;
}
