// ESP32-S3-Touch-AMOLED-2.06 - Phase A: UI + navigation + buttons.
//
// Screens: face <-> home <-> settings.
// Inputs: tilt wake, touch, BOOT (GPIO0) = back, PWR (via AXP2101) = home / shutdown.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include <math.h>
#include <stdio.h>

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "rtc_pcf85063.h"
#include "imu_qmi8658.h"
#include "pmic_axp2101.h"
#include "ui.h"
#include "nvs_flash.h"

#include "ble_watch.h"
#include "notifications.h"
#include "music.h"

#ifndef M_PI
#define M_PI 3.14159265358979f
#endif

// ---- Tunables ----------------------------------------------------------------
#define MAX_BRIGHTNESS      60      // hard cap (user cannot exceed via slider)
#define FACE_TIMEOUT_MS     7000    // short on the face
#define APP_TIMEOUT_MS      15000   // longer in home/apps
#define LOOP_ON_MS          30
#define LOOP_OFF_MS         150

#define TILT_DEBUG          1       // log pitch so we can tune WAKE_ON_DEG
#define ACTIVITY_GRACE_MS   3000    // ignore !viewing for this long after any input

#define WAKE_ON_DEG         25.0f
#define WAKE_OFF_DEG        10.0f
#define TILT_SIGN           (1.0f)

#define G_MIN               0.80f
#define G_MAX               1.25f

#define BOOT_GPIO           GPIO_NUM_0

#define RTC_FORCE_SEED      0
#define SEED_H  19
#define SEED_M  10
// -----------------------------------------------------------------------------

static const char *TAG = "watch";

static volatile int64_t s_last_touch_us = 0;
static esp_pm_lock_handle_t s_no_light_sleep_lock = NULL;

typedef enum { SCREEN_OFF, SCREEN_ON } screen_state_t;
static screen_state_t s_state = SCREEN_OFF;

static void note_activity_now(void)
{
    s_last_touch_us = esp_timer_get_time();
}

static void set_brightness_ramp(int from, int to)
{
    int steps = 6;
    for (int i = 1; i <= steps; i++) {
        int b = from + (to - from) * i / steps;
        bsp_display_lock(0);                // serialize vs LVGL SPI flushes
        bsp_display_brightness_set(b);
        bsp_display_unlock();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    bsp_display_lock(0);
    bsp_display_brightness_set(to);
    bsp_display_unlock();
}

static void screen_power_down(void)
{
    set_brightness_ramp(ui_get_brightness(), 0);
    s_state = SCREEN_OFF;
    ESP_LOGI(TAG, "screen off");
}

static void screen_power_up_to_face(void)
{
    ui_show_face();
    set_brightness_ramp(0, ui_get_brightness());
    note_activity_now();
    s_state = SCREEN_ON;
    ESP_LOGI(TAG, "screen on");
}

// Face value refresh (time + battery). Called by watch_task when screen is on.
static void refresh_face_values(void)
{
    rtc_time_t t;
    if (rtc_get(&t) != ESP_OK) return;
    int pct = axp2101_get_percent();
    ui_update_face(t.hour, t.minute, pct);
}

static bool read_pitch_viewing(bool prev)
{
    float ax=0, ay=0, az=0;
    if (imu_read_accel_g(&ax, &ay, &az) != ESP_OK) return prev;
    float total = sqrtf(ax*ax + ay*ay + az*az);
    if (total < G_MIN || total > G_MAX) return prev;

    float cal[3];
    bool have_cal = imu_calibration_get(cal);

    if (have_cal) {
        // Dot product with calibrated reference. Both vectors are normalized
        // to unit length, so dot = cos(angle between poses).
        float dot = (ax*cal[0] + ay*cal[1] + az*cal[2]) / total;

#if TILT_DEBUG
        static int d1 = 0;
        if (s_state == SCREEN_ON && ++d1 >= 20) {
            d1 = 0;
            ESP_LOGI(TAG, "cal mode  dot=%.2f  prev=%d", dot, prev);
        }
#endif
        // Hysteresis: enter viewing above cos(25°)=0.91, leave below cos(45°)=0.71.
        if (!prev && dot > 0.91f) return true;
        if (prev  && dot < 0.71f) return false;
        return prev;
    } else {
        // No calibration yet - fallback to the pitch heuristic.
        float pitch = TILT_SIGN * atan2f(-ax, sqrtf(ay*ay + az*az)) * 180.0f / (float)M_PI;
#if TILT_DEBUG
        static int d2 = 0;
        if (s_state == SCREEN_ON && ++d2 >= 20) {
            d2 = 0;
            ESP_LOGI(TAG, "pitch mode  pitch=%.0f  ax=%.2f ay=%.2f az=%.2f  prev=%d",
                     pitch, ax, ay, az, prev);
        }
#endif
        if (!prev && pitch > WAKE_ON_DEG)  return true;
        if (prev  && pitch < WAKE_OFF_DEG) return false;
        return prev;
    }
}

// --- Button polling --------------------------------------------------------
static int s_boot_last = 1;    // active-low, 1 = released

// Returns true if a button event woke/caused navigation (resets idle timer).
static bool poll_buttons_once(void)
{
    bool activity = false;

    // BOOT (GPIO0, active low). Debounce by edge-detect on polled samples.
    int boot_now = gpio_get_level(BOOT_GPIO);
    if (s_boot_last == 1 && boot_now == 0) {
        ESP_LOGI(TAG, "BOOT pressed -> back");
        if (s_state == SCREEN_OFF) {
            // Wake first, then go home (face shows initially; one more BOOT -> home).
            screen_power_up_to_face();
            activity = true;
        } else {
            ui_nav_back();
            activity = true;
        }
    }
    s_boot_last = boot_now;

    // PWR via AXP2101 (polled over I2C).
    int pwr = axp2101_pwr_button_poll();
    if (pwr == 1) {
        ESP_LOGI(TAG, "PWR short -> home");
        if (s_state == SCREEN_OFF) screen_power_up_to_face();
        ui_show_home();
        activity = true;
    } else if (pwr == 2) {
        ESP_LOGW(TAG, "PWR long -> shutdown");
        axp2101_shutdown();
        // device loses power here; code below won't run
    }

    return activity;
}

static void watch_task(void *arg)
{
    (void)arg;
    bool viewing = false;
    bool armed = true;

    // Boot into off state.
    screen_power_down();

    while (1) {
        viewing = read_pitch_viewing(viewing);
        bool btn_activity = poll_buttons_once();
        if (btn_activity) note_activity_now();

        int64_t now = esp_timer_get_time();

        if (s_state == SCREEN_OFF) {
            if (!viewing) armed = true;
            if (armed && viewing) {
                screen_power_up_to_face();
            }
        } else {
            // Refresh face values ~2 Hz while on the face.
            if (ui_current() == UI_SCREEN_FACE) {
                static int tick = 0;
                if (++tick >= 2) { tick = 0; refresh_face_values(); }
            }

            // Grace period after touch/button activity: don't let a flaky
            // tilt reading kill the screen right after interaction.
            int64_t since_act = now - s_last_touch_us;
            bool recently_active = since_act < (int64_t)ACTIVITY_GRACE_MS * 1000;

            // Pick timeout based on which screen is showing.
            uint32_t timeout_ms = (ui_current() == UI_SCREEN_FACE)
                                   ? FACE_TIMEOUT_MS : APP_TIMEOUT_MS;
            bool timed_out = (now - s_last_touch_us) > (int64_t)timeout_ms * 1000;

            if (!viewing && !recently_active) {
                screen_power_down();
                armed = true;
            } else if (timed_out) {
                screen_power_down();
                armed = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(s_state == SCREEN_ON ? LOOP_ON_MS : LOOP_OFF_MS));
    }
}

static void setup_boot_button(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    s_boot_last = gpio_get_level(BOOT_GPIO);
}

static void setup_power_management(void)
{
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,     // was true — disabling to avoid freezes
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm));
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ui", &s_no_light_sleep_lock));
    ESP_LOGI(TAG, "PM: 40-160 MHz DFS + light sleep");
}

static void on_ble_conn(bool connected, const char *peer_name)
{
    ESP_LOGI(TAG, "BLE %s%s%s",
             connected ? "connected" : "disconnected",
             peer_name ? " to " : "", peer_name ? peer_name : "");
    ui_set_bluetooth_status(connected, peer_name);
}

static void on_ble_rx(const uint8_t *data, size_t len)
{
    ESP_LOGI(TAG, "RX %u bytes:", (unsigned)len);
    for (size_t i = 0; i < len; i++) {
        if (data[i] >= 32 && data[i] < 127) {
            printf("%c", data[i]);
        } else {
            printf("[%02X]", data[i]);
        }
    }
    printf("\n");
    notifications_handle_rx(data, len);
}

void app_main(void)
{
    bsp_display_start();
    // NVS for wrist calibration + settings persistence.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    bsp_display_brightness_set(0);

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_ERROR_CHECK(rtc_pcf_init(bus));
    ESP_ERROR_CHECK(imu_init(bus));
    ESP_ERROR_CHECK(axp2101_init(bus));
    ESP_ERROR_CHECK(axp2101_pwr_button_init());
    axp2101_log_charge_status();   // <-- add this

    if (RTC_FORCE_SEED || !rtc_time_valid()) {
        ESP_LOGW(TAG, "seeding RTC to %02d:%02d", SEED_H, SEED_M);
        rtc_set(SEED_H, SEED_M, 0, 1, 1, 2025, 3);
    }

    bsp_display_lock(0);
    ui_init();
    music_init();
    ui_set_activity_cb(note_activity_now);
    bsp_display_unlock();

    setup_boot_button();
    setup_power_management();
    xTaskCreate(watch_task, "watch_task", 6144, NULL, 5, NULL);
    notifications_init();

        // BLE comes up last so the UI is already alive to receive connect events.
    ble_watch_init(on_ble_conn, on_ble_rx);
}