# ESP32-S3 Smartwatch Firmware

Custom firmware for the **Waveshare ESP32-S3-Touch-AMOLED-2.06** development board, turning it into a battery-optimized BLE smartwatch that pairs with Android via Gadgetbridge.

## Hardware

- **MCU:** ESP32-S3 (with 8 MB PSRAM, 16 MB flash)
- **Display:** 410×502 CO5300 AMOLED, SH8601 driver
- **Touch:** FT3168 capacitive
- **IMU:** QMI8658 (accelerometer for tilt-to-wake)
- **RTC:** PCF85063
- **PMIC:** AXP2101 (battery management, soft power button)
- **Battery:** 400 mAh LiPo

## Features

### Watch face
- Large 24-hour digital clock + battery percentage
- Black background, 60% maximum brightness
- 7-second screen timeout
- Tilt-to-wake using QMI8658 accelerometer with user-calibrated reference vector
- Notification preview below time (newest incoming)

### Home screen (3×3 grid)
- **Music** tile (top-left)
- **Settings** gear (bottom-right)

### Music player
- Pairs with Gadgetbridge's Bangle.js music protocol
- Shows current track + artist from Spotify / YouTube Music / any media app
- Previous / Play-Pause / Next controls over BLE NUS
- Optimistic UI on play/pause

### Notifications
- Filtered by source: WhatsApp, Email (Gmail/Outlook/Mail), Instagram, TikTok only
- Line-buffered JSON parser, handles Espruino escape sequences
- In-memory ring buffer of last 10 notifications
- Dedicated list screen accessible via home

### Settings
- Brightness slider
- Set Time (manual, auto-synced from phone when Gadgetbridge connects)
- Wrist calibration (saves accelerometer reference vector to NVS)
- Bluetooth on/off toggle

### Buttons
- **BOOT:** Back navigation
- **PWR short press:** Return to home
- **PWR long press:** Soft shutdown via AXP2101

### BLE / Phone integration
- NimBLE stack, Nordic UART Service (NUS)
- Advertises as `Bangle.js XXXX`
- Bonds with phone, keys persist in NVS
- Gadgetbridge Bangle.js protocol compatible
- Auto time sync from phone

## Building

Requires **ESP-IDF v6.0.2** and Python environment set up per Espressif's guide.

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM6 flash monitor
```

Replace `COM6` with your serial port.

## Pairing with phone

1. Install [Gadgetbridge](https://gadgetbridge.org/) from F-Droid on your Android phone
2. On the watch, open **Settings → Bluetooth**, toggle ON
3. In Gadgetbridge, scan for devices and pair with `Bangle.js XXXX`
4. Enable in Gadgetbridge device settings:
   - "Transmitir directamente los intentos de los botones multimedia" (direct media button intents)
   - "Reproductor de audio preferido" → select Spotify (or your preferred app)
5. In Android system settings → Apps → Gadgetbridge → grant notification access

## Project layout

main/
├── main.c # app_main, watch task, power management
├── ui.c / ui.h # LVGL screens: face, home, settings, music, notifications
├── music.c / music.h # Music player state + BLE control
├── notifications.c # JSON parser, Gadgetbridge dispatch
├── ble_watch.c # NimBLE NUS service, bonding
├── imu_qmi8658.c # Accelerometer + tilt calibration
├── pmic_axp2101.c # Battery, power button, charging
├── rtc_pcf85063.c # Real-time clock
└── CMakeLists.txt



## Known issues / TODO

- **Battery life:** Light sleep is disabled while BLE is on. Standby drain is high (~40-50 mA). Needs a power pass with proper connection intervals and BLE wake-on-motion.
- **Charging:** AXP2101 may latch into protection after deep discharge. Physical power cycle + wall charger usually recovers.
- **Notification dismissal:** Receives `notify-` events but doesn't yet remove from list UI.

## License

MIT