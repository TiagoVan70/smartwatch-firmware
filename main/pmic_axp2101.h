#pragma once
// Minimal AXP2101 helper for the ESP32-S3-Touch-AMOLED-2.06.
// Talks directly over the BSP's shared I2C bus (bsp_i2c_get_handle()).
// We only need the battery gauge for milestone 1, so no XPowersLib needed.

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Adds the AXP2101 as a device on the given bus and enables the fuel gauge.
esp_err_t axp2101_init(i2c_master_bus_handle_t bus);

// true if a battery is detected on the PMIC.
bool axp2101_battery_present(void);

// Battery charge, 0..100. Returns -1 if no battery / read error.
int axp2101_get_percent(void);

// Enable the AXP2101's power-key short/long press IRQ reporting.
esp_err_t axp2101_pwr_button_init(void);

// Poll the AXP2101 IRQ status and return any pending power-key event.
// 0 = none, 1 = short press, 2 = long press. Clears events on read.
int axp2101_pwr_button_poll(void);

// Hard shutdown via the AXP2101 (ship mode). Device won't come back until
// the user presses PWR again. We call this on a long-press from the user.
esp_err_t axp2101_shutdown(void);

esp_err_t imu_calibrate_now(void);
bool imu_calibration_get(float out_vec[3]);

#ifdef __cplusplus
}
#endif

void axp2101_log_charge_status(void);