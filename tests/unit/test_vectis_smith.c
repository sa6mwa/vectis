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

static void test_lockdc_store_checkpoint_and_events(void) {
  char directory[] = "/tmp/vectis-smith-store.XXXXXX";
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
  cai_error_cleanup(&caierr);
  vectis_smith_store_destroy(store);
  lc_client_close(client);
  remove_tree(directory);
}

int main(void) {
  test_lockdc_store_checkpoint_and_events();
  return 0;
}
