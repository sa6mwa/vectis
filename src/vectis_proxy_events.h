#ifndef VECTIS_PROXY_EVENTS_H
#define VECTIS_PROXY_EVENTS_H

struct kore_event;

#define VECTIS_PROXY_EVENT_READ 0x01
#define VECTIS_PROXY_EVENT_WRITE 0x02

/* Update one proxy-owned fd. edge_triggered is fixed for the watch lifetime.
 * The caller retains its previous mask and event object until Kore has left
 * the current event batch. An unchanged mask does not rearm the watcher. */
void vectis_proxy_event_update(int fd, struct kore_event *event, int previous,
                               int desired, int edge_triggered);

#endif
