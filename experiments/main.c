#define _GNU_SOURCE

#include "ctm_bridge_protocol.h"
#include "enet_transport.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

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

#ifndef EVIOCGRAB
#define EVIOCGRAB _IOW('E', 0x90, int)
#endif

#define BUS_USB 0x03
#define BUS_BLUETOOTH 0x05
#define DEFAULT_HOST "192.168.0.200"
#define DEFAULT_PORT 48055
#define DEFAULT_VID 0x054c
#define DEFAULT_PID 0
#define DEFAULT_ANY_HID 0
#define DEFAULT_BT_ADDRESS "a0:fa:9c:26:ac:d4"
#define MAX_REPORT 4096
#define PACED_QUEUE_CAP 32
#define MAX_LUNA_ADDRS 8
#define MAX_EVDEV_GRABS 16
#define UI_W 960
#define UI_H 540
#define DEFAULT_AUDIO_LATENCY 0x60
#define STOP_SNIFF_PERIOD_US 5000000ull

typedef struct {
    char host[128];
    int port;
    char path[96];
    char bt_address[32];
    unsigned int vid;
    unsigned int pid;
    int any_hid;
} app_config_t;

typedef struct {
    int fd;
    int writable;
    int is_bluetooth;
    uint16_t vid;
    uint16_t pid;
    uint16_t version;
    uint16_t bus;
    char path[96];
    char serial[64];
    char product[64];
} hid_point_t;

typedef struct {
    uint8_t data[MAX_REPORT];
    size_t len;
    uint64_t enqueued_us;
} queued_report_t;

typedef struct {
    int fd;
    char path[64];
} evdev_grab_t;

typedef struct {
    char state[64];
    char host[128];
    int port;
    char hid_path[96];
    char product[64];
    char serial[64];
    int tcp_connected;
    int hid_open;
    int evdev_grabs;
    int paced_depth;
    unsigned long input_total;
    unsigned long input_poll_wakes_total;
    unsigned long input_reads_total;
    unsigned long input_duplicates_total;
    unsigned long input_gap_count_total;
    unsigned long output_total;
    unsigned long input_fps;
    unsigned long output_fps;
    unsigned long feature_gets;
    unsigned long feature_fails;
    unsigned long output_fails;
    unsigned long paced_drops;
    unsigned long latency_patches;
    unsigned long audio94_patches;
    unsigned int audio_latency;
    int audio94_override;
    double last_input_gap_ms;
    double input_gap_sum_ms_total;
    double input_profile_poll_hz;
    double input_profile_read_hz;
    double input_profile_sent_hz;
    double input_profile_dup_hz;
    double input_profile_gap_avg_ms;
    double input_profile_gap_max_ms;
    double input_gap_window_max_ms;
    uint64_t last_input_us;
    char message[160];
} bridge_status_t;

struct transport_s;
typedef struct {
    struct transport_s *transport;
    hid_point_t *hid;
    int wake_fd;
} input_thread_ctx_t;

typedef struct {
    char address[32];
} stop_sniff_thread_ctx_t;

static volatile sig_atomic_t g_running = 1;
static uint32_t g_send_sequence = 0;
static char g_stopped_addrs[MAX_LUNA_ADDRS][32];
static int g_stopped_count = 0;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_status_mutex = PTHREAD_MUTEX_INITIALIZER;
static bridge_status_t g_status;
static evdev_grab_t g_evdev_grabs[MAX_EVDEV_GRABS];
static int g_evdev_grab_count = 0;

static int valid_bt_address(const char *s);

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void log_line(const char *fmt, ...) {
    pthread_mutex_lock(&g_log_mutex);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&g_log_mutex);
}

static int parse_u32(const char *text, unsigned int *out) {
    if (!text || !*text || !out) return -1;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || value > 0xfffful) return -1;
    *out = (unsigned int)value;
    return 0;
}

static void load_config_file(app_config_t *cfg) {
    const char *paths[] = {
        "/media/developer/apps/usr/palm/applications/com.local.ctmbridge/test.conf",
        "/tmp/ctm-bridge-test.conf",
        NULL
    };
    char line[256];
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq++ = '\0';
            char *nl = strpbrk(eq, "\r\n");
            if (nl) *nl = '\0';
            if (strcmp(line, "host") == 0) snprintf(cfg->host, sizeof(cfg->host), "%s", eq);
            else if (strcmp(line, "port") == 0) cfg->port = atoi(eq);
            else if (strcmp(line, "path") == 0) snprintf(cfg->path, sizeof(cfg->path), "%s", eq);
            else if (strcmp(line, "bt_address") == 0) snprintf(cfg->bt_address, sizeof(cfg->bt_address), "%s", eq);
            else if (strcmp(line, "address") == 0) snprintf(cfg->bt_address, sizeof(cfg->bt_address), "%s", eq);
            else if (strcmp(line, "vid") == 0) parse_u32(eq, &cfg->vid);
            else if (strcmp(line, "pid") == 0) parse_u32(eq, &cfg->pid);
            else if (strcmp(line, "any_hid") == 0) {
                log_line("config any_hid ignored; Sony controller filtering stays enabled");
            }
        }
        fclose(f);
        return;
    }
}

static void parse_args(app_config_t *cfg, int argc, char **argv) {
    snprintf(cfg->host, sizeof(cfg->host), "%s", getenv("CTM_BRIDGE_HOST") ? getenv("CTM_BRIDGE_HOST") : DEFAULT_HOST);
    cfg->port = getenv("CTM_BRIDGE_PORT") ? atoi(getenv("CTM_BRIDGE_PORT")) : DEFAULT_PORT;
    cfg->vid = DEFAULT_VID;
    cfg->pid = DEFAULT_PID;
    cfg->any_hid = DEFAULT_ANY_HID;
    snprintf(cfg->bt_address, sizeof(cfg->bt_address), "%s", DEFAULT_BT_ADDRESS);
    if (getenv("CTM_BRIDGE_HID")) snprintf(cfg->path, sizeof(cfg->path), "%s", getenv("CTM_BRIDGE_HID"));
    if (getenv("CTM_BRIDGE_BT_ADDRESS")) snprintf(cfg->bt_address, sizeof(cfg->bt_address), "%s", getenv("CTM_BRIDGE_BT_ADDRESS"));
    if (getenv("CTM_BRIDGE_ANY_HID")) cfg->any_hid = atoi(getenv("CTM_BRIDGE_ANY_HID")) != 0;
    load_config_file(cfg);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) snprintf(cfg->host, sizeof(cfg->host), "%s", argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) cfg->port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) snprintf(cfg->path, sizeof(cfg->path), "%s", argv[++i]);
        else if (strcmp(argv[i], "--bt-address") == 0 && i + 1 < argc) snprintf(cfg->bt_address, sizeof(cfg->bt_address), "%s", argv[++i]);
        else if (strcmp(argv[i], "--vid") == 0 && i + 1 < argc) parse_u32(argv[++i], &cfg->vid);
        else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) parse_u32(argv[++i], &cfg->pid);
        else if (strcmp(argv[i], "--any-hid") == 0) cfg->any_hid = 1;
        else if (argv[i][0] != '-' && cfg->host[0] == '\0') snprintf(cfg->host, sizeof(cfg->host), "%s", argv[i]);
    }
    if (cfg->port <= 0 || cfg->port > 65535) cfg->port = DEFAULT_PORT;
    if (!valid_bt_address(cfg->bt_address)) cfg->bt_address[0] = '\0';
}

static int read_text_file(const char *path, char *buf, size_t len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return 0;
}

static void trim_line(char *s) {
    char *nl = s ? strpbrk(s, "\r\n") : NULL;
    if (nl) *nl = '\0';
}

static void status_set_state(const char *state, const char *message) {
    pthread_mutex_lock(&g_status_mutex);
    if (state) snprintf(g_status.state, sizeof(g_status.state), "%s", state);
    if (message) snprintf(g_status.message, sizeof(g_status.message), "%s", message);
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_set_config(const app_config_t *cfg) {
    pthread_mutex_lock(&g_status_mutex);
    snprintf(g_status.host, sizeof(g_status.host), "%s", cfg->host);
    g_status.port = cfg->port;
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_set_hid(const hid_point_t *hid) {
    pthread_mutex_lock(&g_status_mutex);
    g_status.hid_open = hid && hid->fd >= 0;
    if (hid) {
        snprintf(g_status.hid_path, sizeof(g_status.hid_path), "%s", hid->path);
        snprintf(g_status.product, sizeof(g_status.product), "%s", hid->product);
        snprintf(g_status.serial, sizeof(g_status.serial), "%s", hid->serial);
    }
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_set_tcp(int connected) {
    pthread_mutex_lock(&g_status_mutex);
    g_status.tcp_connected = connected;
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_note_input(double gap_ms) {
    pthread_mutex_lock(&g_status_mutex);
    g_status.input_total++;
    g_status.last_input_gap_ms = gap_ms;
    if (gap_ms > 0.0) {
        g_status.input_gap_sum_ms_total += gap_ms;
        g_status.input_gap_count_total++;
        if (gap_ms > g_status.input_gap_window_max_ms) {
            g_status.input_gap_window_max_ms = gap_ms;
        }
    }
    g_status.last_input_us = now_us();
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_note_input_poll_wake(void) {
    pthread_mutex_lock(&g_status_mutex);
    g_status.input_poll_wakes_total++;
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_note_input_read(void) {
    pthread_mutex_lock(&g_status_mutex);
    g_status.input_reads_total++;
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_note_input_duplicate(void) {
    pthread_mutex_lock(&g_status_mutex);
    g_status.input_duplicates_total++;
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_get_input_totals(unsigned long *sent_total,
                                    unsigned long *poll_total,
                                    unsigned long *read_total,
                                    unsigned long *dup_total,
                                    unsigned long *gap_count_total,
                                    double *gap_sum_ms_total) {
    pthread_mutex_lock(&g_status_mutex);
    if (sent_total) *sent_total = g_status.input_total;
    if (poll_total) *poll_total = g_status.input_poll_wakes_total;
    if (read_total) *read_total = g_status.input_reads_total;
    if (dup_total) *dup_total = g_status.input_duplicates_total;
    if (gap_count_total) *gap_count_total = g_status.input_gap_count_total;
    if (gap_sum_ms_total) *gap_sum_ms_total = g_status.input_gap_sum_ms_total;
    pthread_mutex_unlock(&g_status_mutex);
}

static double status_take_input_gap_window_max(void) {
    pthread_mutex_lock(&g_status_mutex);
    double value = g_status.input_gap_window_max_ms;
    g_status.input_gap_window_max_ms = 0.0;
    pthread_mutex_unlock(&g_status_mutex);
    return value;
}

static void status_set_counters(
    unsigned long output_total,
    unsigned long input_fps,
    unsigned long output_fps,
    int paced_depth,
    unsigned long feature_gets,
    unsigned long feature_fails,
    unsigned long output_fails,
    unsigned long paced_drops) {
    pthread_mutex_lock(&g_status_mutex);
    g_status.output_total = output_total;
    g_status.input_fps = input_fps;
    g_status.output_fps = output_fps;
    g_status.paced_depth = paced_depth;
    g_status.feature_gets = feature_gets;
    g_status.feature_fails = feature_fails;
    g_status.output_fails = output_fails;
    g_status.paced_drops = paced_drops;
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_set_input_profile(double poll_hz,
                                     double read_hz,
                                     double sent_hz,
                                     double dup_hz,
                                     double gap_avg_ms,
                                     double gap_max_ms) {
    pthread_mutex_lock(&g_status_mutex);
    g_status.input_profile_poll_hz = poll_hz;
    g_status.input_profile_read_hz = read_hz;
    g_status.input_profile_sent_hz = sent_hz;
    g_status.input_profile_dup_hz = dup_hz;
    g_status.input_profile_gap_avg_ms = gap_avg_ms;
    g_status.input_profile_gap_max_ms = gap_max_ms;
    pthread_mutex_unlock(&g_status_mutex);
}

static unsigned int audio_latency_get(void) {
    pthread_mutex_lock(&g_status_mutex);
    unsigned int value = g_status.audio_latency;
    pthread_mutex_unlock(&g_status_mutex);
    return value;
}

static void audio_latency_adjust(int delta) {
    pthread_mutex_lock(&g_status_mutex);
    int value = (int)g_status.audio_latency + delta;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    g_status.audio_latency = (unsigned int)value;
    snprintf(g_status.message, sizeof(g_status.message), "Audio latency 0x%02x", g_status.audio_latency);
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_note_latency_patch(void) {
    pthread_mutex_lock(&g_status_mutex);
    g_status.latency_patches++;
    pthread_mutex_unlock(&g_status_mutex);
}

static int audio94_override_get(void) {
    pthread_mutex_lock(&g_status_mutex);
    int value = g_status.audio94_override;
    pthread_mutex_unlock(&g_status_mutex);
    return value;
}

static void audio94_override_toggle(void) {
    pthread_mutex_lock(&g_status_mutex);
    g_status.audio94_override = !g_status.audio94_override;
    snprintf(g_status.message, sizeof(g_status.message),
             "0x95->0x94 %s", g_status.audio94_override ? "on" : "off");
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_note_audio94_patch(void) {
    pthread_mutex_lock(&g_status_mutex);
    g_status.audio94_patches++;
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_set_evdev_grabs(int count) {
    pthread_mutex_lock(&g_status_mutex);
    g_status.evdev_grabs = count;
    pthread_mutex_unlock(&g_status_mutex);
}

static void status_snapshot(bridge_status_t *out) {
    pthread_mutex_lock(&g_status_mutex);
    *out = g_status;
    pthread_mutex_unlock(&g_status_mutex);
}

static int valid_bt_address(const char *s) {
    if (!s || strlen(s) != 17) return 0;
    for (int i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (s[i] != ':') return 0;
        } else if (!((s[i] >= '0' && s[i] <= '9') ||
                     (s[i] >= 'a' && s[i] <= 'f') ||
                     (s[i] >= 'A' && s[i] <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

static int transport_from_sysfs(const char *hidraw_name) {
    char path[160];
    char uevent[512] = {0};
    snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", hidraw_name);
    if (read_text_file(path, uevent, sizeof(uevent)) != 0) return 0;
    char *hid_id = strstr(uevent, "HID_ID=");
    if (!hid_id) return 0;
    if (strncmp(hid_id + 7, "0003", 4) == 0) return BUS_USB;
    if (strncmp(hid_id + 7, "0005", 4) == 0) return BUS_BLUETOOTH;
    return 0;
}

static void address_from_sysfs(const char *hidraw_name, char *out, size_t out_len) {
    char path[160];
    char uevent[512] = {0};
    out[0] = '\0';
    snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", hidraw_name);
    if (read_text_file(path, uevent, sizeof(uevent)) != 0) return;
    char *uniq = strstr(uevent, "HID_UNIQ=");
    if (!uniq) return;
    uniq += 9;
    trim_line(uniq);
    if (valid_bt_address(uniq)) snprintf(out, out_len, "%s", uniq);
}

static int sony_controller_product(unsigned int product) {
    return product == 0x05c4 || product == 0x09cc ||
           product == 0x0ba0 || product == 0x0ce6 ||
           product == 0x0df2 || product == 0x0e5f;
}

static int sysfs_event_matches_sony_controller(const char *event_name) {
    char path[160];
    char text[512] = {0};
    snprintf(path, sizeof(path), "/sys/class/input/%s/device/uevent", event_name);
    if (read_text_file(path, text, sizeof(text)) != 0) return 0;
    if (!strstr(text, "054C") && !strstr(text, "054c")) return 0;
    return strstr(text, "05C4") || strstr(text, "05c4") ||
           strstr(text, "09CC") || strstr(text, "09cc") ||
           strstr(text, "0BA0") || strstr(text, "0ba0") ||
           strstr(text, "0CE6") || strstr(text, "0ce6") ||
           strstr(text, "0DF2") || strstr(text, "0df2") ||
           strstr(text, "0E5F") || strstr(text, "0e5f");
}

/* Grab every /dev/input/event* that belongs to the bridged controller (matched
 * by its vendor:product in the input MODALIAS) so the pad does not also drive
 * the webOS UI. Generic: keyed on the device we actually opened, not a Sony
 * allow-list, so it works for Xbox and anything else. */
static void grab_device_evdev(unsigned int vid, unsigned int pid) {
    if (g_evdev_grab_count > 0) return;
    if (vid == 0 || pid == 0) {
        log_line("evdev grab skipped: unknown vid/pid");
        status_set_evdev_grabs(0);
        return;
    }
    char want_u[20];
    char want_l[20];
    snprintf(want_u, sizeof(want_u), "v%04Xp%04X", vid, pid);
    snprintf(want_l, sizeof(want_l), "v%04xp%04x", vid, pid);

    DIR *dir = opendir("/dev/input");
    if (!dir) {
        log_line("evdev grab skipped: /dev/input unavailable errno=%d", errno);
        status_set_evdev_grabs(0);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;

        char uevent_path[160];
        char uevent[1024] = {0};
        snprintf(uevent_path, sizeof(uevent_path), "/sys/class/input/%s/device/uevent", ent->d_name);
        if (read_text_file(uevent_path, uevent, sizeof(uevent)) != 0) continue;
        if (!strstr(uevent, want_u) && !strstr(uevent, want_l)) continue;

        char name_path[160];
        char name[256] = {0};
        snprintf(name_path, sizeof(name_path), "/sys/class/input/%s/device/name", ent->d_name);
        (void)read_text_file(name_path, name, sizeof(name));
        trim_line(name);

        char dev_path[64];
        snprintf(dev_path, sizeof(dev_path), "/dev/input/%s", ent->d_name);
        int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            log_line("evdev grab open failed path=%s errno=%d", dev_path, errno);
            continue;
        }
        if (ioctl(fd, EVIOCGRAB, 1) == 0 && g_evdev_grab_count < MAX_EVDEV_GRABS) {
            snprintf(g_evdev_grabs[g_evdev_grab_count].path,
                     sizeof(g_evdev_grabs[g_evdev_grab_count].path),
                     "%s", dev_path);
            g_evdev_grabs[g_evdev_grab_count].fd = fd;
            g_evdev_grab_count++;
            log_line("evdev grabbed path=%s name=%s", dev_path, name[0] ? name : "-");
            continue;
        }
        log_line("evdev grab failed path=%s errno=%d", dev_path, errno);
        close(fd);
    }
    closedir(dir);
    status_set_evdev_grabs(g_evdev_grab_count);
}

static void release_evdev_grabs(void) {
    for (int i = 0; i < g_evdev_grab_count; i++) {
        if (g_evdev_grabs[i].fd >= 0) {
            ioctl(g_evdev_grabs[i].fd, EVIOCGRAB, 0);
            close(g_evdev_grabs[i].fd);
            log_line("evdev released path=%s", g_evdev_grabs[i].path);
            g_evdev_grabs[i].fd = -1;
        }
    }
    g_evdev_grab_count = 0;
    status_set_evdev_grabs(0);
}

static int append_output(char *out, size_t out_len, const char *text) {
    size_t used = strlen(out);
    if (used >= out_len - 1) return 0;
    snprintf(out + used, out_len - used, "%s", text);
    return 1;
}

static int run_command_capture(const char *cmd, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char buf[256];
    while (fgets(buf, sizeof(buf), fp)) {
        append_output(out, out_len, buf);
    }
    return pclose(fp);
}

static int quoted_value_from_key(const char *key_pos, char *out, size_t out_len) {
    const char *p = strchr(key_pos, ':');
    if (!p) return 0;
    p = strchr(p, '"');
    if (!p) return 0;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t n = (size_t)(end - p);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static int looks_like_connected_dualsense(const char *start) {
    char buf[1601];
    size_t len = strlen(start);
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';
    if (!strstr(buf, "\"typeOfDevice\": \"bredr\"") &&
        !strstr(buf, "\"typeOfDevice\":\"bredr\"")) {
        return 0;
    }
    if (!strstr(buf, "\"hid\"") && !strstr(buf, "\"HID\"")) {
        return 0;
    }
    return 1;
}

static int nearest_address_in_window(const char *window, size_t name_offset, char *out, size_t out_len) {
    const char *scan = window;
    size_t best_dist = (size_t)-1;
    char best[32] = {0};

    while ((scan = strstr(scan, "\"address\"")) != NULL) {
        char candidate[32];
        if (quoted_value_from_key(scan, candidate, sizeof(candidate)) &&
            valid_bt_address(candidate)) {
            size_t pos = (size_t)(scan - window);
            size_t dist = pos > name_offset ? pos - name_offset : name_offset - pos;
            if (dist < best_dist) {
                best_dist = dist;
                snprintf(best, sizeof(best), "%s", candidate);
            }
        }
        scan += 9;
    }

    if (!best[0]) return 0;
    snprintf(out, out_len, "%s", best);
    return 1;
}

static int dualsense_address_near_name(const char *payload, const char *name_pos,
                                       char *out, size_t out_len) {
    const char *start = name_pos;
    const char *end = name_pos;
    size_t back = 0;
    size_t forward = 0;
    char window[8193];

    while (start > payload && back++ < 4096) start--;
    while (*end && forward++ < 4096) end++;

    size_t len = (size_t)(end - start);
    if (len > sizeof(window) - 1) len = sizeof(window) - 1;
    memcpy(window, start, len);
    window[len] = '\0';

    if (!looks_like_connected_dualsense(window)) {
        log_line("hid device/getStatus DualSense window missing bredr/hid marker");
    }

    return nearest_address_in_window(window, (size_t)(name_pos - start), out, out_len);
}

static int address_already_stopped(const char *address) {
    for (int i = 0; i < g_stopped_count; i++) {
        if (strcmp(g_stopped_addrs[i], address) == 0) return 1;
    }
    return 0;
}

static void remember_stopped_address(const char *address) {
    if (address_already_stopped(address)) return;
    if (g_stopped_count >= MAX_LUNA_ADDRS) return;
    snprintf(g_stopped_addrs[g_stopped_count++], sizeof(g_stopped_addrs[0]), "%s", address);
}

static int stop_sniff_call(const char *address, int remember, int log_success) {
    if (!valid_bt_address(address)) return -1;
    char cmd[256];
    char output[1024];
    snprintf(cmd, sizeof(cmd),
             "luna-send-pub -n 1 -f "
             "luna://com.webos.service.bluetooth2/device/internal/stopSniff "
             "'{\"address\":\"%s\"}' 2>&1",
             address);
    int rc = run_command_capture(cmd, output, sizeof(output));
    if (rc == 0 && strstr(output, "\"returnValue\": true")) {
        if (remember) remember_stopped_address(address);
        if (log_success) {
            log_line("hid stopSniff address=%s rc=%d output=%.700s", address, rc, output);
        }
        return 0;
    }
    log_line("hid stopSniff issue address=%s rc=%d output=%.700s", address, rc, output);
    return -1;
}

static int stop_sniff(const char *address) {
    if (!valid_bt_address(address)) return -1;
    if (address_already_stopped(address)) return 0;
    return stop_sniff_call(address, 1, 1);
}

static int stop_sniff_periodic(const char *address) {
    return stop_sniff_call(address, 0, 0);
}

static void *stop_sniff_thread_main(void *arg) {
    stop_sniff_thread_ctx_t *ctx = (stop_sniff_thread_ctx_t *)arg;
    while (g_running) {
        (void)stop_sniff_periodic(ctx->address);
        for (uint64_t slept = 0; slept < STOP_SNIFF_PERIOD_US && g_running; slept += 100000ull) {
            usleep(100000);
        }
    }
    return NULL;
}

static int luna_get_dualsense_addresses(char addrs[][32], int max_addrs) {
    static char payload[65536];
    int count = 0;
    int rc = run_command_capture(
        "luna-send-pub -n 1 -f "
        "luna://com.webos.service.bluetooth2/device/getStatus '{}' 2>&1",
        payload, sizeof(payload));
    log_line("hid device/getStatus rc=%d payload=%.1000s", rc, payload);

    const char *p = payload;
    while (count < max_addrs) {
        const char *spaced = strstr(p, "\"name\": \"DualSense Wireless Controller\"");
        const char *compact = strstr(p, "\"name\":\"DualSense Wireless Controller\"");
        if (!spaced && !compact) break;
        if (!spaced) p = compact;
        else if (!compact) p = spaced;
        else p = spaced < compact ? spaced : compact;

        char address[32];
        if (!dualsense_address_near_name(payload, p, address, sizeof(address)) ||
            !valid_bt_address(address)) {
            p += 10;
            continue;
        }

        int dup = 0;
        for (int i = 0; i < count; i++) {
            if (strcmp(addrs[i], address) == 0) dup = 1;
        }
        if (!dup) {
            snprintf(addrs[count++], 32, "%s", address);
            log_line("hid device/getStatus DualSense address=%s", address);
        }
        p += 10;
    }
    log_line("hid device/getStatus DualSense addresses=%d", count);
    return count;
}

static void stop_sniff_for_luna_dualsense(void) {
    char addrs[MAX_LUNA_ADDRS][32];
    int count = luna_get_dualsense_addresses(addrs, MAX_LUNA_ADDRS);
    for (int i = 0; i < count; i++) {
        (void)stop_sniff(addrs[i]);
    }
}

static void request_full_bt_mode(int fd) {
    uint8_t feature[64];
    memset(feature, 0, sizeof(feature));
    feature[0] = 0x05;
    if (ioctl(fd, HIDIOCGFEATURE(sizeof(feature)), feature) < 0) {
        log_line("hid feature 0x05 request failed errno=%d", errno);
    } else {
        log_line("hid feature 0x05 requested");
    }
}

static int open_hid_path(const char *path, const char *hidraw_name, const app_config_t *cfg, hid_point_t *out) {
    struct hidraw_devinfo info;
    memset(&info, 0, sizeof(info));
    int fd = open(path, O_RDWR | O_NONBLOCK);
    int writable = fd >= 0;
    if (fd < 0) fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0) {
        close(fd);
        return -1;
    }

    unsigned int vid = (unsigned short)info.vendor;
    unsigned int pid = (unsigned short)info.product;
    int transport = transport_from_sysfs(hidraw_name);
    if (transport == 0) transport = (int)info.bustype;

    if (vid == 0x005d) {
        close(fd);
        return -1;
    }
    if (cfg->vid && vid != cfg->vid) {
        close(fd);
        return -1;
    }
    if (cfg->pid && pid != cfg->pid) {
        close(fd);
        return -1;
    }
    if (!cfg->any_hid && cfg->vid == 0 && vid != 0x054c) {
        close(fd);
        return -1;
    }
    if (!cfg->any_hid && cfg->pid == 0 && vid == 0x054c && !sony_controller_product(pid)) {
        close(fd);
        return -1;
    }
    if (transport != BUS_BLUETOOTH || !writable) {
        close(fd);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->fd = fd;
    out->writable = writable;
    out->is_bluetooth = transport == BUS_BLUETOOTH;
    out->vid = (uint16_t)vid;
    out->pid = (uint16_t)pid;
    out->version = 0;
    out->bus = (uint16_t)transport;
    snprintf(out->path, sizeof(out->path), "%s", path);
    address_from_sysfs(hidraw_name, out->serial, sizeof(out->serial));
    if (!out->serial[0] && valid_bt_address(cfg->bt_address)) {
        snprintf(out->serial, sizeof(out->serial), "%s", cfg->bt_address);
    }
    if (ioctl(fd, HIDIOCGRAWNAME(sizeof(out->product)), out->product) < 0 || out->product[0] == '\0') {
        snprintf(out->product, sizeof(out->product), "hidraw");
    }
    (void)stop_sniff(out->serial);
    request_full_bt_mode(fd);
    return 0;
}

static int open_hid(const app_config_t *cfg, hid_point_t *out) {
    if (valid_bt_address(cfg->bt_address)) {
        (void)stop_sniff(cfg->bt_address);
    }

    if (cfg->path[0]) {
        const char *name = strrchr(cfg->path, '/');
        name = name ? name + 1 : cfg->path;
        return open_hid_path(cfg->path, name, cfg, out);
    }

    stop_sniff_for_luna_dualsense();
    DIR *dir = opendir("/dev");
    if (!dir) return -1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0) continue;
        char path[96];
        snprintf(path, sizeof(path), "/dev/%s", ent->d_name);
        if (open_hid_path(path, ent->d_name, cfg, out) == 0) {
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return -1;
}

static int connect_tcp(const char *host, int port) {
    char port_text[16];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    snprintf(port_text, sizeof(port_text), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port_text, &hints, &result) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *rp = result; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    if (fd >= 0) {
        int yes = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    }
    return fd;
}

static int send_all(int fd, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *data, size_t len) {
    uint8_t *p = (uint8_t *)data;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int send_msg(int fd, uint16_t type, uint32_t flags, uint32_t request_id, const void *payload, size_t len) {
    ctmb_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = CTMB_MAGIC;
    h.version = CTMB_VERSION;
    h.type = type;
    h.flags = flags;
    h.timestamp_us = now_us();
    h.request_id = request_id;
    h.payload_len = (uint32_t)len;
    pthread_mutex_lock(&g_send_mutex);
    h.sequence = ++g_send_sequence;
    int rc = 0;
    if (send_all(fd, &h, sizeof(h)) != 0) rc = -1;
    else if (len && send_all(fd, payload, len) != 0) rc = -1;
    pthread_mutex_unlock(&g_send_mutex);
    return rc;
}

static int recv_msg(int fd, ctmb_header_t *h, uint8_t **payload) {
    *payload = NULL;
    if (recv_all(fd, h, sizeof(*h)) != 0) return -1;
    if (h->magic != CTMB_MAGIC || h->version != CTMB_VERSION || h->payload_len > CTMB_MAX_PAYLOAD) return -1;
    if (h->payload_len) {
        *payload = (uint8_t *)malloc(h->payload_len);
        if (!*payload) return -1;
        if (recv_all(fd, *payload, h->payload_len) != 0) {
            free(*payload);
            *payload = NULL;
            return -1;
        }
    }
    return 0;
}

/* --- Transport abstraction (additive) -----------------------------------
 *
 * The session loop and input thread used to talk to a raw TCP fd via
 * send_msg()/recv_msg()/poll(). To add the ENet/UDP option without touching
 * the proven TCP path, both now go through transport_t. TRANSPORT_TCP wraps the
 * unchanged fd helpers; TRANSPORT_ENET wraps the ctm_enet_client_t. The dual-
 * probe loop in main() decides which one a given session uses.
 */
typedef enum {
    TRANSPORT_TCP = 0,
    TRANSPORT_ENET = 1
} transport_kind_t;

typedef struct transport_s {
    transport_kind_t kind;
    int fd;                      /* TRANSPORT_TCP */
    ctm_enet_client_t *enet;     /* TRANSPORT_ENET */
} transport_t;

/* Thread-safe send of one framed message over whichever transport is active.
 * Matches send_msg()'s framing exactly. Returns 0 on success, -1 on failure. */
static int transport_send_msg(transport_t *t, uint16_t type, uint32_t flags,
                              uint32_t request_id, const void *payload, size_t len) {
    if (!t) return -1;
    if (t->kind == TRANSPORT_ENET) {
        return enet_client_send_msg(t->enet, type, flags, request_id, payload, len);
    }
    return send_msg(t->fd, type, flags, request_id, payload, len);
}

/* Pop one received message. Returns 1 if one was available (and fills *h /
 * *payload, caller frees *payload), 0 if none, -1 if the link dropped. For TCP
 * this performs a blocking recv of one whole message after poll() said the
 * socket was readable; for ENet it pops from the inbox the service pump filled. */
static int transport_recv_msg(transport_t *t, ctmb_header_t *h, uint8_t **payload) {
    if (!t) return -1;
    if (t->kind == TRANSPORT_ENET) {
        return enet_client_recv_msg(t->enet, h, payload);
    }
    *payload = NULL;
    if (recv_msg(t->fd, h, payload) != 0) {
        return -1;
    }
    return 1;
}

static int transport_connected(transport_t *t) {
    if (!t) return 0;
    if (t->kind == TRANSPORT_ENET) {
        return enet_client_connected(t->enet);
    }
    return t->fd >= 0;
}

static uint32_t crc32_step(uint32_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

static void sign_ds5_hid_output(uint8_t *data, size_t len) {
    if (!data || len < 8) return;
    uint8_t seed = 0xa2;
    uint32_t crc = crc32_step(0xffffffffu, &seed, 1);
    crc = ~crc32_step(crc, data, len - 4);
    data[len - 4] = (uint8_t)(crc & 0xffu);
    data[len - 3] = (uint8_t)((crc >> 8) & 0xffu);
    data[len - 2] = (uint8_t)((crc >> 16) & 0xffu);
    data[len - 1] = (uint8_t)((crc >> 24) & 0xffu);
}

static int apply_audio_latency_override(uint8_t *data, size_t len) {
    if (!data || len < 12 || data[0] != 0x36) return 0;
    size_t pos = 2;
    size_t limit = len - 4;
    while (pos + 2 <= limit) {
        uint8_t block_id = data[pos];
        size_t payload_len = data[pos + 1];
        size_t block_len = payload_len + 2;
        if (block_id == 0 && payload_len == 0) break;
        if (block_len > limit - pos) break;
        if (block_id == 0x91 && payload_len >= 6) {
            uint8_t latency = (uint8_t)audio_latency_get();
            for (size_t i = 3; i <= 7; i++) {
                data[pos + i] = latency;
            }
            sign_ds5_hid_output(data, len);
            status_note_latency_patch();
            return 1;
        }
        pos += block_len;
    }
    return 0;
}

static int apply_audio94_override(uint8_t *data, size_t len) {
    if (!audio94_override_get() || !data || len < 12 || data[0] != 0x36) return 0;
    size_t pos = 2;
    size_t limit = len - 4;
    int patched = 0;
    while (pos + 2 <= limit) {
        uint8_t block_id = data[pos];
        size_t payload_len = data[pos + 1];
        size_t block_len = payload_len + 2;
        if (block_id == 0 && payload_len == 0) break;
        if (block_len > limit - pos) break;
        if (block_id == 0x95) {
            data[pos] = 0x94;
            patched = 1;
        }
        pos += block_len;
    }
    if (patched) {
        sign_ds5_hid_output(data, len);
        status_note_audio94_patch();
    }
    return patched;
}

static void *input_thread_main(void *arg) {
    input_thread_ctx_t *ctx = (input_thread_ctx_t *)arg;
    uint64_t last_sent_us = 0;
    uint8_t last_report[MAX_REPORT];
    size_t last_report_len = 0;
    int have_last_report = 0;

    while (g_running) {
        struct pollfd pfds[2];
        nfds_t nfds = 1;
        pfds[0].fd = ctx->hid->fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        if (ctx->wake_fd >= 0) {
            pfds[1].fd = ctx->wake_fd;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;
            nfds = 2;
        }

        int pr = poll(pfds, nfds, 1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            log_line("input poll failed errno=%d", errno);
            g_running = 0;
            break;
        }
        if (pr == 0) continue;
        if (nfds > 1 && (pfds[1].revents & POLLIN)) break;
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            log_line("input hid poll issue revents=0x%x", pfds[0].revents);
            g_running = 0;
            break;
        }
        if (!(pfds[0].revents & POLLIN)) continue;
        status_note_input_poll_wake();

        for (;;) {
            uint8_t buf[MAX_REPORT];
            ssize_t n = read(ctx->hid->fd, buf, sizeof(buf));
            if (n > 0) {
                uint64_t now = now_us();
                status_note_input_read();

                if (have_last_report &&
                    last_report_len == (size_t)n &&
                    memcmp(last_report, buf, (size_t)n) == 0) {
                    status_note_input_duplicate();
                } else {
                    memcpy(last_report, buf, (size_t)n);
                    last_report_len = (size_t)n;
                    have_last_report = 1;
                }

                double gap_ms = 0.0;
                if (last_sent_us != 0) {
                    gap_ms = (double)(now - last_sent_us) / 1000.0;
                }
                if (transport_send_msg(ctx->transport, CTMB_MSG_INPUT_REPORT, CTMB_FLAG_OK, 0, buf, (size_t)n) != 0) {
                    log_line("input send failed");
                    g_running = 0;
                    break;
                }
                last_sent_us = now;
                status_note_input(gap_ms);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0) log_line("hid read failed errno=%d", errno);
            break;
        }
    }
    return NULL;
}

static int hid_write_report(hid_point_t *hid, uint8_t *data, size_t len) {
    if (!hid || hid->fd < 0 || !data || len == 0) return -1;
    (void)apply_audio_latency_override(data, len);
    (void)apply_audio94_override(data, len);
    ssize_t n = write(hid->fd, data, len);
    return n == (ssize_t)len ? 0 : -1;
}

static int should_pace(const ctmb_host_config_t *cfg, const ctmb_header_t *h, const uint8_t *payload, size_t len) {
    if ((h->flags & CTMB_FLAG_PACED) != 0) return 1;
    if (!payload || len == 0) return 0;
    for (int i = 0; i < cfg->paced_report_count && i < 16; i++) {
        if (payload[0] == cfg->paced_report_ids[i]) return 1;
    }
    return 0;
}

static void queue_paced(queued_report_t *q, int *head, int *count, const uint8_t *data, size_t len, unsigned long *drops) {
    if (len > MAX_REPORT) {
        (*drops)++;
        return;
    }
    if (*count >= PACED_QUEUE_CAP) {
        *head = (*head + 1) % PACED_QUEUE_CAP;
        (*count)--;
        (*drops)++;
    }
    int idx = (*head + *count) % PACED_QUEUE_CAP;
    memcpy(q[idx].data, data, len);
    q[idx].len = len;
    q[idx].enqueued_us = now_us();
    (*count)++;
}

static void drain_paced(hid_point_t *hid, queued_report_t *q, int *head, int *count,
                        uint64_t *next_us, uint32_t pace_us, unsigned long *writes, unsigned long *fails) {
    uint64_t now = now_us();
    if (*count <= 0) {
        *next_us = 0;
        return;
    }
    if (*next_us == 0) *next_us = now;
    while (*count > 0 && now >= *next_us) {
        queued_report_t *r = &q[*head];
        if (hid_write_report(hid, r->data, r->len) == 0) (*writes)++;
        else (*fails)++;
        *head = (*head + 1) % PACED_QUEUE_CAP;
        (*count)--;
        if (pace_us == 0) pace_us = 10667;
        *next_us += pace_us;
        if (*next_us + pace_us < now) *next_us = now + pace_us;
        now = now_us();
    }
    if (*count <= 0) *next_us = 0;
}

/* --- Session driver (transport-agnostic) --------------------------------
 *
 * One bridge session: handshake (HELLO -> HOST_CONFIG), then the output/feature
 * receive loop plus paced HID drain, running until the link drops or g_running
 * clears. Works identically over TCP and ENet via transport_t. The TCP path is
 * unchanged from the original inline loop; ENet is the additive alternative.
 */
typedef struct {
    transport_t *t;
    hid_point_t *hid;
    ctmb_host_config_t *host_cfg;
    queued_report_t *paced_q;
    int *paced_head;
    int *paced_count;
    uint64_t *next_paced_us;
    unsigned long *output_writes;
    unsigned long *output_fails;
    unsigned long *feature_gets;
    unsigned long *feature_fails;
    unsigned long *paced_drops;
} session_ctx_t;

/* Process one received bridge message. Mirrors the original inline handling. */
static void handle_bridge_message(session_ctx_t *s, ctmb_header_t *h, uint8_t *payload) {
    if (h->type == CTMB_MSG_OUTPUT_REPORT) {
        if (should_pace(s->host_cfg, h, payload, h->payload_len)) {
            queue_paced(s->paced_q, s->paced_head, s->paced_count, payload, h->payload_len, s->paced_drops);
        } else if (hid_write_report(s->hid, payload, h->payload_len) == 0) {
            (*s->output_writes)++;
        } else {
            (*s->output_fails)++;
            log_line("hid output write failed errno=%d report=0x%02x len=%u",
                     errno, h->payload_len ? payload[0] : 0, h->payload_len);
        }
    } else if (h->type == CTMB_MSG_FEATURE_GET) {
        (*s->feature_gets)++;
        int ok = 0;
        if (h->payload_len > 0 && h->payload_len <= MAX_REPORT) {
            uint8_t feature[MAX_REPORT];
            memcpy(feature, payload, h->payload_len);
            if (ioctl(s->hid->fd, HIDIOCGFEATURE(h->payload_len), feature) >= 0) {
                ok = 1;
                if (transport_send_msg(s->t, CTMB_MSG_FEATURE_REPORT, CTMB_FLAG_OK, h->request_id,
                                       feature, h->payload_len) != 0) {
                    g_running = 0;
                }
            }
        }
        if (!ok) {
            (*s->feature_fails)++;
            log_line("feature get failed report=0x%02x len=%u errno=%d",
                     h->payload_len ? payload[0] : 0, h->payload_len, errno);
            if (transport_send_msg(s->t, CTMB_MSG_FEATURE_REPORT, 0, h->request_id, NULL, 0) != 0) {
                g_running = 0;
            }
        }
    } else if (h->type == CTMB_MSG_FEATURE_SET) {
        (*s->feature_gets)++;
        int ok = 0;
        if (h->payload_len > 0 && h->payload_len <= MAX_REPORT) {
            uint8_t feature[MAX_REPORT];
            memcpy(feature, payload, h->payload_len);
            if (ioctl(s->hid->fd, HIDIOCSFEATURE(h->payload_len), feature) >= 0) {
                ok = 1;
                if (transport_send_msg(s->t, CTMB_MSG_FEATURE_REPORT, CTMB_FLAG_OK, h->request_id,
                                       NULL, 0) != 0) {
                    g_running = 0;
                }
            }
        }
        if (!ok) {
            (*s->feature_fails)++;
            log_line("feature set failed report=0x%02x len=%u errno=%d",
                     h->payload_len ? payload[0] : 0, h->payload_len, errno);
            if (transport_send_msg(s->t, CTMB_MSG_FEATURE_REPORT, 0, h->request_id, NULL, 0) != 0) {
                g_running = 0;
            }
        }
    } else if (h->type == CTMB_MSG_HOST_CONFIG && h->payload_len >= sizeof(*s->host_cfg)) {
        memcpy(s->host_cfg, payload, sizeof(*s->host_cfg));
    } else {
        log_line("unexpected message type=%u len=%u", h->type, h->payload_len);
    }
}

/* Send HELLO and wait for HOST_CONFIG over either transport. Returns 0 on
 * success. */
static int session_handshake(transport_t *t, const hid_point_t *hid, ctmb_host_config_t *host_cfg) {
    ctmb_device_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.vendor_id = hid->vid;
    caps.product_id = hid->pid;
    caps.version = hid->version;
    caps.bus = hid->bus;
    caps.input_report_len = 1024;
    caps.output_report_len = 1024;
    caps.feature_report_len = 64;
    caps.flags = hid->is_bluetooth ? 1 : 0;
    snprintf(caps.path, sizeof(caps.path), "%s", hid->path);
    snprintf(caps.serial, sizeof(caps.serial), "%s", hid->serial);
    snprintf(caps.product, sizeof(caps.product), "%s", hid->product);
    snprintf(caps.manufacturer, sizeof(caps.manufacturer), "hidraw");
    if (transport_send_msg(t, CTMB_MSG_HELLO, CTMB_FLAG_OK, 0, &caps, sizeof(caps)) != 0) {
        log_line("hello send failed");
        return -1;
    }

    /* Wait (bounded) for HOST_CONFIG. For ENet the service pump must run to
     * deliver it; for TCP recv_msg blocks until the bytes arrive. */
    ctmb_header_t h;
    uint8_t *payload = NULL;
    uint64_t start = now_us();
    for (;;) {
        if (!g_running) return -1;
        if (t->kind == TRANSPORT_ENET) {
            if (enet_client_service(t->enet, 50) < 0) {
                log_line("enet host config wait: link dropped");
                return -1;
            }
        }
        int got = transport_recv_msg(t, &h, &payload);
        if (got < 0) {
            log_line("host config receive failed");
            return -1;
        }
        if (got == 0) {
            if (t->kind == TRANSPORT_TCP) {
                /* recv_msg only returns 0/-1 for TCP via transport_recv_msg's
                 * mapping, so got==0 cannot happen there; guard anyway. */
                return -1;
            }
            if (now_us() - start >= 5000000ull) {
                log_line("host config receive timeout");
                return -1;
            }
            continue;
        }
        if (h.type != CTMB_MSG_HOST_CONFIG || h.payload_len < sizeof(*host_cfg)) {
            log_line("host config unexpected type=%u len=%u", h.type, h.payload_len);
            free(payload);
            return -1;
        }
        memcpy(host_cfg, payload, sizeof(*host_cfg));
        free(payload);
        break;
    }
    if (host_cfg->bt_pace_us == 0) host_cfg->bt_pace_us = 10667;
    log_line("host config pace_us=%u paced_count=%u", host_cfg->bt_pace_us, host_cfg->paced_report_count);
    return 0;
}

/* Run one connected session to completion. Starts/stops its own input thread.
 * Returns when the link drops or g_running clears. Cumulative counters are
 * carried in via pointers so the periodic summary spans reconnects. */
static void run_bridge_session(transport_t *t, hid_point_t *hid,
                               unsigned long *output_writes, unsigned long *output_fails,
                               unsigned long *feature_gets, unsigned long *feature_fails,
                               unsigned long *paced_drops,
                               unsigned long *last_input, unsigned long *last_input_poll,
                               unsigned long *last_input_read, unsigned long *last_input_dup,
                               unsigned long *last_input_gap_count, unsigned long *last_output,
                               double *last_input_gap_sum_ms, uint64_t *last_status_us) {
    ctmb_host_config_t host_cfg;
    queued_report_t paced_q[PACED_QUEUE_CAP];
    int paced_head = 0;
    int paced_count = 0;
    uint64_t next_paced_us = 0;
    int input_wake_pipe[2] = {-1, -1};
    pthread_t input_thread;
    int input_thread_started = 0;

    memset(&host_cfg, 0, sizeof(host_cfg));

    status_set_state("connected", "Waiting for host config");
    if (session_handshake(t, hid, &host_cfg) != 0) {
        status_set_state("disconnected", "Handshake failed");
        return;
    }
    status_set_state("running", "Bridge active");

    if (pipe(input_wake_pipe) != 0) {
        log_line("input wake pipe failed errno=%d", errno);
        status_set_state("disconnected", "Input wake pipe failed");
        return;
    }
    (void)fcntl(input_wake_pipe[0], F_SETFL, fcntl(input_wake_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(input_wake_pipe[1], F_SETFL, fcntl(input_wake_pipe[1], F_GETFL, 0) | O_NONBLOCK);

    input_thread_ctx_t input_ctx;
    input_ctx.transport = t;
    input_ctx.hid = hid;
    input_ctx.wake_fd = input_wake_pipe[0];
    if (pthread_create(&input_thread, NULL, input_thread_main, &input_ctx) == 0) {
        input_thread_started = 1;
    } else {
        log_line("input thread start failed errno=%d", errno);
        close(input_wake_pipe[0]);
        close(input_wake_pipe[1]);
        status_set_state("disconnected", "Input thread start failed");
        return;
    }

    session_ctx_t s;
    s.t = t;
    s.hid = hid;
    s.host_cfg = &host_cfg;
    s.paced_q = paced_q;
    s.paced_head = &paced_head;
    s.paced_count = &paced_count;
    s.next_paced_us = &next_paced_us;
    s.output_writes = output_writes;
    s.output_fails = output_fails;
    s.feature_gets = feature_gets;
    s.feature_fails = feature_fails;
    s.paced_drops = paced_drops;

    int link_alive = 1;
    while (g_running && link_alive) {
        drain_paced(hid, paced_q, &paced_head, &paced_count, &next_paced_us,
                    host_cfg.bt_pace_us, output_writes, output_fails);

        int timeout_ms = 50;
        if (paced_count > 0 && next_paced_us != 0) {
            uint64_t now = now_us();
            timeout_ms = next_paced_us <= now ? 0 : (int)((next_paced_us - now) / 1000u);
            if (timeout_ms > 50) timeout_ms = 50;
        }

        if (t->kind == TRANSPORT_ENET) {
            /* Service pumps acks/resends and decodes inbound packets, then we
             * drain every message it produced this tick. */
            if (enet_client_service(t->enet, (unsigned int)timeout_ms) < 0) {
                status_set_state("disconnected", "ENet link lost");
                log_line("enet link lost");
                link_alive = 0;
            } else {
                ctmb_header_t h;
                uint8_t *payload = NULL;
                while (transport_recv_msg(t, &h, &payload) == 1) {
                    handle_bridge_message(&s, &h, payload);
                    free(payload);
                    payload = NULL;
                }
            }
        } else {
            struct pollfd pfd;
            pfd.fd = t->fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                log_line("poll failed errno=%d", errno);
                link_alive = 0;
            } else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                log_line("tcp poll issue revents=0x%x", pfd.revents);
                status_set_state("disconnected", "TCP link lost");
                link_alive = 0;
            } else if (pfd.revents & POLLIN) {
                ctmb_header_t h;
                uint8_t *payload = NULL;
                int got = transport_recv_msg(t, &h, &payload);
                if (got != 1) {
                    status_set_state("disconnected", "TCP receive failed");
                    log_line("tcp receive failed");
                    free(payload);
                    link_alive = 0;
                } else {
                    handle_bridge_message(&s, &h, payload);
                    free(payload);
                }
            }
        }

        uint64_t now = now_us();
        if (now - *last_status_us >= 5000000ull) {
            double window_s = (double)(now - *last_status_us) / 1000000.0;
            if (window_s <= 0.0) window_s = 0.001;
            unsigned long input_now = 0, input_poll_now = 0, input_read_now = 0, input_dup_now = 0;
            unsigned long input_gap_count_now = 0;
            double input_gap_sum_now = 0.0;
            status_get_input_totals(&input_now,
                                    &input_poll_now,
                                    &input_read_now,
                                    &input_dup_now,
                                    &input_gap_count_now,
                                    &input_gap_sum_now);
            unsigned long input_delta = input_now - *last_input;
            unsigned long input_poll_delta = input_poll_now - *last_input_poll;
            unsigned long input_read_delta = input_read_now - *last_input_read;
            unsigned long input_dup_delta = input_dup_now - *last_input_dup;
            unsigned long input_gap_count_delta = input_gap_count_now - *last_input_gap_count;
            double input_gap_sum_delta = input_gap_sum_now - *last_input_gap_sum_ms;
            unsigned long output_delta = *output_writes - *last_output;
            double input_hz = (double)input_delta / window_s;
            double input_poll_hz = (double)input_poll_delta / window_s;
            double input_read_hz = (double)input_read_delta / window_s;
            double input_dup_hz = (double)input_dup_delta / window_s;
            double input_gap_avg_ms = input_gap_count_delta > 0 ?
                input_gap_sum_delta / (double)input_gap_count_delta : 0.0;
            double input_gap_max_ms = status_take_input_gap_window_max();
            double output_hz = (double)output_delta / window_s;
            log_line("summary window_s=%.3f input_hz=%.3f output_hz=%.3f paced_depth=%d feature_gets=%lu feature_fails=%lu output_fails=%lu paced_drops=%lu",
                     window_s,
                     input_hz,
                     output_hz,
                     paced_count,
                     *feature_gets,
                     *feature_fails,
                     *output_fails,
                     *paced_drops);
            status_set_counters(*output_writes,
                                (unsigned long)(input_hz + 0.5),
                                (unsigned long)(output_hz + 0.5),
                                paced_count,
                                *feature_gets,
                                *feature_fails,
                                *output_fails,
                                *paced_drops);
            status_set_input_profile(input_poll_hz,
                                     input_read_hz,
                                     input_hz,
                                     input_dup_hz,
                                     input_gap_avg_ms,
                                     input_gap_max_ms);
            *last_input = input_now;
            *last_input_poll = input_poll_now;
            *last_input_read = input_read_now;
            *last_input_dup = input_dup_now;
            *last_input_gap_count = input_gap_count_now;
            *last_input_gap_sum_ms = input_gap_sum_now;
            *last_output = *output_writes;
            *last_status_us = now;
        }
    }

    /* Tear down the input thread for this session. */
    if (input_wake_pipe[1] >= 0) {
        uint8_t wake = 1;
        (void)write(input_wake_pipe[1], &wake, sizeof(wake));
    }
    if (input_thread_started) pthread_join(input_thread, NULL);
    if (input_wake_pipe[0] >= 0) close(input_wake_pipe[0]);
    if (input_wake_pipe[1] >= 0) close(input_wake_pipe[1]);
}

static void redirect_stderr(void) {
    const char *paths[] = {
        "/tmp/ctm-bridge-test.log",
        "/media/developer/apps/usr/palm/applications/com.local.ctmbridge/log.txt",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        if (freopen(paths[i], "w", stderr)) {
            setvbuf(stderr, NULL, _IONBF, 0);
            log_line("CTM Bridge Test log started");
            return;
        }
    }
}

static int find_asset(const char *name, char *out, size_t out_len) {
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) return -1;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (slash) *slash = '\0';

    const char *patterns[] = {
        "%s/../assets/%s",
        "%s/../../assets/%s",
        "%s/%s",
        NULL
    };
    for (int i = 0; patterns[i]; i++) {
        snprintf(out, out_len, patterns[i], exe, name);
        FILE *f = fopen(out, "rb");
        if (f) {
            fclose(f);
            return 0;
        }
    }
    return -1;
}

static void ui_text(SDL_Renderer *r, TTF_Font *font, const char *text, int x, int y, SDL_Color color) {
    if (!r || !font || !text) return;
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface) return;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(r, surface);
    if (texture) {
        SDL_Rect dst = {x, y, surface->w, surface->h};
        SDL_RenderCopy(r, texture, NULL, &dst);
        SDL_DestroyTexture(texture);
    }
    SDL_FreeSurface(surface);
}

static void ui_status_line(SDL_Renderer *r, TTF_Font *font, int y, const char *label, const char *value) {
    ui_text(r, font, label, 40, y, (SDL_Color){150, 166, 176, 255});
    ui_text(r, font, value, 220, y, (SDL_Color){236, 242, 245, 255});
}

static void *ui_thread_main(void *arg) {
    (void)arg;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        log_line("SDL_Init failed: %s", SDL_GetError());
        return NULL;
    }
    if (TTF_Init() != 0) {
        log_line("TTF_Init failed: %s", TTF_GetError());
        SDL_Quit();
        return NULL;
    }

    SDL_Window *win = SDL_CreateWindow("CTM Bridge Test",
                                       SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED,
                                       UI_W,
                                       UI_H,
                                       SDL_WINDOW_SHOWN);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!ren && win) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!win || !ren) {
        log_line("SDL window/renderer failed: %s", SDL_GetError());
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        TTF_Quit();
        SDL_Quit();
        return NULL;
    }

    char font_path[1024];
    if (find_asset("font.ttf", font_path, sizeof(font_path)) != 0) {
        snprintf(font_path, sizeof(font_path), "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    }
    TTF_Font *title_font = TTF_OpenFont(font_path, 28);
    TTF_Font *font = TTF_OpenFont(font_path, 18);
    if (!title_font || !font) {
        log_line("TTF_OpenFont failed path=%s error=%s", font_path, TTF_GetError());
    }

    while (g_running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) g_running = 0;
            if (ev.type == SDL_KEYDOWN &&
                (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q)) {
                g_running = 0;
            }
            if (ev.type == SDL_KEYDOWN &&
                (ev.key.keysym.sym == SDLK_MINUS || ev.key.keysym.sym == SDLK_KP_MINUS ||
                 ev.key.keysym.sym == SDLK_LEFT)) {
                audio_latency_adjust(-0x10);
            }
            if (ev.type == SDL_KEYDOWN &&
                (ev.key.keysym.sym == SDLK_PLUS || ev.key.keysym.sym == SDLK_EQUALS ||
                 ev.key.keysym.sym == SDLK_KP_PLUS || ev.key.keysym.sym == SDLK_RIGHT)) {
                audio_latency_adjust(0x10);
            }
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_a) {
                audio94_override_toggle();
            }
            if (ev.type == SDL_MOUSEBUTTONDOWN) {
                int x = ev.button.x;
                int y = ev.button.y;
                if (y >= 394 && y <= 436) {
                    if (x >= 520 && x <= 572) audio_latency_adjust(-0x10);
                    if (x >= 584 && x <= 636) audio_latency_adjust(0x10);
                }
                if (y >= 438 && y <= 474 && x >= 220 && x <= 476) {
                    audio94_override_toggle();
                }
            }
        }

        bridge_status_t st;
        status_snapshot(&st);

        SDL_SetRenderDrawColor(ren, 17, 22, 26, 255);
        SDL_RenderClear(ren);
        SDL_SetRenderDrawColor(ren, st.tcp_connected ? 52 : 90, st.tcp_connected ? 128 : 54, st.tcp_connected ? 84 : 54, 255);
        SDL_Rect top = {0, 0, UI_W, 64};
        SDL_RenderFillRect(ren, &top);
        ui_text(ren, title_font, "CTM Bridge Test", 32, 16, (SDL_Color){245, 248, 250, 255});

        char line[256];
        snprintf(line, sizeof(line), "%s", st.state[0] ? st.state : "starting");
        ui_status_line(ren, font, 88, "State", line);
        snprintf(line, sizeof(line), "%s:%d", st.host[0] ? st.host : "-", st.port);
        ui_status_line(ren, font, 122, "Windows", line);
        snprintf(line, sizeof(line), "%s", st.product[0] ? st.product : "-");
        ui_status_line(ren, font, 156, "Controller", line);
        snprintf(line, sizeof(line), "%s  %s", st.serial[0] ? st.serial : "-", st.hid_path[0] ? st.hid_path : "-");
        ui_status_line(ren, font, 190, "HID", line);
        snprintf(line, sizeof(line), "poll %.1f  read %.1f",
                 st.input_profile_poll_hz,
                 st.input_profile_read_hz);
        ui_status_line(ren, font, 224, "webOS input", line);
        snprintf(line, sizeof(line), "sent %.1f  dup %.1f",
                 st.input_profile_sent_hz,
                 st.input_profile_dup_hz);
        ui_status_line(ren, font, 258, "Input send", line);
        snprintf(line, sizeof(line), "output=%lu/s paced=%d grabbed=%d", st.output_fps, st.paced_depth, st.evdev_grabs);
        ui_status_line(ren, font, 292, "Rates", line);
        snprintf(line, sizeof(line), "avg %.1f max %.1f last %.1f",
                 st.input_profile_gap_avg_ms,
                 st.input_profile_gap_max_ms,
                 st.last_input_gap_ms);
        ui_status_line(ren, font, 326, "Gap ms", line);
        snprintf(line, sizeof(line), "features=%lu fails=%lu output_fails=%lu drops=%lu",
                 st.feature_gets, st.feature_fails, st.output_fails, st.paced_drops);
        ui_status_line(ren, font, 360, "Issues", line);
        snprintf(line, sizeof(line), "0x%02x  patched=%lu", st.audio_latency, st.latency_patches);
        ui_status_line(ren, font, 394, "Latency", line);
        SDL_SetRenderDrawColor(ren, 44, 54, 62, 255);
        SDL_Rect minus = {520, 394, 52, 36};
        SDL_Rect plus = {584, 394, 52, 36};
        SDL_RenderFillRect(ren, &minus);
        SDL_RenderFillRect(ren, &plus);
        ui_text(ren, title_font, "-", 539, 394, (SDL_Color){245, 248, 250, 255});
        ui_text(ren, title_font, "+", 602, 394, (SDL_Color){245, 248, 250, 255});
        snprintf(line, sizeof(line), "%s  patched=%lu", st.audio94_override ? "on" : "off", st.audio94_patches);
        ui_status_line(ren, font, 438, "0x95->0x94", line);
        SDL_SetRenderDrawColor(ren, st.audio94_override ? 52 : 44, st.audio94_override ? 128 : 54, st.audio94_override ? 84 : 62, 255);
        SDL_Rect audio94 = {520, 438, 52, 36};
        SDL_RenderFillRect(ren, &audio94);
        ui_text(ren, font, st.audio94_override ? "on" : "off", 532, 446, (SDL_Color){245, 248, 250, 255});
        ui_status_line(ren, font, 490, "Message", st.message[0] ? st.message : "-");

        SDL_RenderPresent(ren);
        SDL_Delay(100);
    }

    if (title_font) TTF_CloseFont(title_font);
    if (font) TTF_CloseFont(font);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return NULL;
}

int main(int argc, char **argv) {
    app_config_t cfg;
    hid_point_t hid;
    uint64_t last_status_us = now_us();
    unsigned long output_writes = 0, output_fails = 0;
    unsigned long feature_gets = 0, feature_fails = 0, paced_drops = 0;
    unsigned long last_input = 0, last_input_poll = 0, last_input_read = 0, last_input_dup = 0;
    unsigned long last_input_gap_count = 0, last_output = 0;
    double last_input_gap_sum_ms = 0.0;
    pthread_t ui_thread;
    pthread_t stop_sniff_thread;
    stop_sniff_thread_ctx_t stop_sniff_ctx;
    int ui_thread_started = 0;
    int stop_sniff_thread_started = 0;
    ctm_enet_client_t *enet_client = NULL;
    int enet_global_ready = 0;

    memset(&cfg, 0, sizeof(cfg));
    memset(&hid, 0, sizeof(hid));
    memset(&stop_sniff_ctx, 0, sizeof(stop_sniff_ctx));
    memset(&g_status, 0, sizeof(g_status));
    hid.fd = -1;
    g_status.audio_latency = DEFAULT_AUDIO_LATENCY;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    redirect_stderr();
    parse_args(&cfg, argc, argv);
    status_set_config(&cfg);
    status_set_state("starting", "Opening controller");
    if (pthread_create(&ui_thread, NULL, ui_thread_main, NULL) == 0) {
        ui_thread_started = 1;
    } else {
        log_line("ui thread start failed errno=%d", errno);
    }

    log_line("config host=%s port=%d path=%s bt_address=%s vid=0x%04x pid=0x%04x any_hid=%d",
             cfg.host, cfg.port, cfg.path, cfg.bt_address, cfg.vid, cfg.pid, cfg.any_hid);

    if (open_hid(&cfg, &hid) != 0) {
        status_set_state("error", "No matching writable Bluetooth hidraw controller");
        log_line("no matching writable Bluetooth hidraw controller found");
        g_running = 0;
        if (ui_thread_started) pthread_join(ui_thread, NULL);
        return 2;
    }
    status_set_hid(&hid);
    log_line("hid opened path=%s vid=0x%04x pid=0x%04x product=%s serial=%s",
             hid.path, hid.vid, hid.pid, hid.product, hid.serial);

    /* Periodic stopSniff persists across reconnects; it is keyed to the BT
     * address, not the bridge link. */
    const char *sniff_addr = valid_bt_address(cfg.bt_address) ? cfg.bt_address : hid.serial;
    if (valid_bt_address(sniff_addr)) {
        snprintf(stop_sniff_ctx.address, sizeof(stop_sniff_ctx.address), "%s", sniff_addr);
        if (pthread_create(&stop_sniff_thread, NULL, stop_sniff_thread_main, &stop_sniff_ctx) == 0) {
            stop_sniff_thread_started = 1;
            log_line("periodic stopSniff temp address=%s interval_ms=5000", stop_sniff_ctx.address);
        } else {
            log_line("periodic stopSniff thread start failed errno=%d", errno);
        }
    }

    /* ENet is process-global; init once. If it fails we still run TCP-only. */
    if (enet_client_global_init() == 0) {
        enet_global_ready = 1;
        enet_client = enet_client_create();
        if (!enet_client) {
            log_line("enet client create failed; ENet probe disabled");
        }
    } else {
        log_line("enet_initialize failed; ENet probe disabled, TCP only");
    }

    /* Continuous dual-probe loop: try ENet (UDP host:port) first with a short
     * timeout, then TCP, looping forever until one connects. On disconnect we
     * release evdev grabs + show an explicit unplugged state, then resume the
     * loop. We never give up while g_running. evdev is (re)grabbed only while a
     * session is actually live so the pad drives the webOS UI when the bridge
     * is down. */
    while (g_running) {
        transport_t transport;
        memset(&transport, 0, sizeof(transport));
        int connected = 0;

        status_set_state("connecting", "Probing ENet then TCP");

        /* 1) ENet first, brief timeout. */
        if (enet_client) {
            if (enet_client_connect(enet_client, cfg.host, cfg.port, 400) == 0) {
                transport.kind = TRANSPORT_ENET;
                transport.enet = enet_client;
                transport.fd = -1;
                connected = 1;
                status_set_tcp(1);
                log_line("connected via ENet/UDP host=%s port=%d", cfg.host, cfg.port);
            }
        }

        /* 2) Fall back to TCP. */
        if (!connected) {
            int sock = connect_tcp(cfg.host, cfg.port);
            if (sock >= 0) {
                transport.kind = TRANSPORT_TCP;
                transport.fd = sock;
                transport.enet = NULL;
                connected = 1;
                status_set_tcp(1);
                log_line("connected via TCP host=%s port=%d", cfg.host, cfg.port);
            }
        }

        if (!connected) {
            /* Neither transport answered; back off briefly and probe again. */
            status_set_tcp(0);
            status_set_state("connecting", "Waiting for Windows bridge");
            for (int slept = 0; slept < 500 && g_running; slept += 50) {
                usleep(50000);
            }
            continue;
        }

        /* Link is up: grab the controller's evdev nodes so it drives the host,
         * not the webOS UI, then run the session until the link drops. */
        grab_device_evdev(hid.vid, hid.pid);
        run_bridge_session(&transport, &hid,
                           &output_writes, &output_fails, &feature_gets, &feature_fails, &paced_drops,
                           &last_input, &last_input_poll, &last_input_read, &last_input_dup,
                           &last_input_gap_count, &last_output,
                           &last_input_gap_sum_ms, &last_status_us);

        /* Session ended: explicit plug-out. Release the evdev grabs (pad goes
         * back to the webOS UI) and surface the disconnected state. */
        release_evdev_grabs();
        status_set_tcp(0);
        if (transport.kind == TRANSPORT_ENET) {
            enet_client_disconnect(enet_client);
        } else if (transport.fd >= 0) {
            shutdown(transport.fd, SHUT_RDWR);
            close(transport.fd);
        }
        if (g_running) {
            status_set_state("disconnected", "Link lost; controller unplugged, reconnecting");
            log_line("bridge link lost; evdev released, retrying probe loop");
        }
    }

    status_set_state("stopping", "Closing bridge session");
    g_running = 0;
    if (stop_sniff_thread_started) pthread_join(stop_sniff_thread, NULL);
    release_evdev_grabs();
    if (enet_client) {
        enet_client_destroy(enet_client);
        enet_client = NULL;
    }
    if (enet_global_ready) {
        enet_client_global_deinit();
    }
    close(hid.fd);
    status_set_tcp(0);
    status_set_hid(NULL);
    unsigned long final_input = 0;
    status_get_input_totals(&final_input, NULL, NULL, NULL, NULL, NULL);
    log_line("final input=%lu output=%lu feature_gets=%lu feature_fails=%lu output_fails=%lu paced_drops=%lu",
             final_input, output_writes, feature_gets, feature_fails, output_fails, paced_drops);
    if (ui_thread_started) pthread_join(ui_thread, NULL);
    return 0;
}
