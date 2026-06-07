/* Device detection / monitor (D8). Periodic /sys/class/input -> hidraw scan,
 * diffed each tick to emit connect/disconnect; classification is delegated to
 * the controller factory so there is one source of truth. Its own thread. */

#define _GNU_SOURCE

#include "ctm_monitor.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CTM_MONITOR_MAX 64
#define RESCAN_MS 1000

struct ctm_monitor {
    pthread_t thread;
    int started;
    volatile int stop;
    ctm_monitor_cb cb;
    void *ud;
    pthread_mutex_t lock;
    ctm_controller_dev_t list[CTM_MONITOR_MAX];
    int count;
};

/* Read a trimmed /sys text attribute. When: per device field during a scan. */
static int read_attr(const char *path, char *out, size_t out_len)
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

/* Map a sysfs bustype to the transport label the controller layer expects.
 * When: building each ctm_controller_dev_t. */
static void map_bus(const char *bustype, char *out, size_t out_len)
{
    if (strcmp(bustype, "0003") == 0 || strcmp(bustype, "3") == 0) {
        snprintf(out, out_len, "USB");
    } else if (strcmp(bustype, "0005") == 0 || strcmp(bustype, "5") == 0) {
        snprintf(out, out_len, "BT");
    } else {
        snprintf(out, out_len, "%s", bustype[0] ? bustype : "-");
    }
}

/* Resolve /sys/class/input/<input>/device/hidraw/hidrawN -> /dev/hidrawN.
 * Returns 0 if a hidraw node was found. When: per input node during a scan. */
static int find_hidraw_node(const char *input_path, char *out, size_t out_len)
{
    char hidraw_dir[PATH_MAX];
    snprintf(hidraw_dir, sizeof(hidraw_dir), "%s/device/hidraw", input_path);
    DIR *d = opendir(hidraw_dir);
    if (!d) return -1;
    int found = -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "hidraw", 6) == 0) {
            snprintf(out, out_len, "/dev/%s", ent->d_name);
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Find a device by hidraw path in a list. When: scan dedup + diff. */
static int dev_index(const ctm_controller_dev_t *list, int count, const char *path)
{
    for (int i = 0; i < count; ++i) {
        if (strcmp(list[i].path, path) == 0) return i;
    }
    return -1;
}

/* Walk /sys/class/input and build the deduped list of hidraw-backed controllers
 * (one entry per hidraw node), each as a ctm_controller_dev_t. Input-only
 * devices (no hidraw) are skipped — that's the blocked raw-USB path. When: every
 * rescan tick. Returns the count. */
static int scan(ctm_controller_dev_t *out, int max)
{
    int count = 0;
    DIR *dir = opendir("/sys/class/input");
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        if (strncmp(ent->d_name, "input", 5) != 0) continue;

        char input_path[PATH_MAX];
        snprintf(input_path, sizeof(input_path), "/sys/class/input/%s", ent->d_name);

        char path[64];
        if (find_hidraw_node(input_path, path, sizeof(path)) != 0) continue;
        if (dev_index(out, count, path) >= 0) continue;   /* already have this hidraw */

        ctm_controller_dev_t dev;
        memset(&dev, 0, sizeof(dev));
        snprintf(dev.path, sizeof(dev.path), "%s", path);

        char attr[160], bustype[16] = {0};
        snprintf(attr, sizeof(attr), "%s/id/vendor", input_path);
        read_attr(attr, dev.vid, sizeof(dev.vid));
        snprintf(attr, sizeof(attr), "%s/id/product", input_path);
        read_attr(attr, dev.pid, sizeof(dev.pid));
        snprintf(attr, sizeof(attr), "%s/id/bustype", input_path);
        read_attr(attr, bustype, sizeof(bustype));
        map_bus(bustype, dev.bus, sizeof(dev.bus));
        snprintf(attr, sizeof(attr), "%s/name", input_path);
        read_attr(attr, dev.name, sizeof(dev.name));
        snprintf(attr, sizeof(attr), "%s/uniq", input_path);
        read_attr(attr, dev.mac, sizeof(dev.mac));

        out[count++] = dev;
    }
    closedir(dir);
    return count;
}

/* Diff the fresh scan against the held list and fire the callback for each
 * appeared/disappeared device, then swap in the fresh list. When: each tick. */
static void diff_and_publish(ctm_monitor_t *m, ctm_controller_dev_t *fresh, int fresh_count)
{
    /* Removed: in old list, not in fresh. */
    for (int i = 0; i < m->count; ++i) {
        if (dev_index(fresh, fresh_count, m->list[i].path) < 0 && m->cb) {
            m->cb(m->ud, &m->list[i], 0);
        }
    }
    /* Added: in fresh, not in old. */
    for (int i = 0; i < fresh_count; ++i) {
        if (dev_index(m->list, m->count, fresh[i].path) < 0 && m->cb) {
            m->cb(m->ud, &fresh[i], 1);
        }
    }
    pthread_mutex_lock(&m->lock);
    memcpy(m->list, fresh, (size_t)fresh_count * sizeof(fresh[0]));
    m->count = fresh_count;
    pthread_mutex_unlock(&m->lock);
}

/* Monitor thread body: rescan + diff every RESCAN_MS until stop. When: the
 * monitor's own thread, started by ctm_monitor_start. */
static void *monitor_thread(void *arg)
{
    ctm_monitor_t *m = (ctm_monitor_t *)arg;
    while (!m->stop) {
        ctm_controller_dev_t fresh[CTM_MONITOR_MAX];
        int n = scan(fresh, CTM_MONITOR_MAX);
        diff_and_publish(m, fresh, n);
        for (int slept = 0; slept < RESCAN_MS && !m->stop; slept += 50) {
            usleep(50000);
        }
    }
    return NULL;
}

ctm_monitor_t *ctm_monitor_start(ctm_monitor_cb cb, void *ud)
{
    ctm_monitor_t *m = (ctm_monitor_t *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->cb = cb;
    m->ud = ud;
    pthread_mutex_init(&m->lock, NULL);
    if (pthread_create(&m->thread, NULL, monitor_thread, m) != 0) {
        pthread_mutex_destroy(&m->lock);
        free(m);
        return NULL;
    }
    m->started = 1;
    return m;
}

int ctm_monitor_list(ctm_monitor_t *m, ctm_controller_dev_t *out, int max)
{
    if (!m || !out || max <= 0) return 0;
    pthread_mutex_lock(&m->lock);
    int n = m->count < max ? m->count : max;
    memcpy(out, m->list, (size_t)n * sizeof(out[0]));
    pthread_mutex_unlock(&m->lock);
    return n;
}

void ctm_monitor_stop(ctm_monitor_t *m)
{
    if (!m) return;
    m->stop = 1;
    if (m->started) pthread_join(m->thread, NULL);
    pthread_mutex_destroy(&m->lock);
    free(m);
}
