#include "ble_watch.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>
#include "store/config/ble_store_config.h"
#include "host/ble_hs_mbuf.h"

static const char *TAG = "ble";

static uint16_t s_conn_handle = 0xFFFF;


// Nordic UART Service UUIDs (little-endian byte order for BLE_UUID128_INIT).
// Human: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
static const ble_uuid128_t NUS_SVC_UUID =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_RX_UUID =    // phone -> watch (write)
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_TX_UUID =    // watch -> phone (notify)
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                     0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static ble_watch_conn_cb_t s_on_conn = NULL;
static ble_watch_rx_cb_t   s_on_rx   = NULL;
static uint16_t s_tx_attr_handle = 0;
static bool     s_enabled = false;
static bool     s_host_ready = false;
static char     s_device_name[32] = "Bangle.js aabb";

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gap_event_cb(struct ble_gap_event *event, void *arg);

// GATT service table: NUS with one write (RX) and one notify (TX) char.
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &NUS_RX_UUID.u,
                .access_cb  = gatt_access_cb,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid       = &NUS_TX_UUID.u,
                .access_cb  = gatt_access_cb,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_attr_handle,
            },
            { 0 }   // terminator
        },
    },
    { 0 }   // terminator
};

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 512) len = 512;
        uint8_t buf[513];
        uint16_t out_len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &out_len) == 0 && s_on_rx) {
            s_on_rx(buf, out_len);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static esp_err_t start_advertising(void)
{
    if (!s_host_ready) return ESP_ERR_INVALID_STATE;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) return ESP_OK;  // already connected

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)s_device_name;
    fields.name_len = strlen(s_device_name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return ESP_FAIL; }

    // Secondary data: the 128-bit NUS UUID (so Gadgetbridge sees it on scan).
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.uuids128 = (ble_uuid128_t *)&NUS_SVC_UUID;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d", rc);

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "advertising as \"%s\"", s_device_name);
    return ESP_OK;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected, handle=%u", s_conn_handle);
            // Request security (triggers bonding on first pair, encrypts on reconnect).
            ble_gap_security_initiate(s_conn_handle);
            if (s_on_conn) s_on_conn(true, "Phone");
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            if (s_enabled) start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (s_on_conn) s_on_conn(false, NULL);
        if (s_enabled) start_advertising();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe handle=%u cur_notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU update conn=%u mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) { ESP_LOGE(TAG, "ensure_addr rc=%d", rc); return; }

    uint8_t addr_type;
    ble_hs_id_infer_auto(0, &addr_type);
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE addr %02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    // Device name includes 2 bytes of MAC so Gadgetbridge sees distinct devices.
    snprintf(s_device_name, sizeof(s_device_name),
             "Bangle.js %02x%02x", addr[1], addr[0]);
    ble_svc_gap_device_name_set(s_device_name);

    s_host_ready = true;
    if (s_enabled) start_advertising();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset reason=%d", reason); }

static void host_task(void *arg)
{
    nimble_port_run();   // returns only when stack stops
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

esp_err_t ble_watch_init(ble_watch_conn_cb_t on_conn, ble_watch_rx_cb_t on_rx)
{
    s_on_conn = on_conn;
    s_on_rx   = on_rx;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err)); return err; }

    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap       = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding      = 1;
    ble_hs_cfg.sm_mitm         = 0;
    ble_hs_cfg.sm_sc           = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_count rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "gatts_add rc=%d", rc); return ESP_FAIL; }

    ble_store_config_init();   // enable persisted bonds
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "init done");
    return ESP_OK;
}

esp_err_t ble_watch_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (enabled) {
        return start_advertising();
    } else {
        ble_gap_adv_stop();
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return ESP_OK;
    }
}

bool ble_watch_is_enabled(void)   { return s_enabled; }
bool ble_watch_is_connected(void) { return s_conn_handle != BLE_HS_CONN_HANDLE_NONE; }

esp_err_t ble_watch_send(const uint8_t *data, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return ESP_ERR_INVALID_STATE;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return ESP_ERR_NO_MEM;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_attr_handle, om);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}
void ble_watch_tx(const char *s)
{
    if (!s) return;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_tx_attr_handle == 0) {
        ESP_LOGW(TAG, "tx skipped: conn=%u tx_handle=%u", s_conn_handle, s_tx_attr_handle);
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s, strlen(s));
    if (!om) {
        ESP_LOGW(TAG, "tx mbuf alloc failed");
        return;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_attr_handle, om);
    ESP_LOGI(TAG, "tx rc=%d len=%u", rc, (unsigned)strlen(s));
}