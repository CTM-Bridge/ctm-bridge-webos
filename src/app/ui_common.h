#ifndef CTM_UI_COMMON_H
#define CTM_UI_COMMON_H

/* UI half (LVGL + SDL) of the app's shared state for the ctm_bridge_lvgl_ui app.
 * The headless types, constants, global state and the enumeration/agent/plug API
 * now live in ctm_state.h (included below); here we keep the SDL/LVGL display,
 * the widget globals, the standalone-app input state, and the per-controller
 * detail windows (ui_window_*.c). */

#include <SDL.h>

#include "lvgl.h"
#include "draw/sdl/lv_draw_sdl.h"

#include "ctm_state.h"   /* headless types, constants, state + core API */

/* Puck expanded-view activity watch: one entry per HID interface (slot). The UI
 * drains each hidraw for traffic to show connected/idle — pure activity, no byte
 * interpretation (the TV stays blind). Holds an lv_obj_t row label, so it is UI. */
typedef struct {
    char node[64];       /* /dev/hidrawN */
    int  iface;          /* USB interface number (2-5 = Controller 0-3, 6 = service) */
    int  fd;             /* O_RDONLY|O_NONBLOCK; -1 = not watching */
    uint64_t last_ms;    /* CLOCK_MONOTONIC ms of last traffic */
    lv_obj_t *status;    /* row status label (rebound each render) */
} puck_slot_t;

/* ---- UI global state (SDL + LVGL; defined in ui_common.c) ---------------- */
extern SDL_Window *g_window;
extern SDL_Renderer *g_renderer;
extern SDL_Texture *g_texture;
extern lv_disp_draw_buf_t g_draw_buf;
extern lv_disp_drv_t g_disp_drv;
extern lv_draw_sdl_drv_param_t g_sdl_param;

extern lv_obj_t *g_device_list;
extern lv_obj_t *g_status_label;
extern lv_obj_t *g_detail_panel;
extern lv_obj_t *g_log_area;
extern lv_obj_t *g_debug_button_label;
extern lv_obj_t *g_latency_value_label;
extern lv_obj_t *g_haptics_value_label;
extern lv_obj_t *g_headset_volume_value_label;
extern lv_obj_t *g_speaker_volume_value_label;
extern lv_obj_t *g_detail_status_label;   /* live bridging-status strip */
extern puck_slot_t g_puck_slots[8];        /* puck expanded-view activity rows */
extern int g_puck_slot_count;
extern char g_puck_watch_key[96];          /* logical key whose slots we're watching */
extern bool g_puck_watch_plugged;          /* the watched puck is bridged (fds released) */
extern lv_obj_t *g_ds5_patch_high_value_label;
extern lv_obj_t *g_ds5_patch_low_value_label;
extern lv_obj_t *g_ds5_patch2_high_value_label;
extern lv_obj_t *g_ds5_patch2_low_value_label;
extern lv_obj_t *g_row_buttons[MAX_DEVICES];
extern lv_obj_t *g_plug_labels[MAX_DEVICES];
extern lv_obj_t *g_expand_labels[MAX_DEVICES];
extern int g_device_list_width;
extern bool g_debug_visible;

extern uint32_t g_key;
extern bool g_key_pending;
extern int g_pointer_x;
extern int g_pointer_y;
extern bool g_pointer_down;

/* ---- ui_common.c: UI-thread log flush (pushes the shared g_log buffer to the
 * on-screen console; the buffer + helpers live in ctm_state.c) ---- */
void ctm_ui_log_flush(void);

/* ---- per-controller detail windows (ui_window_*.c) ---- */
const char *audio_mode_name(tv_bridge_audio_mode_t mode);
lv_obj_t *detail_label(lv_obj_t *parent, const char *text, int x, int y, int w,
                       const lv_font_t *font, lv_color_t color);
int detail_add_identity(lv_obj_t *parent, const logical_device_t *item, int y, int width);
int detail_add_audio_modes(lv_obj_t *parent, const tv_bridge_worker_settings_t *settings,
                           int y, int width);
int detail_add_slider(lv_obj_t *parent, const char *label_text, int value, int min, int max,
                      int y, int width, int which);
void build_generic_detail(lv_obj_t *parent, const logical_device_t *item, int width);
void build_ds4_detail(lv_obj_t *parent, const logical_device_t *item,
                      tv_bridge_worker_settings_t *settings, int width);
void build_ds5_detail(lv_obj_t *parent, const logical_device_t *item,
                      tv_bridge_worker_settings_t *settings, int width);
void build_xbox_detail(lv_obj_t *parent, const logical_device_t *item,
                       tv_bridge_worker_settings_t *settings, int width);
void build_steam_puck_detail(lv_obj_t *parent, const logical_device_t *item, int width);
void update_details(void);

#endif /* CTM_UI_COMMON_H */
