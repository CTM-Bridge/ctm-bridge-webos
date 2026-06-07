/* Shared detail-panel widgets + the update_details() dispatcher. The
 * per-controller panels live in ui_window_<kind>.c. Moved verbatim out of
 * lvgl_ui.c; de-static'd and prototyped in ui_common.h. */

#define _GNU_SOURCE

#include "ui_common.h"

#include <stdio.h>
#include <string.h>
const char *audio_mode_name(tv_bridge_audio_mode_t mode)
{
    switch (mode) {
        case TV_BRIDGE_AUDIO_AUTO: return "Auto";
        case TV_BRIDGE_AUDIO_SPEAKER: return "Speaker";
        case TV_BRIDGE_AUDIO_HEADSET: return "Headset";
        case TV_BRIDGE_AUDIO_BOTH: return "Speaker + headset";
        case TV_BRIDGE_AUDIO_OFF:
        default: return "Off";
    }
}

lv_obj_t *detail_label(lv_obj_t *parent, const char *text, int x, int y, int w,
                              const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text && text[0] ? text : "-");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, w);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_line_space(label, 4, 0);
    return label;
}

void audio_mode_cb(lv_event_t *event)
{
    int mode = (int)(intptr_t)lv_event_get_user_data(event);
    if (g_selected_index < 0 || g_selected_index >= g_devices.count) return;
    logical_device_t *item = &g_devices.items[g_selected_index];
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    if (!settings) return;
    settings->audio_mode = (tv_bridge_audio_mode_t)mode;
    apply_settings_to_session(item);
    log_append("%s audio output %s", item->name, audio_mode_name(settings->audio_mode));
    update_details();
}

void slider_cb(lv_event_t *event)
{
    int which = (int)(intptr_t)lv_event_get_user_data(event);
    if (g_selected_index < 0 || g_selected_index >= g_devices.count) return;
    logical_device_t *item = &g_devices.items[g_selected_index];
    ui_device_settings_t *record = ui_record_for_item(item);
    if (!record) return;
    tv_bridge_worker_settings_t *settings = &record->settings;

    lv_obj_t *slider = lv_event_get_target(event);
    int value = (int)lv_slider_get_value(slider);
    char text[32];
    if (which == 1) {
        settings->latency_ms = (unsigned int)value;
        snprintf(text, sizeof(text), "%d ms", value);
        if (g_latency_value_label) lv_label_set_text(g_latency_value_label, text);
        apply_settings_to_session(item);
    } else if (which == 2) {
        settings->haptics_gain_centi = (unsigned int)value;
        snprintf(text, sizeof(text), "%u.%02u", settings->haptics_gain_centi / 100,
                 settings->haptics_gain_centi % 100);
        if (g_haptics_value_label) lv_label_set_text(g_haptics_value_label, text);
        apply_settings_to_session(item);
    } else if (which == 3) {
        record->headset_volume_percent = (unsigned int)value;
        settings->headset_volume_percent = (unsigned int)value;
        snprintf(text, sizeof(text), "%d", value);
        if (g_headset_volume_value_label) lv_label_set_text(g_headset_volume_value_label, text);
        apply_settings_to_session(item);
        log_append("%s headset volume %d", item->name, value);
    } else if (which == 4) {
        record->speaker_volume_percent = (unsigned int)value;
        settings->speaker_volume_percent = (unsigned int)value;
        snprintf(text, sizeof(text), "%d", value);
        if (g_speaker_volume_value_label) lv_label_set_text(g_speaker_volume_value_label, text);
        apply_settings_to_session(item);
        log_append("%s speaker volume %d", item->name, value);
    } else if (which == 5) {
        settings->ds5_patch_high_nibble = (unsigned int)(value & 0x0f);
        snprintf(text, sizeof(text), "%X", value & 0x0f);
        if (g_ds5_patch_high_value_label) lv_label_set_text(g_ds5_patch_high_value_label, text);
        apply_settings_to_session(item);
        log_append("%s 0x90 patch high nibble %X", item->name, value & 0x0f);
    } else if (which == 6) {
        settings->ds5_patch_low_nibble = (unsigned int)(value & 0x0f);
        snprintf(text, sizeof(text), "%X", value & 0x0f);
        if (g_ds5_patch_low_value_label) lv_label_set_text(g_ds5_patch_low_value_label, text);
        apply_settings_to_session(item);
        log_append("%s 0x90 patch low nibble %X", item->name, value & 0x0f);
    } else if (which == 7) {
        settings->ds5_patch2_high_nibble = (unsigned int)(value & 0x0f);
        snprintf(text, sizeof(text), "%X", value & 0x0f);
        if (g_ds5_patch2_high_value_label) lv_label_set_text(g_ds5_patch2_high_value_label, text);
        apply_settings_to_session(item);
        log_append("%s 0x90 second byte high nibble %X", item->name, value & 0x0f);
    } else if (which == 8) {
        settings->ds5_patch2_low_nibble = (unsigned int)(value & 0x0f);
        snprintf(text, sizeof(text), "%X", value & 0x0f);
        if (g_ds5_patch2_low_value_label) lv_label_set_text(g_ds5_patch2_low_value_label, text);
        apply_settings_to_session(item);
        log_append("%s 0x90 second byte low nibble %X", item->name, value & 0x0f);
    }
}

int detail_add_identity(lv_obj_t *parent, const logical_device_t *item, int y, int width)
{
    char line[256];
    snprintf(line, sizeof(line), "Transport %s   VID:PID %s:%s   State %s",
             bus_label(item->bus),
             item->vid[0] ? item->vid : "----",
             item->pid[0] ? item->pid : "----",
             item->plugged ? "plugged" : "not plugged");
    detail_label(parent, line, 0, y, width, &lv_font_montserrat_14, lv_color_hex(0xc7d4dc));
    y += 32;
    snprintf(line, sizeof(line), "MAC %s   USB busid %s",
             item->mac[0] ? item->mac : "-",
             item->usb_busid[0] ? item->usb_busid : "-");
    detail_label(parent, line, 0, y, width, &lv_font_montserrat_14, lv_color_hex(0x9fb2bd));
    return y + 38;
}

int detail_add_audio_modes(lv_obj_t *parent, const tv_bridge_worker_settings_t *settings,
                                  int y, int width)
{
    detail_label(parent, "Audio output", 0, y, width, &lv_font_montserrat_16, lv_color_hex(0xf5f8fa));
    y += 30;

    const char *labels[] = {"Auto", "Off", "Speaker", "Headset", "Both"};
    const int modes[] = {
        TV_BRIDGE_AUDIO_AUTO,
        TV_BRIDGE_AUDIO_OFF,
        TV_BRIDGE_AUDIO_SPEAKER,
        TV_BRIDGE_AUDIO_HEADSET,
        TV_BRIDGE_AUDIO_BOTH
    };
    int gap = 8;
    int button_w = (width - gap * 4) / 5;
    if (button_w < 70) button_w = 70;
    for (int i = 0; i < 5; ++i) {
        bool selected = settings && settings->audio_mode == (tv_bridge_audio_mode_t)modes[i];
        lv_obj_t *button = lv_btn_create(parent);
        lv_obj_set_pos(button, i * (button_w + gap), y);
        lv_obj_set_size(button, button_w, 38);
        lv_obj_set_style_radius(button, 6, 0);
        lv_obj_set_style_bg_color(button, selected ? lv_color_hex(0x0f766e) : lv_color_hex(0x1f2933), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x0d9488), LV_STATE_PRESSED);
        lv_obj_add_event_cb(button, audio_mode_cb, LV_EVENT_CLICKED, (void *)(intptr_t)modes[i]);
        if (lv_group_get_default()) lv_group_add_obj(lv_group_get_default(), button);

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_center(label);
    }
    return y + 58;
}

int detail_add_slider(lv_obj_t *parent, const char *label_text, int value, int min, int max,
                             int y, int width, int which)
{
    int label_w = 150;
    int value_w = 64;
    int gap = 12;
    int slider_x = label_w + gap;
    int slider_w = width - slider_x - value_w - gap;
    if (slider_w < 120) {
        label_w = 122;
        slider_x = label_w + gap;
        slider_w = width - slider_x - value_w - gap;
    }
    if (slider_w < 80) slider_w = 80;

    detail_label(parent, label_text, 0, y + 2, label_w, &lv_font_montserrat_14, lv_color_hex(0xf5f8fa));
    char value_text[32];
    if (which == 1) {
        snprintf(value_text, sizeof(value_text), "%d ms", value);
    } else if (which == 2) {
        snprintf(value_text, sizeof(value_text), "%d.%02d", value / 100, value % 100);
    } else if (which == 3 || which == 4) {
        snprintf(value_text, sizeof(value_text), "%d", value);
    } else if (which >= 5 && which <= 8) {
        snprintf(value_text, sizeof(value_text), "%X", value & 0x0f);
    } else {
        snprintf(value_text, sizeof(value_text), "%d%%", value);
    }
    lv_obj_t *value_label = detail_label(parent, value_text, slider_x + slider_w + gap, y + 2, value_w,
                                         &lv_font_montserrat_14, lv_color_hex(0x7dd3fc));
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    if (which == 1) g_latency_value_label = value_label;
    if (which == 2) g_haptics_value_label = value_label;
    if (which == 3) g_headset_volume_value_label = value_label;
    if (which == 4) g_speaker_volume_value_label = value_label;
    if (which == 5) g_ds5_patch_high_value_label = value_label;
    if (which == 6) g_ds5_patch_low_value_label = value_label;
    if (which == 7) g_ds5_patch2_high_value_label = value_label;
    if (which == 8) g_ds5_patch2_low_value_label = value_label;

    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_pos(slider, slider_x, y + 8);
    lv_obj_set_size(slider, slider_w, 16);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_clear_flag(slider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x1f2933), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x38bdf8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xe0f2fe), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)which);
    if (lv_group_get_default()) lv_group_add_obj(lv_group_get_default(), slider);
    return y + 42;
}

void update_details(void)
{
    if (!g_detail_panel) return;
    lv_obj_clean(g_detail_panel);
    g_latency_value_label = NULL;
    g_haptics_value_label = NULL;
    g_headset_volume_value_label = NULL;
    g_speaker_volume_value_label = NULL;
    g_ds5_patch_high_value_label = NULL;
    g_ds5_patch_low_value_label = NULL;
    g_ds5_patch2_high_value_label = NULL;
    g_ds5_patch2_low_value_label = NULL;
    g_detail_status_label = NULL;

    if (g_selected_index < 0 || g_selected_index >= g_devices.count) {
        detail_label(g_detail_panel, "No device selected", 0, 0, lv_obj_get_width(g_detail_panel),
                     &lv_font_montserrat_20, lv_color_hex(0xf5f8fa));
        detail_label(g_detail_panel, "Click a row to select a device.",
                     0, 42, lv_obj_get_width(g_detail_panel),
                     &lv_font_montserrat_14, lv_color_hex(0x9fb2bd));
        return;
    }

    logical_device_t *item = &g_devices.items[g_selected_index];
    tv_bridge_worker_settings_t *settings = settings_for_item(item);
    int width = lv_obj_get_width(g_detail_panel);
    if (width < 320) width = 520;
    width -= 8;

    const char *kind = bridge_kind_for_item(item);
    if (strcmp(kind, "ds5") == 0 && settings) {
        build_ds5_detail(g_detail_panel, item, settings, width);
    } else if (strcmp(kind, "ds4") == 0 && settings) {
        build_ds4_detail(g_detail_panel, item, settings, width);
    } else if (strcmp(kind, "xbox") == 0) {
        build_xbox_detail(g_detail_panel, item, settings, width);
    } else if (strncmp(item->key, "steam:", 6) == 0) {
        build_steam_puck_detail(g_detail_panel, item, width);
    } else {
        build_generic_detail(g_detail_panel, item, width);
    }

    /* Live bridging-status strip pinned to the panel bottom; filled by the UI
     * status timer (status_timer_cb). Recreated on each rebuild — the no-
     * selection path above leaves g_detail_status_label NULL so the timer
     * no-ops and never touches a freed object. */
    g_detail_status_label = lv_label_create(g_detail_panel);
    lv_label_set_long_mode(g_detail_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g_detail_status_label, width);
    lv_obj_set_style_text_font(g_detail_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_detail_status_label, lv_color_hex(0x7dd3fc), 0);
    lv_obj_set_style_bg_color(g_detail_status_label, lv_color_hex(0x0b1015), 0);
    lv_obj_set_style_bg_opa(g_detail_status_label, LV_OPA_80, 0);
    lv_obj_set_style_pad_ver(g_detail_status_label, 3, 0);
    lv_obj_set_style_pad_hor(g_detail_status_label, 6, 0);
    lv_label_set_text(g_detail_status_label, "");
    lv_obj_align(g_detail_status_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

