/* Xbox detail panel. Placeholder for now: Xbox over BT is a verbatim relay
 * (the GIP translation is Windows-side), so the panel is identical to the
 * generic one. This file is the home for Xbox-specific UI as it grows
 * (mirrors controller_xbox in the D2 plan). */

#define _GNU_SOURCE

#include "ui_common.h"

void build_xbox_detail(lv_obj_t *parent, const logical_device_t *item,
                       tv_bridge_worker_settings_t *settings, int width)
{
    (void)settings;
    build_generic_detail(parent, item, width);
}
