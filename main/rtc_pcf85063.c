#include "rtc_pcf85063.h"
#include "esp_log.h"
#include "esp_check.h"

#define PCF85063_ADDR       0x51
#define REG_CTRL1           0x00
#define REG_SEC             0x04   // bit7 = OS (oscillator stopped / time invalid)
// order from 0x04: SEC, MIN, HOUR, DAY, WEEKDAY, MONTH, YEAR (all BCD)

static const char *TAG = "pcf85063";
static i2c_master_dev_handle_t s_dev = NULL;

static inline uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

static esp_err_t reg_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[8];
    if (len + 1 > sizeof(tmp)) return ESP_ERR_INVALID_SIZE;
    tmp[0] = reg;
    for (size_t i = 0; i < len; i++) tmp[i + 1] = buf[i];
    return i2c_master_transmit(s_dev, tmp, len + 1, 100);
}

esp_err_t rtc_pcf_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PCF85063_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
    return err;
}

bool rtc_time_valid(void)
{
    uint8_t sec = 0;
    if (reg_read(REG_SEC, &sec, 1) != ESP_OK) return false;
    return (sec & 0x80) == 0;   // OS bit clear => time is trustworthy
}

esp_err_t rtc_set(uint8_t hour, uint8_t minute, uint8_t second,
                  uint8_t day, uint8_t month, uint16_t year, uint8_t weekday)
{
    uint8_t ctrl1 = 0x00;                 // 24h mode, RTC running
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL1, &ctrl1, 1), TAG, "ctrl1");

    uint8_t t[7];
    t[0] = dec2bcd(second) & 0x7F;        // writing SEC also clears the OS bit
    t[1] = dec2bcd(minute);
    t[2] = dec2bcd(hour);                 // 24h
    t[3] = dec2bcd(day);
    t[4] = weekday & 0x07;
    t[5] = dec2bcd(month);
    t[6] = dec2bcd((uint8_t)(year % 100));
    return reg_write(REG_SEC, t, 7);
}

esp_err_t rtc_get(rtc_time_t *out)
{
    uint8_t t[3];
    ESP_RETURN_ON_ERROR(reg_read(REG_SEC, t, 3), TAG, "read");
    out->second = bcd2dec(t[0] & 0x7F);
    out->minute = bcd2dec(t[1] & 0x7F);
    out->hour   = bcd2dec(t[2] & 0x3F);
    return ESP_OK;
}
