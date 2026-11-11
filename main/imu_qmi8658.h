#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// IMU INT1 pin on this board (from Waveshare 2.06 schematic).
#define IMU_INT_GPIO    21

// Normal mode: accelerometer streaming for pitch calculation.
esp_err_t imu_init(i2c_master_bus_handle_t bus);

// Read acceleration in g. Any pointer may be NULL.
esp_err_t imu_read_accel_g(float *ax, float *ay, float *az);

// Switch to Wake-on-Motion: chip wakes INT1 only when motion exceeds
// threshold_mg. Normal data stops. Use when screen is off.
// threshold_mg: motion threshold in milli-g (1..255). Lower = more sensitive.
esp_err_t imu_enter_wom(uint8_t threshold_mg);

// Switch back to normal accelerometer streaming. Use when screen turns on.
esp_err_t imu_exit_wom(void);

#ifdef __cplusplus
// Capture current pose as wrist calibration reference and persist to NVS.
esp_err_t imu_calibrate_now(void);

// Returns true if a calibration is loaded. If true, fills out_vec[3] with
// the saved normalized reference vector (x,y,z).
bool imu_calibration_get(float out_vec[3]);
}
#endif