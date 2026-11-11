#include "ui.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "rtc_pcf85063.h"
#include "imu_qmi8658.h"
#include <stdio.h>
#include <stdint.h>

#include <string.h>

#include "notifications.h"
#include "music.h"

// Forward declaration (in case header include path is broken)
esp_err_t imu_calibrate_now(void);

// Forward decl so we can call BLE from the switch callback.
esp_err_t ble_watch_set_enabled(bool enabled);

static const char *TAG = "ui";

LV_FONT_DECLARE(watch_time_132);
LV_FONT_DECLARE(watch_small_30);

static lv_obj_t *s_scr_face, *s_scr_home, *s_scr_settings;
static lv_obj_t *s_scr_brightness, *s_scr_set_time, *s_scr_calibrate, *s_scr_bluetooth;
static lv_obj_t *s_face_time, *s_face_batt;
static lv_obj_t *s_time_roller_h, *s_time_roller_m;

static ui_screen_t s_current = UI_SCREEN_FACE;
static void (*s_activity_cb)(void) = NULL;
static int s_brightness = 60;

static bool s_bt_enabled = false;
static char s_bt_device_name[32] = "None";
static lv_obj_t *s_bt_status_label = NULL;
static lv_obj_t *s_bt_device_label = NULL;

static lv_obj_t *s_scr_notif_list = NULL;
static lv_obj_t *s_scr_notif_detail = NULL;
static lv_obj_t *s_face_notif_container = NULL;
static lv_obj_t *s_face_notif_src = NULL;
static lv_obj_t *s_face_notif_body = NULL;

static lv_obj_t *s_notif_list_cont = NULL;
static lv_obj_t *s_detail_src_label = NULL;
static lv_obj_t *s_detail_title_label = NULL;
static lv_obj_t *s_detail_body_label = NULL;
static int       s_detail_index = 0;

// --- Helpers --------------------------------------------------------------
static void notify_activity(lv_event_t *e) { (void)e; if (s_activity_cb) s_activity_cb(); }

static void load_scr(lv_obj_t *scr, ui_screen_t id)
{
    bsp_display_lock(0);
    lv_screen_load(scr);          // instant, no anim
    s_current = id;
    bsp_display_unlock();
    if (s_activity_cb) s_activity_cb();
    
}
// --- Face ----------------------------------------------------------------
static void face_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_TOP) {
        lv_screen_load(s_scr_home);
        s_current = UI_SCREEN_HOME;
        if (s_activity_cb) s_activity_cb();
    }
}

static void face_notif_tap_cb(lv_event_t *e)
{
    (void)e;
    if (notifications_count() > 0) ui_show_notif_list();
}

static void build_face(void)
{
    s_scr_face = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_face, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr_face, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_face, LV_OBJ_FLAG_SCROLLABLE);

    s_face_time = lv_label_create(s_scr_face);
    lv_obj_set_style_text_font(s_face_time, &watch_time_132, 0);
    lv_obj_set_style_text_color(s_face_time, lv_color_white(), 0);
    lv_label_set_text(s_face_time, "--:--");
    lv_obj_align(s_face_time, LV_ALIGN_CENTER, 0, -70);

    s_face_batt = lv_label_create(s_scr_face);
    lv_obj_set_style_text_font(s_face_batt, &watch_small_30, 0);
    lv_obj_set_style_text_color(s_face_batt, lv_color_hex(0xB0B0B0), 0);
    lv_label_set_text(s_face_batt, "--%");
    lv_obj_align(s_face_batt, LV_ALIGN_CENTER, 0, 40);

    // Notification preview area, hidden by default.
    s_face_notif_container = lv_obj_create(s_scr_face);
    lv_obj_set_size(s_face_notif_container, 360, 90);
    lv_obj_align(s_face_notif_container, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_bg_color(s_face_notif_container, lv_color_hex(0x151515), 0);
    lv_obj_set_style_bg_opa(s_face_notif_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_face_notif_container, 0, 0);
    lv_obj_set_style_radius(s_face_notif_container, 14, 0);
    lv_obj_set_style_pad_all(s_face_notif_container, 14, 0);
    lv_obj_remove_flag(s_face_notif_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_face_notif_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_face_notif_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_face_notif_container, face_notif_tap_cb, LV_EVENT_CLICKED, NULL);

    s_face_notif_src = lv_label_create(s_face_notif_container);
    lv_obj_set_style_text_font(s_face_notif_src, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_face_notif_src, lv_color_hex(0x8AB4FF), 0);
    lv_label_set_text(s_face_notif_src, "");
    lv_obj_align(s_face_notif_src, LV_ALIGN_TOP_LEFT, 0, 0);

    s_face_notif_body = lv_label_create(s_face_notif_container);
    lv_obj_set_style_text_font(s_face_notif_body, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_face_notif_body, lv_color_white(), 0);
    lv_label_set_long_mode(s_face_notif_body, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_face_notif_body, 330);
    lv_label_set_text(s_face_notif_body, "");
    lv_obj_align(s_face_notif_body, LV_ALIGN_TOP_LEFT, 0, 24);

    lv_obj_add_event_cb(s_scr_face, face_gesture_cb,  LV_EVENT_GESTURE,  NULL);
    lv_obj_add_event_cb(s_scr_face, notify_activity,  LV_EVENT_PRESSING, NULL);
}

// --- Home (3x3 grid, only Settings tile is real) --------------------------
static void gear_tap_cb(lv_event_t *e) { (void)e; ui_show_settings(); }

static void music_tap_cb(lv_event_t *e)
{
    (void)e;
    ui_show_music();
}

static void build_home(void)
{
    s_scr_home = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_home, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr_home, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_home, LV_OBJ_FLAG_SCROLLABLE);

    const int tile = 130;
    const int cols = 3, rows = 3;
    const int gap_x = (410 - tile * cols) / (cols + 1);
    const int gap_y = (502 - tile * rows) / (rows + 1);
    int col = 2, row = 2;    // bottom-right
    int x = gap_x + col * (tile + gap_x);
    int y = gap_y + row * (tile + gap_y);

    lv_obj_t *t = lv_obj_create(s_scr_home);
    lv_obj_set_size(t, tile, tile);
    lv_obj_set_pos(t, x, y);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t, gear_tap_cb, LV_EVENT_CLICKED, NULL);

    // Big grey gear.
    lv_obj_t *icon = lv_label_create(t);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xB0B0B0), 0);
    lv_label_set_text(icon, LV_SYMBOL_SETTINGS);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *lbl = lv_label_create(t);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_label_set_text(lbl, "Settings");
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 36);

    // Music tile (top-left)
    col = 0; row = 0;
    x = gap_x + col * (tile + gap_x);
    y = gap_y + row * (tile + gap_y);

    lv_obj_t *mt = lv_obj_create(s_scr_home);
    lv_obj_set_size(mt, tile, tile);
    lv_obj_set_pos(mt, x, y);
    lv_obj_set_style_bg_opa(mt, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mt, 0, 0);
    lv_obj_set_style_pad_all(mt, 0, 0);
    lv_obj_remove_flag(mt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mt, music_tap_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *micon = lv_label_create(mt);
    lv_obj_set_style_text_font(micon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(micon, lv_color_hex(0xB0B0B0), 0);
    lv_label_set_text(micon, LV_SYMBOL_AUDIO);
    lv_obj_align(micon, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *mlbl = lv_label_create(mt);
    lv_obj_set_style_text_font(mlbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(mlbl, lv_color_white(), 0);
    lv_label_set_text(mlbl, "Music");
    lv_obj_align(mlbl, LV_ALIGN_CENTER, 0, 36);

    lv_obj_add_event_cb(s_scr_home, notify_activity, LV_EVENT_PRESSING, NULL);
}

// --- Settings list --------------------------------------------------------
static void item_brightness_cb(lv_event_t *e) { (void)e; ui_show_brightness(); }
static void item_set_time_cb(lv_event_t *e)   { (void)e; ui_show_set_time(); }
static void item_calibrate_cb(lv_event_t *e)  { (void)e; ui_show_calibrate(); }
static void item_bluetooth_cb(lv_event_t *e)  { (void)e; ui_show_bluetooth(); }



static lv_obj_t *add_big_settings_tile(lv_obj_t *parent, const char *icon,
                                       const char *text, uint32_t icon_color,
                                       lv_event_cb_t cb)
{
    lv_obj_t *tile = lv_button_create(parent);
    lv_obj_set_size(tile, 370, 72);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 16, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);

    lv_obj_t *icon_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon_lbl, lv_color_hex(icon_color), 0);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_align(icon_lbl, LV_ALIGN_LEFT_MID, 26, 0);

    lv_obj_t *text_lbl = lv_label_create(tile);
    lv_obj_set_style_text_font(text_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(text_lbl, lv_color_white(), 0);
    lv_label_set_text(text_lbl, text);
    lv_obj_align(text_lbl, LV_ALIGN_LEFT_MID, 82, 0);

    lv_obj_t *arrow = lv_label_create(tile);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x606060), 0);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -24, 0);

    if (cb) lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, NULL);
    return tile;
}

static void build_settings(void)
{
    s_scr_settings = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_settings, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr_settings, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_settings, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_scr_settings);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Settings");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    // Flex-column container holding the big tiles.
    lv_obj_t *cont = lv_obj_create(s_scr_settings);
    lv_obj_set_size(cont, 400, 410);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 4, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 12, 0);

    add_big_settings_tile(cont, LV_SYMBOL_EYE_OPEN,  "Brightness", 0xFFC147, item_brightness_cb);
    add_big_settings_tile(cont, LV_SYMBOL_SETTINGS,  "Set Time",   0x60B0F0, item_set_time_cb);
    add_big_settings_tile(cont, LV_SYMBOL_REFRESH,   "Calibrate",  0x80E080, item_calibrate_cb);
    add_big_settings_tile(cont, LV_SYMBOL_BLUETOOTH, "Bluetooth",  0x4A90E2, item_bluetooth_cb);

    lv_obj_add_event_cb(s_scr_settings, notify_activity, LV_EVENT_PRESSING, NULL);
}

// --- Brightness sub-screen ------------------------------------------------
static void brightness_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    if (v < 5) v = 5;
    if (v > 60) v = 60;
    s_brightness = v;
    bsp_display_brightness_set(v);
}

static void build_brightness(void)
{
    s_scr_brightness = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_brightness, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr_brightness, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_brightness, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(s_scr_brightness);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_text(t, "Brightness");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *icon = lv_label_create(s_scr_brightness);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xB0B0B0), 0);
    lv_label_set_text(icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -70);

    lv_obj_t *s = lv_slider_create(s_scr_brightness);
    lv_obj_set_size(s, 310, 18);
    lv_obj_align(s, LV_ALIGN_CENTER, 0, 30);
    lv_slider_set_range(s, 5, 60);
    lv_slider_set_value(s, s_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_add_event_cb(s_scr_brightness, notify_activity, LV_EVENT_PRESSING, NULL);
}

// --- Set Time -------------------------------------------------------------
static void time_save_cb(lv_event_t *e)
{
    (void)e;
    int h = lv_roller_get_selected(s_time_roller_h);
    int m = lv_roller_get_selected(s_time_roller_m);
    ESP_LOGI(TAG, "set time %02d:%02d", h, m);
    rtc_set((uint8_t)h, (uint8_t)m, 0, 1, 1, 2025, 3);
    ui_show_settings();
}

static void build_set_time(void)
{
    s_scr_set_time = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_set_time, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr_set_time, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_set_time, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(s_scr_set_time);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_text(t, "Set Time");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);

    static char hours_opts[200];
    int pos = 0;
    for (int i = 0; i < 24; i++)
        pos += snprintf(hours_opts + pos, sizeof(hours_opts) - pos, "%02d%s", i, i < 23 ? "\n" : "");

    static char mins_opts[400];
    pos = 0;
    for (int i = 0; i < 60; i++)
        pos += snprintf(mins_opts + pos, sizeof(mins_opts) - pos, "%02d%s", i, i < 59 ? "\n" : "");

    s_time_roller_h = lv_roller_create(s_scr_set_time);
    lv_roller_set_options(s_time_roller_h, hours_opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_time_roller_h, 3);
    lv_obj_set_style_text_font(s_time_roller_h, &lv_font_montserrat_28, 0);
    lv_obj_align(s_time_roller_h, LV_ALIGN_CENTER, -70, -10);

    lv_obj_t *colon = lv_label_create(s_scr_set_time);
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(colon, lv_color_white(), 0);
    lv_label_set_text(colon, ":");
    lv_obj_align(colon, LV_ALIGN_CENTER, 0, -10);

    s_time_roller_m = lv_roller_create(s_scr_set_time);
    lv_roller_set_options(s_time_roller_m, mins_opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_time_roller_m, 3);
    lv_obj_set_style_text_font(s_time_roller_m, &lv_font_montserrat_28, 0);
    lv_obj_align(s_time_roller_m, LV_ALIGN_CENTER, 70, -10);

    lv_obj_t *save = lv_button_create(s_scr_set_time);
    lv_obj_set_size(save, 160, 50);
    lv_obj_align(save, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_radius(save, 12, 0);
    lv_obj_add_event_cb(save, time_save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_lbl = lv_label_create(save);
    lv_obj_set_style_text_color(save_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_18, 0);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_center(save_lbl);

    lv_obj_add_event_cb(s_scr_set_time, notify_activity, LV_EVENT_PRESSING, NULL);
}

// --- Calibrate wrist ------------------------------------------------------
static void calibrate_save_cb(lv_event_t *e)
{
    (void)e;
    if (imu_calibrate_now() == ESP_OK) {
        ESP_LOGI(TAG, "wrist calibration saved");
    } else {
        ESP_LOGW(TAG, "wrist calibration failed");
    }
    ui_show_settings();
}

static void build_calibrate(void)
{
    s_scr_calibrate = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_calibrate, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr_calibrate, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_calibrate, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(s_scr_calibrate);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_label_set_text(t, "Calibrate");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *msg = lv_label_create(s_scr_calibrate);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(msg, 340);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg,
        "Hold the watch in your natural reading position, then press Save.");
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *save = lv_button_create(s_scr_calibrate);
    lv_obj_set_size(save, 160, 50);
    lv_obj_align(save, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_radius(save, 12, 0);
    lv_obj_add_event_cb(save, calibrate_save_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_lbl = lv_label_create(save);
    lv_obj_set_style_text_color(save_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_18, 0);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_center(save_lbl);

    lv_obj_add_event_cb(s_scr_calibrate, notify_activity, LV_EVENT_PRESSING, NULL);
}

// --- Bluetooth (placeholder for Phase 3) ---------------------------------
static void bt_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_bt_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "bluetooth toggled: %s", s_bt_enabled ? "ON" : "OFF");
    if (s_bt_status_label) {
        lv_label_set_text(s_bt_status_label, s_bt_enabled ? "Status: On" : "Status: Off");
    }
    ble_watch_set_enabled(s_bt_enabled);
}

static void build_bluetooth(void)
{
    s_scr_bluetooth = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_bluetooth, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr_bluetooth, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_bluetooth, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_scr_bluetooth);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Bluetooth");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    lv_obj_t *icon = lv_label_create(s_scr_bluetooth);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x4A90E2), 0);
    lv_label_set_text(icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 95);

    // Big switch.
    lv_obj_t *sw = lv_switch_create(s_scr_bluetooth);
    lv_obj_set_size(sw, 100, 54);
    lv_obj_align(sw, LV_ALIGN_TOP_MID, 0, 195);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x303030), 0);              // off
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x4A90E2), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (s_bt_enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, bt_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_bt_status_label = lv_label_create(s_scr_bluetooth);
    lv_obj_set_style_text_font(s_bt_status_label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_bt_status_label, lv_color_hex(0xC0C0C0), 0);
    lv_label_set_text(s_bt_status_label, s_bt_enabled ? "Status: On" : "Status: Off");
    lv_obj_align(s_bt_status_label, LV_ALIGN_TOP_MID, 0, 285);

    s_bt_device_label = lv_label_create(s_scr_bluetooth);
    lv_obj_set_style_text_font(s_bt_device_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_bt_device_label, lv_color_hex(0x808080), 0);
    char buf[48];
    snprintf(buf, sizeof(buf), "Connected: %s", s_bt_device_name);
    lv_label_set_text(s_bt_device_label, buf);
    lv_obj_align(s_bt_device_label, LV_ALIGN_TOP_MID, 0, 325);

    lv_obj_add_event_cb(s_scr_bluetooth, notify_activity, LV_EVENT_PRESSING, NULL);
}

// --- Notification list screen ---------------------------------------------
static void notif_list_item_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    ui_show_notif_detail(idx);
}

static void populate_notif_list(void)
{
    if (!s_notif_list_cont) return;
    lv_obj_clean(s_notif_list_cont);

    int n = notifications_count();
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_notif_list_cont);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x606060), 0);
        lv_label_set_text(empty, "No notifications");
        lv_obj_center(empty);
        return;
    }

    for (int i = 0; i < n; i++) {
        const notification_t *notif = notifications_get(i);
        if (!notif) continue;

        lv_obj_t *card = lv_button_create(s_notif_list_cont);
        lv_obj_set_size(card, 370, 88);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_add_event_cb(card, notif_list_item_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *src = lv_label_create(card);
        lv_obj_set_style_text_font(src, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(src, lv_color_hex(0x8AB4FF), 0);
        char srcbuf[128];
        snprintf(srcbuf, sizeof(srcbuf), "%s%s%s",
                 notif->src,
                 (notif->title[0]) ? " • " : "",
                 notif->title);
        lv_label_set_text(src, srcbuf);
        lv_obj_set_width(src, 340);
        lv_label_set_long_mode(src, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(src, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *body = lv_label_create(card);
        lv_obj_set_style_text_font(body, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(body, lv_color_white(), 0);
        lv_label_set_text(body, notif->body);
        lv_obj_set_width(body, 340);
        lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 26);
    }
}

static void build_notif_list(void)
{
    s_scr_notif_list = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_notif_list, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr_notif_list, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_notif_list, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_scr_notif_list);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Notifications");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    s_notif_list_cont = lv_obj_create(s_scr_notif_list);
    lv_obj_set_size(s_notif_list_cont, 400, 420);
    lv_obj_align(s_notif_list_cont, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_opa(s_notif_list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_notif_list_cont, 0, 0);
    lv_obj_set_style_pad_all(s_notif_list_cont, 4, 0);
    lv_obj_set_flex_flow(s_notif_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_notif_list_cont, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_notif_list_cont, 10, 0);

    lv_obj_add_event_cb(s_scr_notif_list, notify_activity, LV_EVENT_PRESSING, NULL);
}

// --- Notification detail screen -------------------------------------------
static void build_notif_detail(void)
{
    s_scr_notif_detail = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_notif_detail, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr_notif_detail, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr_notif_detail, LV_OBJ_FLAG_SCROLLABLE);

    s_detail_src_label = lv_label_create(s_scr_notif_detail);
    lv_obj_set_style_text_font(s_detail_src_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_detail_src_label, lv_color_hex(0x8AB4FF), 0);
    lv_label_set_text(s_detail_src_label, "");
    lv_obj_align(s_detail_src_label, LV_ALIGN_TOP_MID, 0, 25);

    s_detail_title_label = lv_label_create(s_scr_notif_detail);
    lv_obj_set_style_text_font(s_detail_title_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_detail_title_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_detail_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_detail_title_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_detail_title_label, 370);
    lv_label_set_text(s_detail_title_label, "");
    lv_obj_align(s_detail_title_label, LV_ALIGN_TOP_MID, 0, 65);

    s_detail_body_label = lv_label_create(s_scr_notif_detail);
    lv_obj_set_style_text_font(s_detail_body_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_detail_body_label, lv_color_hex(0xDADADA), 0);
    lv_obj_set_style_text_align(s_detail_body_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_detail_body_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_detail_body_label, 370);
    lv_label_set_text(s_detail_body_label, "");
    lv_obj_align(s_detail_body_label, LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_add_event_cb(s_scr_notif_detail, notify_activity, LV_EVENT_PRESSING, NULL);
}

static void populate_notif_detail(void)
{
    const notification_t *n = notifications_get(s_detail_index);
    if (!n) return;
    lv_label_set_text(s_detail_src_label,   n->src);
    lv_label_set_text(s_detail_title_label, n->title);
    lv_label_set_text(s_detail_body_label,  n->body);
}

static lv_obj_t *s_music_scr = NULL;
static lv_obj_t *s_music_track_lbl = NULL;
static lv_obj_t *s_music_artist_lbl = NULL;
static lv_obj_t *s_music_playpause_btn = NULL;
static lv_obj_t *s_music_playpause_lbl = NULL;

// --- Public API -----------------------------------------------------------
void ui_init(void)
{
    build_face();
    build_home();
    build_settings();
    build_brightness();
    build_set_time();
    build_calibrate();
    build_bluetooth();
    build_notif_list();
    build_notif_detail();
    lv_screen_load(s_scr_face);
    s_current = UI_SCREEN_FACE;
    ESP_LOGI(TAG, "UI built");
}

void ui_update_face(unsigned hour, unsigned minute, int battery_pct)
{
    char buf[8];
    bsp_display_lock(0);
    snprintf(buf, sizeof(buf), "%02u:%02u", hour, minute);
    lv_label_set_text(s_face_time, buf);
    if (battery_pct < 0) snprintf(buf, sizeof(buf), "--%%");
    else { if (battery_pct > 100) battery_pct = 100;
           snprintf(buf, sizeof(buf), "%u%%", (unsigned)(battery_pct & 0xFF)); }
    lv_label_set_text(s_face_batt, buf);
    bsp_display_unlock();
}

void ui_show_face(void)       { load_scr(s_scr_face,       UI_SCREEN_FACE); }
void ui_show_home(void)       { load_scr(s_scr_home,       UI_SCREEN_HOME); }
void ui_show_settings(void)   { load_scr(s_scr_settings,   UI_SCREEN_SETTINGS); }
void ui_show_brightness(void) { load_scr(s_scr_brightness, UI_SCREEN_BRIGHTNESS); }
void ui_show_set_time(void)   { load_scr(s_scr_set_time,   UI_SCREEN_SET_TIME); }
void ui_show_calibrate(void)  { load_scr(s_scr_calibrate,  UI_SCREEN_CALIBRATE); }
void ui_show_bluetooth(void)  { load_scr(s_scr_bluetooth,  UI_SCREEN_BLUETOOTH); }

void ui_nav_back(void)
{
    switch (s_current) {
        case UI_SCREEN_BRIGHTNESS:
        case UI_SCREEN_SET_TIME:
        case UI_SCREEN_CALIBRATE:
        case UI_SCREEN_BLUETOOTH:      ui_show_settings();  break;
        case UI_SCREEN_SETTINGS:       ui_show_home();      break;
        case UI_SCREEN_MUSIC:          ui_show_home();      break;
        case UI_SCREEN_HOME:           ui_show_face();      break;
        case UI_SCREEN_NOTIF_DETAIL:   ui_show_notif_list();break;
        case UI_SCREEN_NOTIFS:         ui_show_face();      break;
        case UI_SCREEN_FACE:           ui_show_home();      break;
    }
}

ui_screen_t ui_current(void) { return s_current; }
void ui_set_activity_cb(void (*cb)(void)) { s_activity_cb = cb; }
int  ui_get_brightness(void) { return s_brightness; }
void ui_set_bluetooth_connected_device(const char *name)
{
    strncpy(s_bt_device_name, (name && *name) ? name : "None", sizeof(s_bt_device_name) - 1);
    s_bt_device_name[sizeof(s_bt_device_name) - 1] = '\0';
    if (s_bt_device_label) {
        bsp_display_lock(0);
        char buf[48];
        snprintf(buf, sizeof(buf), "Connected: %s", s_bt_device_name);
        lv_label_set_text(s_bt_device_label, buf);
        bsp_display_unlock();
    }
}

bool ui_get_bluetooth_enabled(void) { return s_bt_enabled; }

void ui_set_bluetooth_status(bool connected, const char *device_name)
{
    bsp_display_lock(0);
    if (s_bt_device_label) {
        char buf[48];
        if (connected && device_name && *device_name)
            snprintf(buf, sizeof(buf), "Connected: %s", device_name);
        else
            snprintf(buf, sizeof(buf), "Connected: None");
        lv_label_set_text(s_bt_device_label, buf);
    }
    bsp_display_unlock();
}
void ui_show_notif_list(void)
{
    bsp_display_lock(0);
    populate_notif_list();
    bsp_display_unlock();
    load_scr(s_scr_notif_list, UI_SCREEN_NOTIFS);
}

void ui_show_notif_detail(int index)
{
    s_detail_index = index;
    bsp_display_lock(0);
    populate_notif_detail();
    bsp_display_unlock();
    load_scr(s_scr_notif_detail, UI_SCREEN_NOTIF_DETAIL);
}

void ui_notification_added(void)
{
    bsp_display_lock(0);
    int n = notifications_count();
    if (n == 0) {
        lv_obj_add_flag(s_face_notif_container, LV_OBJ_FLAG_HIDDEN);
    } else {
        const notification_t *notif = notifications_get(0);
        char src_buf[128];
        snprintf(src_buf, sizeof(src_buf), "%s%s%s",
                 notif->src,
                 notif->title[0] ? " • " : "",
                 notif->title);
        lv_label_set_text(s_face_notif_src, src_buf);
        lv_label_set_text(s_face_notif_body, notif->body);
        lv_obj_remove_flag(s_face_notif_container, LV_OBJ_FLAG_HIDDEN);
    }
    // If the list screen is live, refresh it too.
    if (s_current == UI_SCREEN_NOTIFS) populate_notif_list();
    bsp_display_unlock();
}

static void music_btn_cb(lv_event_t *e)
{
    const char *cmd = (const char *)lv_event_get_user_data(e);
    if (!cmd) return;

    if (strcmp(cmd, "playpause") == 0) {
        music_snapshot_t s; music_get_snapshot(&s);
        // Optimistic flip
        music_set_local_state(s.state == MUSIC_STATE_PLAY ? MUSIC_STATE_PAUSE : MUSIC_STATE_PLAY);
        ui_music_updated();
    }
    music_send_control(cmd);
}

static void build_music_screen(void)
{
    s_music_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_music_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_music_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_music_scr, LV_OBJ_FLAG_SCROLLABLE);

    // --- Album art placeholder (big circle) ---
    lv_obj_t *art = lv_obj_create(s_music_scr);
    lv_obj_set_size(art, 180, 180);
    lv_obj_align(art, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_radius(art, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(art, lv_color_hex(0x1DB954), 0);  // Spotify green
    lv_obj_set_style_border_width(art, 0, 0);
    lv_obj_set_style_shadow_color(art, lv_color_hex(0x1DB954), 0);
    lv_obj_set_style_shadow_width(art, 40, 0);
    lv_obj_set_style_shadow_opa(art, LV_OPA_30, 0);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *note = lv_label_create(art);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(note, lv_color_black(), 0);
    lv_obj_center(note);

    // --- Track title ---
    s_music_track_lbl = lv_label_create(s_music_scr);
    lv_obj_set_width(s_music_track_lbl, 370);
    lv_label_set_long_mode(s_music_track_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_music_track_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_music_track_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_music_track_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_music_track_lbl, "No music");
    lv_obj_align(s_music_track_lbl, LV_ALIGN_TOP_MID, 0, 240);

    // --- Artist ---
    s_music_artist_lbl = lv_label_create(s_music_scr);
    lv_obj_set_width(s_music_artist_lbl, 370);
    lv_label_set_long_mode(s_music_artist_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_music_artist_lbl, lv_color_hex(0xB0B0B0), 0);
    lv_obj_set_style_text_font(s_music_artist_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_music_artist_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_music_artist_lbl, "");
    lv_obj_align(s_music_artist_lbl, LV_ALIGN_TOP_MID, 0, 285);

    // --- Buttons ---
    const int SIDE = 80;
    const int CENTER = 110;

    // Previous
    lv_obj_t *prev = lv_btn_create(s_music_scr);
    lv_obj_set_size(prev, SIDE, SIDE);
    lv_obj_set_style_radius(prev, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(prev, lv_color_hex(0x282828), 0);
    lv_obj_set_style_shadow_width(prev, 0, 0);
    lv_obj_align(prev, LV_ALIGN_BOTTOM_LEFT, 40, -50);
    lv_obj_add_event_cb(prev, music_btn_cb, LV_EVENT_CLICKED, (void *)"previous");
    lv_obj_t *pl = lv_label_create(prev);
    lv_label_set_text(pl, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(pl, lv_color_white(), 0);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_28, 0);
    lv_obj_center(pl);

    // Play/Pause (center, big, green)
    s_music_playpause_btn = lv_btn_create(s_music_scr);
    lv_obj_set_size(s_music_playpause_btn, CENTER, CENTER);
    lv_obj_set_style_radius(s_music_playpause_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_music_playpause_btn, lv_color_hex(0x1DB954), 0);
    lv_obj_set_style_shadow_color(s_music_playpause_btn, lv_color_hex(0x1DB954), 0);
    lv_obj_set_style_shadow_width(s_music_playpause_btn, 30, 0);
    lv_obj_set_style_shadow_opa(s_music_playpause_btn, LV_OPA_40, 0);
    lv_obj_align(s_music_playpause_btn, LV_ALIGN_BOTTOM_MID, 0, -35);
    lv_obj_add_event_cb(s_music_playpause_btn, music_btn_cb, LV_EVENT_CLICKED, (void *)"playpause");
    s_music_playpause_lbl = lv_label_create(s_music_playpause_btn);
    lv_label_set_text(s_music_playpause_lbl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_music_playpause_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_music_playpause_lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(s_music_playpause_lbl);

    // Next
    lv_obj_t *nxt = lv_btn_create(s_music_scr);
    lv_obj_set_size(nxt, SIDE, SIDE);
    lv_obj_set_style_radius(nxt, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(nxt, lv_color_hex(0x282828), 0);
    lv_obj_set_style_shadow_width(nxt, 0, 0);
    lv_obj_align(nxt, LV_ALIGN_BOTTOM_RIGHT, -40, -50);
    lv_obj_add_event_cb(nxt, music_btn_cb, LV_EVENT_CLICKED, (void *)"next");
    lv_obj_t *nl = lv_label_create(nxt);
    lv_label_set_text(nl, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(nl, lv_color_white(), 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_28, 0);
    lv_obj_center(nl);
}

static void refresh_music_screen(void)
{
    if (!s_music_scr) return;
    music_snapshot_t s; music_get_snapshot(&s);
    if (s.has_info) {
        lv_label_set_text(s_music_track_lbl,  s.track[0]  ? s.track  : "(unknown)");
        lv_label_set_text(s_music_artist_lbl, s.artist[0] ? s.artist : "");
    } else {
        lv_label_set_text(s_music_track_lbl, "No music");
        lv_label_set_text(s_music_artist_lbl, "");
    }
    lv_label_set_text(s_music_playpause_lbl,
        s.state == MUSIC_STATE_PLAY ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void ui_music_updated(void)
{
    if (!s_music_scr) return;
    if (lv_scr_act() != s_music_scr) return;
    bsp_display_lock(0);
    refresh_music_screen();
    bsp_display_unlock();
}
void ui_show_music(void)
{
    if (!s_music_scr) build_music_screen();
    refresh_music_screen();
    lv_scr_load(s_music_scr);
    s_current = UI_SCREEN_MUSIC;
}