#include "vectis_internal.h"
#include <assert.h>
#include <cai/cai.h>
#include <dirent.h>
#include <lc/lc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vectis/vectis.h>

#ifdef VECTIS_TEST_SMITH_FAULTS
static int fail_scope_acquire;
static size_t fail_malloc_size;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size);
int __real_lc_acquire(lc_client *client, const lc_acquire_req *req,
                      lc_lease **out, lc_error *error);
int __wrap_lc_acquire(lc_client *client, const lc_acquire_req *req,
                      lc_lease **out, lc_error *error);

void *__wrap_malloc(size_t size) {
  if (fail_malloc_size != 0u && size == fail_malloc_size) {
    fail_malloc_size = 0u;
    return NULL;
  }
  return __real_malloc(size);
}

int __wrap_lc_acquire(lc_client *client, const lc_acquire_req *req,
                      lc_lease **out, lc_error *error) {
  if (fail_scope_acquire && strstr(req->key, "scope/") != NULL) {
    fail_scope_acquire = 0;
    *out = NULL;
    error->code = LC_ERR_INVALID;
    return LC_ERR_INVALID;
  }
  return __real_lc_acquire(client, req, out, error);
}
#endif

typedef struct smith_replay {
  unsigned long long sequences[4];
  char types[4][32];
  size_t count;
} smith_replay;

static void remove_tree(const char *path) {
  DIR *directory;
  struct dirent *entry;
  char child[1024];

  directory = opendir(path);
  if (directory == NULL) {
    return;
  }
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    assert(snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) > 0);
    if (entry->d_type == DT_DIR) {
      remove_tree(child);
    } else {
      assert(unlink(child) == 0);
    }
  }
  assert(closedir(directory) == 0);
  assert(rmdir(path) == 0);
}

static int replay_event(void *context, const cai_agent_session_event *event,
                        cai_error *error) {
  smith_replay *replay;

  (void)error;
  replay = (smith_replay *)context;
  assert(replay->count <
         sizeof(replay->sequences) / sizeof(replay->sequences[0]));
  replay->sequences[replay->count] = event->sequence;
  assert(snprintf(replay->types[replay->count],
                  sizeof(replay->types[replay->count]), "%s", event->type) > 0);
  ++replay->count;
  return CAI_OK;
}

static size_t failing_checkpoint_read(void *context, void *buffer,
                                      size_t capacity, cai_error *error) {
  int *partial;

  partial = (int *)context;
  if (*partial && capacity != 0u) {
    *partial = 0;
    ((char *)buffer)[0] = '{';
    return 1u;
  }
  error->code = CAI_ERR_TRANSPORT;
  return 0u;
}

static void settle_offline_turn(vectis_smith *smith) {
  vectis_error error;
  cai_agent_run_state state;
  int i;
  for (i = 0; i < 200; ++i) {
    (void)vectis_smith_pump(smith, 10L, &error);
    assert(vectis_smith_state(smith, &state, &error) == VECTIS_OK);
    if (state == CAI_AGENT_FAILED || state == CAI_AGENT_COMPLETED)
      return;
  }
  assert(0 && "offline turn did not settle");
}

static void test_lockdc_store_checkpoint_and_events(void) {
  char directory[1024];
  char endpoint[sizeof(directory) + 32u];
  const char *endpoints[1];
  const char checkpoint[] = "{\"version\":1}";
  cai_agent_session_event first;
  cai_agent_session_event second;
  cai_agent_session_store const *callbacks;
  vectis_smith_store_config store_config;
  vectis_smith_store *store;
  vectis_error vectis_error;
  cai_error caierr;
  lc_client_config client_config;
  lc_error lcerr;
  lc_client *client;
  cai_source *state;
  cai_source *loaded;
  vectis_source vectis_source;
  char session_id[CAI_AGENT_SESSION_ID_MAX];
  unsigned long long watermark;
  char contents[64];
  size_t nread;
  smith_replay replay;

  assert(getcwd(directory, sizeof(directory) - 40u) != NULL);
  strcat(directory, "/vectis-smith-store.XXXXXX");
  assert(mkdtemp(directory) != NULL);
  assert(snprintf(endpoint, sizeof(endpoint), "pouch://%s?single_writer=false",
                  directory) > 0);
  endpoints[0] = endpoint;
  lc_client_config_init(&client_config);
  client_config.endpoints = endpoints;
  client_config.endpoint_count = 1u;
  client_config.default_namespace = "vectis.smith.test";
  client = NULL;
  lc_error_init(&lcerr);
  assert(lc_client_open(&client_config, &client, &lcerr) == LC_OK);
  lc_error_cleanup(&lcerr);

  vectis_smith_store_config_init(&store_config);
  store_config.client = client;
  store_config.owner = "vectis-smith-store-test";
  store = NULL;
  vectis_error_clear(&vectis_error);
  assert(vectis_smith_store_new(&store_config, &store, &vectis_error) ==
         VECTIS_OK);
  callbacks = vectis_smith_store_session_store(store);
  assert(callbacks != NULL);
  assert(callbacks->load_latest(callbacks->context, "workspace", session_id,
                                sizeof(session_id), &loaded, &watermark,
                                &caierr) == CAI_OK);
  assert(loaded == NULL);

  cai_error_init(&caierr);
  vectis_source =
      vectis_source_from_memory(checkpoint, sizeof(checkpoint) - 1u);
  assert(vectis_cai_source_from_source(&vectis_source, &state, &vectis_error) ==
         VECTIS_OK);
  assert(callbacks->checkpoint(callbacks->context, "workspace", "session-1",
                               state, 4u, &caierr) == CAI_OK);
  cai_source_close(state);
  assert(callbacks->load_latest(callbacks->context, "workspace", session_id,
                                sizeof(session_id), &loaded, &watermark,
                                &caierr) == CAI_OK);
  assert(strcmp(session_id, "session-1") == 0);
  assert(watermark == 4u);
  nread = cai_source_read(loaded, contents, sizeof(contents), &caierr);
  assert(nread == sizeof(checkpoint) - 1u);
  assert(memcmp(contents, checkpoint, nread) == 0);
  cai_source_close(loaded);

  memset(&first, 0, sizeof(first));
  first.sequence = 5u;
  first.type = "steering_queued";
  first.data = "first";
  memset(&second, 0, sizeof(second));
  second.sequence = 6u;
  second.type = "turn_queued";
  second.data = "second";
  assert(callbacks->append_event(callbacks->context, "workspace", "session-1",
                                 &first, &caierr) == CAI_OK);
  assert(callbacks->append_event(callbacks->context, "workspace", "session-1",
                                 &second, &caierr) == CAI_OK);
  memset(&replay, 0, sizeof(replay));
  assert(callbacks->load_events_after(callbacks->context, "workspace",
                                      "session-1", 4u, replay_event, &replay,
                                      &caierr) == CAI_OK);
  assert(replay.count == 2u);
  assert(replay.sequences[0] == 5u);
  assert(replay.sequences[1] == 6u);
  assert(strcmp(replay.types[0], "steering_queued") == 0);
  assert(strcmp(replay.types[1], "turn_queued") == 0);
  {
    cai_source_callbacks source_callbacks;
    int attempt;
    int partial;

    for (attempt = 0; attempt < 2; ++attempt) {
      partial = attempt;
      memset(&source_callbacks, 0, sizeof(source_callbacks));
      source_callbacks.context = &partial;
      source_callbacks.read = failing_checkpoint_read;
      assert(cai_source_from_callbacks(&source_callbacks, &state, &caierr) ==
             CAI_OK);
      assert(callbacks->checkpoint(callbacks->context, "workspace", "session-1",
                                   state, 6u, &caierr) == CAI_ERR_TRANSPORT);
      cai_source_close(state);
      /* Reopen the store to verify persisted recovery state, not cached data.
       */
      vectis_smith_store_destroy(store);
      assert(vectis_smith_store_new(&store_config, &store, &vectis_error) ==
             VECTIS_OK);
      callbacks = vectis_smith_store_session_store(store);
      assert(callbacks->load_latest(callbacks->context, "workspace", session_id,
                                    sizeof(session_id), &loaded, &watermark,
                                    &caierr) == CAI_OK);
      assert(strcmp(session_id, "session-1") == 0);
      assert(watermark == 4u);
      nread = cai_source_read(loaded, contents, sizeof(contents), &caierr);
      assert(nread == sizeof(checkpoint) - 1u);
      assert(memcmp(contents, checkpoint, nread) == 0);
      cai_source_close(loaded);
      memset(&replay, 0, sizeof(replay));
      assert(callbacks->load_events_after(callbacks->context, "workspace",
                                          "session-1", watermark, replay_event,
                                          &replay, &caierr) == CAI_OK);
      assert(replay.count == 2u);
      assert(replay.sequences[0] == 5u && replay.sequences[1] == 6u);
    }
  }
#ifdef VECTIS_TEST_SMITH_FAULTS
  /* Simulate a checkpoint interrupted after the session commit, before the
   * scope update. Bytes and watermark must still come from the same commit. */
  vectis_source = vectis_source_from_memory("new checkpoint", 14u);
  assert(vectis_cai_source_from_source(&vectis_source, &state, &vectis_error) ==
         VECTIS_OK);
  fail_scope_acquire = 1;
  assert(callbacks->checkpoint(callbacks->context, "workspace", "session-1",
                               state, 6u, &caierr) != CAI_OK);
  assert(fail_scope_acquire == 0);
  cai_source_close(state);
  assert(callbacks->load_latest(callbacks->context, "workspace", session_id,
                                sizeof(session_id), &loaded, &watermark,
                                &caierr) == CAI_OK);
  assert(watermark == 6u);
  assert(cai_source_read(loaded, contents, sizeof(contents), &caierr) == 14u);
  assert(memcmp(contents, "new checkpoint", 14u) == 0);
  cai_source_close(loaded);
  memset(&replay, 0, sizeof(replay));
  assert(callbacks->load_events_after(callbacks->context, "workspace",
                                      "session-1", watermark, replay_event,
                                      &replay, &caierr) == CAI_OK);
  assert(replay.count == 0u);
  {
    char type[10001];
    char data[10003];
    int i;
    memset(type, 't', sizeof(type) - 1u);
    type[sizeof(type) - 1u] = '\0';
    memset(data, 'd', sizeof(data) - 1u);
    data[sizeof(data) - 1u] = '\0';
    first.type = type;
    first.data = data;
    first.sequence = 7u;
    for (i = 0; i < 2; ++i) {
      fail_malloc_size = i == 0 ? sizeof(type) : sizeof(data);
      assert(callbacks->append_event(callbacks->context, "workspace",
                                     "session-1", &first,
                                     &caierr) == CAI_ERR_NOMEM);
      assert(fail_malloc_size == 0u);
    }
  }
#endif
  cai_error_cleanup(&caierr);
  {
    vectis_smith_config config;
    vectis_smith *smith;
    char checkpoint_text[65536];
    vectis_smith_config_init(&config);
    config.store = store;
    config.client_config.api_key = "offline-test-key";
    config.client_config.base_url = "unsupported://offline-test";
    config.client_config.timeout_ms = 10L;
    config.runtime.workspace_directory = directory;
    config.runtime.session_scope = "named-test";
    config.runtime.disable_terminal = 1;
    config.runtime.session_id = "named-one";
    config.runtime.resume_latest = 1;
    assert(vectis_smith_open(&config, &smith, &vectis_error) == VECTIS_OK);
    assert(strcmp(vectis_smith_session_id(smith), "named-one") == 0);
    assert(vectis_smith_submit_queued(smith, "remember first conversation",
                                      &vectis_error) == VECTIS_OK);
    settle_offline_turn(smith);
    vectis_smith_close(smith);
    config.runtime.session_id = "named-two";
    assert(vectis_smith_open(&config, &smith, &vectis_error) == VECTIS_OK);
    assert(strcmp(vectis_smith_session_id(smith), "named-two") == 0);
    assert(vectis_smith_submit_queued(smith, "second conversation",
                                      &vectis_error) == VECTIS_OK);
    settle_offline_turn(smith);
    vectis_smith_close(smith);
    config.runtime.session_id = "named-one";
    assert(vectis_smith_open(&config, &smith, &vectis_error) == VECTIS_OK);
    assert(strcmp(vectis_smith_session_id(smith), "named-one") == 0);
    assert(vectis_smith_submit_queued(smith, "followup", &vectis_error) ==
           VECTIS_OK);
    settle_offline_turn(smith);
    vectis_smith_close(smith);
    cai_error_init(&caierr);
    assert(callbacks->load_latest(callbacks->context, "named-test", session_id,
                                  sizeof(session_id), &loaded, &watermark,
                                  &caierr) == CAI_OK);
    assert(loaded != NULL && strcmp(session_id, "named-one") == 0);
    nread = cai_source_read(loaded, checkpoint_text,
                            sizeof(checkpoint_text) - 1u, &caierr);
    checkpoint_text[nread] = '\0';
    assert(strstr(checkpoint_text, "remember first conversation") != NULL);
    cai_source_close(loaded);
    cai_error_cleanup(&caierr);
  }
  vectis_smith_store_destroy(store);
  lc_client_close(client);
  remove_tree(directory);
}

int main(void) {
  test_lockdc_store_checkpoint_and_events();
  return 0;
}
