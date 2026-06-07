/* Agent control client (discovery + commands), bridge sessions, process
 * spawning, the per-device settings store, and plug-in/out orchestration.
 * Moved verbatim out of lvgl_ui.c; de-static'd and prototyped in ui_common.h. */

#define _GNU_SOURCE

#include "ctm_state.h"
#include "ctm_bridge_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
tv_bridge_worker_settings_t default_settings_for_item(const logical_device_t *item)
{
    tv_bridge_worker_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    settings.kind = TV_BRIDGE_KIND_HID;
    settings.audio_mode = TV_BRIDGE_AUDIO_AUTO;
    settings.latency_ms = 48;
    settings.haptics_gain_centi = 100;
    settings.headset_volume_percent = 100;
    settings.speaker_volume_percent = 100;
    settings.ds5_patch_high_nibble = 0xf;
    settings.ds5_patch_low_nibble = 0xd;
    settings.ds5_patch2_high_nibble = 0xf;
    settings.ds5_patch2_low_nibble = 0x7;

    const char *kind = bridge_kind_for_item(item);
    if (strcmp(kind, "ds5") == 0) {
        settings.kind = TV_BRIDGE_KIND_DS5;
        settings.headset_volume_percent = 0x4d;
        settings.speaker_volume_percent = 0x41;
    } else if (strcmp(kind, "ds4") == 0) {
        settings.kind = TV_BRIDGE_KIND_DS4;
        settings.haptics_gain_centi = 0;
    }
    return settings;
}

ui_device_settings_t *ui_record_for_item(const logical_device_t *item)
{
    if (!item) return NULL;
    for (int i = 0; i < g_settings_count; ++i) {
        if (strcmp(g_settings[i].key, item->key) == 0) {
            return &g_settings[i];
        }
    }
    if (g_settings_count >= MAX_DEVICES) {
        return NULL;
    }
    snprintf(g_settings[g_settings_count].key, sizeof(g_settings[0].key), "%s", item->key);
    g_settings[g_settings_count].settings = default_settings_for_item(item);
    g_settings[g_settings_count].headset_volume_percent =
        g_settings[g_settings_count].settings.headset_volume_percent;
    g_settings[g_settings_count].speaker_volume_percent =
        g_settings[g_settings_count].settings.speaker_volume_percent;
    return &g_settings[g_settings_count++];
}

tv_bridge_worker_settings_t *settings_for_item(const logical_device_t *item)
{
    ui_device_settings_t *record = ui_record_for_item(item);
    return record ? &record->settings : NULL;
}

void apply_settings_to_session(const logical_device_t *item)
{
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!item || !settings) return;
    int session = session_index_for_key(item->key);
    if (session >= 0 && g_sessions[session].controller) {
        ctm_controller_set_settings(g_sessions[session].controller, settings);
    }
}

int run_child_wait(char *const argv[])
{
    /* posix_spawn (clone/vfork semantics) instead of fork()+exec. fork() copies
     * the parent's page tables — cheap in the small standalone app, but expensive
     * and a recurring stall inside the large moonlight process (video buffers).
     * The stopSniff worker calls this every 500 ms, so the fork stall was starving
     * the bridge -> BT sniff mode (~115 Hz) + added latency. posix_spawn does not
     * copy the address space, so its cost is independent of process size. */
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_RDWR, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_RDWR, 0);

    extern char **environ;
    pid_t pid = -1;
    int spawn_rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_rc != 0) {
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void stop_sniff_once(const char *mac)
{
    if (!valid_bt_address(mac)) {
        return;
    }
    char payload[96];
    snprintf(payload, sizeof(payload), "{\"address\":\"%s\"}", mac);
    char *const argv[] = {
        "luna-send-pub",
        "-n",
        "1",
        "-f",
        "luna://com.webos.service.bluetooth2/device/internal/stopSniff",
        payload,
        NULL
    };
    (void)run_child_wait(argv);
}

void *stop_sniff_worker(void *arg)
{
    (void)arg;
    while (g_running) {
        char macs[MAX_DEVICES][64];
        int count = 0;
        pthread_mutex_lock(&g_bt_mac_mutex);
        count = g_bt_mac_count;
        if (count > MAX_DEVICES) count = MAX_DEVICES;
        for (int i = 0; i < count; ++i) {
            snprintf(macs[i], sizeof(macs[i]), "%s", g_bt_macs[i]);
        }
        pthread_mutex_unlock(&g_bt_mac_mutex);

        for (int i = 0; i < count; ++i) {
            stop_sniff_once(macs[i]);
        }
        usleep(500000);
    }
    return NULL;
}

void publish_bt_macs(void)
{
    char macs[MAX_DEVICES][64];
    int count = 0;
    for (int i = 0; i < g_devices.count && count < MAX_DEVICES; ++i) {
        const logical_device_t *item = &g_devices.items[i];
        if (strcmp(bus_label(item->bus), "BT") != 0 || !valid_bt_address(item->mac)) {
            continue;
        }
        bool duplicate = false;
        for (int j = 0; j < count; ++j) {
            if (strcmp(macs[j], item->mac) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            snprintf(macs[count++], sizeof(macs[0]), "%s", item->mac);
        }
    }

    pthread_mutex_lock(&g_bt_mac_mutex);
    g_bt_mac_count = count;
    for (int i = 0; i < count; ++i) {
        snprintf(g_bt_macs[i], sizeof(g_bt_macs[0]), "%s", macs[i]);
    }
    pthread_mutex_unlock(&g_bt_mac_mutex);
}

bool discover_agent_once(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return false;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 180000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CTM_AGENT_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const char probe[] = "CTM_DISCOVER_V1";
    sendto(fd, probe, sizeof(probe) - 1, 0, (struct sockaddr *)&addr, sizeof(addr));

    char buf[256];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
    close(fd);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    if (!starts_with(buf, "CTM_AGENT_V1")) {
        return false;
    }
    const char *port = strstr(buf, "port=");
    g_agent_port = port ? atoi(port + 5) : CTM_AGENT_PORT;
    if (g_agent_port <= 0 || g_agent_port > 65535) {
        g_agent_port = CTM_AGENT_PORT;
    }
    const char *ip = inet_ntoa(from.sin_addr);
    snprintf(g_agent_host, sizeof(g_agent_host), "%s", ip ? ip : "");
    g_agent_online = g_agent_host[0] != '\0';
    return g_agent_online;
}

int send_agent_command(const char *command, char *response, size_t response_len)
{
    if (!g_agent_host[0] && !discover_agent_once()) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_agent_port);
    if (inet_aton(g_agent_host, &addr.sin_addr) == 0 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        g_agent_online = false;
        return -1;
    }

    char line[512];
    snprintf(line, sizeof(line), "%s\n", command);
    if (send(fd, line, strlen(line), 0) < 0) {
        close(fd);
        return -1;
    }
    ssize_t n = recv(fd, response, response_len > 0 ? response_len - 1 : 0, 0);
    close(fd);
    if (response_len > 0) {
        response[n > 0 ? n : 0] = '\0';
    }
    return n > 0 && response && starts_with(response, "OK") ? 0 : -1;
}

int session_index_for_key(const char *key)
{
    for (int i = 0; i < g_session_count; ++i) {
        if (strcmp(g_sessions[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

int next_bridge_port(void)
{
    int port = CTM_BRIDGE_BASE_PORT;
    for (;;) {
        bool used = false;
        for (int i = 0; i < g_session_count; ++i) {
            if (g_sessions[i].port == port) {
                used = true;
                break;
            }
        }
        if (!used) {
            return port;
        }
        ++port;
    }
}

int first_scan_index_for_item(const logical_device_t *item)
{
    if (!item || item->device_count <= 0) return -1;
    int index = item->device_indices[0];
    return index >= 0 && index < g_scan.count ? index : -1;
}

void make_bridge_busid(const logical_device_t *item, char *out, size_t out_len)
{
    static unsigned seq;
    const char *kind = bridge_kind_for_item(item);
    snprintf(out, out_len, "ctm-%s-%u", kind, ++seq);
}

bool add_session(const char *key, const char *busid, ctm_controller_t *controller, int port)
{
    int index = session_index_for_key(key);
    if (index >= 0) {
        if (g_sessions[index].controller && g_sessions[index].controller != controller) {
            ctm_controller_plug_out(g_sessions[index].controller);
            ctm_controller_destroy(g_sessions[index].controller);
        }
        snprintf(g_sessions[index].busid, sizeof(g_sessions[index].busid), "%s", busid ? busid : "");
        g_sessions[index].port = port;
        g_sessions[index].controller = controller;
        return true;
    }
    if (g_session_count >= MAX_SESSIONS) {
        return false;
    }
    snprintf(g_sessions[g_session_count].key, sizeof(g_sessions[0].key), "%s", key);
    snprintf(g_sessions[g_session_count].busid, sizeof(g_sessions[0].busid), "%s", busid ? busid : "");
    g_sessions[g_session_count].port = port;
    g_sessions[g_session_count].controller = controller;
    g_session_count++;
    return true;
}

void stop_session(const char *key)
{
    int index = session_index_for_key(key);
    if (index < 0) {
        return;
    }
    if (g_sessions[index].controller) {
        ctm_controller_plug_out(g_sessions[index].controller);
        ctm_controller_destroy(g_sessions[index].controller);
        g_sessions[index].controller = NULL;
    }
    if (g_sessions[index].port > 0) {
        char cmd[160];
        char response[256];
        snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", g_sessions[index].busid);
        (void)send_agent_command(cmd, response, sizeof(response));
    }
    memmove(&g_sessions[index], &g_sessions[index + 1],
            (size_t)(g_session_count - index - 1) * sizeof(g_sessions[0]));
    g_session_count--;
}

void release_local_sessions_on_exit(void)
{
    for (int i = 0; i < g_session_count; ++i) {
        if (g_sessions[i].controller) {
            ctm_controller_plug_out(g_sessions[i].controller);
            ctm_controller_destroy(g_sessions[i].controller);
            g_sessions[i].controller = NULL;
        }
    }
    g_session_count = 0;
}

const char *bridge_kind_for_item(const logical_device_t *item)
{
    if (!item) return "hid";
    if (strcmp(item->vid, "054c") == 0 && strcmp(item->pid, "0ce6") == 0) return "ds5";
    if (strcmp(item->vid, "054c") == 0 &&
        (strcmp(item->pid, "09cc") == 0 || strcmp(item->pid, "05c4") == 0)) return "ds4";
    if (strcmp(item->vid, "045e") == 0 &&
        (is_xbox_pid(item->pid) || contains_ci(item->name, "xbox"))) return "xbox";
    if (strcmp(item->vid, "28de") == 0 && strcmp(item->pid, "1304") == 0) return "puck";
    return "hid";
}

/* Session key for ONE specific hidraw node of a logical device, so each
 * interface can be plugged/unplugged independently. When: per-node plug + its
 * button state. */
void node_session_key(const logical_device_t *item, int scan_index, char *out, size_t out_len)
{
    const char *tag = "node";
    if (scan_index >= 0 && scan_index < g_scan.count && g_scan.devices[scan_index].hidraw[0]) {
        tag = g_scan.devices[scan_index].hidraw;
    }
    snprintf(out, out_len, "%s#%s", item ? item->key : "", tag);
}

/* Core plug: start the host bridge, build a controller from one chosen scan
 * node, plug it in, and register the session under session_key. Shared by the
 * whole-device plug (plug_in_item) and the per-interface plug (plug_in_node).
 * When: any Plug button. */
/* Serialize the cached puck enumeration (g_puck_enum) into a CTMB_MSG_ENUM
 * payload: [ctmb_enum_info_t][descriptors blob][ per iface: ctmb_enum_iface_t +
 * report_desc ]. Caller frees. NULL if no valid enumeration. */
static uint8_t *build_puck_enum_payload(int *out_len)
{
    if (!g_puck_enum.valid) return NULL;
    int size = (int)sizeof(ctmb_enum_info_t) + g_puck_enum.descriptors_len;
    for (int i = 0; i < g_puck_enum.if_count; ++i) {
        size += (int)sizeof(ctmb_enum_iface_t) + g_puck_enum.ifs[i].rdesc_len;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf) return NULL;
    int off = 0;
    ctmb_enum_info_t info;
    memset(&info, 0, sizeof(info));
    info.descriptors_len = (uint16_t)g_puck_enum.descriptors_len;
    info.iface_count = (uint8_t)g_puck_enum.if_count;
    info.full_speed = 1;   /* the puck is full-speed (speed=12) */
    memcpy(buf + off, &info, sizeof(info)); off += (int)sizeof(info);
    memcpy(buf + off, g_puck_enum.descriptors, (size_t)g_puck_enum.descriptors_len);
    off += g_puck_enum.descriptors_len;
    for (int i = 0; i < g_puck_enum.if_count; ++i) {
        ctmb_enum_iface_t ie;
        memset(&ie, 0, sizeof(ie));
        ie.interface_number = (uint8_t)g_puck_enum.ifs[i].num;
        ie.iface_class = (uint8_t)strtol(g_puck_enum.ifs[i].cls, NULL, 16);
        ie.report_desc_len = (uint16_t)g_puck_enum.ifs[i].rdesc_len;
        memcpy(buf + off, &ie, sizeof(ie)); off += (int)sizeof(ie);
        memcpy(buf + off, g_puck_enum.ifs[i].rdesc, (size_t)g_puck_enum.ifs[i].rdesc_len);
        off += g_puck_enum.ifs[i].rdesc_len;
    }
    *out_len = off;
    return buf;
}

static bool plug_in_scan_index(logical_device_t *item, int scan_index, const char *session_key)
{
    if (!item || scan_index < 0 || scan_index >= g_scan.count) {
        return false;
    }
    if (!g_agent_online && !discover_agent_once()) {
        log_append("Windows agent not found");
        return false;
    }

    const device_info_t *dev = &g_scan.devices[scan_index];
    char response[512];
    int port = next_bridge_port();
    char cmd[256];
    char busid[32];
    const char *kind = bridge_kind_for_item(item);
    make_bridge_busid(item, busid, sizeof(busid));
    snprintf(cmd, sizeof(cmd), "BRIDGE_START %s %d %s", kind, port, busid);
    if (send_agent_command(cmd, response, sizeof(response)) != 0) {
        log_append("agent bridge start failed: %s", response);
        return false;
    }

    /* Build the neutral descriptor and hand the device to a controller — one
     * mechanism for DS5/DS4/xbox/puck/generic (factory picks the ops). */
    ctm_controller_dev_t cdev;
    memset(&cdev, 0, sizeof(cdev));
    snprintf(cdev.vid, sizeof(cdev.vid), "%s", item->vid);
    snprintf(cdev.pid, sizeof(cdev.pid), "%s", item->pid);
    snprintf(cdev.bus, sizeof(cdev.bus), "%s", bus_label(item->bus));
    snprintf(cdev.name, sizeof(cdev.name), "%s", item->name);
    snprintf(cdev.path, sizeof(cdev.path), "%s", dev->node);
    snprintf(cdev.mac, sizeof(cdev.mac), "%s", item->mac);

    ctm_controller_t *controller = ctm_controller_create(&cdev);
    if (!controller) {
        log_append("controller create failed");
        snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
        (void)send_agent_command(cmd, response, sizeof(response));
        return false;
    }
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (settings) {
        ctm_controller_set_settings(controller, settings);
    }
    /* Composite (puck): forward the cached USB enumeration so the host builds the
     * full composite device from the puck's own descriptors (Stage 2). */
    if (strcmp(kind, "puck") == 0 && g_puck_enum.valid) {
        int elen = 0;
        uint8_t *epl = build_puck_enum_payload(&elen);
        if (epl) {
            ctm_controller_set_enum_payload(controller, epl, elen);
            free(epl);
            log_append("forwarding puck enumeration to host (%d bytes)", elen);
        }
    }
    if (ctm_controller_plug_in(controller, g_agent_host, port) != 0) {
        log_append("controller plug-in failed");
        ctm_controller_destroy(controller);
        snprintf(cmd, sizeof(cmd), "BRIDGE_STOP %s", busid);
        (void)send_agent_command(cmd, response, sizeof(response));
        return false;
    }
    add_session(session_key, busid, controller, port);
    log_append("controller started kind=%s node=%s busid=%s host=%s port=%d",
               kind, dev->node, busid, g_agent_host, port);
    return true;
}

/* Plug the whole device using its first hidraw node (the default). When: the
 * logical row's Plug button. */
bool plug_in_item(logical_device_t *item)
{
    if (!item) {
        return false;
    }
    int scan_index = first_scan_index_for_item(item);
    if (scan_index < 0) {
        log_append("no hidraw node for %s", item->name);
        return false;
    }
    return plug_in_scan_index(item, scan_index, item->key);
}

/* Plug ONE chosen hidraw interface (keyed per node, independent of the whole-
 * device plug). When: a sub-row Plug button — pick exactly which interface of a
 * composite device to bridge. */
bool plug_in_node(logical_device_t *item, int scan_index)
{
    if (!item || scan_index < 0 || scan_index >= g_scan.count) {
        return false;
    }
    char key[96];
    node_session_key(item, scan_index, key, sizeof(key));
    return plug_in_scan_index(item, scan_index, key);
}

