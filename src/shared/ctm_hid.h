#ifndef CTM_HID_H
#define CTM_HID_H

#include <stddef.h>
#include <stdint.h>

#include "ctm_bridge_protocol.h"  /* ctmb_device_caps_t */

/* HID report-descriptor helpers shared by the bridge binaries. Extracted from
 * byte-identical copies that lived in hidraw_bridge.c and tv_bridge_worker.c. */

uint32_t hid_item_u32(const uint8_t *p, size_t n);
void     derive_report_lengths(const uint8_t *desc, uint32_t desc_len,
                               ctmb_device_caps_t *caps);
uint32_t read_report_descriptor(int fd, uint8_t *out, uint32_t out_cap);

/* Top-level device class from a report descriptor: first Usage Page + Usage,
 * plus the largest report payload in bytes. Used to tell a composite device's
 * interfaces apart (keyboard / mouse / vendor-gamepad). */
void        ctm_hid_top_usage(const uint8_t *desc, uint32_t desc_len,
                              uint16_t *usage_page, uint16_t *usage,
                              uint32_t *max_report_bytes);

/* Short static human label for a usage page/usage ("keyboard", "mouse",
 * "gamepad", "vendor", ...). When: labelling interfaces in the UI. */
const char *ctm_hid_usage_label(uint16_t usage_page, uint16_t usage);

#endif /* CTM_HID_H */
