#define _GNU_SOURCE

#include "tv_bridge_worker.h"
#include "ctm_bridge_protocol.h"
#include "enet_transport.h"
#include "ctm_hid.h"
#include "ctm_transport.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/hidraw.h>
#include <linux/input.h>
#endif

#ifndef HIDIOCGRAWINFO
struct hidraw_devinfo {
    unsigned int bustype;
    short vendor;
    short product;
};
#define HIDIOCGRAWINFO _IOR('H', 0x03, struct hidraw_devinfo)
#endif

#ifndef HIDIOCGRAWNAME
#define HIDIOCGRAWNAME(len) _IOC(_IOC_READ, 'H', 0x04, len)
#endif

#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x07, len)
#endif

#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x06, len)
#endif

#ifndef HIDIOCGRDESCSIZE
#define HIDIOCGRDESCSIZE _IOR('H', 0x01, int)
#endif

#define MAX_REPORT_DESCRIPTOR 4096

#ifdef HIDRAW_MAX_DESCRIPTOR_SIZE
typedef struct hidraw_report_descriptor rd_t;
#else
struct rd_t_local {
    uint32_t size;
    unsigned char value[MAX_REPORT_DESCRIPTOR];
};
typedef struct rd_t_local rd_t;
#ifndef HIDIOCGRDESC
#define HIDIOCGRDESC _IOR('H', 0x02, rd_t)
#endif
#endif

#ifndef EVIOCGRAB
#define EVIOCGRAB _IOW('E', 0x90, int)
#endif

#define BUS_BLUETOOTH 0x05
#define MAX_REPORT 4096
#define PACED_QUEUE_CAP 32
#define MAX_EVDEV_GRABS 16

typedef struct {
    uint8_t data[MAX_REPORT];
    size_t len;
} queued_report_t;

typedef struct {
    int fd;
    char path[64];
} evdev_grab_t;

struct tv_bridge_worker {
    tv_bridge_worker_config_t cfg;
    pthread_t thread;
    pthread_t input_thread;
    int input_thread_started;
    volatile int stop;
    ctm_transport_t xport;
    ctm_enet_client_t *enet;     /* process-owned client; borrowed by xport */
    int hid_fd;
    int wake_pipe[2];
    pthread_mutex_t hid_mutex;
    pthread_mutex_t settings_mutex;
    tv_bridge_worker_settings_t settings;
    evdev_grab_t evdev_grabs[MAX_EVDEV_GRABS];
    int evdev_grab_count;
};

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* ENet is process-global. enet_initialize() is idempotent-safe to call once and
 * leave initialized for the process lifetime (no refcount), so we run it under
 * pthread_once -- multiple workers in this process share one initialization and
 * we never deinit (process exit reclaims it). g_enet_ready is set only when the
 * library actually came up; if not, workers run TCP-only. */
static pthread_once_t g_enet_once = PTHREAD_ONCE_INIT;
static int g_enet_ready = 0;

static void enet_global_init_once(void)
{
    if (enet_client_global_init() == 0) {
        g_enet_ready = 1;
    } else {
        fprintf(stderr, "bridge worker enet_initialize failed; ENet probe disabled, TCP only\n");
    }
}

static int send_msg(tv_bridge_worker_t *w, uint16_t type, uint32_t flags,
                    uint32_t request_id, const void *payload, size_t len)
{
    if (!w) return -1;
    return ctm_transport_send_msg(&w->xport, type, flags, request_id, payload, len);
}

/* Pop one received framed message over whichever transport is live. Returns 1
 * if a message was available (fills *h/*payload, caller frees *payload), 0 if
 * none right now (ENet only), -1 if the link dropped. */
static int worker_recv_msg(tv_bridge_worker_t *w, ctmb_header_t *h, uint8_t **payload)
{
    return ctm_transport_recv_msg(&w->xport, h, payload);
}

/* Continuous dual-probe (ENet brief, then TCP) until one connects or the worker
 * is stopped. Returns 0 on connect, -1 if stopped before connecting. */
static int worker_connect_dual_probe(tv_bridge_worker_t *w)
{
    while (!w->stop) {
        if (ctm_transport_connect_once(&w->xport, w->cfg.host, w->cfg.port, 400) == 0) {
            fprintf(stderr, "bridge worker connected via %s host=%s port=%d\n",
                    w->xport.kind == CTM_TRANSPORT_ENET ? "ENet/UDP" : "TCP",
                    w->cfg.host, w->cfg.port);
            return 0;
        }
        /* Neither answered; back off briefly and probe again. */
        for (int slept = 0; slept < 500 && !w->stop; slept += 50) {
            usleep(50000);
        }
    }
    return -1;
}

static int read_text_file(const char *path, char *out, size_t out_len)
{
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, out_len - 1);
    close(fd);
    if (n <= 0) return -1;
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' ' || out[n - 1] == '\t')) {
        out[--n] = '\0';
    }
    return 0;
}

static int hex_equals(const char *text, unsigned int value)
{
    if (!text || !text[0]) return 0;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 16);
    return end != text && parsed == value;
}

static void request_full_bt_mode(int fd)
{
    uint8_t feature[64];
    memset(feature, 0, sizeof(feature));
    feature[0] = 0x05;
    if (ioctl(fd, HIDIOCGFEATURE(sizeof(feature)), feature) < 0) {
        fprintf(stderr, "bridge worker feature 0x05 failed errno=%d\n", errno);
    }
}

/* hid_item_u32 / derive_report_lengths / read_report_descriptor live in
 * shared/ctm_hid.c (see ctm_hid.h). */

static int open_hid(tv_bridge_worker_t *w, ctmb_device_caps_t *caps,
                    uint8_t *report_desc, uint32_t *report_desc_len)
{
    int fd = open(w->cfg.path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;

    struct hidraw_devinfo info;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0) {
        close(fd);
        return -1;
    }
    unsigned int vid = (unsigned short)info.vendor;
    unsigned int pid = (unsigned short)info.product;
    if ((w->cfg.vid && vid != w->cfg.vid) || (w->cfg.pid && pid != w->cfg.pid)) {
        close(fd);
        return -1;
    }

    memset(caps, 0, sizeof(*caps));
    caps->vendor_id = (uint16_t)vid;
    caps->product_id = (uint16_t)pid;
    caps->bus = info.bustype ? (uint16_t)info.bustype : BUS_BLUETOOTH;
    caps->input_report_len = 1024;
    caps->output_report_len = 1024;
    caps->feature_report_len = 64;
    caps->flags = 1;
    snprintf(caps->path, sizeof(caps->path), "%s", w->cfg.path);
    snprintf(caps->serial, sizeof(caps->serial), "%s", w->cfg.bt_address);
    snprintf(caps->manufacturer, sizeof(caps->manufacturer), "hidraw");
    if (ioctl(fd, HIDIOCGRAWNAME(sizeof(caps->product) - 1), caps->product) < 0 ||
        caps->product[0] == '\0') {
        snprintf(caps->product, sizeof(caps->product), "hidraw");
    }

    *report_desc_len = read_report_descriptor(fd, report_desc, MAX_REPORT_DESCRIPTOR);
    if (*report_desc_len) {
        derive_report_lengths(report_desc, *report_desc_len, caps);
        if (caps->input_report_len < 1024) caps->input_report_len = 1024;
        if (caps->output_report_len < 1024) caps->output_report_len = 1024;
    }

    request_full_bt_mode(fd);
    return fd;
}

static void grab_matching_evdev(tv_bridge_worker_t *w)
{
    DIR *dir = opendir("/sys/class/input");
    if (!dir) {
        fprintf(stderr, "bridge worker evdev grab skipped errno=%d\n", errno);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;
        char vendor_path[160];
        char product_path[160];
        char vendor[32] = {0};
        char product[32] = {0};
        snprintf(vendor_path, sizeof(vendor_path), "/sys/class/input/%s/id/vendor", ent->d_name);
        snprintf(product_path, sizeof(product_path), "/sys/class/input/%s/id/product", ent->d_name);
        if (read_text_file(vendor_path, vendor, sizeof(vendor)) != 0 ||
            read_text_file(product_path, product, sizeof(product)) != 0 ||
            !hex_equals(vendor, w->cfg.vid) ||
            !hex_equals(product, w->cfg.pid)) {
            continue;
        }

        char input_dir[160];
        snprintf(input_dir, sizeof(input_dir), "/sys/class/input/%s", ent->d_name);
        DIR *input = opendir(input_dir);
        if (!input) continue;
        struct dirent *child;
        while ((child = readdir(input)) != NULL && w->evdev_grab_count < MAX_EVDEV_GRABS) {
            if (strncmp(child->d_name, "event", 5) != 0) continue;
            char dev_path[64];
            snprintf(dev_path, sizeof(dev_path), "/dev/input/%s", child->d_name);
            int fd = open(dev_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                int idx = w->evdev_grab_count++;
                w->evdev_grabs[idx].fd = fd;
                snprintf(w->evdev_grabs[idx].path, sizeof(w->evdev_grabs[idx].path), "%s", dev_path);
                fprintf(stderr, "bridge worker grabbed %s\n", dev_path);
            } else {
                close(fd);
            }
        }
        closedir(input);
    }
    closedir(dir);
}

static void release_evdev_grabs(tv_bridge_worker_t *w)
{
    for (int i = 0; i < w->evdev_grab_count; ++i) {
        if (w->evdev_grabs[i].fd >= 0) {
            ioctl(w->evdev_grabs[i].fd, EVIOCGRAB, 0);
            close(w->evdev_grabs[i].fd);
            w->evdev_grabs[i].fd = -1;
        }
    }
    w->evdev_grab_count = 0;
}

static int should_pace(const ctmb_host_config_t *cfg, const ctmb_header_t *h,
                       const uint8_t *payload, size_t len)
{
    if ((h->flags & CTMB_FLAG_PACED) != 0) return 1;
    if (!payload || len == 0) return 0;
    for (int i = 0; i < cfg->paced_report_count && i < 16; i++) {
        if (payload[0] == cfg->paced_report_ids[i]) return 1;
    }
    return 0;
}

static void queue_paced(queued_report_t *q, int *head, int *count,
                        const uint8_t *data, size_t len)
{
    if (len > MAX_REPORT) return;
    if (*count >= PACED_QUEUE_CAP) {
        *head = (*head + 1) % PACED_QUEUE_CAP;
        (*count)--;
    }
    int idx = (*head + *count) % PACED_QUEUE_CAP;
    memcpy(q[idx].data, data, len);
    q[idx].len = len;
    (*count)++;
}

static uint32_t crc32_step(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

static void sign_bt_output_report(uint8_t *data, size_t len)
{
    if (!data || len < 8) return;
    uint8_t seed = 0xa2;
    uint32_t crc = crc32_step(0xffffffffu, &seed, 1);
    crc = ~crc32_step(crc, data, len - 4);
    data[len - 4] = (uint8_t)(crc & 0xffu);
    data[len - 3] = (uint8_t)((crc >> 8) & 0xffu);
    data[len - 2] = (uint8_t)((crc >> 16) & 0xffu);
    data[len - 1] = (uint8_t)((crc >> 24) & 0xffu);
}

static tv_bridge_worker_settings_t copy_settings(tv_bridge_worker_t *w)
{
    tv_bridge_worker_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    if (!w) return settings;
    pthread_mutex_lock(&w->settings_mutex);
    settings = w->settings;
    pthread_mutex_unlock(&w->settings_mutex);
    return settings;
}

static uint8_t ds5_audio_block_for_mode(tv_bridge_audio_mode_t mode)
{
    switch (mode) {
        case TV_BRIDGE_AUDIO_SPEAKER: return 0x93;
        case TV_BRIDGE_AUDIO_HEADSET: return 0x96;
        case TV_BRIDGE_AUDIO_BOTH: return 0x95;
        case TV_BRIDGE_AUDIO_OFF:
        default: return 0x00;
    }
}

static int ds5_mode_uses_speaker(tv_bridge_audio_mode_t mode)
{
    return mode == TV_BRIDGE_AUDIO_SPEAKER || mode == TV_BRIDGE_AUDIO_BOTH;
}

static int ds5_mode_uses_headset(tv_bridge_audio_mode_t mode)
{
    return mode == TV_BRIDGE_AUDIO_HEADSET || mode == TV_BRIDGE_AUDIO_BOTH;
}

static int ds5_mode_uses_audio(tv_bridge_audio_mode_t mode)
{
    return ds5_mode_uses_speaker(mode) || ds5_mode_uses_headset(mode);
}

static double ds5_haptics_gain(unsigned int gain_centi)
{
    double gain = (double)gain_centi / 100.0;
    if (gain <= 0.0) return 0.0;
    if (gain >= 5.0) return 5.0;
    if (gain <= 1.0) return gain;
    return 1.0 + pow((gain - 1.0) / 4.0, 1.35) * 4.0;
}

static uint8_t ds5_volume_raw_byte(unsigned int value)
{
    return (uint8_t)(value > 0x64u ? 0x64u : value);
}

static int apply_ds5_settings(uint8_t *data, size_t *len_io,
                              const tv_bridge_worker_settings_t *settings)
{
    size_t len = len_io ? *len_io : 0;
    if (!data || !settings || len < 12 || (data[0] != 0x36 && data[0] != 0x32)) return 0;

    int patched = 0;
    size_t pos = 2;
    size_t limit = len - 4;

    /* AUTO: don't touch route, volume, audio-control bits, haptics gain.
     * Only patch the latency byte (0x91 timing block). The game (host) is
     * authority for everything else. */
    if (settings->audio_mode == TV_BRIDGE_AUDIO_AUTO) {
        uint8_t auto_latency = (uint8_t)settings->latency_ms;
        if (auto_latency < 20) auto_latency = 20;
        while (pos + 2 <= limit) {
            uint8_t block_id = data[pos];
            size_t payload_len = data[pos + 1];
            size_t block_len = payload_len + 2;
            if (block_id == 0 && payload_len == 0) break;
            if (block_len > limit - pos) break;
            if (block_id == 0x91 && payload_len >= 6) {
                for (size_t i = 3; i <= 7; ++i) {
                    if (data[pos + i] != auto_latency) {
                        data[pos + i] = auto_latency;
                        patched = 1;
                    }
                }
            }
            pos += block_len;
        }
        if (patched) sign_bt_output_report(data, len);
        return 0;
    }

    uint8_t audio_block = ds5_audio_block_for_mode(settings->audio_mode);
    uint8_t latency = (uint8_t)settings->latency_ms;
    uint8_t headset_volume = ds5_volume_raw_byte(settings->headset_volume_percent);
    uint8_t speaker_volume = ds5_volume_raw_byte(settings->speaker_volume_percent);
    uint8_t target_headset_volume = 0;
    uint8_t target_speaker_volume = 0;
    uint8_t target_audio_flags = 0;
    if (latency < 20) latency = 20;

    switch (settings->audio_mode) {
        case TV_BRIDGE_AUDIO_HEADSET:
            target_headset_volume = headset_volume;
            break;
        case TV_BRIDGE_AUDIO_SPEAKER:
            target_speaker_volume = speaker_volume;
            target_audio_flags = 0x30;
            break;
        case TV_BRIDGE_AUDIO_BOTH:
            target_headset_volume = headset_volume;
            target_speaker_volume = speaker_volume;
            target_audio_flags = 0x30;
            break;
        case TV_BRIDGE_AUDIO_OFF:
        default:
            break;
    }

    while (pos + 2 <= limit) {
        uint8_t block_id = data[pos];
        size_t payload_len = data[pos + 1];
        size_t block_len = payload_len + 2;
        if (block_id == 0 && payload_len == 0) break;
        if (block_len > limit - pos) break;

        if (block_id == 0x90 && payload_len >= 8) {
            /* Only patch confirmed audio fields; preserve effect/rumble state bytes. */
            if ((data[pos + 2] & 0xb0u) != 0xb0u) {
                data[pos + 2] = (uint8_t)(data[pos + 2] | 0xb0u);
                patched = 1;
            }
            if ((data[pos + 3] & 0x80u) != 0x80u) {
                data[pos + 3] = (uint8_t)(data[pos + 3] | 0x80u);
                patched = 1;
            }
            if (data[pos + 6] != target_headset_volume) {
                data[pos + 6] = target_headset_volume;
                patched = 1;
            }
            if (data[pos + 7] != target_speaker_volume) {
                data[pos + 7] = target_speaker_volume;
                patched = 1;
            }
            if (data[pos + 9] != target_audio_flags) {
                data[pos + 9] = target_audio_flags;
                patched = 1;
            }
        } else if ((block_id == 0x93 || block_id == 0x94 || block_id == 0x95 || block_id == 0x96) && audio_block != 0) {
            if (data[pos] != audio_block) {
                data[pos] = audio_block;
                patched = 1;
            }
        } else if (block_id == 0x91 && payload_len >= 6) {
            for (size_t i = 3; i <= 7; ++i) {
                if (data[pos + i] != latency) {
                    data[pos + i] = latency;
                    patched = 1;
                }
            }
        } else if (block_id == 0x92 && payload_len >= 2 && settings->haptics_gain_centi != 100) {
            double gain = ds5_haptics_gain(settings->haptics_gain_centi);
            for (size_t i = 2; i < block_len; ++i) {
                int sample = (int)(int8_t)data[pos + i];
                int scaled = (int)lrint((double)sample * gain);
                if (scaled < -128) scaled = -128;
                if (scaled > 127) scaled = 127;
                uint8_t value = (uint8_t)(int8_t)scaled;
                if (data[pos + i] != value) {
                    data[pos + i] = value;
                    patched = 1;
                }
            }
        }
        pos += block_len;
    }

    if (patched) sign_bt_output_report(data, len);
    return 0;
}

static uint8_t ds4_audio_target_for_mode(tv_bridge_audio_mode_t mode)
{
    switch (mode) {
        case TV_BRIDGE_AUDIO_SPEAKER: return 0x02;
        case TV_BRIDGE_AUDIO_HEADSET: return 0x24;
        case TV_BRIDGE_AUDIO_BOTH: return 0x26;
        case TV_BRIDGE_AUDIO_OFF:
        default: return 0x00;
    }
}

/* DS4 volume bytes are clamped 0..0x4F per the controller wiki
 * (https://controllers.fandom.com/wiki/Sony_DualShock_4). User testing
 * with a 0x7F cap confirmed the firmware ceiling at ~80% slider, which
 * maps to byte value 80 (= 0x50, just above 0x4F). Capping at 0x4F so
 * the slider's top corresponds to the actual max audible volume. */
static uint8_t ds4_volume_raw_byte(unsigned int value)
{
    return (uint8_t)(value > 0x4fu ? 0x4fu : value);
}

static int apply_ds4_settings(uint8_t *data, size_t len,
                              const tv_bridge_worker_settings_t *settings)
{
    if (!data || !settings || len < 84 || data[0] != 0x15) return 0;

    int patched = 0;
    uint8_t target = ds4_audio_target_for_mode(settings->audio_mode);
    uint8_t headset_volume = ds4_volume_raw_byte(settings->headset_volume_percent);
    uint8_t speaker_volume = ds4_volume_raw_byte(settings->speaker_volume_percent);

    /* AUTO mode = frame passes through unmodified. DS4 BT 0x15 has no
     * dedicated latency block we own, so AUTO is a pure no-touch path. */
    if (settings->audio_mode == TV_BRIDGE_AUDIO_AUTO) {
        return 0;
    }

    /* OFF mode = no patching at all. Frame passes through unmodified.
     * ctm-usbip emits BT[80]=0x00 by default; what the controller does
     * with that is a separate "figure out true off" task. */
    if (settings->audio_mode == TV_BRIDGE_AUDIO_OFF) {
        return 0;
    }

    /* BT[3] = BTSetStateData enable-update bitfield. Per the wiki:
     *   bit 4 (0x10) EnableVolumeLeftUpdate  (headphone L)
     *   bit 5 (0x20) EnableVolumeRightUpdate (headphone R)
     *   bit 7 (0x80) EnableVolumeSpeakerUpdate
     * DriftGuard ORs 0x30 | 0x80 = 0xB0 whenever it writes volumes; the
     * controller otherwise ignores the volume bytes. We assert the same
     * mask in any audible mode and clear it when route=off, preserving
     * the rumble/LED enable bits in the low nibble that the game owns. */
    const uint8_t kAudioEnableMask = 0xB0;
    int audible = (settings->audio_mode == TV_BRIDGE_AUDIO_SPEAKER ||
                   settings->audio_mode == TV_BRIDGE_AUDIO_HEADSET ||
                   settings->audio_mode == TV_BRIDGE_AUDIO_BOTH);
    uint8_t enable_byte = data[3];
    if (audible) enable_byte = (uint8_t)(enable_byte | kAudioEnableMask);
    else         enable_byte = (uint8_t)(enable_byte & (uint8_t)~kAudioEnableMask);
    if (data[3] != enable_byte) {
        data[3] = enable_byte;
        patched = 1;
    }

    if (data[2] != 0xa8) {
        data[2] = 0xa8;
        patched = 1;
    }
    if (data[80] != target) {
        data[80] = target;
        patched = 1;
    }
    if (data[21] != headset_volume) {
        data[21] = headset_volume;
        patched = 1;
    }
    if (data[22] != headset_volume) {
        data[22] = headset_volume;
        patched = 1;
    }
    if (data[24] != speaker_volume) {
        data[24] = speaker_volume;
        patched = 1;
    }
    /* BT[25] = UNK_AUDIO1:7 | UNK_AUDIO2:1 per wiki. Wiki notes
     * "appears to be set to 5 for audio" / "set to 1 for audio" =>
     * combined byte 0x85 = (1<<7) | 5. Untouched-zero packets keep
     * decoding cleanly in HEADSET mode but glitch when rumble fires;
     * setting 0x85 in audible modes is the hypothesis fix for the
     * audio-during-rumble breakage. Clear on off. */
    uint8_t unk_audio = (uint8_t)(audible ? 0x85 : 0x00);
    if (data[25] != unk_audio) {
        data[25] = unk_audio;
        patched = 1;
    }
    if (patched) sign_bt_output_report(data, len);
    return 0;
}

static int apply_output_settings(tv_bridge_worker_t *w, uint8_t *data, size_t *len_io)
{
    size_t len = len_io ? *len_io : 0;
    tv_bridge_worker_settings_t settings = copy_settings(w);
    if (settings.kind == TV_BRIDGE_KIND_DS5) {
        return apply_ds5_settings(data, len_io, &settings);
    }
    if (settings.kind == TV_BRIDGE_KIND_DS4) {
        return apply_ds4_settings(data, len, &settings);
    }
    return 0;
}

static int hid_write_report(tv_bridge_worker_t *w, const uint8_t *data, size_t len)
{
    if (!w || w->hid_fd < 0 || !data || len == 0) return -1;
    uint8_t patched[MAX_REPORT];
    if (len > sizeof(patched)) return -1;
    memcpy(patched, data, len);
    size_t patched_len = len;
    if (apply_output_settings(w, patched, &patched_len)) {
        return 0;
    }
    pthread_mutex_lock(&w->hid_mutex);
    ssize_t n = write(w->hid_fd, patched, patched_len);
    pthread_mutex_unlock(&w->hid_mutex);
    return n == (ssize_t)patched_len ? 0 : -1;
}

static void drain_paced(tv_bridge_worker_t *w, queued_report_t *q, int *head, int *count,
                        uint64_t *next_us, uint32_t pace_us)
{
    uint64_t now = now_us();
    if (*count <= 0) {
        *next_us = 0;
        return;
    }
    if (*next_us == 0) *next_us = now;
    while (*count > 0 && now >= *next_us) {
        queued_report_t *r = &q[*head];
        (void)hid_write_report(w, r->data, r->len);
        *head = (*head + 1) % PACED_QUEUE_CAP;
        (*count)--;
        if (pace_us == 0) pace_us = 10667;
        *next_us += pace_us;
        if (*next_us + pace_us < now) *next_us = now + pace_us;
        now = now_us();
    }
    if (*count <= 0) *next_us = 0;
}

static void *input_thread_main(void *arg)
{
    tv_bridge_worker_t *w = (tv_bridge_worker_t *)arg;
    while (!w->stop) {
        struct pollfd pfds[2];
        pfds[0].fd = w->hid_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = w->wake_pipe[0];
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;

        int pr = poll(pfds, 2, 1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if (pfds[1].revents & POLLIN) break;
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(pfds[0].revents & POLLIN)) continue;

        for (;;) {
            uint8_t buf[MAX_REPORT];
            ssize_t n = read(w->hid_fd, buf, sizeof(buf));
            if (n > 0) {
                if (send_msg(w, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, 0, buf, (size_t)n) != 0) {
                    w->stop = 1;
                    break;
                }
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            break;
        }
    }
    return NULL;
}

static int send_hello(tv_bridge_worker_t *w, const ctmb_device_caps_t *caps,
                      const uint8_t *report_desc, uint32_t report_desc_len)
{
    ctmb_hid_descriptor_info_t desc_info;
    memset(&desc_info, 0, sizeof(desc_info));
    desc_info.report_descriptor_len = report_desc_len;

    size_t hello_len = sizeof(*caps) + sizeof(desc_info) + report_desc_len;
    uint8_t *hello = (uint8_t *)malloc(hello_len);
    if (!hello) return -1;
    memcpy(hello, caps, sizeof(*caps));
    memcpy(hello + sizeof(*caps), &desc_info, sizeof(desc_info));
    if (report_desc_len) {
        memcpy(hello + sizeof(*caps) + sizeof(desc_info), report_desc, report_desc_len);
    }
    int rc = send_msg(w, CTMB_MSG_HELLO, CTMB_FLAG_OK, 0, hello, hello_len);
    free(hello);
    return rc;
}

/* Dispatch one received bridge message (output/feature/host-config). Shared by
 * the TCP and ENet paths so behaviour is identical regardless of transport. */
static void worker_handle_message(tv_bridge_worker_t *w, ctmb_host_config_t *host_cfg,
                                  queued_report_t *paced_q, int *paced_head, int *paced_count,
                                  const ctmb_header_t *h, uint8_t *payload)
{
    if (h->type == CTMB_MSG_OUTPUT_REPORT) {
        if (should_pace(host_cfg, h, payload, h->payload_len)) {
            queue_paced(paced_q, paced_head, paced_count, payload, h->payload_len);
        } else {
            (void)hid_write_report(w, payload, h->payload_len);
        }
    } else if (h->type == CTMB_MSG_FEATURE_GET) {
        int ok = 0;
        if (h->payload_len > 0 && h->payload_len <= MAX_REPORT) {
            uint8_t feature[MAX_REPORT];
            memcpy(feature, payload, h->payload_len);
            pthread_mutex_lock(&w->hid_mutex);
            int ioctl_rc = ioctl(w->hid_fd, HIDIOCGFEATURE(h->payload_len), feature);
            pthread_mutex_unlock(&w->hid_mutex);
            if (ioctl_rc >= 0) {
                ok = send_msg(w, CTMB_MSG_FEATURE_REPORT, CTMB_FLAG_OK,
                              h->request_id, feature, h->payload_len) == 0;
            }
        }
        if (!ok) {
            (void)send_msg(w, CTMB_MSG_FEATURE_REPORT, 0, h->request_id, NULL, 0);
        }
    } else if (h->type == CTMB_MSG_FEATURE_SET) {
        int ok = 0;
        if (h->payload_len > 0 && h->payload_len <= MAX_REPORT) {
            uint8_t feature[MAX_REPORT];
            memcpy(feature, payload, h->payload_len);
            pthread_mutex_lock(&w->hid_mutex);
            ok = ioctl(w->hid_fd, HIDIOCSFEATURE(h->payload_len), feature) >= 0;
            pthread_mutex_unlock(&w->hid_mutex);
        }
        (void)send_msg(w, CTMB_MSG_FEATURE_REPORT, ok ? CTMB_FLAG_OK : 0,
                       h->request_id, NULL, 0);
    } else if (h->type == CTMB_MSG_HOST_CONFIG && h->payload_len >= sizeof(*host_cfg)) {
        memcpy(host_cfg, payload, sizeof(*host_cfg));
        if (host_cfg->bt_pace_us == 0) host_cfg->bt_pace_us = 10667;
    }
}

/* Send HELLO and wait (bounded) for HOST_CONFIG over whichever transport is
 * live. For ENet the service pump must run to flush HELLO and decode the reply;
 * for TCP recv_msg blocks until the bytes arrive. Mirrors src/main.c. Returns 0
 * on success. */
static int worker_handshake(tv_bridge_worker_t *w, const ctmb_device_caps_t *caps,
                            const uint8_t *report_desc, uint32_t report_desc_len,
                            ctmb_host_config_t *host_cfg)
{
    if (send_hello(w, caps, report_desc, report_desc_len) != 0) {
        fprintf(stderr, "bridge worker HELLO failed\n");
        return -1;
    }

    ctmb_header_t h;
    uint8_t *payload = NULL;
    uint64_t start = now_us();
    for (;;) {
        if (w->stop) return -1;
        if (w->xport.kind == CTM_TRANSPORT_ENET) {
            if (ctm_transport_service(&w->xport, 50) < 0) {
                fprintf(stderr, "bridge worker host config wait: ENet link dropped\n");
                return -1;
            }
        }
        int got = worker_recv_msg(w, &h, &payload);
        if (got < 0) {
            fprintf(stderr, "bridge worker host config receive failed\n");
            return -1;
        }
        if (got == 0) {
            /* ENet only: nothing yet, bounded wait. */
            if (now_us() - start >= 5000000ull) {
                fprintf(stderr, "bridge worker host config timeout\n");
                return -1;
            }
            continue;
        }
        if (h.type != CTMB_MSG_HOST_CONFIG || h.payload_len < sizeof(*host_cfg)) {
            fprintf(stderr, "bridge worker host config unexpected type=%u len=%u\n",
                    h.type, h.payload_len);
            free(payload);
            return -1;
        }
        memcpy(host_cfg, payload, sizeof(*host_cfg));
        free(payload);
        break;
    }
    if (host_cfg->bt_pace_us == 0) host_cfg->bt_pace_us = 10667;
    return 0;
}

/* Run one connected session to completion: handshake, start the input thread,
 * then the output/feature receive loop plus paced HID drain, until the link
 * drops or w->stop. Returns with the input thread torn down. */
static void worker_run_session(tv_bridge_worker_t *w, const ctmb_device_caps_t *caps,
                               const uint8_t *report_desc, uint32_t report_desc_len)
{
    ctmb_host_config_t host_cfg;
    queued_report_t paced_q[PACED_QUEUE_CAP];
    int paced_head = 0;
    int paced_count = 0;
    uint64_t next_paced_us = 0;

    memset(&host_cfg, 0, sizeof(host_cfg));
    memset(paced_q, 0, sizeof(paced_q));

    if (worker_handshake(w, caps, report_desc, report_desc_len, &host_cfg) != 0) {
        return;
    }

    /* Drain the wake pipe so a stale wake byte from a previous session does not
     * immediately stop this session's input thread. */
    {
        uint8_t drain[64];
        while (read(w->wake_pipe[0], drain, sizeof(drain)) > 0) { /* discard */ }
    }

    w->input_thread_started = 0;
    if (pthread_create(&w->input_thread, NULL, input_thread_main, w) == 0) {
        w->input_thread_started = 1;
    } else {
        fprintf(stderr, "bridge worker input thread failed errno=%d\n", errno);
        return;
    }

    fprintf(stderr, "bridge worker active host=%s port=%d path=%s product=%s transport=%s\n",
            w->cfg.host, w->cfg.port, w->cfg.path, caps->product,
            w->xport.kind == CTM_TRANSPORT_ENET ? "ENet/UDP" : "TCP");

    int link_alive = 1;
    while (!w->stop && link_alive) {
        drain_paced(w, paced_q, &paced_head, &paced_count, &next_paced_us, host_cfg.bt_pace_us);

        int timeout_ms = 50;
        if (paced_count > 0 && next_paced_us != 0) {
            uint64_t now = now_us();
            timeout_ms = next_paced_us <= now ? 0 : (int)((next_paced_us - now) / 1000u);
            if (timeout_ms > 50) timeout_ms = 50;
        }

        if (w->xport.kind == CTM_TRANSPORT_ENET) {
            /* Service pumps acks/resends + queued outbound (input reports) and
             * decodes inbound packets, then we drain everything produced.
             * Use a tight 1 ms tick (NOT timeout_ms, which is up to 50 ms for
             * the TCP poll / output pacing): the input thread only QUEUES reports
             * to the ENet outbox because ENet isn't thread-safe, so a slow tick
             * batches 500 Hz input into ~50 ms bursts. Output pacing is still
             * honored via drain_paced() at the top of the loop each tick. */
            if (ctm_transport_service(&w->xport, 1) < 0) {
                fprintf(stderr, "bridge worker ENet link lost\n");
                link_alive = 0;
            } else {
                ctmb_header_t h;
                uint8_t *payload = NULL;
                while (worker_recv_msg(w, &h, &payload) == 1) {
                    worker_handle_message(w, &host_cfg, paced_q, &paced_head, &paced_count,
                                          &h, payload);
                    free(payload);
                    payload = NULL;
                }
            }
        } else {
            struct pollfd pfd;
            pfd.fd = w->xport.fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                link_alive = 0;
            } else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                link_alive = 0;
            } else if (pfd.revents & POLLIN) {
                ctmb_header_t h;
                uint8_t *payload = NULL;
                if (worker_recv_msg(w, &h, &payload) != 1) {
                    free(payload);
                    link_alive = 0;
                } else {
                    worker_handle_message(w, &host_cfg, paced_q, &paced_head, &paced_count,
                                          &h, payload);
                    free(payload);
                }
            }
        }
    }

    /* Tear down this session's input thread. */
    if (w->wake_pipe[1] >= 0) {
        (void)write(w->wake_pipe[1], "x", 1);
    }
    if (w->input_thread_started) {
        pthread_join(w->input_thread, NULL);
        w->input_thread_started = 0;
    }
}

static void *worker_main(void *arg)
{
    tv_bridge_worker_t *w = (tv_bridge_worker_t *)arg;
    ctmb_device_caps_t caps;
    uint8_t report_desc[MAX_REPORT_DESCRIPTOR];
    uint32_t report_desc_len = 0;

    w->hid_fd = open_hid(w, &caps, report_desc, &report_desc_len);
    if (w->hid_fd < 0) {
        fprintf(stderr, "bridge worker hid open failed path=%s errno=%d\n", w->cfg.path, errno);
        w->stop = 1;
        return NULL;
    }

    /* One wake pipe for the whole worker lifetime: tv_bridge_worker_stop() and
     * each session teardown write to it to break the input thread's poll(). */
    if (pipe(w->wake_pipe) != 0) {
        fprintf(stderr, "bridge worker wake pipe failed errno=%d\n", errno);
        w->stop = 1;
        return NULL;
    }
    (void)fcntl(w->wake_pipe[0], F_SETFL, fcntl(w->wake_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(w->wake_pipe[1], F_SETFL, fcntl(w->wake_pipe[1], F_GETFL, 0) | O_NONBLOCK);

    /* Continuous dual-probe loop: ENet first, then TCP, looping until one
     * connects; on disconnect release the evdev grabs and resume probing. We
     * never give up while !w->stop. evdev is (re)grabbed only while a session
     * is live so the pad drives the webOS UI when the bridge is down. */
    while (!w->stop) {
        if (worker_connect_dual_probe(w) != 0) {
            break; /* stopped before connecting */
        }

        grab_matching_evdev(w);
        worker_run_session(w, &caps, report_desc, report_desc_len);
        release_evdev_grabs(w);

        /* Session ended: drop the transport and (unless stopping) probe again. */
        ctm_transport_disconnect(&w->xport);
        if (!w->stop) {
            fprintf(stderr, "bridge worker link lost; evdev released, retrying probe loop\n");
        }
    }

    w->stop = 1;
    return NULL;
}

int tv_bridge_worker_start(tv_bridge_worker_t **out, const tv_bridge_worker_config_t *cfg)
{
    if (!out || !cfg || !cfg->host[0] || !cfg->path[0] || cfg->port <= 0) return -1;
    tv_bridge_worker_t *w = (tv_bridge_worker_t *)calloc(1, sizeof(*w));
    if (!w) return -1;
    w->cfg = *cfg;
    w->enet = NULL;
    w->hid_fd = -1;
    w->wake_pipe[0] = -1;
    w->wake_pipe[1] = -1;
    for (int i = 0; i < MAX_EVDEV_GRABS; ++i) w->evdev_grabs[i].fd = -1;
    pthread_mutex_init(&w->hid_mutex, NULL);
    pthread_mutex_init(&w->settings_mutex, NULL);
    w->settings = cfg->settings;

    /* Bring up ENet once per process, then give this worker its own client. If
     * either step fails we simply run TCP-only -- the probe loop guards on
     * w->enet being non-NULL. */
    pthread_once(&g_enet_once, enet_global_init_once);
    if (g_enet_ready) {
        w->enet = enet_client_create();
        if (!w->enet) {
            fprintf(stderr, "bridge worker enet client create failed; ENet probe disabled\n");
        }
    }
    ctm_transport_init(&w->xport, w->enet);

    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
        ctm_transport_destroy(&w->xport);
        pthread_mutex_destroy(&w->settings_mutex);
        pthread_mutex_destroy(&w->hid_mutex);
        free(w);
        return -1;
    }
    *out = w;
    return 0;
}

void tv_bridge_worker_update_settings(tv_bridge_worker_t *w,
                                      const tv_bridge_worker_settings_t *settings)
{
    if (!w || !settings) return;
    pthread_mutex_lock(&w->settings_mutex);
    w->settings = *settings;
    pthread_mutex_unlock(&w->settings_mutex);
}

void tv_bridge_worker_stop(tv_bridge_worker_t *w)
{
    if (!w) return;
    w->stop = 1;
    if (w->xport.fd >= 0) shutdown(w->xport.fd, SHUT_RDWR);
    if (w->wake_pipe[1] >= 0) {
        (void)write(w->wake_pipe[1], "x", 1);
    }
    pthread_join(w->thread, NULL);
    ctm_transport_disconnect(&w->xport);
    if (w->hid_fd >= 0) close(w->hid_fd);
    if (w->wake_pipe[0] >= 0) close(w->wake_pipe[0]);
    if (w->wake_pipe[1] >= 0) close(w->wake_pipe[1]);
    /* Worker thread has joined, so no one else touches the ENet client now. */
    if (w->enet) {
        enet_client_destroy(w->enet);
        w->enet = NULL;
    }
    release_evdev_grabs(w);
    ctm_transport_destroy(&w->xport);
    pthread_mutex_destroy(&w->settings_mutex);
    pthread_mutex_destroy(&w->hid_mutex);
    free(w);
}
