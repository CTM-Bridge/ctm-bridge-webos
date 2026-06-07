/* Xbox controller, BT only (works-ish; GIP translation is Windows-side). USB
 * Xbox is the blocked usbip/input-only path and never reaches here. STAGE 1:
 * classification only; on_plug_init reserved for a TV-side BT handshake if one
 * proves necessary. */

#define _GNU_SOURCE

#include "ctm_controller.h"

#include <string.h>

static bool xbox_pid(const char *pid)
{
    static const char *const pids[] = {
        "02d1", "02dd", "02e0", "02e3", "02ea", "02fd",
        "0b00", "0b05", "0b0a", "0b12", "0b13", "0b20",
    };
    for (size_t i = 0; i < sizeof(pids) / sizeof(pids[0]); ++i) {
        if (strcmp(pid, pids[i]) == 0) return true;
    }
    return false;
}

static bool xbox_matches(const ctm_controller_dev_t *dev)
{
    return dev &&
           strcmp(dev->vid, "045e") == 0 &&
           strcmp(dev->bus, "BT") == 0 &&
           (xbox_pid(dev->pid) || (dev->name[0] && strcasestr(dev->name, "xbox")));
}

const ctm_controller_ops_t ctm_controller_xbox_ops = {
    .kind = "xbox",
    .matches = xbox_matches,
    .select_node = NULL,
    .on_plug_init = NULL,   /* STAGE 2: reserved for BT init/handshake if needed */
    .patch_output = NULL,   /* none: verbatim relay, Windows map does GIP */
    .set_settings = NULL,
};
