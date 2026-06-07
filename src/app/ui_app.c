#include <SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/hidraw.h>
#endif

#include "lvgl.h"
#include "draw/sdl/lv_draw_sdl.h"
#include "ui_common.h"
#include "ctm_monitor.h"   /* D8 device detection (live connect/disconnect) */


static void render_device_list(void);

/* D8 device-detection monitor: runs on its own thread; its callback logs each
 * change and flags the UI to re-scan promptly, so the list is live. */
static ctm_monitor_t *g_monitor;
static volatile int g_monitor_dirty;

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y, int w,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text && text[0] ? text : "-");
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, w);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static void style_row(lv_obj_t *row, bool selected)
{
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(row, selected ? lv_color_hex(0x38bdf8) : lv_color_hex(0x263540), 0);
    lv_obj_set_style_bg_color(row, selected ? lv_color_hex(0x12384a) : lv_color_hex(0x18232c), 0);
    lv_obj_set_style_bg_color(row, selected ? lv_color_hex(0x16455d) : lv_color_hex(0x202e38), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
}

static void update_row_styles(void)
{
    for (int i = 0; i < g_devices.count; ++i) {
        if (g_row_buttons[i]) {
            style_row(g_row_buttons[i], i == g_selected_index);
        }
    }
}

static void discovery_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    bool old = g_agent_online;
    if (discover_agent_once() && !old) {
        log_append("Windows agent found at %s:%d", g_agent_host, g_agent_port);
    }
}

static void row_clicked_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= g_devices.count) {
        return;
    }
    g_selected_index = index;
    snprintf(g_selected_key, sizeof(g_selected_key), "%s", g_devices.items[index].key);
    update_row_styles();
    update_details();
}

static void plug_button_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= g_devices.count) {
        return;
    }

    logical_device_t *item = &g_devices.items[index];
    bool requested_state = !item->plugged;
    if (requested_state) {
        if (!plug_in_item(item)) {
            update_details();
            return;
        }
    } else {
        stop_session(item->key);
        log_append("plug out requested for %s", item->name);
    }

    item->plugged = requested_state;
    set_plug_key(item->key, item->plugged);
    g_selected_index = index;
    snprintf(g_selected_key, sizeof(g_selected_key), "%s", item->key);

    if (g_plug_labels[index]) {
        lv_label_set_text(g_plug_labels[index], item->plugged ? "Plug out" : "Plug in");
    }
    update_row_styles();
    update_details();
}

static void expand_button_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= g_devices.count) {
        return;
    }

    logical_device_t *item = &g_devices.items[index];
    if (!logical_device_can_expand(item)) {
        return;
    }

    bool expanded = !expand_key_is_set(item->key);
    set_expand_key(item->key, expanded);
    g_selected_index = index;
    snprintf(g_selected_key, sizeof(g_selected_key), "%s", item->key);
    render_device_list();
    update_details();
}

/* Puck expanded-view activity watch ----------------------------------------
 * Show connected/idle per controller slot from hidraw traffic alone (no byte
 * parsing -> the TV stays blind). The interface set comes from the puck's USB
 * device-dir walk (resolved via /sys/class/input -> /sys/devices; never the
 * flaky /sys/class/hidraw realpath, never /sys/bus). Watch only while the puck
 * row is expanded; release the fds when it collapses or gets bridged. */

static void puck_watch_close(void)
{
    for (int i = 0; i < g_puck_slot_count; ++i) {
        if (g_puck_slots[i].fd >= 0) close(g_puck_slots[i].fd);
    }
    g_puck_slot_count = 0;
    g_puck_watch_key[0] = '\0';
    g_puck_watch_plugged = false;
}

/* (Re)build the slot watch for the puck `item`: one entry per HID interface
 * (Controllers 0-3 + Service) from the USB device-dir walk, in interface order;
 * open each hidraw read-only/non-blocking unless the puck is bridged (then the
 * controller layer owns them). Re-syncs on key or plugged change. */
static void puck_watch_open(const logical_device_t *item)
{
    if (!item) return;
    if (strcmp(g_puck_watch_key, item->key) == 0 &&
        g_puck_watch_plugged == item->plugged) {
        return;
    }
    puck_watch_close();
    g_puck_watch_plugged = item->plugged;

    char usbdir[PATH_MAX];
    if (puck_usb_device_dir(item->vid, item->pid, usbdir, sizeof(usbdir)) == 0) {
        puck_if_t ifs[16];
        int nif = puck_enumerate_ifaces(usbdir, ifs, 16);
        for (int i = 0; i < nif && g_puck_slot_count < 8; ++i) {
            if (strncmp(ifs[i].cls, "03", 2) != 0 || !ifs[i].node[0]) continue;  /* HID only */
            puck_slot_t *slot = &g_puck_slots[g_puck_slot_count++];
            memset(slot, 0, sizeof(*slot));
            snprintf(slot->node, sizeof(slot->node), "%s", ifs[i].node);
            slot->iface = ifs[i].num;
            slot->fd = item->plugged ? -1
                                     : open(slot->node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        }
    }
    snprintf(g_puck_watch_key, sizeof(g_puck_watch_key), "%s", item->key);
}

static void render_device_list(void)
{
    lv_obj_clean(g_device_list);
    memset(g_row_buttons, 0, sizeof(g_row_buttons));
    memset(g_plug_labels, 0, sizeof(g_plug_labels));
    memset(g_expand_labels, 0, sizeof(g_expand_labels));

    if (g_devices.count == 0) {
        lv_obj_t *empty = lv_label_create(g_device_list);
        lv_label_set_text(empty, "No HID devices visible to the native app");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0xb6c5cf), 0);
        lv_obj_set_pos(empty, 18, 22);
        return;
    }

    const int row_h = 60;
    const int child_h = 38;
    const int gap = 8;
    const int row_w = g_device_list_width - 20;
    const int button_w = 112;
    const int expand_w = 78;
    int y = 0;

    for (int i = 0; i < g_devices.count; ++i) {
        const logical_device_t *item = &g_devices.items[i];
        const bool expandable = logical_device_can_expand(item);
        const bool expanded = expandable && expand_key_is_set(item->key);
        lv_obj_t *row = lv_obj_create(g_device_list);
        g_row_buttons[i] = row;
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_size(row, row_w, row_h);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (lv_group_get_default()) {
            lv_group_add_obj(lv_group_get_default(), row);
        }
        style_row(row, i == g_selected_index);

        int name_x = expandable ? expand_w + 18 : 16;
        if (expandable) {
            lv_obj_t *expand_btn = lv_btn_create(row);
            lv_obj_set_size(expand_btn, expand_w, 34);
            lv_obj_set_pos(expand_btn, 12, 13);
            lv_obj_set_style_radius(expand_btn, 6, 0);
            lv_obj_set_style_bg_color(expand_btn, expanded ? lv_color_hex(0x334155) : lv_color_hex(0x1f2933), 0);
            lv_obj_set_style_bg_color(expand_btn, lv_color_hex(0x475569), LV_STATE_PRESSED);
            lv_obj_add_event_cb(expand_btn, expand_button_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            if (lv_group_get_default()) {
                lv_group_add_obj(lv_group_get_default(), expand_btn);
            }
            g_expand_labels[i] = lv_label_create(expand_btn);
            lv_label_set_text(g_expand_labels[i], expanded ? "Hide" : "Show");
            lv_obj_set_style_text_font(g_expand_labels[i], &lv_font_montserrat_14, 0);
            lv_obj_center(g_expand_labels[i]);
        }

        make_label(row, item->name, name_x, 20, row_w - name_x - button_w - 30,
                   &lv_font_montserrat_18, lv_color_hex(0xf5f8fa));

        lv_obj_t *plug_btn = lv_btn_create(row);
        lv_obj_set_size(plug_btn, button_w, 38);
        lv_obj_set_pos(plug_btn, row_w - button_w - 14, 14);
        lv_obj_set_style_radius(plug_btn, 6, 0);
        lv_obj_set_style_bg_color(plug_btn, item->plugged ? lv_color_hex(0x7f1d1d) : lv_color_hex(0x0f766e), 0);
        lv_obj_set_style_bg_color(plug_btn, item->plugged ? lv_color_hex(0x991b1b) : lv_color_hex(0x0d9488), LV_STATE_PRESSED);
        lv_obj_add_event_cb(plug_btn, plug_button_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (lv_group_get_default()) {
            lv_group_add_obj(lv_group_get_default(), plug_btn);
        }
        g_plug_labels[i] = lv_label_create(plug_btn);
        lv_label_set_text(g_plug_labels[i], item->plugged ? "Plug out" : "Plug in");
        lv_obj_set_style_text_font(g_plug_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_center(g_plug_labels[i]);

        y += row_h + gap;

        if (expanded) {
            /* Puck (the only expandable device): read-only slot rows —
             * Controller 0-3 + Service — each showing its hidraw and whether it
             * currently has traffic. No per-row plug; the global Plug above
             * bridges the whole composite. */
            puck_watch_open(item);
            for (int s = 0; s < g_puck_slot_count; ++s) {
                puck_slot_t *slot = &g_puck_slots[s];
                lv_obj_t *subrow = lv_obj_create(g_device_list);
                lv_obj_set_pos(subrow, 28, y);
                lv_obj_set_size(subrow, row_w - 28, child_h);
                lv_obj_clear_flag(subrow, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_style_radius(subrow, 6, 0);
                lv_obj_set_style_border_width(subrow, 1, 0);
                lv_obj_set_style_border_color(subrow, lv_color_hex(0x263540), 0);
                lv_obj_set_style_bg_color(subrow, lv_color_hex(0x111a21), 0);
                lv_obj_set_style_pad_all(subrow, 0, 0);

                char tag[32];
                if (slot->iface >= 2 && slot->iface <= 5) {
                    snprintf(tag, sizeof(tag), "Controller %d", slot->iface - 2);
                } else if (slot->iface == 6) {
                    snprintf(tag, sizeof(tag), "Service");
                } else {
                    snprintf(tag, sizeof(tag), "Interface %d", slot->iface);
                }
                char line[160];
                snprintf(line, sizeof(line), "%s   %s", tag, slot->node);
                make_label(subrow, line, 14, 10, row_w - 168,
                           &lv_font_montserrat_14, lv_color_hex(0xb6c5cf));

                lv_obj_t *st = lv_label_create(subrow);   /* filled by status_timer_cb */
                lv_label_set_text(st, "...");
                lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(st, lv_color_hex(0xc7d4dc), 0);
                lv_obj_align(st, LV_ALIGN_RIGHT_MID, -10, 0);
                slot->status = st;

                y += child_h + 4;
            }
            y += gap;
        }
    }

    /* Stop watching the puck's slots once its row is no longer expanded. */
    if (g_puck_watch_key[0] && !expand_key_is_set(g_puck_watch_key)) {
        puck_watch_close();
    }
}

static void refresh_devices(void)
{
    enumerate_devices(&g_scan);
    build_logical_devices(&g_scan, &g_devices);
    publish_bt_macs();

    /* Stage 1: capture the puck's USB enumeration once, at physical presence,
     * so it's cached and ready before any bridge Plug in. */
    {
        bool puck_present = false;
        for (int i = 0; i < g_devices.count; ++i) {
            if (strcmp(bridge_kind_for_item(&g_devices.items[i]), "puck") == 0) {
                puck_enum_capture(g_devices.items[i].vid, g_devices.items[i].pid);
                puck_present = true;
                break;
            }
        }
        if (!puck_present) g_puck_enum.valid = 0;
    }

    int selected = -1;
    if (g_selected_key[0]) {
        for (int i = 0; i < g_devices.count; ++i) {
            if (strcmp(g_devices.items[i].key, g_selected_key) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && g_devices.count > 0) {
        selected = 0;
        snprintf(g_selected_key, sizeof(g_selected_key), "%s", g_devices.items[0].key);
    }
    g_selected_index = selected;

    char status[96];
    snprintf(status, sizeof(status), "%d device%s | Windows %s",
             g_devices.count,
             g_devices.count == 1 ? "" : "s",
             g_agent_online ? g_agent_host : "not found");
    lv_label_set_text(g_status_label, status);   /* cheap; refresh every tick */

    /* Re-render the list only when the device SET changed (count, keys, plug-
     * or expand-state). Selection-only changes are handled by the click path
     * (update_row_styles), so the 2 s scan no longer rebuilds the list every
     * tick — that full rebuild was the visible flicker / focus loss. Signature
     * is an FNV-1a hash of the rendered state. */
    uint64_t sig = 1469598103934665603ULL;       /* FNV-1a offset basis */
#define SIG_MIX(p, n) do {                                              \
        const unsigned char *_b = (const unsigned char *)(p);          \
        for (size_t _i = 0; _i < (size_t)(n); ++_i) {                   \
            sig ^= _b[_i];                                             \
            sig *= 1099511628211ULL;                                   \
        }                                                              \
    } while (0)
    SIG_MIX(&g_devices.count, sizeof(g_devices.count));
    for (int i = 0; i < g_devices.count; ++i) {
        const logical_device_t *item = &g_devices.items[i];
        unsigned char st = (unsigned char)((item->plugged ? 1 : 0) |
                                           (logical_device_can_expand(item) ? 2 : 0) |
                                           (expand_key_is_set(item->key) ? 4 : 0));
        SIG_MIX(item->key, strlen(item->key));
        SIG_MIX(&st, 1);
    }
#undef SIG_MIX

    static uint64_t s_last_sig;
    static bool s_have_rendered;
    if (!s_have_rendered || sig != s_last_sig) {
        s_last_sig = sig;
        s_have_rendered = true;
        render_device_list();
        update_details();
        log_append("scan inputs=%d hidraw=%d logical=%d",
                   g_scan.input_count, g_scan.count, g_devices.count);
    }
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh_devices();
}

static void refresh_button_cb(lv_event_t *event)
{
    (void)event;
    refresh_devices();
}

static void debug_button_cb(lv_event_t *event)
{
    (void)event;
    g_debug_visible = !g_debug_visible;
    if (g_debug_visible) {
        lv_obj_clear_flag(g_log_area, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_debug_button_label, "Hide Debug");
        log_append("debug console shown");
    } else {
        lv_obj_add_flag(g_log_area, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_debug_button_label, "Show Debug");
    }
}

static void keypad_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->key = g_key;
    if (g_key_pending) {
        data->state = LV_INDEV_STATE_PRESSED;
        g_key_pending = false;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void pointer_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->point.x = g_pointer_x;
    data->point.y = g_pointer_y;
    data->state = g_pointer_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void set_key(uint32_t key)
{
    g_key = key;
    g_key_pending = true;
}

static void handle_sdl_event(const SDL_Event *event)
{
    switch (event->type) {
        case SDL_QUIT:
            g_running = false;
            break;
        case SDL_MOUSEMOTION:
            g_pointer_x = event->motion.x;
            g_pointer_y = event->motion.y;
            break;
        case SDL_MOUSEBUTTONDOWN:
            g_pointer_x = event->button.x;
            g_pointer_y = event->button.y;
            g_pointer_down = true;
            break;
        case SDL_MOUSEBUTTONUP:
            g_pointer_x = event->button.x;
            g_pointer_y = event->button.y;
            g_pointer_down = false;
            break;
        case SDL_KEYDOWN:
            switch (event->key.keysym.sym) {
                case SDLK_UP: set_key(LV_KEY_UP); break;
                case SDLK_DOWN: set_key(LV_KEY_DOWN); break;
                case SDLK_LEFT: set_key(LV_KEY_LEFT); break;
                case SDLK_RIGHT: set_key(LV_KEY_RIGHT); break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                case SDLK_SPACE:
                    set_key(LV_KEY_ENTER);
                    break;
                case SDLK_TAB:
                    set_key(LV_KEY_NEXT);
                    break;
                case SDLK_BACKSPACE:
                case SDLK_ESCAPE:
                case SDLK_q:
                    g_running = false;
                    break;
#ifdef SDLK_AC_BACK
                case SDLK_AC_BACK:
                    g_running = false;
                    break;
#endif
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

static void display_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *src)
{
    LV_UNUSED(src);
    if (area->x2 < 0 || area->y2 < 0 ||
        area->x1 > disp_drv->hor_res - 1 || area->y1 > disp_drv->ver_res - 1) {
        lv_disp_flush_ready(disp_drv);
        return;
    }

    if (lv_disp_flush_is_last(disp_drv)) {
        SDL_SetRenderTarget(g_renderer, NULL);
        SDL_SetRenderDrawColor(g_renderer, 17, 22, 26, 255);
        SDL_RenderClear(g_renderer);
        SDL_SetTextureBlendMode(g_texture, SDL_BLENDMODE_BLEND);
        SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
        SDL_RenderPresent(g_renderer);
        SDL_SetRenderTarget(g_renderer, g_texture);
    }
    lv_disp_flush_ready(disp_drv);
}

static int init_lvgl_display(int width, int height)
{
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) {
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_renderer) {
        log_append("SDL_CreateRenderer failed: %s", SDL_GetError());
        return -1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    g_texture = lv_draw_sdl_create_screen_texture(g_renderer, width, height);
    if (!g_texture) {
        log_append("lv_draw_sdl_create_screen_texture failed: %s", SDL_GetError());
        return -1;
    }

    lv_disp_draw_buf_init(&g_draw_buf, g_texture, NULL, (uint32_t)(width * height));
    lv_disp_drv_init(&g_disp_drv);
    g_sdl_param.renderer = g_renderer;
    g_sdl_param.user_data = NULL;
    g_disp_drv.user_data = &g_sdl_param;
    g_disp_drv.draw_buf = &g_draw_buf;
    g_disp_drv.flush_cb = display_flush_cb;
    g_disp_drv.hor_res = (lv_coord_t)width;
    g_disp_drv.ver_res = (lv_coord_t)height;
    g_disp_drv.dpi = 160;
    SDL_SetRenderTarget(g_renderer, g_texture);

    lv_disp_t *disp = lv_disp_drv_register(&g_disp_drv);
    if (!disp) {
        log_append("lv_disp_drv_register failed");
        return -1;
    }
    lv_disp_set_default(disp);
    return 0;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 150, 44);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return label;
}

static void build_ui(int width, int height)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x11161a), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, width, 72);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x18232c), 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "CTM Device Bridge");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xf5f8fa), 0);
    lv_obj_set_pos(title, 28, 20);

    g_status_label = lv_label_create(header);
    lv_label_set_text(g_status_label, "starting");
    lv_obj_set_style_text_font(g_status_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xaab6bf), 0);
    lv_obj_align(g_status_label, LV_ALIGN_RIGHT_MID, -350, 0);

    lv_obj_t *refresh_label = make_button(header, "Refresh", refresh_button_cb);
    lv_obj_set_pos(lv_obj_get_parent(refresh_label), width - 330, 14);

    g_debug_button_label = make_button(header, "Hide Debug", debug_button_cb);
    lv_obj_set_pos(lv_obj_get_parent(g_debug_button_label), width - 170, 14);

    int margin = 24;
    int top = 96;
    int content_h = height - top - margin;
    int left_w = (width * 42) / 100;
    if (left_w < 520) left_w = 520;
    if (left_w > width - 620) left_w = width - 620;
    int right_x = margin + left_w + 16;
    int right_w = width - right_x - margin;

    lv_obj_t *left = lv_obj_create(scr);
    lv_obj_set_pos(left, margin, top);
    lv_obj_set_size(left, left_w, content_h);
    lv_obj_set_style_radius(left, 8, 0);
    lv_obj_set_style_bg_color(left, lv_color_hex(0x121a20), 0);
    lv_obj_set_style_border_width(left, 1, 0);
    lv_obj_set_style_border_color(left, lv_color_hex(0x2c3d49), 0);
    lv_obj_set_style_pad_all(left, 0, 0);

    lv_obj_t *devices_title = lv_label_create(left);
    lv_label_set_text(devices_title, "Devices");
    lv_obj_set_style_text_font(devices_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(devices_title, lv_color_hex(0xf5f8fa), 0);
    lv_obj_set_pos(devices_title, 18, 14);

    lv_obj_t *devices_hint = lv_label_create(left);
    lv_label_set_text(devices_hint, "Live HID devices");
    lv_label_set_long_mode(devices_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(devices_hint, left_w - 220);
    lv_obj_set_style_text_font(devices_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(devices_hint, lv_color_hex(0x8fa4af), 0);
    lv_obj_set_pos(devices_hint, 118, 18);

    g_device_list_width = left_w - 24;
    g_device_list = lv_obj_create(left);
    lv_obj_set_pos(g_device_list, 12, 56);
    lv_obj_set_size(g_device_list, g_device_list_width, content_h - 68);
    lv_obj_set_style_bg_opa(g_device_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_device_list, 0, 0);
    lv_obj_set_style_pad_all(g_device_list, 0, 0);
    lv_obj_set_scrollbar_mode(g_device_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *right = lv_obj_create(scr);
    lv_obj_set_pos(right, right_x, top);
    lv_obj_set_size(right, right_w, content_h);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(right, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(right, 8, 0);
    lv_obj_set_style_bg_color(right, lv_color_hex(0x10171d), 0);
    lv_obj_set_style_border_width(right, 1, 0);
    lv_obj_set_style_border_color(right, lv_color_hex(0x2c3d49), 0);
    lv_obj_set_style_pad_all(right, 16, 0);

    lv_obj_t *detail_title = lv_label_create(right);
    lv_label_set_text(detail_title, "Details");
    lv_obj_set_style_text_font(detail_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(detail_title, lv_color_hex(0xf5f8fa), 0);
    lv_obj_set_pos(detail_title, 0, 0);

    g_detail_panel = lv_obj_create(right);
    lv_obj_set_pos(g_detail_panel, 0, 44);
    lv_obj_set_size(g_detail_panel, right_w - 28, content_h - 292);
    lv_obj_set_style_bg_opa(g_detail_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_detail_panel, 0, 0);
    lv_obj_set_style_pad_all(g_detail_panel, 0, 0);
    lv_obj_clear_flag(g_detail_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_detail_panel, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(g_detail_panel, LV_SCROLLBAR_MODE_OFF);

    g_log_area = lv_textarea_create(right);
    lv_obj_set_pos(g_log_area, 0, content_h - 230);
    lv_obj_set_size(g_log_area, right_w - 28, 214);
    lv_textarea_set_cursor_click_pos(g_log_area, false);
    lv_textarea_set_one_line(g_log_area, false);
    lv_textarea_set_text(g_log_area, "");
    lv_obj_set_style_text_font(g_log_area, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(g_log_area, lv_color_hex(0x0b1116), 0);
    lv_obj_set_style_border_color(g_log_area, lv_color_hex(0x263540), 0);

    lv_group_t *group = lv_group_create();
    lv_group_set_default(group);
    lv_group_add_obj(group, lv_obj_get_parent(refresh_label));
    lv_group_add_obj(group, lv_obj_get_parent(g_debug_button_label));
}

static int create_window(int *width, int *height)
{
    SDL_DisplayMode mode;
    int win_w = 1280;
    int win_h = 720;
    if (SDL_GetDisplayMode(0, 0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
        win_w = mode.w;
        win_h = mode.h;
    }

    g_window = SDL_CreateWindow("CTM Device Bridge",
                                SDL_WINDOWPOS_UNDEFINED,
                                SDL_WINDOWPOS_UNDEFINED,
                                win_w,
                                win_h,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_window) {
        log_append("SDL_CreateWindow failed: %s", SDL_GetError());
        return -1;
    }
    SDL_GetWindowSize(g_window, width, height);
    if (*width <= 0 || *height <= 0) {
        *width = win_w;
        *height = win_h;
    }
    return 0;
}

/* Route controller log lines into the app's buffered console. Thread-safe
 * (log_append buffers under a mutex; the UI timer renders it). When: registered
 * once at startup as the controller log sink. */
static void ui_controller_log(const char *line)
{
    log_append("%s", line);
}

/* UI-thread timer: flush the buffered log to the on-screen console (~250 ms),
 * so a burst of lines doesn't re-render the textarea per line. */
static void log_flush_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ctm_ui_log_flush();
}

/* Monitor thread: a controller appeared (present=1) or disappeared (present=0).
 * Log it (classified by the controller factory + identified) and flag the UI to
 * re-scan. Thread-safe: touches only log_append + an int flag, never LVGL. */
static void ui_monitor_cb(void *ud, const ctm_controller_dev_t *dev, int present)
{
    (void)ud;
    const ctm_controller_ops_t *ops = ctm_controller_ops_for(dev);
    log_append("monitor %s %s [%s:%s %s] %s",
               present ? "connect" : "disconnect",
               ops ? ops->kind : "?",
               dev->vid[0] ? dev->vid : "----",
               dev->pid[0] ? dev->pid : "----",
               dev->bus[0] ? dev->bus : "-",
               dev->path[0] ? dev->path : dev->name);
    g_monitor_dirty = 1;
}

/* UI-thread timer: if the monitor flagged a change, re-scan + re-render now
 * (event-driven liveness) rather than waiting for the 2 s fallback tick. */
static void monitor_poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_monitor_dirty) {
        g_monitor_dirty = 0;
        refresh_devices();
    }
}

/* UI-thread timer: refresh the detail panel's live-status strip for the
 * selected device — connected/transport, input Hz (computed from the report
 * counter delta), output count, and the controller's last event. ~500 ms. */
static void status_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Puck expanded view: drain each watched slot hidraw for traffic and show
     * connected / none from activity alone (no byte interpretation). Runs every
     * tick regardless of selection so the slots stay live while expanded. */
    if (g_puck_slot_count > 0) {
        struct timespec pts;
        clock_gettime(CLOCK_MONOTONIC, &pts);
        uint64_t now_ms = (uint64_t)pts.tv_sec * 1000ull + (uint64_t)pts.tv_nsec / 1000000ull;
        for (int s = 0; s < g_puck_slot_count; ++s) {
            puck_slot_t *slot = &g_puck_slots[s];
            if (slot->fd >= 0) {
                uint8_t buf[256];
                while (read(slot->fd, buf, sizeof(buf)) > 0) slot->last_ms = now_ms;
            }
            if (!slot->status) continue;
            const char *txt;
            uint32_t col;
            if (slot->fd < 0) {
                txt = g_puck_watch_plugged ? "bridging" : "off";
                col = 0x7f8c98;
            } else if (now_ms - slot->last_ms < 2000) {
                txt = LV_SYMBOL_OK " connected";
                col = 0x4ade80;
            } else {
                txt = "no controller";
                col = 0x7f8c98;
            }
            lv_obj_set_style_text_color(slot->status, lv_color_hex(col), 0);
            lv_label_set_text(slot->status, txt);
        }
    }

    if (!g_detail_status_label) {
        return;
    }
    if (g_selected_index < 0 || g_selected_index >= g_devices.count) {
        lv_label_set_text(g_detail_status_label, "");
        return;
    }
    const logical_device_t *item = &g_devices.items[g_selected_index];
    int si = session_index_for_key(item->key);
    if (si < 0) {
        /* Fall back to a per-interface (node) session for this device, so the
         * status strip reflects a hidraw plugged via a sub-row button — its
         * input Hz tells you whether that interface carries gamepad data. */
        size_t klen = strlen(item->key);
        for (int i = 0; i < g_session_count; ++i) {
            if (strncmp(g_sessions[i].key, item->key, klen) == 0 &&
                g_sessions[i].key[klen] == '#') {
                si = i;
                break;
            }
        }
    }
    ctm_controller_t *ctrl = (si >= 0) ? g_sessions[si].controller : NULL;
    if (!ctrl) {
        lv_label_set_text(g_detail_status_label, LV_SYMBOL_STOP "  not bridging");
        return;
    }

    ctm_controller_status_t st;
    ctm_controller_get_status(ctrl, &st);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;

    /* Input rate = delta(reports_in)/delta(t); reset when the selected
     * controller changes so a switch doesn't show a bogus spike. */
    static const void *prev_ctrl;
    static unsigned long prev_in;
    static uint64_t prev_us;
    double in_hz = 0.0;
    if (prev_ctrl == (const void *)ctrl && prev_us != 0 && now > prev_us) {
        double dt = (double)(now - prev_us) / 1000000.0;
        if (dt > 0.0 && st.reports_in >= prev_in) {
            in_hz = (double)(st.reports_in - prev_in) / dt;
        }
    }
    prev_ctrl = (const void *)ctrl;
    prev_in = st.reports_in;
    prev_us = now;

    char line[200];
    snprintf(line, sizeof(line), "%s  %s | in %.0f Hz (%lu) | out %lu%s%s",
             st.connected ? LV_SYMBOL_OK : LV_SYMBOL_REFRESH,
             st.transport_enet ? "UDP" : "TCP",
             in_hz, st.reports_in, st.reports_out,
             st.last_event[0] ? "  |  " : "",
             st.last_event);
    lv_label_set_text(g_detail_status_label, line);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    lv_init();

    int width = 0;
    int height = 0;
    if (create_window(&width, &height) != 0 || init_lvgl_display(width, height) != 0) {
        SDL_Quit();
        return 1;
    }

    build_ui(width, height);

    static lv_indev_drv_t keypad_drv;
    lv_indev_drv_init(&keypad_drv);
    keypad_drv.type = LV_INDEV_TYPE_KEYPAD;
    keypad_drv.read_cb = keypad_read_cb;
    lv_indev_t *keypad = lv_indev_drv_register(&keypad_drv);
    lv_indev_set_group(keypad, lv_group_get_default());

    static lv_indev_drv_t pointer_drv;
    lv_indev_drv_init(&pointer_drv);
    pointer_drv.type = LV_INDEV_TYPE_POINTER;
    pointer_drv.read_cb = pointer_read_cb;
    lv_indev_drv_register(&pointer_drv);

    log_append("SDL/LVGL UI started");
    log_append("uid=%u gid=%u", (unsigned)getuid(), (unsigned)getgid());
    log_append("/sys entries=%d /sys/class/input entries=%d /dev entries=%d /dev/input entries=%d",
               count_dir_entries("/sys"),
               count_dir_entries("/sys/class/input"),
               count_dir_entries("/dev"),
               count_dir_entries("/dev/input"));
    if (pthread_create(&g_stop_sniff_thread, NULL, stop_sniff_worker, NULL) == 0) {
        g_stop_sniff_thread_started = true;
        log_append("stopSniff worker interval=500ms");
    } else {
        log_append("stopSniff worker start failed");
    }
    discover_agent_once();
    refresh_devices();
    lv_timer_create(refresh_timer_cb, 2000, NULL);
    lv_timer_create(discovery_timer_cb, 2000, NULL);
    lv_timer_create(log_flush_timer_cb, 250, NULL);
    ctm_controller_set_log_sink(ui_controller_log);
    g_monitor = ctm_monitor_start(ui_monitor_cb, NULL);
    log_append(g_monitor ? "monitor started (rescan 1s)" : "monitor start failed");
    lv_timer_create(monitor_poll_timer_cb, 250, NULL);
    lv_timer_create(status_timer_cb, 500, NULL);

    while (g_running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            handle_sdl_event(&event);
        }
        lv_timer_handler();
        SDL_Delay(5);
    }

    ctm_monitor_stop(g_monitor);
    if (g_texture) {
        SDL_DestroyTexture(g_texture);
    }
    if (g_renderer) {
        SDL_DestroyRenderer(g_renderer);
    }
    if (g_window) {
        SDL_DestroyWindow(g_window);
    }
    if (g_stop_sniff_thread_started) {
        pthread_join(g_stop_sniff_thread, NULL);
    }
    release_local_sessions_on_exit();
    SDL_Quit();
    return 0;
}
