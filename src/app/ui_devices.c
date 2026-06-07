/* Device enumeration, classification, and the logical-device model for the
 * app. Moved verbatim out of lvgl_ui.c; functions de-static'd and prototyped
 * in ui_common.h. Pure relocation, no behavior change. */

#define _GNU_SOURCE

#include "ctm_state.h"
#include "ctm_hid.h"   /* read_report_descriptor + interface classification */

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
device_info_t *find_or_add_device(scan_result_t *result, const char *hidraw)
{
    for (int i = 0; i < result->count; ++i) {
        if (strcmp(result->devices[i].hidraw, hidraw) == 0) {
            return &result->devices[i];
        }
    }
    if (result->count >= MAX_DEVICES) {
        return NULL;
    }

    device_info_t *dev = &result->devices[result->count++];
    memset(dev, 0, sizeof(*dev));
    snprintf(dev->hidraw, sizeof(dev->hidraw), "%s", hidraw);
    snprintf(dev->node, sizeof(dev->node), "/dev/%s", hidraw);
    return dev;
}

device_info_t *find_or_add_input_device(scan_result_t *result,
                                               const char *input_name,
                                               const char *usb_busid)
{
    for (int i = 0; i < result->count; ++i) {
        device_info_t *dev = &result->devices[i];
        if (dev->hidraw[0]) {
            continue;
        }
        if (usb_busid && usb_busid[0] && strcmp(dev->usb_busid, usb_busid) == 0) {
            return dev;
        }
        if (input_name && input_name[0] && strstr(dev->inputs, input_name)) {
            return dev;
        }
    }
    if (result->count >= MAX_DEVICES) {
        return NULL;
    }

    device_info_t *dev = &result->devices[result->count++];
    memset(dev, 0, sizeof(*dev));
    if (usb_busid && usb_busid[0]) {
        snprintf(dev->usb_busid, sizeof(dev->usb_busid), "%s", usb_busid);
    }
    return dev;
}

void usb_busid_from_input_path(const char *input_path, char *out, size_t out_len)
{
    out[0] = '\0';
    char device_link[PATH_MAX];
    char real[PATH_MAX];
    snprintf(device_link, sizeof(device_link), "%s/device", input_path);
    if (!realpath(device_link, real)) {
        return;
    }

    char copy[PATH_MAX];
    snprintf(copy, sizeof(copy), "%s", real);
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strchr(part, '-')) {
            continue;
        }
        if (strchr(part, ':')) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%s", part);
            char *colon = strchr(tmp, ':');
            if (colon) *colon = '\0';
            snprintf(out, out_len, "%s", tmp);
            return;
        }
        if (part[0] >= '0' && part[0] <= '9') {
            snprintf(out, out_len, "%s", part);
        }
    }
}

void inspect_hidraw(device_info_t *dev)
{
    if (!dev || !dev->node[0]) {
        return;
    }

    struct stat st;
    if (stat(dev->node, &st) != 0) {
        return;
    }

    int fd = open(dev->node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        dev->readable = true;
        dev->writable = true;
    } else {
        fd = open(dev->node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            dev->readable = true;
        }
    }
    if (fd < 0) {
        return;
    }

    struct hidraw_devinfo info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
        if (!dev->bus[0]) snprintf(dev->bus, sizeof(dev->bus), "%04x", info.bustype);
        if (!dev->vid[0]) snprintf(dev->vid, sizeof(dev->vid), "%04x", (unsigned short)info.vendor);
        if (!dev->pid[0]) snprintf(dev->pid, sizeof(dev->pid), "%04x", (unsigned short)info.product);
    }

    char raw_name[TEXT_LEN] = {0};
    if (!dev->name[0] && ioctl(fd, HIDIOCGRAWNAME(sizeof(raw_name) - 1), raw_name) >= 0) {
        snprintf(dev->name, sizeof(dev->name), "%s", raw_name);
    }

    char raw_phys[TEXT_LEN] = {0};
    if (!dev->phys[0] && ioctl(fd, HIDIOCGRAWPHYS(sizeof(raw_phys) - 1), raw_phys) >= 0) {
        snprintf(dev->phys, sizeof(dev->phys), "%s", raw_phys);
    }

    char raw_uniq[64] = {0};
    if (!dev->mac[0] && ioctl(fd, HIDIOCGRAWUNIQ(sizeof(raw_uniq) - 1), raw_uniq) >= 0) {
        snprintf(dev->mac, sizeof(dev->mac), "%s", raw_uniq);
    }

    int desc_size = 0;
    if (dev->report_descriptor_bytes <= 0 && ioctl(fd, HIDIOCGRDESCSIZE, &desc_size) == 0 && desc_size > 0) {
        dev->report_descriptor_bytes = desc_size;
    }

    /* Classify the interface from its report descriptor (top-level usage), so
     * composite devices (Steam Controller: keyboard/mouse/vendor) are legible
     * and we can pick the gamepad-bearing (vendor) interface. */
    if (!dev->iface[0]) {
        uint8_t desc[4096];
        uint32_t dlen = read_report_descriptor(fd, desc, sizeof(desc));
        if (dlen) {
            uint16_t up = 0, us = 0;
            uint32_t mb = 0;
            ctm_hid_top_usage(desc, dlen, &up, &us, &mb);
            dev->usage_page = up;
            dev->usage = us;
            snprintf(dev->iface, sizeof(dev->iface), "%s %uB",
                     ctm_hid_usage_label(up, us), (unsigned)mb);
        }
    }

    close(fd);
}

const char *bus_label(const char *bus)
{
    if (strcmp(bus, "0003") == 0 || strcmp(bus, "3") == 0) return "USB";
    if (strcmp(bus, "0005") == 0 || strcmp(bus, "5") == 0) return "BT";
    return bus[0] ? bus : "-";
}

bool is_steam_puck_device(const device_info_t *dev)
{
    return dev && strcmp(dev->vid, "28de") == 0 && strcmp(dev->pid, "1304") == 0;
}

bool is_ds5_device(const device_info_t *dev)
{
    return dev && strcmp(dev->vid, "054c") == 0 && strcmp(dev->pid, "0ce6") == 0;
}

bool is_ds4_device(const device_info_t *dev)
{
    return dev && strcmp(dev->vid, "054c") == 0 &&
           (strcmp(dev->pid, "09cc") == 0 || strcmp(dev->pid, "05c4") == 0);
}

bool is_xbox_pid(const char *pid)
{
    return pid &&
           (strcmp(pid, "02d1") == 0 || strcmp(pid, "02dd") == 0 ||
            strcmp(pid, "02e0") == 0 || strcmp(pid, "02e3") == 0 ||
            strcmp(pid, "02ea") == 0 ||
            strcmp(pid, "02fd") == 0 || strcmp(pid, "0b00") == 0 ||
            strcmp(pid, "0b05") == 0 || strcmp(pid, "0b0a") == 0 ||
            strcmp(pid, "0b12") == 0 || strcmp(pid, "0b13") == 0 ||
            strcmp(pid, "0b20") == 0);
}

bool is_xbox_device(const device_info_t *dev)
{
    return dev && strcmp(dev->vid, "045e") == 0 &&
           (is_xbox_pid(dev->pid) || contains_ci(dev->name, "xbox"));
}

bool is_xpad_compatible_pid(const char *vid, const char *pid)
{
    if (!vid || !pid) return false;
    if (strcmp(vid, "045e") == 0 &&
        (strcmp(pid, "028e") == 0 || strcmp(pid, "028f") == 0 ||
         strcmp(pid, "0291") == 0 || strcmp(pid, "0719") == 0 ||
         is_xbox_pid(pid))) {
        return true;
    }
    return false;
}

bool is_gulikit_named_device(const char *name)
{
    return contains_ci(name, "gulikit") || contains_ci(name, "guli");
}

bool is_xpad_input_only_candidate(const char *bus, const char *vid,
                                         const char *pid, const char *name,
                                         const char *usb_busid)
{
    return usb_busid && usb_busid[0] &&
           (strcmp(bus_label(bus), "USB") == 0) &&
           (is_xpad_compatible_pid(vid, pid) ||
            is_gulikit_named_device(name) ||
            contains_ci(name, "xinput") ||
            contains_ci(name, "x-box") ||
            contains_ci(name, "xbox"));
}

void steam_root_from_phys(const char *phys, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", phys && phys[0] ? phys : "unknown");
    char *input = strstr(out, "/input");
    if (input) {
        *input = '\0';
    }
}

void logical_key_for_device(const device_info_t *dev, char *out, size_t out_len)
{
    if (is_steam_puck_device(dev)) {
        /* Group ALL interfaces of the dongle (up to 4 slots x 3 interfaces)
         * under one logical device, so every hidraw is visible under one
         * expand instead of splitting by per-interface phys. */
        snprintf(out, out_len, "steam:%s:%s", dev->vid, dev->pid);
    } else if (dev && !dev->hidraw[0] && dev->usb_busid[0]) {
        snprintf(out, out_len, "usb:%s", dev->usb_busid);
    } else if (dev && !dev->hidraw[0] && dev->inputs[0]) {
        snprintf(out, out_len, "input:%s", dev->inputs);
    } else {
        snprintf(out, out_len, "hid:%s", dev->hidraw);
    }
}

void logical_name_for_device(const device_info_t *dev, char *out, size_t out_len)
{
    if (is_steam_puck_device(dev)) {
        snprintf(out, out_len, "Valve Software Steam Controller Puck");
    } else if (is_ds5_device(dev)) {
        snprintf(out, out_len, "Sony DS5 Controller");
    } else if (is_ds4_device(dev)) {
        snprintf(out, out_len, "Sony DS4 Controller");
    } else if (is_gulikit_named_device(dev->name)) {
        snprintf(out, out_len, "Gulikit Gamepad");
    } else if (is_xbox_device(dev)) {
        snprintf(out, out_len, "Microsoft Xbox Controller");
    } else if (dev && !dev->hidraw[0] && is_xpad_compatible_pid(dev->vid, dev->pid)) {
        snprintf(out, out_len, "XInput-compatible Gamepad");
    } else {
        snprintf(out, out_len, "%s", dev->name[0] ? dev->name : "Unnamed HID device");
    }
}

bool plug_key_is_set(const char *key)
{
    for (int i = 0; i < g_plugged_key_count; ++i) {
        if (strcmp(g_plugged_keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}

void set_plug_key(const char *key, bool plugged)
{
    for (int i = 0; i < g_plugged_key_count; ++i) {
        if (strcmp(g_plugged_keys[i], key) == 0) {
            if (!plugged) {
                memmove(&g_plugged_keys[i], &g_plugged_keys[i + 1],
                        (size_t)(g_plugged_key_count - i - 1) * sizeof(g_plugged_keys[0]));
                g_plugged_key_count--;
            }
            return;
        }
    }
    if (plugged && g_plugged_key_count < MAX_DEVICES) {
        snprintf(g_plugged_keys[g_plugged_key_count++], sizeof(g_plugged_keys[0]), "%s", key);
    }
}

bool expand_key_is_set(const char *key)
{
    for (int i = 0; i < g_expanded_key_count; ++i) {
        if (strcmp(g_expanded_keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}

void set_expand_key(const char *key, bool expanded)
{
    for (int i = 0; i < g_expanded_key_count; ++i) {
        if (strcmp(g_expanded_keys[i], key) == 0) {
            if (!expanded) {
                memmove(&g_expanded_keys[i], &g_expanded_keys[i + 1],
                        (size_t)(g_expanded_key_count - i - 1) * sizeof(g_expanded_keys[0]));
                g_expanded_key_count--;
            }
            return;
        }
    }
    if (expanded && g_expanded_key_count < MAX_DEVICES) {
        snprintf(g_expanded_keys[g_expanded_key_count++], sizeof(g_expanded_keys[0]), "%s", key);
    }
}

bool logical_device_can_expand(const logical_device_t *item)
{
    return item && starts_with(item->key, "steam:") && item->device_count > 1;
}

logical_device_t *find_or_add_logical_device(logical_result_t *logical, const device_info_t *dev, int scan_index)
{
    char key[96];
    logical_key_for_device(dev, key, sizeof(key));

    for (int i = 0; i < logical->count; ++i) {
        if (strcmp(logical->items[i].key, key) == 0) {
            logical_device_t *item = &logical->items[i];
            if (item->device_count < MAX_DEVICES) {
                item->device_indices[item->device_count++] = scan_index;
            }
            return item;
        }
    }

    if (logical->count >= MAX_DEVICES) {
        return NULL;
    }

    logical_device_t *item = &logical->items[logical->count++];
    memset(item, 0, sizeof(*item));
    snprintf(item->key, sizeof(item->key), "%s", key);
    logical_name_for_device(dev, item->name, sizeof(item->name));
    snprintf(item->bus, sizeof(item->bus), "%s", dev->bus);
    snprintf(item->vid, sizeof(item->vid), "%s", dev->vid);
    snprintf(item->pid, sizeof(item->pid), "%s", dev->pid);
    snprintf(item->mac, sizeof(item->mac), "%s", dev->mac);
    snprintf(item->usb_busid, sizeof(item->usb_busid), "%s", dev->usb_busid);
    item->plugged = plug_key_is_set(item->key);
    item->device_indices[item->device_count++] = scan_index;
    return item;
}

void build_logical_devices(const scan_result_t *scan, logical_result_t *logical)
{
    memset(logical, 0, sizeof(*logical));
    for (int i = 0; i < scan->count; ++i) {
        find_or_add_logical_device(logical, &scan->devices[i], i);
    }
}

/* --- Steam Puck USB-interface enumeration (shared by the list + detail) -----
 * Resolve the puck's USB device dir and enumerate its interfaces straight from
 * sysfs, so both the expanded list and the detail panel see the FULL set —
 * including the CDC interfaces and the input-less Service interface a
 * /sys/class/input scan can't reach. Paths are derived (host controller varies:
 * xhci/vhci); never hardcoded, never via /sys/bus (absent in the jail). */

/* The /dev node for a USB interface dir: a hidraw (HID) or ttyACMx (CDC). */
static void iface_node(const char *ifdir, char *out, size_t outlen)
{
    out[0] = '\0';
    DIR *d = opendir(ifdir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && out[0] == '\0') {
        if (e->d_name[0] == '.') continue;
        if (strncmp(e->d_name, "ttyACM", 6) == 0) {
            snprintf(out, outlen, "/dev/%s", e->d_name);
            break;
        }
        if (strcmp(e->d_name, "tty") == 0) {
            char tdir[PATH_MAX];
            snprintf(tdir, sizeof(tdir), "%s/tty", ifdir);
            DIR *t = opendir(tdir);
            if (t) {
                struct dirent *te;
                while ((te = readdir(t)) != NULL) {
                    if (te->d_name[0] == '.') continue;
                    snprintf(out, outlen, "/dev/%s", te->d_name);
                    break;
                }
                closedir(t);
            }
            continue;
        }
        char hdir[PATH_MAX];
        snprintf(hdir, sizeof(hdir), "%s/%s/hidraw", ifdir, e->d_name);
        DIR *h = opendir(hdir);
        if (h) {
            struct dirent *he;
            while ((he = readdir(h)) != NULL) {
                if (strncmp(he->d_name, "hidraw", 6) == 0) {
                    snprintf(out, outlen, "/dev/%s", he->d_name);
                    break;
                }
            }
            closedir(h);
        }
    }
    closedir(d);
}

int puck_usb_device_dir(const char *vid, const char *pid, char *out, size_t out_len)
{
    DIR *d = opendir("/sys/class/input");
    if (!d) return -1;
    struct dirent *e;
    int rc = -1;
    while ((e = readdir(d)) != NULL && rc != 0) {
        if (strncmp(e->d_name, "input", 5) != 0) continue;
        char attr[PATH_MAX], v[16] = {0}, p[16] = {0};
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/vendor", e->d_name);
        read_text_file(attr, v, sizeof(v));
        snprintf(attr, sizeof(attr), "/sys/class/input/%s/id/product", e->d_name);
        read_text_file(attr, p, sizeof(p));
        if (strcmp(v, vid) != 0 || strcmp(p, pid) != 0) continue;
        char link[PATH_MAX], real[PATH_MAX];
        snprintf(link, sizeof(link), "/sys/class/input/%s/device", e->d_name);
        if (!realpath(link, real)) continue;
        while (real[0]) {                       /* walk up to the USB device dir (has idVendor) */
            char idf[PATH_MAX];
            struct stat st;
            snprintf(idf, sizeof(idf), "%s/idVendor", real);
            if (stat(idf, &st) == 0) {
                snprintf(out, out_len, "%s", real);
                rc = 0;
                break;
            }
            char *s = strrchr(real, '/');
            if (!s || s == real) break;
            *s = '\0';
        }
    }
    closedir(d);
    return rc;
}

int puck_enumerate_ifaces(const char *usbdir, puck_if_t *out, int max)
{
    const char *base = strrchr(usbdir, '/');
    base = base ? base + 1 : usbdir;
    size_t blen = strlen(base);
    int n = 0;
    DIR *d = opendir(usbdir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (strncmp(e->d_name, base, blen) != 0 || e->d_name[blen] != ':') continue;
        char ifdir[PATH_MAX], attr[PATH_MAX], num[16] = {0};
        snprintf(ifdir, sizeof(ifdir), "%s/%s", usbdir, e->d_name);
        snprintf(attr, sizeof(attr), "%s/bInterfaceClass", ifdir);
        if (read_text_file(attr, out[n].cls, sizeof(out[n].cls)) != 0) continue;
        snprintf(attr, sizeof(attr), "%s/bInterfaceNumber", ifdir);
        read_text_file(attr, num, sizeof(num));
        out[n].num = (int)strtol(num, NULL, 16);
        iface_node(ifdir, out[n].node, sizeof(out[n].node));
        snprintf(out[n].dir, sizeof(out[n].dir), "%s", e->d_name);
        n++;
    }
    closedir(d);
    for (int a = 0; a < n; ++a) {
        for (int b = a + 1; b < n; ++b) {
            if (out[b].num < out[a].num) { puck_if_t t = out[a]; out[a] = out[b]; out[b] = t; }
        }
    }
    return n;
}

/* Raw (binary) sysfs read — for `descriptors` and `report_descriptor` blobs
 * (read_text_file would trim/stop at NULs). Returns bytes read. */
static int read_binary_file(const char *path, uint8_t *out, int max)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int total = 0, n;
    while (total < max && (n = (int)read(fd, out + total, (size_t)(max - total))) > 0) {
        total += n;
    }
    close(fd);
    return total;
}

/* The HID report descriptor for one interface dir (<usbdir>/<dir>/<hid>/report_descriptor). */
static int read_iface_rdesc(const char *usbdir, const char *dir, uint8_t *out, int max)
{
    char ifdir[300];
    snprintf(ifdir, sizeof(ifdir), "%s/%s", usbdir, dir);
    DIR *d = opendir(ifdir);
    if (!d) return 0;
    struct dirent *e;
    int len = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char rd[400];
        snprintf(rd, sizeof(rd), "%s/%s/report_descriptor", ifdir, e->d_name);
        len = read_binary_file(rd, out, max);
        if (len > 0) break;
        len = 0;
    }
    closedir(d);
    return len;
}

/* Stage 1: capture the puck's full USB enumeration once (device+config blob +
 * each HID interface's report descriptor) and cache it in g_puck_enum. Idempotent
 * per USB device; invalidates if the puck is gone. Returns 0 on success. */
int puck_enum_capture(const char *vid, const char *pid)
{
    char usbdir[256];
    if (puck_usb_device_dir(vid, pid, usbdir, sizeof(usbdir)) != 0) {
        g_puck_enum.valid = 0;
        return -1;
    }
    if (g_puck_enum.valid && strcmp(g_puck_enum.usbdir, usbdir) == 0) {
        return 0;                                   /* already captured this device */
    }
    memset(&g_puck_enum, 0, sizeof(g_puck_enum));
    snprintf(g_puck_enum.usbdir, sizeof(g_puck_enum.usbdir), "%s", usbdir);

    char attr[300];
    snprintf(attr, sizeof(attr), "%s/descriptors", usbdir);
    g_puck_enum.descriptors_len = read_binary_file(attr, g_puck_enum.descriptors, PUCK_ENUM_MAX_DESC);
    snprintf(attr, sizeof(attr), "%s/serial", usbdir);
    read_text_file(attr, g_puck_enum.serial, sizeof(g_puck_enum.serial));

    puck_if_t ifs[16];
    int nif = puck_enumerate_ifaces(usbdir, ifs, 16);
    for (int i = 0; i < nif && g_puck_enum.if_count < PUCK_ENUM_MAX_IF; ++i) {
        puck_enum_if_t *de = &g_puck_enum.ifs[g_puck_enum.if_count++];
        de->num = ifs[i].num;
        snprintf(de->cls, sizeof(de->cls), "%s", ifs[i].cls);
        snprintf(de->node, sizeof(de->node), "%s", ifs[i].node);
        if (strncmp(ifs[i].cls, "03", 2) == 0) {
            de->rdesc_len = read_iface_rdesc(usbdir, ifs[i].dir, de->rdesc, PUCK_ENUM_MAX_RDESC);
        }
    }
    g_puck_enum.valid = 1;
    log_append("puck enum: %s desc=%dB ifaces=%d serial=%s",
               usbdir, g_puck_enum.descriptors_len, g_puck_enum.if_count, g_puck_enum.serial);
    for (int i = 0; i < g_puck_enum.if_count; ++i) {
        log_append("  if 1.%d cls=%s rdesc=%dB %s", g_puck_enum.ifs[i].num,
                   g_puck_enum.ifs[i].cls, g_puck_enum.ifs[i].rdesc_len,
                   g_puck_enum.ifs[i].node[0] ? g_puck_enum.ifs[i].node : "-");
    }
    return 0;
}

void enumerate_devices(scan_result_t *result)
{
    memset(result, 0, sizeof(*result));

    DIR *input_dir = opendir("/sys/class/input");
    if (input_dir) {
        struct dirent *input_ent;
        while ((input_ent = readdir(input_dir)) != NULL) {
            if (!starts_with(input_ent->d_name, "input")) {
                continue;
            }
            result->input_count++;

            char input_path[PATH_MAX];
            snprintf(input_path, sizeof(input_path), "/sys/class/input/%s", input_ent->d_name);

            char name[TEXT_LEN] = {0};
            char phys[TEXT_LEN] = {0};
            char bus[16] = {0};
            char vid[16] = {0};
            char pid[16] = {0};
            char version[16] = {0};
            char uniq[64] = {0};
            char usb_busid[64] = {0};
            char child_path[PATH_MAX];
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/name", input_path), child_path), name, sizeof(name));
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/phys", input_path), child_path), phys, sizeof(phys));
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/id/bustype", input_path), child_path), bus, sizeof(bus));
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/id/vendor", input_path), child_path), vid, sizeof(vid));
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/id/product", input_path), child_path), pid, sizeof(pid));
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/id/version", input_path), child_path), version, sizeof(version));
            read_text_file((snprintf(child_path, sizeof(child_path), "%s/uniq", input_path), child_path), uniq, sizeof(uniq));
            usb_busid_from_input_path(input_path, usb_busid, sizeof(usb_busid));

            char event_names[TEXT_LEN] = {0};
            char input_node[64] = {0};
            DIR *one_input = opendir(input_path);
            if (one_input) {
                struct dirent *child;
                while ((child = readdir(one_input)) != NULL) {
                    if (starts_with(child->d_name, "js")) {
                        append_unique(event_names, sizeof(event_names), child->d_name);
                        snprintf(input_node, sizeof(input_node), "/dev/input/%s", child->d_name);
                    } else if (starts_with(child->d_name, "event")) {
                        append_unique(event_names, sizeof(event_names), child->d_name);
                        if (!input_node[0]) {
                            snprintf(input_node, sizeof(input_node), "/dev/input/%s", child->d_name);
                        }
                    }
                }
                closedir(one_input);
            }

            char hidraw_path[PATH_MAX];
            snprintf(hidraw_path, sizeof(hidraw_path), "%s/device/hidraw", input_path);
            DIR *hidraw_dir = opendir(hidraw_path);
            if (!hidraw_dir) {
                if (is_xpad_input_only_candidate(bus, vid, pid, name, usb_busid)) {
                    device_info_t *dev = find_or_add_input_device(result, input_ent->d_name, usb_busid);
                    if (dev) {
                        if (!dev->node[0]) {
                            snprintf(dev->node, sizeof(dev->node), "%s",
                                     input_node[0] ? input_node : input_path);
                        }
                        if (!dev->name[0]) snprintf(dev->name, sizeof(dev->name), "%s", name);
                        if (!dev->phys[0]) snprintf(dev->phys, sizeof(dev->phys), "%s", phys);
                        if (!dev->bus[0]) snprintf(dev->bus, sizeof(dev->bus), "%s", bus);
                        if (!dev->vid[0]) snprintf(dev->vid, sizeof(dev->vid), "%s", vid);
                        if (!dev->pid[0]) snprintf(dev->pid, sizeof(dev->pid), "%s", pid);
                        if (!dev->version[0]) snprintf(dev->version, sizeof(dev->version), "%s", version);
                        if (!dev->mac[0]) snprintf(dev->mac, sizeof(dev->mac), "%s", uniq);
                        if (!dev->usb_busid[0]) snprintf(dev->usb_busid, sizeof(dev->usb_busid), "%s", usb_busid);
                        append_unique(dev->inputs, sizeof(dev->inputs), input_ent->d_name);
                        append_unique(dev->events, sizeof(dev->events), event_names);
                    }
                }
                continue;
            }

            struct dirent *hidraw_ent;
            while ((hidraw_ent = readdir(hidraw_dir)) != NULL) {
                if (!starts_with(hidraw_ent->d_name, "hidraw")) {
                    continue;
                }
                device_info_t *dev = find_or_add_device(result, hidraw_ent->d_name);
                if (!dev) {
                    continue;
                }
                if (!dev->name[0]) snprintf(dev->name, sizeof(dev->name), "%s", name);
                if (!dev->phys[0]) snprintf(dev->phys, sizeof(dev->phys), "%s", phys);
                if (!dev->bus[0]) snprintf(dev->bus, sizeof(dev->bus), "%s", bus);
                if (!dev->vid[0]) snprintf(dev->vid, sizeof(dev->vid), "%s", vid);
                if (!dev->pid[0]) snprintf(dev->pid, sizeof(dev->pid), "%s", pid);
                if (!dev->version[0]) snprintf(dev->version, sizeof(dev->version), "%s", version);
                if (!dev->mac[0]) snprintf(dev->mac, sizeof(dev->mac), "%s", uniq);
                if (!dev->usb_busid[0]) snprintf(dev->usb_busid, sizeof(dev->usb_busid), "%s", usb_busid);
                append_unique(dev->inputs, sizeof(dev->inputs), input_ent->d_name);
                append_unique(dev->events, sizeof(dev->events), event_names);

                snprintf(child_path, sizeof(child_path), "%s/device/report_descriptor", input_path);
                struct stat report_st;
                if (dev->report_descriptor_bytes <= 0 && stat(child_path, &report_st) == 0) {
                    dev->report_descriptor_bytes = (int)report_st.st_size;
                }
            }
            closedir(hidraw_dir);
        }
        closedir(input_dir);
    }

    for (int i = 0; i < 64; ++i) {
        char hidraw[32];
        char node[64];
        snprintf(hidraw, sizeof(hidraw), "hidraw%d", i);
        snprintf(node, sizeof(node), "/dev/%s", hidraw);
        struct stat st;
        if (stat(node, &st) != 0) {
            continue;
        }
        device_info_t *dev = find_or_add_device(result, hidraw);
        if (dev) {
            inspect_hidraw(dev);
        }
    }

    for (int i = 0; i < result->count; ++i) {
        inspect_hidraw(&result->devices[i]);
    }
}

