#include <assert.h>
#include <pthread.h>
#include <string.h>

#include <vectis/vectis.h>

typedef struct thread_gate {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int ready;
  int done;
} thread_gate;

static void *hold_thread(void *userdata) {
  thread_gate *gate;

  gate = (thread_gate *)userdata;
  assert(pthread_mutex_lock(&gate->mutex) == 0);
  gate->ready = 1;
  assert(pthread_cond_signal(&gate->cond) == 0);
  while (!gate->done) {
    assert(pthread_cond_wait(&gate->cond, &gate->mutex) == 0);
  }
  assert(pthread_mutex_unlock(&gate->mutex) == 0);
  return NULL;
}

static vectis_status reply(vectis_app *app, vectis_request *request,
                           vectis_response *response, void *userdata,
                           vectis_error *error) {
  (void)app;
  (void)request;
  (void)userdata;
  return vectis_response_text(response, 200, "text/plain", "ok", error);
}

int main(void) {
  vectis_app_config config;
  vectis_app *app;
  vectis_route_config route;
  vectis_error error;
  vectis_status status;
  thread_gate gate;
  pthread_t thread;

  vectis_app_config_init(&config);
  config.tls.mode = VECTIS_TLS_MODE_DISABLED;
  config.tls.bind = "127.0.0.1";
  config.tls.port = 0u;
  app = vectis_app_new(&config, &error);
  assert(app != NULL);
  route = vectis_route(VECTIS_HTTP_GET, "/thread-guard", reply, NULL);
  assert(app->route(app, &route, &error) == VECTIS_OK);

  memset(&gate, 0, sizeof(gate));
  assert(pthread_mutex_init(&gate.mutex, NULL) == 0);
  assert(pthread_cond_init(&gate.cond, NULL) == 0);
  assert(pthread_create(&thread, NULL, hold_thread, &gate) == 0);
  assert(pthread_mutex_lock(&gate.mutex) == 0);
  while (!gate.ready) {
    assert(pthread_cond_wait(&gate.cond, &gate.mutex) == 0);
  }
  assert(pthread_mutex_unlock(&gate.mutex) == 0);

  status = app->start(app, &error);
  assert(status == VECTIS_ERR_STATE);
  assert(strstr(error.message, "single-threaded") != NULL);

  assert(pthread_mutex_lock(&gate.mutex) == 0);
  gate.done = 1;
  assert(pthread_cond_signal(&gate.cond) == 0);
  assert(pthread_mutex_unlock(&gate.mutex) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(pthread_cond_destroy(&gate.cond) == 0);
  assert(pthread_mutex_destroy(&gate.mutex) == 0);
  app->close(app);
  return 0;
}
