/* Steam Controller (puck) detail panel — info only (no controls). Full USB
 * composition read live from sysfs: every interface (HID + the CDC serial and
 * the input-less Service the hidraw/input scans can't see), its class, its /dev
 * node, and a plain role. Uses the shared puck USB-dir resolver
 * (/sys/class/input -> /sys/devices; host controller derived, never /sys/bus).
 * Everything is observed from sysfs — no protocol assumptions. */

#define _GNU_SOURCE

#include "ui_common.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

void build_steam_puck_detail(lv_obj_t *parent, const logical_device_t *item, int width)
{
    int y = 0;
    detail_label(parent, "Valve Steam Controller Puck", 0, y, width,
                 &lv_font_montserrat_20, lv_color_hex(0xf5f8fa));
    y += 38;
    y = detail_add_identity(parent, item, y, width);

    char usbdir[PATH_MAX];
    if (puck_usb_device_dir(item->vid, item->pid, usbdir, sizeof(usbdir)) != 0) {
        detail_label(parent, "USB composition unavailable.",
                     0, y, width, &lv_font_montserrat_14, lv_color_hex(0x9fb2bd));
        return;
    }

    char serial[64] = {0}, attr[PATH_MAX];
    snprintf(attr, sizeof(attr), "%s/serial", usbdir);
    if (read_text_file(attr, serial, sizeof(serial)) == 0 && serial[0]) {
        char sline[128];
        snprintf(sline, sizeof(sline), "serial %s", serial);
        detail_label(parent, sline, 0, y, width, &lv_font_montserrat_14, lv_color_hex(0x9fb2bd));
        y += 26;
    }

    detail_label(parent, "Connections", 0, y, width,
                 &lv_font_montserrat_16, lv_color_hex(0xf5f8fa));
    y += 30;

    puck_if_t ifs[16];
    int nif = puck_enumerate_ifaces(usbdir, ifs, 16);
    for (int i = 0; i < nif; ++i) {
        char rolebuf[24];
        const char *role;
        if (strncmp(ifs[i].cls, "03", 2) == 0) {
            if (ifs[i].num >= 2 && ifs[i].num <= 5) {
                snprintf(rolebuf, sizeof(rolebuf), "Controller %d", ifs[i].num - 2);
                role = rolebuf;
            } else if (ifs[i].num == 6) {
                role = "Service / management";
            } else {
                role = "HID interface";
            }
        } else if (strncmp(ifs[i].cls, "02", 2) == 0) {
            role = "CDC-ACM serial (COM)";
        } else if (strncmp(ifs[i].cls, "0a", 2) == 0 || strncmp(ifs[i].cls, "0A", 2) == 0) {
            role = "CDC data";
        } else {
            role = "(other)";
        }
        char line[200];
        snprintf(line, sizeof(line), "1.%d   cls=%s   %s   %s",
                 ifs[i].num, ifs[i].cls, ifs[i].node[0] ? ifs[i].node : "-", role);
        detail_label(parent, line, 0, y, width, &lv_font_montserrat_14, lv_color_hex(0xc7d4dc));
        y += 26;
    }

    y += 6;
    detail_label(parent,
                 "Controllers 0-3 boot as keyboard+mouse until gamepad mode. COM (ttyACM0) not bridged.",
                 0, y, width, &lv_font_montserrat_14, lv_color_hex(0x7f8c98));
}
