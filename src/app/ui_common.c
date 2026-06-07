/* UI definition unit (SDL + LVGL): the display, widget globals and standalone-app
 * input state, plus the UI-thread log flush. Headless state and helpers moved to
 * ctm_state.c; declared extern via ctm_state.h (included by ui_common.h). */

#include "ui_common.h"

SDL_Window *g_window;
SDL_Renderer *g_renderer;
SDL_Texture *g_texture;
lv_disp_draw_buf_t g_draw_buf;
lv_disp_drv_t g_disp_drv;
lv_draw_sdl_drv_param_t g_sdl_param;

lv_obj_t *g_device_list;
lv_obj_t *g_status_label;
lv_obj_t *g_detail_panel;
lv_obj_t *g_log_area;
lv_obj_t *g_debug_button_label;
lv_obj_t *g_latency_value_label;
lv_obj_t *g_haptics_value_label;
lv_obj_t *g_headset_volume_value_label;
lv_obj_t *g_speaker_volume_value_label;
lv_obj_t *g_detail_status_label;
puck_slot_t g_puck_slots[8];
int g_puck_slot_count;
char g_puck_watch_key[96];
bool g_puck_watch_plugged;
lv_obj_t *g_ds5_patch_high_value_label;
lv_obj_t *g_ds5_patch_low_value_label;
lv_obj_t *g_ds5_patch2_high_value_label;
lv_obj_t *g_ds5_patch2_low_value_label;
lv_obj_t *g_row_buttons[MAX_DEVICES];
lv_obj_t *g_plug_labels[MAX_DEVICES];
lv_obj_t *g_expand_labels[MAX_DEVICES];
int g_device_list_width;
bool g_debug_visible = true;

uint32_t g_key;
bool g_key_pending;
int g_pointer_x;
int g_pointer_y;
bool g_pointer_down;

/* Flush the buffered log into the on-screen console. UI thread only (touches
 * LVGL); the mutex is held across the update so a concurrent log_append() on a
 * controller thread can't mutate g_log mid-render. The buffer (g_log), the mutex
 * (g_log_mutex) and the dirty flag (g_log_dirty) live in ctm_state.c. When: a
 * ~250 ms UI timer. */
void ctm_ui_log_flush(void)
{
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_dirty && g_log_area) {
        lv_textarea_set_text(g_log_area, g_log);
        lv_textarea_set_cursor_pos(g_log_area, LV_TEXTAREA_CURSOR_LAST);
        g_log_dirty = false;
    }
    pthread_mutex_unlock(&g_log_mutex);
}
