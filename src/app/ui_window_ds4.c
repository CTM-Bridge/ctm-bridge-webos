/* DualShock 4 detail panel. Moved verbatim out of lvgl_ui.c; de-static'd,
 * prototyped in ui_common.h. */

#define _GNU_SOURCE

#include "ui_common.h"

#include <stdio.h>
void build_ds4_detail(lv_obj_t *parent, const logical_device_t *item,
                             tv_bridge_worker_settings_t *settings, int width)
{
    ui_device_settings_t *record = ui_record_for_item(item);
    int y = 0;
    detail_label(parent, "Sony DS4 Controller", 0, y, width, &lv_font_montserrat_20, lv_color_hex(0xf5f8fa));
    y += 38;
    y = detail_add_identity(parent, item, y, width);
    y = detail_add_audio_modes(parent, settings, y, width);
    /* DS4 volume bytes are 0..0x4F (79) per the controller wiki firmware
     * ceiling. Slider max matches so the top of the slider == max audible. */
    y = detail_add_slider(parent, "Headset volume", record ? (int)record->headset_volume_percent : 0x4f,
                          0, 0x4f, y, width, 3);
    y = detail_add_slider(parent, "Speaker volume", record ? (int)record->speaker_volume_percent : 0x4f,
                          0, 0x4f, y, width, 4);
}

