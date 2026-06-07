#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usbdevice_fs.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef USBDEVFS_DISCONNECT_CLAIM
#define USBDEVFS_DISCONNECT_CLAIM _IOR('U', 27, struct usbdevfs_disconnect_claim)
#endif

#ifndef USBDEVFS_DISCONNECT_CLAIM_EXCEPT_DRIVER
#define USBDEVFS_DISCONNECT_CLAIM_EXCEPT_DRIVER 0x01
#endif

#define USBIP_VERSION 0x0111u
#define OP_REQ_DEVLIST 0x8005u
#define OP_REP_DEVLIST 0x0005u
#define OP_REQ_IMPORT 0x8003u
#define OP_REP_IMPORT 0x0003u
#define CMD_SUBMIT 0x00000001u
#define CMD_UNLINK 0x00000002u
#define RET_SUBMIT 0x00000003u
#define RET_UNLINK 0x00000004u
#define USBIP_DIR_OUT 0u
#define USBIP_DIR_IN 1u
#define USBIP_NON_ISO 0xffffffffu
#define USBIP_PORT 3240
#define MAX_USB_DEVICES 128
#define MAX_INTERFACES 32
#define MAX_DESCRIPTORS 8192
#define MAX_TRANSFER (1024u * 1024u)

typedef struct {
    uint8_t cls;
    uint8_t subcls;
    uint8_t proto;
} usbip_interface_t;

typedef struct {
    char sysfs_name[32];
    char sysfs_path[256];
    char devnode[64];
    uint8_t descriptors[MAX_DESCRIPTORS];
    size_t descriptors_len;
    int busnum;
    int devnum;
    uint32_t speed;
    uint16_t vid;
    uint16_t pid;
    uint16_t bcd_device;
    uint8_t dev_class;
    uint8_t dev_subclass;
    uint8_t dev_protocol;
    uint8_t config_value;
    uint8_t num_configurations;
    uint8_t num_interfaces;
    uint8_t endpoint_type[256];
    usbip_interface_t interfaces[MAX_INTERFACES];
} usbip_device_t;

typedef struct pending_urb {
    uint32_t seqnum;
    uint32_t direction;
    uint32_t packets;
    struct usbdevfs_urb *urb;
    uint8_t *buffer;
    struct pending_urb *next;
} pending_urb_t;

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static void append_u8(uint8_t **p, uint8_t v) {
    *(*p)++ = v;
}

static void append_be16(uint8_t **p, uint16_t v) {
    *(*p)++ = (uint8_t)(v >> 8);
    *(*p)++ = (uint8_t)v;
}

static void append_be32(uint8_t **p, uint32_t v) {
    *(*p)++ = (uint8_t)(v >> 24);
    *(*p)++ = (uint8_t)(v >> 16);
    *(*p)++ = (uint8_t)(v >> 8);
    *(*p)++ = (uint8_t)v;
}

static void append_fixed(uint8_t **p, const char *s, size_t n) {
    size_t len = s ? strlen(s) : 0;
    if (len > n) len = n;
    if (len) memcpy(*p, s, len);
    if (len < n) memset(*p + len, 0, n - len);
    *p += n;
}

static int send_all(int fd, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *data, size_t len) {
    uint8_t *p = (uint8_t *)data;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_text(const char *path, char *out, size_t out_len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, out_len - 1);
    close(fd);
    if (n <= 0) return -1;
    out[n] = '\0';
    char *nl = strpbrk(out, "\r\n");
    if (nl) *nl = '\0';
    return 0;
}

static int read_attr_u32(const char *dir, const char *name, unsigned int base, uint32_t *out) {
    char path[320];
    char text[64];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (read_text(path, text, sizeof(text)) != 0) return -1;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, base);
    if (end == text) return -1;
    *out = (uint32_t)value;
    return 0;
}

static int read_binary_file(const char *path, uint8_t *out, size_t out_cap, size_t *out_len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, out_cap);
    close(fd);
    if (n <= 0) return -1;
    *out_len = (size_t)n;
    return 0;
}

static uint32_t speed_from_text(const char *text) {
    if (!text) return 0;
    if (strncmp(text, "1.5", 3) == 0) return 1;
    if (strncmp(text, "12", 2) == 0) return 2;
    if (strncmp(text, "480", 3) == 0) return 3;
    if (strncmp(text, "5000", 4) == 0) return 5;
    if (strncmp(text, "10000", 5) == 0 || strncmp(text, "20000", 5) == 0) return 6;
    return 0;
}

static void parse_descriptors(usbip_device_t *dev) {
    memset(dev->endpoint_type, 0, sizeof(dev->endpoint_type));
    dev->num_interfaces = 0;
    if (dev->descriptors_len >= 18 && dev->descriptors[1] == USB_DT_DEVICE) {
        const uint8_t *d = dev->descriptors;
        dev->dev_class = d[4];
        dev->dev_subclass = d[5];
        dev->dev_protocol = d[6];
        dev->vid = read_le16(d + 8);
        dev->pid = read_le16(d + 10);
        dev->bcd_device = read_le16(d + 12);
        dev->num_configurations = d[17];
    }

    size_t off = 0;
    while (off + 2 <= dev->descriptors_len) {
        uint8_t len = dev->descriptors[off];
        uint8_t type = dev->descriptors[off + 1];
        if (len < 2 || off + len > dev->descriptors_len) break;
        const uint8_t *d = dev->descriptors + off;
        if (type == USB_DT_CONFIG && len >= 9) {
            dev->config_value = d[5];
        } else if (type == USB_DT_INTERFACE && len >= 9) {
            if (dev->num_interfaces < MAX_INTERFACES) {
                dev->interfaces[dev->num_interfaces].cls = d[5];
                dev->interfaces[dev->num_interfaces].subcls = d[6];
                dev->interfaces[dev->num_interfaces].proto = d[7];
                dev->num_interfaces++;
            }
        } else if (type == USB_DT_ENDPOINT && len >= 7) {
            uint8_t ep = d[2];
            dev->endpoint_type[ep] = d[3] & USB_ENDPOINT_XFERTYPE_MASK;
        }
        off += len;
    }
}

static int load_usb_devices(usbip_device_t *devices, int max_devices) {
    DIR *dir = opendir("/sys/bus/usb/devices");
    if (!dir) return -1;
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_devices) {
        if (ent->d_name[0] == '.') continue;
        if (strncmp(ent->d_name, "usb", 3) == 0) continue;
        if (!strchr(ent->d_name, '-')) continue;

        size_t name_len = strlen(ent->d_name);
        const char sysfs_prefix[] = "/sys/bus/usb/devices/";
        if (name_len >= sizeof(((usbip_device_t *)0)->sysfs_name) ||
            sizeof(sysfs_prefix) - 1 + name_len >= sizeof(((usbip_device_t *)0)->sysfs_path)) {
            continue;
        }

        usbip_device_t dev;
        memset(&dev, 0, sizeof(dev));
        memcpy(dev.sysfs_name, ent->d_name, name_len + 1);
        memcpy(dev.sysfs_path, sysfs_prefix, sizeof(sysfs_prefix) - 1);
        memcpy(dev.sysfs_path + sizeof(sysfs_prefix) - 1, ent->d_name, name_len + 1);

        char desc_path[320];
        snprintf(desc_path, sizeof(desc_path), "%s/descriptors", dev.sysfs_path);
        if (read_binary_file(desc_path, dev.descriptors, sizeof(dev.descriptors), &dev.descriptors_len) != 0) {
            continue;
        }
        uint32_t tmp = 0;
        if (read_attr_u32(dev.sysfs_path, "busnum", 10, &tmp) != 0) continue;
        dev.busnum = (int)tmp;
        if (read_attr_u32(dev.sysfs_path, "devnum", 10, &tmp) != 0) continue;
        dev.devnum = (int)tmp;

        char speed_text[64] = {0};
        char speed_path[320];
        snprintf(speed_path, sizeof(speed_path), "%s/speed", dev.sysfs_path);
        if (read_text(speed_path, speed_text, sizeof(speed_text)) == 0) {
            dev.speed = speed_from_text(speed_text);
        }

        parse_descriptors(&dev);
        if (dev.vid == 0 || dev.pid == 0) continue;
        snprintf(dev.devnode, sizeof(dev.devnode), "/dev/bus/usb/%03d/%03d", dev.busnum, dev.devnum);
        if (access(dev.devnode, R_OK | W_OK) != 0) {
            fprintf(stderr, "usbip skip busid=%s devnode=%s errno=%d\n", dev.sysfs_name, dev.devnode, errno);
            continue;
        }
        devices[count++] = dev;
    }
    closedir(dir);
    return count;
}

static void append_device(uint8_t **p, const usbip_device_t *dev, int include_interfaces) {
    append_fixed(p, dev->sysfs_path, 256);
    append_fixed(p, dev->sysfs_name, 32);
    append_be32(p, (uint32_t)dev->busnum);
    append_be32(p, (uint32_t)dev->devnum);
    append_be32(p, dev->speed);
    append_be16(p, dev->vid);
    append_be16(p, dev->pid);
    append_be16(p, dev->bcd_device);
    append_u8(p, dev->dev_class);
    append_u8(p, dev->dev_subclass);
    append_u8(p, dev->dev_protocol);
    append_u8(p, dev->config_value);
    append_u8(p, dev->num_configurations);
    append_u8(p, dev->num_interfaces);
    if (include_interfaces) {
        for (uint8_t i = 0; i < dev->num_interfaces; i++) {
            append_u8(p, dev->interfaces[i].cls);
            append_u8(p, dev->interfaces[i].subcls);
            append_u8(p, dev->interfaces[i].proto);
            append_u8(p, 0);
        }
    }
}

static int send_devlist(int client) {
    usbip_device_t devices[MAX_USB_DEVICES];
    int count = load_usb_devices(devices, MAX_USB_DEVICES);
    if (count < 0) count = 0;
    size_t cap = 12 + (size_t)count * (312 + MAX_INTERFACES * 4);
    uint8_t *buf = (uint8_t *)calloc(1, cap);
    if (!buf) return -1;
    uint8_t *p = buf;
    append_be16(&p, USBIP_VERSION);
    append_be16(&p, OP_REP_DEVLIST);
    append_be32(&p, 0);
    append_be32(&p, (uint32_t)count);
    for (int i = 0; i < count; i++) {
        append_device(&p, &devices[i], 1);
    }
    int rc = send_all(client, buf, (size_t)(p - buf));
    free(buf);
    fprintf(stderr, "usbip devlist devices=%d\n", count);
    return rc;
}

static int find_device_by_busid(const char *busid, usbip_device_t *out) {
    usbip_device_t devices[MAX_USB_DEVICES];
    int count = load_usb_devices(devices, MAX_USB_DEVICES);
    if (count < 0) return -1;
    for (int i = 0; i < count; i++) {
        if (strncmp(devices[i].sysfs_name, busid, sizeof(devices[i].sysfs_name)) == 0) {
            *out = devices[i];
            return 0;
        }
    }
    return -1;
}

static int send_import(int client, const usbip_device_t *dev, int ok) {
    uint8_t buf[12 + 312];
    memset(buf, 0, sizeof(buf));
    uint8_t *p = buf;
    append_be16(&p, USBIP_VERSION);
    append_be16(&p, OP_REP_IMPORT);
    append_be32(&p, ok ? 0 : 1);
    if (ok) append_device(&p, dev, 0);
    return send_all(client, buf, (size_t)(p - buf));
}

static void claim_interfaces(int fd, const usbip_device_t *dev) {
    for (uint8_t i = 0; i < dev->num_interfaces; i++) {
#ifdef USBDEVFS_DISCONNECT_CLAIM
        struct usbdevfs_disconnect_claim dc;
        memset(&dc, 0, sizeof(dc));
        dc.interface = i;
        dc.flags = USBDEVFS_DISCONNECT_CLAIM_EXCEPT_DRIVER;
        (void)ioctl(fd, USBDEVFS_DISCONNECT_CLAIM, &dc);
#endif
        int iface = i;
        if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) == 0) {
            fprintf(stderr, "usbip claimed interface=%u\n", i);
        }
    }
}

static int send_ret_submit(int client, uint32_t seqnum, int32_t status,
                           uint32_t actual_length, uint32_t start_frame,
                           uint32_t packets, const uint8_t *payload,
                           const uint32_t *iso_actual, const int32_t *iso_status,
                           const uint8_t *iso_descs) {
    size_t payload_len = payload ? actual_length : 0;
    size_t iso_len = (packets != USBIP_NON_ISO && packets < 4096) ? (size_t)packets * 16 : 0;
    size_t total = 48 + payload_len + iso_len;
    uint8_t *buf = (uint8_t *)calloc(1, total ? total : 1);
    if (!buf) return -1;
    uint8_t *p = buf;
    append_be32(&p, RET_SUBMIT);
    append_be32(&p, seqnum);
    append_be32(&p, 0);
    append_be32(&p, 0);
    append_be32(&p, 0);
    append_be32(&p, (uint32_t)status);
    append_be32(&p, status == 0 ? actual_length : 0);
    append_be32(&p, start_frame);
    append_be32(&p, packets);
    append_be32(&p, 0);
    append_be32(&p, 0);
    append_be32(&p, 0);
    if (payload_len) {
        memcpy(p, payload, payload_len);
        p += payload_len;
    }
    if (iso_len) {
        for (uint32_t i = 0; i < packets; i++) {
            const uint8_t *src = iso_descs + (size_t)i * 16;
            append_be32(&p, read_be32(src));
            append_be32(&p, read_be32(src + 4));
            append_be32(&p, iso_actual ? iso_actual[i] : 0);
            append_be32(&p, iso_status ? (uint32_t)iso_status[i] : (uint32_t)status);
        }
    }
    int rc = send_all(client, buf, (size_t)(p - buf));
    free(buf);
    return rc;
}

static int send_ret_unlink(int client, uint32_t seqnum, int32_t status) {
    uint8_t buf[48];
    memset(buf, 0, sizeof(buf));
    uint8_t *p = buf;
    append_be32(&p, RET_UNLINK);
    append_be32(&p, seqnum);
    append_be32(&p, 0);
    append_be32(&p, 0);
    append_be32(&p, 0);
    append_be32(&p, (uint32_t)status);
    for (int i = 0; i < 6; i++) append_be32(&p, 0);
    return send_all(client, buf, sizeof(buf));
}

static pending_urb_t *pending_find(pending_urb_t **head, struct usbdevfs_urb *urb) {
    pending_urb_t **pp = head;
    while (*pp) {
        if ((*pp)->urb == urb) {
            pending_urb_t *item = *pp;
            *pp = item->next;
            item->next = NULL;
            return item;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

static pending_urb_t *pending_find_seq(pending_urb_t **head, uint32_t seqnum) {
    pending_urb_t **pp = head;
    while (*pp) {
        if ((*pp)->seqnum == seqnum) return *pp;
        pp = &(*pp)->next;
    }
    return NULL;
}

static void pending_push(pending_urb_t **head, pending_urb_t *item) {
    item->next = *head;
    *head = item;
}

static void pending_free(pending_urb_t *item) {
    if (!item) return;
    free(item->urb);
    free(item->buffer);
    free(item);
}

static int endpoint_urb_type(const usbip_device_t *dev, uint8_t ep) {
    switch (dev->endpoint_type[ep]) {
        case USB_ENDPOINT_XFER_ISOC: return USBDEVFS_URB_TYPE_ISO;
        case USB_ENDPOINT_XFER_INT: return USBDEVFS_URB_TYPE_INTERRUPT;
        case USB_ENDPOINT_XFER_BULK:
        default: return USBDEVFS_URB_TYPE_BULK;
    }
}

static int handle_control(int client, int usbfd, uint32_t seqnum, uint32_t direction,
                          uint32_t transfer_length, const uint8_t setup[8],
                          const uint8_t *out_data) {
    if (transfer_length > MAX_TRANSFER) {
        return send_ret_submit(client, seqnum, -EINVAL, 0, 0, USBIP_NON_ISO, NULL, NULL, NULL, NULL);
    }
    uint8_t *buffer = NULL;
    if (transfer_length) {
        buffer = (uint8_t *)calloc(1, transfer_length);
        if (!buffer) return -1;
        if (direction == USBIP_DIR_OUT && out_data) memcpy(buffer, out_data, transfer_length);
    }
    struct usbdevfs_ctrltransfer ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.bRequestType = setup[0];
    ctrl.bRequest = setup[1];
    ctrl.wValue = read_le16(setup + 2);
    ctrl.wIndex = read_le16(setup + 4);
    ctrl.wLength = read_le16(setup + 6);
    ctrl.timeout = 5000;
    ctrl.data = buffer;
    int rc = ioctl(usbfd, USBDEVFS_CONTROL, &ctrl);
    int32_t status = rc < 0 ? -errno : 0;
    uint32_t actual = rc < 0 ? 0u : (uint32_t)rc;
    const uint8_t *payload = (direction == USBIP_DIR_IN && status == 0) ? buffer : NULL;
    int send_rc = send_ret_submit(client, seqnum, status, actual, 0, USBIP_NON_ISO, payload, NULL, NULL, NULL);
    free(buffer);
    return send_rc;
}

static int submit_data_urb(int usbfd, const usbip_device_t *dev, pending_urb_t **pending,
                           uint32_t seqnum, uint32_t direction, uint32_t ep,
                           uint32_t transfer_flags, uint32_t transfer_length,
                           uint32_t packets, const uint8_t *out_data,
                           const uint8_t *iso_descs) {
    if (transfer_length > MAX_TRANSFER || (packets != USBIP_NON_ISO && packets > 4096)) {
        return -EINVAL;
    }
    uint8_t ep_addr = (uint8_t)((ep & 0x0f) | (direction == USBIP_DIR_IN ? 0x80 : 0x00));
    size_t urb_size = sizeof(struct usbdevfs_urb);
    if (packets != USBIP_NON_ISO && packets > 0) {
        urb_size += ((size_t)packets - 1) * sizeof(struct usbdevfs_iso_packet_desc);
    }
    pending_urb_t *item = (pending_urb_t *)calloc(1, sizeof(*item));
    if (!item) return -ENOMEM;
    item->urb = (struct usbdevfs_urb *)calloc(1, urb_size);
    item->buffer = transfer_length ? (uint8_t *)calloc(1, transfer_length) : NULL;
    if (!item->urb || (transfer_length && !item->buffer)) {
        pending_free(item);
        return -ENOMEM;
    }
    if (direction == USBIP_DIR_OUT && out_data && transfer_length) {
        memcpy(item->buffer, out_data, transfer_length);
    }
    item->seqnum = seqnum;
    item->direction = direction;
    item->packets = packets;
    item->urb->type = endpoint_urb_type(dev, ep_addr);
    if (packets != USBIP_NON_ISO) item->urb->type = USBDEVFS_URB_TYPE_ISO;
    item->urb->endpoint = ep_addr;
    item->urb->status = 0;
    item->urb->flags = (int)transfer_flags;
    item->urb->buffer = item->buffer;
    item->urb->buffer_length = (int)transfer_length;
    item->urb->number_of_packets = packets == USBIP_NON_ISO ? 0 : (int)packets;
    item->urb->usercontext = item;
    if (packets != USBIP_NON_ISO && iso_descs) {
        for (uint32_t i = 0; i < packets; i++) {
            const uint8_t *src = iso_descs + (size_t)i * 16;
            item->urb->iso_frame_desc[i].length = read_be32(src + 4);
            item->urb->iso_frame_desc[i].actual_length = 0;
            item->urb->iso_frame_desc[i].status = 0;
        }
    }
    if (ioctl(usbfd, USBDEVFS_SUBMITURB, item->urb) < 0) {
        int err = -errno;
        pending_free(item);
        return err;
    }
    pending_push(pending, item);
    return 0;
}

static int reap_ready(int client, int usbfd, pending_urb_t **pending) {
    int completed = 0;
    for (;;) {
        struct usbdevfs_urb *urb = NULL;
        if (ioctl(usbfd, USBDEVFS_REAPURBNDELAY, &urb) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return completed;
            return -1;
        }
        pending_urb_t *item = pending_find(pending, urb);
        if (!item) continue;
        uint32_t actual = 0;
        uint32_t *iso_actual = NULL;
        int32_t *iso_status = NULL;
        uint8_t *iso_descs = NULL;
        if (item->packets != USBIP_NON_ISO && item->packets > 0) {
            iso_actual = (uint32_t *)calloc(item->packets, sizeof(uint32_t));
            iso_status = (int32_t *)calloc(item->packets, sizeof(int32_t));
            iso_descs = (uint8_t *)calloc((size_t)item->packets, 16);
            for (uint32_t i = 0; i < item->packets; i++) {
                uint32_t len = (uint32_t)item->urb->iso_frame_desc[i].length;
                uint32_t act = (uint32_t)item->urb->iso_frame_desc[i].actual_length;
                iso_actual[i] = act;
                iso_status[i] = item->urb->iso_frame_desc[i].status;
                actual += act;
                uint8_t *p = iso_descs + (size_t)i * 16;
                append_be32(&p, 0);
                append_be32(&p, len);
                append_be32(&p, 0);
                append_be32(&p, 0);
            }
        } else if (item->urb->actual_length > 0) {
            actual = (uint32_t)item->urb->actual_length;
        }
        const uint8_t *payload = (item->direction == USBIP_DIR_IN && item->urb->status == 0) ? item->buffer : NULL;
        if (send_ret_submit(client, item->seqnum, item->urb->status, actual, 0,
                            item->packets, payload, iso_actual, iso_status, iso_descs) != 0) {
            free(iso_actual);
            free(iso_status);
            free(iso_descs);
            pending_free(item);
            return -1;
        }
        free(iso_actual);
        free(iso_status);
        free(iso_descs);
        pending_free(item);
        completed++;
    }
}

static void cancel_pending(int usbfd, pending_urb_t **pending) {
    pending_urb_t *p = *pending;
    while (p) {
        (void)ioctl(usbfd, USBDEVFS_DISCARDURB, p->urb);
        p = p->next;
    }
    while (*pending) {
        struct usbdevfs_urb *urb = NULL;
        if (ioctl(usbfd, USBDEVFS_REAPURBNDELAY, &urb) < 0) break;
        pending_urb_t *item = pending_find(pending, urb);
        pending_free(item);
    }
    while (*pending) {
        pending_urb_t *next = (*pending)->next;
        pending_free(*pending);
        *pending = next;
    }
}

static void import_session(int client, const usbip_device_t *dev, int usbfd) {
    pending_urb_t *pending = NULL;
    uint64_t submit_count = 0;
    uint64_t complete_count = 0;
    time_t last_log = time(NULL);
    fprintf(stderr, "usbip import busid=%s devnode=%s vid=0x%04x pid=0x%04x\n",
            dev->sysfs_name, dev->devnode, dev->vid, dev->pid);

    while (g_running) {
        int completed = reap_ready(client, usbfd, &pending);
        if (completed < 0) break;
        complete_count += (uint64_t)completed;
        struct pollfd pfd;
        pfd.fd = client;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (pr > 0 && (pfd.revents & POLLIN)) {
            uint8_t header[48];
            if (recv_all(client, header, sizeof(header)) != 0) break;
            uint32_t command = read_be32(header);
            uint32_t seqnum = read_be32(header + 4);
            uint32_t direction = read_be32(header + 12);
            uint32_t ep = read_be32(header + 16);
            if (command == CMD_UNLINK) {
                uint32_t unlink_seq = read_be32(header + 20);
                pending_urb_t *item = pending_find_seq(&pending, unlink_seq);
                if (item) (void)ioctl(usbfd, USBDEVFS_DISCARDURB, item->urb);
                (void)send_ret_unlink(client, seqnum, 0);
                continue;
            }
            if (command != CMD_SUBMIT) break;
            uint32_t transfer_flags = read_be32(header + 20);
            uint32_t transfer_length = read_be32(header + 24);
            uint32_t packets = read_be32(header + 32);
            uint8_t setup[8];
            memcpy(setup, header + 40, sizeof(setup));
            if (transfer_length > MAX_TRANSFER || (packets != USBIP_NON_ISO && packets > 4096)) break;
            uint8_t *out_data = NULL;
            if (direction == USBIP_DIR_OUT && transfer_length > 0) {
                out_data = (uint8_t *)malloc(transfer_length);
                if (!out_data) break;
                if (recv_all(client, out_data, transfer_length) != 0) {
                    free(out_data);
                    break;
                }
            }
            uint8_t *iso_descs = NULL;
            if (packets != USBIP_NON_ISO) {
                iso_descs = (uint8_t *)malloc((size_t)packets * 16);
                if (!iso_descs) {
                    free(out_data);
                    break;
                }
                if (recv_all(client, iso_descs, (size_t)packets * 16) != 0) {
                    free(out_data);
                    free(iso_descs);
                    break;
                }
            }
            int is_control = (ep & 0x0f) == 0;
            if (is_control) {
                (void)handle_control(client, usbfd, seqnum, direction, transfer_length, setup, out_data);
            } else {
                int status = submit_data_urb(usbfd, dev, &pending, seqnum, direction, ep,
                                            transfer_flags, transfer_length, packets, out_data, iso_descs);
                if (status != 0) {
                    (void)send_ret_submit(client, seqnum, status, 0, 0, packets, NULL, NULL, NULL, iso_descs);
                } else {
                    submit_count++;
                }
            }
            free(out_data);
            free(iso_descs);
        }
        time_t now = time(NULL);
        if (now - last_log >= 5) {
            pending_urb_t *p = pending;
            int depth = 0;
            while (p) {
                depth++;
                p = p->next;
            }
            fprintf(stderr, "usbip session busid=%s submit=%llu complete=%llu pending=%d\n",
                    dev->sysfs_name,
                    (unsigned long long)submit_count,
                    (unsigned long long)complete_count,
                    depth);
            last_log = now;
        }
    }
    cancel_pending(usbfd, &pending);
    fprintf(stderr, "usbip import closed busid=%s\n", dev->sysfs_name);
}

static void *client_thread_main(void *arg) {
    int client = *(int *)arg;
    free(arg);
    int yes = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    uint8_t op[8];
    if (recv_all(client, op, sizeof(op)) != 0) {
        close(client);
        return NULL;
    }
    uint16_t version = read_be16(op);
    uint16_t code = read_be16(op + 2);
    if (version != USBIP_VERSION) {
        close(client);
        return NULL;
    }
    if (code == OP_REQ_DEVLIST) {
        (void)send_devlist(client);
        close(client);
        return NULL;
    }
    if (code == OP_REQ_IMPORT) {
        char busid[33];
        memset(busid, 0, sizeof(busid));
        if (recv_all(client, busid, 32) != 0) {
            close(client);
            return NULL;
        }
        usbip_device_t dev;
        memset(&dev, 0, sizeof(dev));
        if (find_device_by_busid(busid, &dev) != 0) {
            (void)send_import(client, &dev, 0);
            close(client);
            return NULL;
        }
        int usbfd = open(dev.devnode, O_RDWR | O_CLOEXEC);
        if (usbfd < 0) {
            fprintf(stderr, "usbip open failed busid=%s devnode=%s errno=%d\n", busid, dev.devnode, errno);
            (void)send_import(client, &dev, 0);
            close(client);
            return NULL;
        }
        claim_interfaces(usbfd, &dev);
        if (send_import(client, &dev, 1) == 0) {
            import_session(client, &dev, usbfd);
        }
        close(usbfd);
    }
    close(client);
    return NULL;
}

static int listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void print_devices(void) {
    usbip_device_t devices[MAX_USB_DEVICES];
    int count = load_usb_devices(devices, MAX_USB_DEVICES);
    if (count < 0) {
        fprintf(stderr, "no usb devices: errno=%d\n", errno);
        return;
    }
    for (int i = 0; i < count; i++) {
        fprintf(stdout, "%s  %04x:%04x  bus=%03d dev=%03d  %s\n",
                devices[i].sysfs_name,
                devices[i].vid,
                devices[i].pid,
                devices[i].busnum,
                devices[i].devnum,
                devices[i].devnode);
    }
}

int main(int argc, char **argv) {
    int port = USBIP_PORT;
    int list_only = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--list") == 0) {
            list_only = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [--port 3240] [--list]\n", argv[0]);
            return 0;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (list_only) {
        print_devices();
        return 0;
    }
    int server = listen_socket(port);
    if (server < 0) {
        fprintf(stderr, "usbip listen failed port=%d errno=%d\n", port, errno);
        return 2;
    }
    fprintf(stderr, "ctm usbip exporter listening on 0.0.0.0:%d\n", port);
    while (g_running) {
        struct pollfd pfd;
        pfd.fd = server;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 250);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        int client = accept(server, NULL, NULL);
        if (client < 0) continue;
        int *arg = (int *)malloc(sizeof(int));
        if (!arg) {
            close(client);
            continue;
        }
        *arg = client;
        pthread_t thread;
        if (pthread_create(&thread, NULL, client_thread_main, arg) == 0) {
            pthread_detach(thread);
        } else {
            close(client);
            free(arg);
        }
    }
    close(server);
    return 0;
}
