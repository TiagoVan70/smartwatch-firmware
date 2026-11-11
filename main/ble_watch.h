#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Called when a central (phone) connects or disconnects.
// peer_name is NULL on disconnect; a readable name (or "Phone") on connect.
typedef void (*ble_watch_conn_cb_t)(bool connected, const char *peer_name);

// Called on incoming bytes from the Nordic UART Service RX characteristic.
typedef void (*ble_watch_rx_cb_t)(const uint8_t *data, size_t len);

void ble_watch_tx(const char *s);

// Initialise NimBLE host + controller + NUS GATT service.
// Does NOT start advertising — call ble_watch_set_enabled(true) for that.
esp_err_t ble_watch_init(ble_watch_conn_cb_t on_conn, ble_watch_rx_cb_t on_rx);

// Start/stop advertising. Safe to call before init (becomes a no-op).
esp_err_t ble_watch_set_enabled(bool enabled);
bool      ble_watch_is_enabled(void);
bool      ble_watch_is_connected(void);

// Send bytes over NUS TX (notify). Returns error if not connected.
esp_err_t ble_watch_send(const uint8_t *data, size_t len);