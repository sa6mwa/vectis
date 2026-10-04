#ifndef VECTIS_SUPERVISOR_INTERNAL_H
#define VECTIS_SUPERVISOR_INTERNAL_H
#include <vectis/supervisor.h>
vectis_supervisor_host *vectis_supervisor_host_create(void);
void vectis_supervisor_host_destroy(vectis_supervisor_host *);
int vectis_supervisor_host_requested(const vectis_supervisor_host *);
vectis_status vectis_supervisor_host_prepare(vectis_supervisor_host *,
                                             vectis_error *);
void vectis_supervisor_host_producer(vectis_supervisor_host *);
vectis_status vectis_supervisor_host_start(vectis_supervisor_host *,
                                           vectis_error *);
vectis_status vectis_supervisor_host_stop(vectis_supervisor_host *, long,
                                          vectis_error *);
/* Adds private lifecycle readiness to the same poll as public channels/timers.
 */
vectis_status vectis_supervisor_host_poll(vectis_supervisor_host *, int fd,
                                          long timeout_ms, int *ready,
                                          vectis_error *);
int vectis_supervisor_host_declaring(const vectis_supervisor_host *);
#endif
