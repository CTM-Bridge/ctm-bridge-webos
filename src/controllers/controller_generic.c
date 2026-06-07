/* Generic HID controller — verbatim relay, and the factory fallback. Its
 * matches() is always true so it claims any device no specific type took. */

#define _GNU_SOURCE

#include "ctm_controller.h"

static bool generic_matches(const ctm_controller_dev_t *dev)
{
    (void)dev;
    return true;
}

const ctm_controller_ops_t ctm_controller_generic_ops = {
    .kind = "generic",
    .matches = generic_matches,
    .select_node = NULL,
    .on_plug_init = NULL,
    .patch_output = NULL,   /* verbatim */
    .set_settings = NULL,
};
