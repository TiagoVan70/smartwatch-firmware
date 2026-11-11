#include "pmic_axp2101.h"
#include "esp_log.h"

#define AXP2101_ADDR              0x34
#define AXP2101_REG_STATUS1       0x00   // bit3 = battery present
#define AXP2101_REG_GAUGE_WDT     0x18   // bit3 = fuel-gauge enable
#define AXP2101_REG_BAT_PERCENT   0xA4   // battery percentage, 0..100

static const char *TAG = "axp2101";
static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t reg_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

esp_err_t axp2101_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_ADDR,
        .scl_speed_hz    = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }

    // Enable the fuel gauge (reg 0x18 bit3) so reg 0xA4 reports a real %.
    uint8_t v = 0;
    if (reg_read(AXP2101_REG_GAUGE_WDT, &v) == ESP_OK) {
        reg_write(AXP2101_REG_GAUGE_WDT, v | (1 << 3));
    }
    uint8_t adc_en = 0;
    if (reg_read(0x30, &adc_en) == ESP_OK) {
        reg_write(0x30, adc_en | 0x05);
    }
    // Force input current limit to 1000 mA (0x03) - needed after deep discharge
    reg_write(0x16, 0x03);
    // Ensure charger enable bit 1 is set
    uint8_t ce = 0;
    if (reg_read(0x18, &ce) == ESP_OK) {
        reg_write(0x18, ce | (1 << 1));
    }
    ESP_LOGI(TAG, "init ok, battery present=%d, first read=%d%%",
             axp2101_battery_present(), axp2101_get_percent());
    return ESP_OK;
}

bool axp2101_battery_present(void)
{
    uint8_t s = 0;
    if (reg_read(AXP2101_REG_STATUS1, &s) != ESP_OK) return false;
    return (s & (1 << 3)) != 0;
}

int axp2101_get_percent(void)
{
    // Read the gauge directly. We intentionally do NOT gate on the
    // battery-present bit: on this board it can read 0 even with a pack
    // attached, which was making the face show "--%".
    uint8_t p = 0;
    if (reg_read(AXP2101_REG_BAT_PERCENT, &p) != ESP_OK) return -1;
    if (p == 0xFF) return -1;      // gauge not ready / no battery
    if (p > 100) p = 100;
    return p;
}
// ---- Power button handling -----------------------------------------------
#define AXP2101_REG_INTEN2        0x41   // IRQ enable 2 (PKEY bits here)
#define AXP2101_REG_INTSTS2       0x49   // IRQ status 2 (write 1 to clear)
#define AXP2101_REG_COMMON_CONFIG 0x10   // bit 0 = SOFF (shutdown)

#define INT2_PKEY_LONG            (1 << 2)
#define INT2_PKEY_SHORT           (1 << 3)

esp_err_t axp2101_pwr_button_init(void)
{
    // Enable short + long press IRQs in INTEN2.
    uint8_t v = 0;
    reg_read(AXP2101_REG_INTEN2, &v);
    v |= (INT2_PKEY_LONG | INT2_PKEY_SHORT);
    reg_write(AXP2101_REG_INTEN2, v);

    // Clear any stale pending IRQs.
    reg_write(AXP2101_REG_INTSTS2, 0xFF);
    ESP_LOGI(TAG, "PWR button IRQs enabled");
    return ESP_OK;
}

int axp2101_pwr_button_poll(void)
{
    uint8_t s = 0;
    if (reg_read(AXP2101_REG_INTSTS2, &s) != ESP_OK) return 0;
    int event = 0;
    uint8_t ack = 0;
    if (s & INT2_PKEY_LONG)  { event = 2; ack |= INT2_PKEY_LONG;  }
    else if (s & INT2_PKEY_SHORT) { event = 1; ack |= INT2_PKEY_SHORT; }
    if (ack) reg_write(AXP2101_REG_INTSTS2, ack);
    return event;
}

esp_err_t axp2101_shutdown(void)
{
    ESP_LOGW(TAG, "shutdown!");
    uint8_t v = 0;
    reg_read(AXP2101_REG_COMMON_CONFIG, &v);
    v |= 0x01;   // SOFF: software power off
    return reg_write(AXP2101_REG_COMMON_CONFIG, v);
}
void axp2101_log_charge_status(void)
{
    uint8_t v, h, l;
    reg_read(0x00, &v);  ESP_LOGI(TAG, "PMU_STATUS1=0x%02X", v);
    reg_read(0x01, &v);  ESP_LOGI(TAG, "PMU_STATUS2=0x%02X", v);
    reg_read(0x18, &v);  ESP_LOGI(TAG, "MODULE_ENABLE=0x%02X", v);
    reg_read(0x61, &v);  ESP_LOGI(TAG, "IPRECHG=0x%02X", v);
    reg_read(0x62, &v);  ESP_LOGI(TAG, "ICC_CFG=0x%02X", v);
    reg_read(0x30, &v);  ESP_LOGI(TAG, "ADC_EN=0x%02X", v);
    reg_read(0x16, &v);  ESP_LOGI(TAG, "IIN_LIMIT=0x%02X", v);
    reg_read(0x19, &v);  ESP_LOGI(TAG, "CHG_CFG=0x%02X", v);
    // VBAT (battery voltage): regs 0x34/0x35, 14-bit mV
    reg_read(0x34, &h);
    reg_read(0x35, &l);
    uint16_t vbat = ((h & 0x3F) << 8) | l;
    ESP_LOGI(TAG, "VBAT=%u mV", vbat);

    // VBUS voltage: regs 0x38/0x39
    reg_read(0x38, &h);
    reg_read(0x39, &l);
    uint16_t vbus = ((h & 0x3F) << 8) | l;
    ESP_LOGI(TAG, "VBUS=%u mV", vbus);
}