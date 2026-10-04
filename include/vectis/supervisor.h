#ifndef VECTIS_SUPERVISOR_H
#define VECTIS_SUPERVISOR_H
#include <stdint.h>
#include <vectis/vectis.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct vectis_supervisor_host vectis_supervisor_host;
typedef struct vectis_supervisor vectis_supervisor;
typedef struct vectis_supervisor_channel vectis_supervisor_channel;
typedef enum vectis_supervisor_state {
  VECTIS_SUPERVISOR_DECLARED,
  VECTIS_SUPERVISOR_MATERIALIZING,
  VECTIS_SUPERVISOR_RUNNING,
  VECTIS_SUPERVISOR_STOPPING,
  VECTIS_SUPERVISOR_STOPPED,
  VECTIS_SUPERVISOR_FAILED
} vectis_supervisor_state;
typedef enum vectis_supervisor_event_type {
  VECTIS_SUPERVISOR_START,
  VECTIS_SUPERVISOR_STOP,
  VECTIS_SUPERVISOR_MESSAGE,
  VECTIS_SUPERVISOR_SIGNAL,
  VECTIS_SUPERVISOR_TIMER,
  VECTIS_SUPERVISOR_PROBE
} vectis_supervisor_event_type;
typedef struct vectis_supervisor_event {
  vectis_supervisor_event_type type;
  uint32_t channel_id;
  uint32_t kind;
  uint32_t correlation_id;
  uint32_t flags;
  const void *payload; /* borrowed only for the duration of the callback */
  size_t payload_size;
  long deadline_ms; /* remaining shutdown budget for STOP */
} vectis_supervisor_event;
typedef vectis_status (*vectis_supervisor_handler_fn)(
    void *context, const vectis_supervisor_event *event, vectis_error *error);
typedef struct vectis_supervisor_config {
  const char *name;
  size_t capacity;
  size_t max_frame_size;
  const uint32_t *signal_kinds;
  size_t signal_kind_count;
  int start_with_app;
  int stop_on_failure; /* false: fail app; true: stop only this scope */
  vectis_supervisor_handler_fn handler;
  void *context;
} vectis_supervisor_config;
/* New receiver shell; existing app ABI is untouched. All handles are app-owned.
 */
struct vectis_supervisor {
  vectis_status (*start)(vectis_supervisor *, vectis_error *);
  vectis_status (*stop)(vectis_supervisor *, long, vectis_error *);
  vectis_status (*wait)(vectis_supervisor *, long, vectis_error *);
  vectis_supervisor_state (*state)(const vectis_supervisor *);
  vectis_status (*channel)(vectis_supervisor *, vectis_supervisor_channel **,
                           vectis_error *);
  vectis_status (*timer)(vectis_supervisor *, uint32_t, long, vectis_error *);
  void (*close)(vectis_supervisor *);
  void *impl;
  void *reserved[8];
};
/* Resolves Vectis's existing encrypted Pouch defaults after startup/fork.
 * Requested paths are copied into caller buffers; no client is opened. */
vectis_status
vectis_app_workflow_storage_defaults(vectis_app *, int need_endpoint,
                                     int need_key_file, char *endpoint,
                                     size_t endpoint_capacity, char *key_file,
                                     size_t key_file_capacity, vectis_error *);
struct lc_client_config;
/* Vectis-owned workflow/source clients borrow the app's native lockd logger.
 * Close every derived job/lease/dispatcher/client before closing the app. */
vectis_status vectis_app_configure_workflow_client(vectis_app *,
                                                   struct lc_client_config *,
                                                   vectis_error *);
void vectis_supervisor_config_init(vectis_supervisor_config *config);
vectis_status vectis_app_supervisor_host(vectis_app *app,
                                         vectis_supervisor_host **out,
                                         vectis_error *error);
vectis_status vectis_supervisor_new(vectis_app *app,
                                    const vectis_supervisor_config *config,
                                    vectis_supervisor **out,
                                    vectis_error *error);
vectis_status vectis_supervisor_start(vectis_supervisor *, vectis_error *);
vectis_status vectis_supervisor_stop(vectis_supervisor *, long, vectis_error *);
vectis_status vectis_supervisor_wait(vectis_supervisor *, long, vectis_error *);
vectis_supervisor_state vectis_supervisor_get_state(const vectis_supervisor *);
vectis_status vectis_supervisor_channel_get(vectis_supervisor *,
                                            vectis_supervisor_channel **,
                                            vectis_error *);
/* Full = ERR_CONFLICT, closed = ERR_STATE, empty receive = ERR_TIMEOUT.
 * send never waits for capacity. Signals have separate finite coalesced slots.
 */
vectis_status vectis_supervisor_channel_send(vectis_supervisor_channel *,
                                             uint32_t kind, const void *payload,
                                             size_t size, vectis_error *);
vectis_status vectis_supervisor_channel_send_frame(
    vectis_supervisor_channel *, uint32_t kind, uint32_t correlation_id,
    uint32_t flags, const void *payload, size_t size, vectis_error *);
vectis_status vectis_supervisor_channel_signal(vectis_supervisor_channel *,
                                               uint32_t kind, vectis_error *);
/* Receive is restricted to the channel owner process and thread. Scope
 * stop/start preserves queued frames/signals; app reprepare creates fresh IPC.
 */
vectis_status vectis_supervisor_channel_receive(vectis_supervisor_channel *,
                                                vectis_supervisor_event *,
                                                void *buffer, size_t capacity,
                                                vectis_error *);
/* Descriptor is borrowed; poll only, do not read or close. */
int vectis_supervisor_channel_fd(const vectis_supervisor_channel *);
vectis_status vectis_supervisor_timer(vectis_supervisor *, uint32_t kind,
                                      long every_ms, vectis_error *);
/* Disabled timers neither deliver nor shorten host poll deadlines. Enabling
 * resets the next monotonic deadline to one full interval from now. */
vectis_status vectis_supervisor_timer_enable(vectis_supervisor *, uint32_t, int,
                                             vectis_error *);
/* Copies the latest callback failure; out->code is OK until a failure or after
 * a successful explicit restart. Only the owner may inspect it. */
vectis_status vectis_supervisor_last_error(const vectis_supervisor *,
                                           vectis_error *out);
/* Stop closes ingress without waiting on producer locks. Embedders must join
 * their own producer threads before app close/destroy or restart; handles are
 * app-owned and concurrent destruction is not supported. Darwin lacks robust
 * mutex recovery: a dead lock owner requires stop and fresh app preparation. */
/* Coalesces process-local source readiness, independent of IPC signals. */
vectis_status vectis_supervisor_schedule(vectis_supervisor *, vectis_error *);
/* pump invokes callbacks serially on the application's calling owner thread.
 * timeout_ms = 0 is a probe, -1 permits waiting indefinitely. No recursion. */
vectis_status vectis_supervisor_host_pump(vectis_supervisor_host *,
                                          long timeout_ms, vectis_error *);
/* Intentional embedder fork: child closes the owner receive ends. */
void vectis_supervisor_host_after_fork_producer(vectis_supervisor_host *);
int vectis_supervisor_host_dispatching(const vectis_supervisor_host *);
void vectis_supervisor_close(vectis_supervisor *);
#ifdef __cplusplus
}
#endif
#endif
