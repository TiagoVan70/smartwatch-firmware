#pragma once
// Minimal PCF85063 RTC helper for the ESP32-S3-Touch-AMOLED-2.06.
// Direct I2C over the BSP shared bus. 24h mode, BCD registers.

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t hour;    // 0..23
    uint8_t minute;  // 0..59
    uint8_t second;  // 0..59
} rtc_time_t;

// Add the RTC as a device on the given bus.
esp_err_t rtc_pcf_init(i2c_master_bus_handle_t bus);

// true if the oscillator has been running with valid time
// (false right after first power-up / battery loss -> needs seeding).
bool rtc_time_valid(void);

// Seed the clock. day/month/year keep the registers valid even though
// this test only displays the time. year is full (e.g. 2025).
esp_err_t rtc_set(uint8_t hour, uint8_t minute, uint8_t second,
                  uint8_t day, uint8_t month, uint16_t year, uint8_t weekday);

// Read current time.
esp_err_t rtc_get(rtc_time_t *out);

#ifdef __cplusplus
}
#endif
