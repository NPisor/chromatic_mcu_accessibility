#include "ble_bridge.h"

#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "esp_random.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nvs_flash.h"
#include "system/cheats.h"

static BLEBridge_State_t _state = kBLEBridge_State_Stopped;
static BLEBridge_StateCb_t _state_cb = NULL;
static uint16_t _status_val_handle;
static bool _notify_enabled = false;
static bool _proxy_notify_enabled = false;
static uint16_t _conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t _own_addr_type = BLE_OWN_ADDR_PUBLIC;
static bool _enabled = true;
static int _last_adv_rc = 0;
static const char *_last_adv_rc_str = "";
static char _addr_str[18] = "unknown";
static ble_addr_t _chosen_addr;
static const char *TAG = "BLEBridge";
static bool _hs_ready = false;
static bool _pending_adv_start = false;
static BLEBridge_ProxyRxCb_t _proxy_rx_cb = NULL;
static uint16_t _proxy_val_handle;

static void format_addr_str(const ble_addr_t *addr, char *out, size_t out_len)
{
    if (addr == NULL || out == NULL || out_len < 18)
    {
        return;
    }
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr->val[5], addr->val[4], addr->val[3],
             addr->val[2], addr->val[1], addr->val[0]);
}

static const char *adv_err_string(int rc)
{
    switch (rc)
    {
        case 0:
            return "";
        case BLE_HS_EALREADY:
            return "already";
        case BLE_HS_EBUSY:
            return "busy";
        case BLE_HS_EINVAL:
            return "invalid";
        case BLE_HS_ESTALLED:
            return "stalled";
        case BLE_HS_ENOMEM:
            return "no-mem";
        case BLE_HS_ECONTROLLER:
            return "ctrl";
        case BLE_HS_ETIMEOUT_HCI:
            return "hci-timeout";
        default:
            return "fail";
    }
}

static void load_public_addr(void)
{
    ble_addr_t pub_addr;
    memset(&pub_addr, 0, sizeof(pub_addr));
    if (ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, pub_addr.val, NULL) == 0)
    {
        _own_addr_type = BLE_OWN_ADDR_PUBLIC;
        _chosen_addr = pub_addr;
        format_addr_str(&_chosen_addr, _addr_str, sizeof(_addr_str));
    }
}

const char *BLEBridge_GetAddrStr(void)
{
    return _addr_str;
}

static const ble_uuid128_t _svc_uuid = BLE_UUID128_INIT(
    0x01,0x00,0x1d,0x6a,0x4d,0x9f,0xee,0x9a,0x5f,0x4c,0xfc,0x78,0x00,0x04,0xf4,0xe1);

static const ble_uuid128_t _status_uuid = BLE_UUID128_INIT(
    0x02,0x00,0x1d,0x6a,0x4d,0x9f,0xee,0x9a,0x5f,0x4c,0xfc,0x78,0x00,0x04,0xf4,0xe1);

static const ble_uuid128_t _ingest_uuid = BLE_UUID128_INIT(
    0x03,0x00,0x1d,0x6a,0x4d,0x9f,0xee,0x9a,0x5f,0x4c,0xfc,0x78,0x00,0x04,0xf4,0xe1);

static const ble_uuid128_t _proxy_uuid = BLE_UUID128_INIT(
    0x04,0x00,0x1d,0x6a,0x4d,0x9f,0xee,0x9a,0x5f,0x4c,0xfc,0x78,0x00,0x04,0xf4,0xe1);

static int ble_bridge_gap_event(struct ble_gap_event *event, void *arg);
static void ble_bridge_start_advertising(void);
static int ble_bridge_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
static void ble_bridge_on_sync(void);
static void ble_bridge_host_task(void *param);
static void ble_bridge_on_reset(int reason);

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &_status_uuid.u,
                .access_cb = ble_bridge_chr_access,
                .val_handle = &_status_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &_ingest_uuid.u,
                .access_cb = ble_bridge_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &_proxy_uuid.u,
                .access_cb = ble_bridge_chr_access,
                .val_handle = &_proxy_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {0},
        },
    },
    {0},
};

static void set_state(BLEBridge_State_t s)
{
    if (_state != s)
    {
        _state = s;
        if (_state_cb != NULL)
        {
            _state_cb(s);
        }
    }
}

int BLEBridge_GetLastAdvError(void)
{
    return _last_adv_rc;
}

const char *BLEBridge_GetLastAdvErrorStr(void)
{
    return _last_adv_rc_str;
}

void BLEBridge_Init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    ret = nimble_port_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "nimble_port_init failed rc=%d", ret);
        return;
    }

    esp_bt_controller_status_t ctrl_stat = esp_bt_controller_get_status();
    ESP_LOGI(TAG, "bt ctrl status (post-hci-init)=%d", ctrl_stat);

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_hs_cfg.reset_cb = ble_bridge_on_reset;
    ble_hs_cfg.sync_cb = ble_bridge_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "gatt count cfg failed rc=%d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "gatt add svcs failed rc=%d", rc);
        return;
    }

    nimble_port_freertos_init(ble_bridge_host_task);
    set_state(kBLEBridge_State_Stopped);
}

static void ble_bridge_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_bridge_on_reset(int reason)
{
    (void)reason;
    _hs_ready = false;
}

static void ble_bridge_on_sync(void)
{
    _hs_ready = true;
    load_public_addr();
    ESP_LOGI(TAG, "on_sync addr_type=%d addr=%s", _own_addr_type, _addr_str);
    ble_svc_gap_device_name_set("Chromatic");
    if (_enabled)
    {
        ble_bridge_start_advertising();
    }
}

static void ble_bridge_start_advertising(void)
{
    _pending_adv_start = false;

    if (!_enabled)
    {
        ESP_LOGI(TAG, "adv_start skipped: disabled");
        _last_adv_rc = 0;
        _last_adv_rc_str = "";
        set_state(kBLEBridge_State_Stopped);
        return;
    }

    if (!ble_hs_synced())
    {
        _last_adv_rc = 0;
        _last_adv_rc_str = "";
        _pending_adv_start = true;
        ESP_LOGW(TAG, "adv_start deferred: host not ready");
        return;
    }

    ble_gap_adv_stop();

    // Use a stable public address for reconnects
    if (_own_addr_type != BLE_OWN_ADDR_PUBLIC)
    {
        load_public_addr();
    }
    ESP_LOGI(TAG, "adv_start addr_type=%d addr=%s", _own_addr_type, _addr_str);

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    // Put the full name into the scan response (keeps adv packet under 31 bytes)
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (uint8_t *)"Chromatic";
    rsp.name_len = (uint8_t)strlen("Chromatic");
    rsp.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "adv_set_fields failed rc=%d", rc);
        return;
    }

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_bridge_gap_event, NULL);
    if (rc == BLE_HS_EALREADY)
    {
        rc = 0;
    }

    _last_adv_rc = rc;
    _last_adv_rc_str = adv_err_string(rc);

    ESP_LOGI(TAG, "adv_start rc=%d (%s)", rc, _last_adv_rc_str);

    if (rc == 0)
    {
        set_state(kBLEBridge_State_Advertising);
    }
}

void BLEBridge_StartAdvertising(void)
{
    ESP_LOGI(TAG, "StartAdvertising requested (state=%d synced=%d pending=%d)", _state, ble_hs_synced(), _pending_adv_start);
    ble_bridge_start_advertising();
}

void BLEBridge_StopAdvertising(void)
{
    ESP_LOGI(TAG, "StopAdvertising requested (state=%d)", _state);
    _enabled = false;
    _pending_adv_start = false;
    ble_gap_adv_stop();
    // If already connected, keep the connection/notify state; otherwise mark stopped.
    if (_state != kBLEBridge_State_Connected)
    {
        _conn_handle = BLE_HS_CONN_HANDLE_NONE;
        _notify_enabled = false;
        set_state(kBLEBridge_State_Stopped);
    }
    _last_adv_rc = 0;
    _last_adv_rc_str = adv_err_string(_last_adv_rc);
    ESP_LOGI(TAG, "advertising stopped");
}

BLEBridge_State_t BLEBridge_GetState(void)
{
    return _state;
}

void BLEBridge_RegisterStateCb(BLEBridge_StateCb_t cb)
{
    _state_cb = cb;
}

void BLEBridge_SetEnabled(bool enabled)
{
    _enabled = enabled;
    if (_enabled)
    {
        ble_bridge_start_advertising();
    }
    else
    {
        BLEBridge_StopAdvertising();
    }
}

bool BLEBridge_IsEnabled(void)
{
    return _enabled;
}

static size_t fill_status_payload(uint8_t *buf, size_t len)
{
    if ((buf == NULL) || (len == 0))
    {
        return 0;
    }

    size_t used = 0;

    // Prefix with address so the app can remember/reconnect to this device.
    const char *addr = BLEBridge_GetAddrStr();
    if ((addr != NULL) && (addr[0] != '\0'))
    {
        const size_t addr_len = strlen(addr);
        if (addr_len + 5 < len) // "ADDR:" + addr + "\n"
        {
            memcpy(buf, "ADDR:", 5);
            memcpy(buf + 5, addr, addr_len);
            used = 5 + addr_len;
            buf[used++] = '\n';
        }
    }

    // Export current slots as newline-delimited 8-char codes for app/device sync
    used += Cheats_ExportSlots((char *)(buf + used), (len > used) ? (len - used) : 0);

    if (used < len)
    {
        buf[used] = '\0';
    }

    return used;
}

static int send_proxy_notify(const uint8_t *data, uint16_t len)
{
    if (!_proxy_notify_enabled || _conn_handle == BLE_HS_CONN_HANDLE_NONE)
    {
        return BLE_HS_ENOTCONN;
    }
    if (data == NULL || len == 0)
    {
        return BLE_HS_EINVAL;
    }

    int rc = BLE_HS_ENOMEM;
    for (int attempt = 0; attempt < 5; attempt++)
    {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (om == NULL)
        {
            rc = BLE_HS_ENOMEM;
        }
        else
        {
            rc = ble_gatts_notify_custom(_conn_handle, _proxy_val_handle, om);
            if (rc != 0)
            {
                os_mbuf_free_chain(om);
            }
        }

        if (rc == 0)
        {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (rc != 0)
    {
        ESP_LOGW(TAG, "proxy notify failed rc=%d", rc);
    }
    return rc;
}

void BLEBridge_PushStatusNotify(const uint8_t *data, uint8_t len)
{
    if (!_notify_enabled || _conn_handle == BLE_HS_CONN_HANDLE_NONE)
    {
        return;
    }
    if (data == NULL || len == 0)
    {
        return;
    }
    int rc = BLE_HS_ENOMEM;
    for (int attempt = 0; attempt < 5; attempt++)
    {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (om == NULL)
        {
            rc = BLE_HS_ENOMEM;
        }
        else
        {
            rc = ble_gatts_notify_custom(_conn_handle, _status_val_handle, om);
            if (rc != 0)
            {
                os_mbuf_free_chain(om);
            }
        }

        if (rc == 0)
        {
            break;
        }

        // Back off briefly if the controller/stack is busy
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (rc != 0)
    {
        ESP_LOGW(TAG, "notify failed rc=%d", rc);
    }
}

int BLEBridge_SendProxy(const uint8_t *data, uint16_t len)
{
    return send_proxy_notify(data, len);
}

void BLEBridge_RegisterProxyRxCb(BLEBridge_ProxyRxCb_t cb)
{
    _proxy_rx_cb = cb;
}

static int ble_bridge_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ble_uuid_cmp(ctxt->chr->uuid, &_status_uuid.u) == 0)
    {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        {
            uint8_t payload[256];
            memset(payload, 0, sizeof(payload));
            size_t used = fill_status_payload(payload, sizeof(payload));
            if (used == 0)
            {
                payload[0] = '\0';
                used = 1;
            }
            return os_mbuf_append(ctxt->om, payload, used) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }
    else if (ble_uuid_cmp(ctxt->chr->uuid, &_ingest_uuid.u) == 0)
    {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
        {
            size_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len > 256)
            {
                len = 256; // cap to avoid oversized writes
            }
            char buf[257];
            memset(buf, 0, sizeof(buf));
            int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
            if (rc == 0)
            {
                size_t applied = Cheats_ImportCodes(buf, len);
                ESP_LOGI(TAG, "ingest wrote %u bytes, applied %u slot changes", (unsigned)len, (unsigned)applied);
            }
            return 0;
        }
    }
    else if (ble_uuid_cmp(ctxt->chr->uuid, &_proxy_uuid.u) == 0)
    {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
        {
            size_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len > 256)
            {
                len = 256;
            }
            uint8_t buf[256];
            memset(buf, 0, sizeof(buf));
            int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
            if (rc == 0)
            {
                ESP_LOGI(TAG, "proxy rx %u bytes", (unsigned)len);
                if (_proxy_rx_cb != NULL)
                {
                    _proxy_rx_cb(buf, len);
                }
            }
            return 0;
        }
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_bridge_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0)
            {
                _conn_handle = event->connect.conn_handle;
                set_state(kBLEBridge_State_Connected);
                ESP_LOGI(TAG, "connected");
            }
            else
            {
                ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
                ble_bridge_start_advertising();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            _conn_handle = BLE_HS_CONN_HANDLE_NONE;
            set_state(kBLEBridge_State_Stopped);
            ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
            ble_bridge_start_advertising();
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGW(TAG, "adv complete; restarting");
            ble_bridge_start_advertising();
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == _status_val_handle)
            {
                _notify_enabled = event->subscribe.cur_notify;
                ESP_LOGI(TAG, "subscribe notify=%d", _notify_enabled);
            }
            else if (event->subscribe.attr_handle == _proxy_val_handle)
            {
                _proxy_notify_enabled = event->subscribe.cur_notify;
                ESP_LOGI(TAG, "proxy subscribe notify=%d", _proxy_notify_enabled);
                if (_proxy_notify_enabled)
                {
                    static const uint8_t ping[] = "RA_PROXY_PING";
                    send_proxy_notify(ping, sizeof(ping) - 1);
                }
            }
            break;

        default:
            break;
    }

    return 0;
}
