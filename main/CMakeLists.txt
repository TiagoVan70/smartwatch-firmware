idf_component_register(
    SRCS
        "main.c"
        "ui.c"
        "ble_watch.c"
        "notifications.c"
        "rtc_pcf85063.c"
        "imu_qmi8658.c"
        "pmic_axp2101.c"
        "fonts/watch_time_132.c"
        "fonts/watch_small_30.c"
        "music.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2c esp_timer esp_pm esp_driver_gpio esp_hw_support
        esp_lcd
        waveshare__esp32_s3_touch_amoled_2_06
        espressif__esp_lvgl_port
        nvs_flash
        bt)