#pragma once

#include "lvgl.h"
#include <stdbool.h>


typedef enum {
    UI_SCREEN_FACE,
    UI_SCREEN_HOME,
    UI_SCREEN_SETTINGS,
    UI_SCREEN_MUSIC,          // <-- the only line you add
    UI_SCREEN_BRIGHTNESS,
    UI_SCREEN_SET_TIME,
    UI_SCREEN_CALIBRATE,
    UI_SCREEN_BLUETOOTH,
    UI_SCREEN_NOTIFS,
    UI_SCREEN_NOTIF_DETAIL,
} ui_screen_t;

void ui_init(void);
void ui_update_face(unsigned hour, unsigned minute, int battery_pct);

void ui_show_face(void);
void ui_show_home(void);
void ui_show_settings(void);
void ui_show_brightness(void);
void ui_show_set_time(void);
void ui_show_calibrate(void);
void ui_show_bluetooth(void);
void ui_nav_back(void);

// Bluetooth UI state helpers (BLE itself comes in Phase 3).
void ui_set_bluetooth_connected_device(const char *name);
bool ui_get_bluetooth_enabled(void);

ui_screen_t ui_current(void);
void ui_set_activity_cb(void (*cb)(void));
int  ui_get_brightness(void);

void ui_set_bluetooth_status(bool connected, const char *device_name);


// New public functions:
void ui_show_notif_list(void);
void ui_show_notif_detail(int index);
void ui_notification_added(void);   // called by notifications module

void ui_music_updated(void);
void ui_show_music(void);

