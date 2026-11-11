#include "imu_qmi8658.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <math.h>

#define QMI8658_ADDR        0x6B
#define REG_WHO_AM_I        0x00
#define REG_CTRL1           0x02
#define REG_CTRL2           0x03
#define REG_CTRL7           0x08
#define REG_CTRL9           0x0A
#define REG_CAL1_L          0x0B
#define REG_CAL1_H          0x0C
#define REG_STATUS1         0x2F
#define REG_AX_L            0x35
#define REG_RESET           0x60
#define RESET_CMD           0xB0

#define ACC_RANGE_2G        0x00
#define ACC_RANGE_8G        0x02
#define ACC_ODR_LP_128HZ    0x0C
#define CTRL9_CMD_WRITE_WOM 0x08

#define ACC_SCALE_G         (2.0f / 32768.0f)

static const char *TAG = "qmi8658";
static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

// Send a CTRL9 command and wait for completion ACK in STATUSINT.
static esp_err_t ctrl9_cmd(uint8_t cmd)
{
    reg_write(REG_CTRL9, cmd);
    // Poll STATUSINT (0x2D) bit 7 for CmdDone; crude but adequate.
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        uint8_t s = 0;
        uint8_t reg = 0x2D;
        if (i2c_master_transmit_receive(s_dev, &reg, 1, &s, 1, 100) == ESP_OK) {
            if (s & 0x80) {
                reg_write(REG_CTRL9, 0x00);  // ack
                return ESP_OK;
            }
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t imu_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = QMI8658_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) { ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err)); return err; }

    reg_write(REG_RESET, RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t who = 0;
    reg_read(REG_WHO_AM_I, &who, 1);
    ESP_LOGI(TAG, "WHO_AM_I=0x%02X (expect 0x05)", who);

    reg_write(REG_CTRL1, 0x40);
    reg_write(REG_CTRL2, (ACC_RANGE_2G << 4) | ACC_ODR_LP_128HZ);
    reg_write(REG_CTRL7, 0x01);
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t imu_read_accel_g(float *ax, float *ay, float *az)
{
    uint8_t d[6];
    esp_err_t err = reg_read(REG_AX_L, d, 6);
    if (err != ESP_OK) return err;

    int16_t rx = (int16_t)((d[1] << 8) | d[0]);
    int16_t ry = (int16_t)((d[3] << 8) | d[2]);
    int16_t rz = (int16_t)((d[5] << 8) | d[4]);

    if (ax) *ax = rx * ACC_SCALE_G;
    if (ay) *ay = ry * ACC_SCALE_G;
    if (az) *az = rz * ACC_SCALE_G;
    return ESP_OK;
}

esp_err_t imu_enter_wom(uint8_t threshold_mg)
{
    // Full reset to known state (per QMI8658 app note for WoM config).
    reg_write(REG_RESET, RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Disable all sensors first.
    reg_write(REG_CTRL7, 0x00);

    // Accel: 8g range, low-power 128Hz ODR.
    reg_write(REG_CTRL2, (ACC_RANGE_8G << 4) | ACC_ODR_LP_128HZ);

    // Threshold in mg (CAL1_L).
    reg_write(REG_CAL1_L, threshold_mg);

    // CAL1_H: upper two bits = INT config, lower six = blanking samples.
    //   10b (0x80) = INT1, initial value 1 (idle high, pulses low on motion).
    //   blanking 0x20 = 32 samples ignored after enabling WoM.
    reg_write(REG_CAL1_H, 0x80 | 0x20);

    // Issue CTRL9 command to apply WoM settings.
    esp_err_t err = ctrl9_cmd(CTRL9_CMD_WRITE_WOM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WoM ctrl9 cmd ack timed out, continuing");
    }

    // Enable accelerometer.
    reg_write(REG_CTRL7, 0x01);
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t imu_exit_wom(void)
{
    // Back to normal: full reset, then re-init.
    reg_write(REG_RESET, RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(20));
    reg_write(REG_CTRL1, 0x40);
    reg_write(REG_CTRL2, (ACC_RANGE_2G << 4) | ACC_ODR_LP_128HZ);
    reg_write(REG_CTRL7, 0x01);
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}
// ---- Wrist calibration (persisted in NVS) ------------------------------
#define CAL_NS   "watch"
#define CAL_KEY  "wrist_cal"

static float s_cal_vec[3] = {0, 0, 0};
static bool  s_cal_ok = false;

esp_err_t imu_calibrate_now(void)
{
    float ax, ay, az;
    if (imu_read_accel_g(&ax, &ay, &az) != ESP_OK) return ESP_FAIL;
    float total = sqrtf(ax*ax + ay*ay + az*az);
    if (total < 0.5f) return ESP_FAIL;
    s_cal_vec[0] = ax / total;
    s_cal_vec[1] = ay / total;
    s_cal_vec[2] = az / total;
    s_cal_ok = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(CAL_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, CAL_KEY, s_cal_vec, sizeof(s_cal_vec));
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "cal saved: %.2f %.2f %.2f", s_cal_vec[0], s_cal_vec[1], s_cal_vec[2]);
    return err;
}

bool imu_calibration_get(float out_vec[3])
{
    // Try to load from NVS once.
    if (!s_cal_ok) {
        nvs_handle_t h;
        if (nvs_open(CAL_NS, NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(s_cal_vec);
            if (nvs_get_blob(h, CAL_KEY, s_cal_vec, &len) == ESP_OK) {
                s_cal_ok = true;
                ESP_LOGI(TAG, "cal loaded: %.2f %.2f %.2f",
                         s_cal_vec[0], s_cal_vec[1], s_cal_vec[2]);
            }
            nvs_close(h);
        }
    }
    if (!s_cal_ok) return false;
    out_vec[0] = s_cal_vec[0];
    out_vec[1] = s_cal_vec[1];
    out_vec[2] = s_cal_vec[2];
    return true;
}