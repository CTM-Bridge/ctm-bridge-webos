/* Generic HID detail panel (also the fallback for unclassified devices).
 * Moved verbatim out of lvgl_ui.c; de-static'd, prototyped in ui_common.h. */

#define _GNU_SOURCE

#include "ui_common.h"

#include <stdio.h>
void build_generic_detail(lv_obj_t *parent, const logical_device_t *item, int width)
{
    int y = 0;
    detail_label(parent, item->name, 0, y, width, &lv_font_montserrat_20, lv_color_hex(0xf5f8fa));
    y += 38;
    y = detail_add_identity(parent, item, y, width);
    detail_label(parent, "Interfaces", 0, y, width, &lv_font_montserrat_16, lv_color_hex(0xf5f8fa));
    y += 30;
    for (int i = 0; i < item->device_count; ++i) {
        int scan_index = item->device_indices[i];
        if (scan_index < 0 || scan_index >= g_scan.count) continue;
        const device_info_t *dev = &g_scan.devices[scan_index];
        char line[384];
        snprintf(line, sizeof(line), "d%d  %s  %s  %dB  %s",
                 i + 1,
                 dev->hidraw[0] ? dev->hidraw : dev->node,
                 dev->readable ? (dev->writable ? "rw" : "ro") : "sys",
                 dev->report_descriptor_bytes,
                 dev->inputs[0] ? dev->inputs : "-");
        detail_label(parent, line, 0, y, width, &lv_font_montserrat_14, lv_color_hex(0xc7d4dc));
        y += 28;
    }
}

