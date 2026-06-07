/* ctm_hidraw_bridge -- single-/dev/hidrawN ↔ TCP relay using ctmb protocol.
 *
 * Built for use on the TV side of the CTM stack to expose a USB-HID device
 * (specifically the Valve Steam Controller "puck", VID 0x28de PID 0x1304)
 * to a Windows host that runs ctm-usbip and re-presents it as a virtual
 * USB device. Same protocol shape as the existing BT bridge in main.c
 * (ctm_bridge_protocol.h) so the Windows side does not need a new code
 * path -- only a new physical backend that points at this TCP listener
 * instead of a Bluetooth HID handle.
 *
 * Usage:
 *   ctm_hidraw_bridge --device /dev/hidraw3 --port 48056 [--bind 0.0.0.0]
 *
 * No SDL, no UI, no Luna. Plain POSIX. Single-client at a time. On client
 * disconnect we close the hidraw fd and loop back to accept() so a fresh
 * Windows session can re-attach without restarting the daemon.
 */

#define _GNU_SOURCE

#include "ctm_bridge_protocol.h"
#include "enet_transport.h"
#include "ctm_hid.h"
#include "ctm_transport.h"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
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
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/hidraw.h>
#endif

#ifndef HIDIOCGRAWINFO
struct hidraw_devinfo { unsigned int bustype; short vendor; short product; };
#define HIDIOCGRAWINFO _IOR('H', 0x03, struct hidraw_devinfo)
#endif
#ifndef HIDIOCGRAWNAME
#define HIDIOCGRAWNAME(len) _IOC(_IOC_READ, 'H', 0x04, len)
#endif
#ifndef HIDIOCGRDESCSIZE
#define HIDIOCGRDESCSIZE _IOR('H', 0x01, int)
#endif
#ifndef HIDIOCGRAWPHYS
#define HIDIOCGRAWPHYS(len) _IOC(_IOC_READ, 'H', 0x05, len)
#endif
#ifndef HIDIOCGRAWUNIQ
#define HIDIOCGRAWUNIQ(len) _IOC(_IOC_READ, 'H', 0x08, len)
#endif
/* linux/hidraw.h provides struct hidraw_report_descriptor; if absent on the
   target sysroot, fall back to a local definition matching the kernel's. */
#ifdef HIDRAW_MAX_DESCRIPTOR_SIZE
typedef struct hidraw_report_descriptor rd_t;
#else
struct rd_t_local { uint32_t size; unsigned char value[4096]; };
typedef struct rd_t_local rd_t;
#ifndef HIDIOCGRDESC
#define HIDIOCGRDESC _IOR('H', 0x02, rd_t)
#endif
#endif
#ifndef HIDIOCGFEATURE
#define HIDIOCGFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x07, len)
#endif
#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len) _IOC(_IOC_READ | _IOC_WRITE, 'H', 0x06, len)
#endif

#define MAX_HID_REPORT 4096
#define DEFAULT_PORT   48056
#define DEFAULT_BIND   "0.0.0.0"

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void log_line(const char *fmt, ...) {
    char ts[32];
    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    fprintf(stderr, "[%s] ", ts);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}


/* hid_item_u32 / derive_report_lengths / read_report_descriptor live in
 * shared/ctm_hid.c (see ctm_hid.h). */

static int open_hidraw(const char *path, ctmb_device_caps_t *caps_out) {
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) return -1;
    memset(caps_out, 0, sizeof(*caps_out));

    struct hidraw_devinfo info = {0};
    if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
        caps_out->bus = (uint16_t)info.bustype;
        caps_out->vendor_id = (uint16_t)info.vendor;
        caps_out->product_id = (uint16_t)info.product;
    }
    char name[64] = {0};
    if (ioctl(fd, HIDIOCGRAWNAME(sizeof(name) - 1), name) >= 0) {
        strncpy(caps_out->product, name, sizeof(caps_out->product) - 1);
    }
    strncpy(caps_out->path, path, sizeof(caps_out->path) - 1);
    return fd;
}

/* Apply one received client message to the hidraw device. Shared by the TCP and
 * ENet paths so behaviour is identical regardless of transport. */
static void hb_apply_message(ctm_transport_t *t, int hfd, const ctmb_header_t *hdr, uint8_t *payload) {
    switch (hdr->type) {
        case CTMB_MSG_OUTPUT_REPORT: {
            if (hdr->payload_len) {
                ssize_t w = write(hfd, payload, hdr->payload_len);
                if (w < 0) log_line("hidraw write failed: %s", strerror(errno));
            }
            break;
        }
        case CTMB_MSG_FEATURE_GET: {
            /* payload[0] = report id; payload_len is the requested feature size. */
            if (hdr->payload_len >= 1) {
                uint8_t rb[MAX_HID_REPORT];
                size_t report_len = hdr->payload_len;
                if (report_len > sizeof(rb)) report_len = sizeof(rb);
                memset(rb, 0, sizeof(rb));
                memcpy(rb, payload, report_len);
                int n = ioctl(hfd, HIDIOCGFEATURE(report_len), rb);
                if (n >= 0) {
                    ctm_transport_send_msg(t, CTMB_MSG_FEATURE_REPORT, CTMB_FLAG_OK,
                                       hdr->request_id, rb, (uint32_t)n);
                } else {
                    log_line("HIDIOCGFEATURE failed report=0x%02x len=%u: %s",
                             payload[0], (unsigned)report_len, strerror(errno));
                    ctm_transport_send_msg(t, CTMB_MSG_FEATURE_REPORT, 0,
                                       hdr->request_id, NULL, 0);
                }
            }
            break;
        }
        case CTMB_MSG_FEATURE_SET: {
            int ok = 0;
            if (hdr->payload_len) {
                int n = ioctl(hfd, HIDIOCSFEATURE(hdr->payload_len), payload);
                if (n >= 0) {
                    ok = 1;
                } else {
                    log_line("HIDIOCSFEATURE failed: %s", strerror(errno));
                }
            }
            ctm_transport_send_msg(t, CTMB_MSG_FEATURE_REPORT, ok ? CTMB_FLAG_OK : 0,
                               hdr->request_id, NULL, 0);
            break;
        }
        default:
            /* Ignore HELLO/HOST_CONFIG/LOG from client for v1. */
            break;
    }
}

static int handle_client(ctm_transport_t *t, const char *dev_path) {
    ctmb_device_caps_t caps;
    int hfd = open_hidraw(dev_path, &caps);
    if (hfd < 0) {
        log_line("open %s failed: %s", dev_path, strerror(errno));
        ctm_transport_send_msg(t, CTMB_MSG_ERROR, 0, 0, "hidraw open failed", 18);
        return -1;
    }
    log_line("opened %s vid=%04x pid=%04x bus=%04x name=\"%s\"",
             dev_path, caps.vendor_id, caps.product_id, caps.bus, caps.product);

    uint8_t report_desc[MAX_HID_REPORT];
    uint32_t report_desc_len = read_report_descriptor(hfd, report_desc, sizeof(report_desc));
    if (report_desc_len) {
        derive_report_lengths(report_desc, report_desc_len, &caps);
    }

    ctmb_hid_descriptor_info_t desc_info;
    memset(&desc_info, 0, sizeof(desc_info));
    desc_info.report_descriptor_len = report_desc_len;
    uint32_t hello_len = (uint32_t)(sizeof(caps) + sizeof(desc_info) + report_desc_len);
    uint8_t *hello = (uint8_t *)malloc(hello_len);
    if (!hello) {
        close(hfd);
        return -1;
    }
    memcpy(hello, &caps, sizeof(caps));
    memcpy(hello + sizeof(caps), &desc_info, sizeof(desc_info));
    if (report_desc_len) {
        memcpy(hello + sizeof(caps) + sizeof(desc_info), report_desc, report_desc_len);
    }

    /* HELLO carries caps plus the real HID report descriptor when available. */
    if (ctm_transport_send_msg(t, CTMB_MSG_HELLO, CTMB_FLAG_OK, 0, hello, hello_len) < 0) {
        log_line("HELLO send failed: %s", strerror(errno));
        free(hello);
        close(hfd); return -1;
    }
    free(hello);

    uint8_t  buf[MAX_HID_REPORT];

    while (!g_stop) {
        if (t->kind == CTM_TRANSPORT_ENET) {
            /* No client fd to poll for ENet: poll the hidraw fd briefly, then
             * service the ENet host (pumps acks/resends + queued outbound and
             * decodes inbound), then drain every decoded message. */
            struct pollfd hp;
            hp.fd = hfd; hp.events = POLLIN; hp.revents = 0;
            int pr = poll(&hp, 1, 8);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (hp.revents & POLLIN) {
                ssize_t r = read(hfd, buf, sizeof(buf));
                if (r > 0) {
                    if (ctm_transport_send_msg(t, CTMB_MSG_INPUT_REPORT, 0, 0, buf, (uint32_t)r) < 0) break;
                } else if (r < 0 && errno != EAGAIN && errno != EINTR) {
                    log_line("hidraw read failed: %s", strerror(errno));
                    break;
                }
            }
            if (ctm_transport_service(t, 2) < 0) {
                log_line("ENet client disconnected");
                break;
            }
            ctmb_header_t hdr;
            uint8_t *payload = NULL;
            while (ctm_transport_recv_msg(t, &hdr, &payload) == 1) {
                hb_apply_message(t, hfd, &hdr, payload);
                free(payload);
                payload = NULL;
            }
            continue;
        }

        struct pollfd fds[2];
        fds[0].fd = hfd; fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = t->fd; fds[1].events = POLLIN; fds[1].revents = 0;
        int pr = poll(fds, 2, 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & POLLIN) {
            ssize_t r = read(hfd, buf, sizeof(buf));
            if (r > 0) {
                if (ctm_transport_send_msg(t, CTMB_MSG_INPUT_REPORT, 0, 0, buf, (uint32_t)r) < 0) break;
            } else if (r < 0 && errno != EAGAIN && errno != EINTR) {
                log_line("hidraw read failed: %s", strerror(errno));
                break;
            }
        }
        if (fds[1].revents & POLLIN) {
            ctmb_header_t hdr;
            uint8_t *payload = NULL;
            int got = ctm_transport_recv_msg(t, &hdr, &payload);
            if (got <= 0) { log_line("client closed"); free(payload); break; }
            hb_apply_message(t, hfd, &hdr, payload);
            free(payload);
        }
    }
    close(hfd);
    return 0;
}

static void escape_json(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { /* skip control chars to keep JSON valid */ }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static int do_enumerate(void) {
    /* Pure /dev/hidrawN + ioctl walk. webOS dev jail mounts /sys/devices
       but not /sys/class/hidraw, so we cannot rely on /sys/class/hidraw/*.
       Everything we need comes from HID ioctls anyway. */
    printf("[");
    int first = 1;
    for (int i = 0; i < 32; i++) {
        char dev[64];
        snprintf(dev, sizeof(dev), "/dev/hidraw%d", i);

        struct stat devst;
        if (stat(dev, &devst) < 0) continue;  /* node doesn't exist, skip */

        if (!first) printf(",");
        first = 0;
        printf("\n  {\"index\":%d,\"node\":\"%s\"", i, dev);
        printf(",\"dev_mode\":\"0%o\",\"dev_uid\":%u,\"dev_gid\":%u",
               devst.st_mode & 0777, devst.st_uid, devst.st_gid);

        int fd = open(dev, O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            int e = errno;
            fd = open(dev, O_RDONLY | O_NONBLOCK);
            printf(",\"readable\":%s,\"writable\":false,\"open_error\":\"%s\"",
                   fd >= 0 ? "true" : "false", strerror(e));
        } else {
            printf(",\"readable\":true,\"writable\":true");
        }
        if (fd < 0) { printf("}"); continue; }

        struct hidraw_devinfo info = {0};
        if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
            printf(",\"bus\":\"%04x\",\"vid\":\"%04x\",\"pid\":\"%04x\"",
                   (unsigned)info.bustype, (unsigned short)info.vendor,
                   (unsigned short)info.product);
        }

        char name[256] = {0}, esc[600];
        if (ioctl(fd, HIDIOCGRAWNAME(sizeof(name) - 1), name) >= 0) {
            escape_json(name, esc, sizeof(esc));
            printf(",\"name\":\"%s\"", esc);
        }

        char phys[256] = {0};
        if (ioctl(fd, HIDIOCGRAWPHYS(sizeof(phys) - 1), phys) >= 0) {
            escape_json(phys, esc, sizeof(esc));
            printf(",\"phys\":\"%s\"", esc);
        }

        char uniq[256] = {0};
        if (ioctl(fd, HIDIOCGRAWUNIQ(sizeof(uniq) - 1), uniq) >= 0) {
            escape_json(uniq, esc, sizeof(esc));
            printf(",\"uniq\":\"%s\"", esc);
        }

        int desc_size = 0;
        if (ioctl(fd, HIDIOCGRDESCSIZE, &desc_size) == 0 && desc_size > 0) {
            rd_t rd;
            rd.size = (uint32_t)desc_size;
            if (ioctl(fd, HIDIOCGRDESC, &rd) == 0) {
                printf(",\"report_descriptor_bytes\":%d", desc_size);
                printf(",\"report_descriptor_hex\":\"");
                for (int k = 0; k < desc_size && k < (int)sizeof(rd.value); k++) {
                    printf("%02x", rd.value[k]);
                }
                printf("\"");
            }
        }

        close(fd);
        printf("}");
    }
    printf("\n]\n");
    return 0;
}


int main(int argc, char **argv) {
    const char *dev_path = "/dev/hidraw0";
    const char *bind_addr = DEFAULT_BIND;
    const char *connect_host = NULL;
    int port = DEFAULT_PORT;
    int enumerate = 0;
    int reconnect_ms = 2000;

    static const struct option opts[] = {
        {"device",      required_argument, 0, 'd'},
        {"port",        required_argument, 0, 'p'},
        {"bind",        required_argument, 0, 'b'},
        {"connect",     required_argument, 0, 'c'},
        {"enumerate",   no_argument,       0, 'e'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0},
    };
    int o;
    while ((o = getopt_long(argc, argv, "d:p:b:c:eh", opts, NULL)) != -1) {
        switch (o) {
            case 'd': dev_path = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'b': bind_addr = optarg; break;
            case 'c': connect_host = optarg; break;
            case 'e': enumerate = 1; break;
            case 'h':
            default:
                fprintf(stderr,
                    "usage: %s --device /dev/hidrawN --port %d [--bind 0.0.0.0]    (listen)\n"
                    "       %s --device /dev/hidrawN --connect host:port            (dial out)\n"
                    "       %s --enumerate\n",
                    argv[0], DEFAULT_PORT, argv[0], argv[0]);
                return 0;
        }
    }

    if (enumerate) return do_enumerate();

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    /* Dial-out mode: connect to a remote ctm-usbip BridgeBackend listener
       and keep retrying with backoff until the user kills us. Each attempt is a
       continuous ENet->TCP dual-probe: try ENet (UDP host:port) first with a
       short timeout, then TCP, matching src/main.c so an --enet agent and a
       plain TCP agent both just work. */
    if (connect_host) {
        char host_buf[128] = {0};
        int dial_port = 0;
        const char *colon = strchr(connect_host, ':');
        if (colon) {
            size_t hlen = (size_t)(colon - connect_host);
            if (hlen >= sizeof(host_buf)) hlen = sizeof(host_buf) - 1;
            memcpy(host_buf, connect_host, hlen);
            dial_port = atoi(colon + 1);
        } else {
            strncpy(host_buf, connect_host, sizeof(host_buf) - 1);
            dial_port = port;
        }
        log_line("dial-out mode host=%s port=%d device=%s", host_buf, dial_port, dev_path);

        /* ENet is process-global; init once. If it fails we run TCP-only. */
        ctm_enet_client_t *enet = NULL;
        if (enet_client_global_init() == 0) {
            enet = enet_client_create();
            if (!enet) log_line("enet client create failed; ENet probe disabled, TCP only");
        } else {
            log_line("enet_initialize failed; ENet probe disabled, TCP only");
        }

        ctm_transport_t t;
        ctm_transport_init(&t, enet);
        while (!g_stop) {
            /* Continuous ENet->TCP dual-probe; retry with backoff until killed. */
            if (ctm_transport_connect_once(&t, host_buf, dial_port, 400) != 0) {
                struct timespec ts = {reconnect_ms / 1000,
                                      (reconnect_ms % 1000) * 1000000L};
                nanosleep(&ts, NULL);
                continue;
            }
            log_line("connected via %s to %s:%d",
                     t.kind == CTM_TRANSPORT_ENET ? "ENet/UDP" : "TCP", host_buf, dial_port);
            handle_client(&t, dev_path);
            ctm_transport_disconnect(&t);
            log_line("disconnected; retrying in %d ms", reconnect_ms);
        }
        ctm_transport_destroy(&t);
        if (enet) {
            enet_client_destroy(enet);
            enet_client_global_deinit();
        }
        log_line("bye");
        return 0;
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sin = {0};
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &sin.sin_addr) <= 0) {
        fprintf(stderr, "bad bind addr: %s\n", bind_addr);
        return 1;
    }
    if (bind(srv, (struct sockaddr *)&sin, sizeof(sin)) < 0) { perror("bind"); return 1; }
    if (listen(srv, 1) < 0) { perror("listen"); return 1; }

    log_line("listening on %s:%d device=%s", bind_addr, port, dev_path);

    while (!g_stop) {
        struct sockaddr_in cli_addr;
        socklen_t alen = sizeof(cli_addr);
        int cli = accept(srv, (struct sockaddr *)&cli_addr, &alen);
        if (cli < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli_addr.sin_addr, ip, sizeof(ip));
        log_line("client connected from %s:%u", ip, ntohs(cli_addr.sin_port));
        /* Listen mode is always TCP (the TV app uses --connect dial-out). */
        ctm_transport_t t;
        ctm_transport_init(&t, NULL);
        ctm_transport_attach_tcp(&t, cli);
        handle_client(&t, dev_path);
        ctm_transport_disconnect(&t);
        ctm_transport_destroy(&t);
        log_line("client closed");
    }
    close(srv);
    log_line("bye");
    return 0;
}
