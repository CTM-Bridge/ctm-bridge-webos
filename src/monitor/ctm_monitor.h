#ifndef CTM_MONITOR_H
#define CTM_MONITOR_H

/* Device detection / monitor (D8). A dedicated component (its own thread) that
 * discovers connected controllers, classifies each via the controller factory,
 * and notifies on connect/disconnect so the UI can populate its list. It does
 * NOT bridge anything — that's the controller layer. UI-independent: it deals
 * only in ctm_controller_dev_t.
 *
 * Hotplug today is a periodic rescan with diff (always-correct fallback);
 * netlink-uevent push is a future refinement. */

#include "ctm_controller.h"   /* ctm_controller_dev_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctm_monitor ctm_monitor_t;

/* Connect/disconnect callback, invoked on the monitor thread: present=1 when a
 * device appears, 0 when it goes away. `dev` is valid only for the call. */
typedef void (*ctm_monitor_cb)(void *ud, const ctm_controller_dev_t *dev, int present);

/* Start the scan+watch thread. cb may be NULL (then poll via ctm_monitor_list).
 * Returns NULL on failure. When: app startup. */
ctm_monitor_t *ctm_monitor_start(ctm_monitor_cb cb, void *ud);

/* Snapshot the current device list into out[0..max-1]; returns the count.
 * Thread-safe. When: a UI refresh that wants the whole list. */
int ctm_monitor_list(ctm_monitor_t *m, ctm_controller_dev_t *out, int max);

/* Stop + join the monitor thread and free it. When: app shutdown. */
void ctm_monitor_stop(ctm_monitor_t *m);

#ifdef __cplusplus
}
#endif

#endif /* CTM_MONITOR_H */
