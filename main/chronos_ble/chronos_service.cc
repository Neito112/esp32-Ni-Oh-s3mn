#include "chronos_service.h"

#include <esp_log.h>
#include <sys/time.h>
#include <cstring>

#if CONFIG_BT_NIMBLE_ENABLED
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

#define TAG "ChronosService"

// Chronos Service UUID: 0xFEE0
static const ble_uuid16_t CHRONOS_SVC_UUID = BLE_UUID16_INIT(0xFEE0);
// RX Characteristic (Phone -> ESP32 Write without response): 0xFEE1
static const ble_uuid16_t CHRONOS_CHR_RX_UUID = BLE_UUID16_INIT(0xFEE1);
// TX Characteristic (ESP32 -> Phone Notify): 0xFEE2
static const ble_uuid16_t CHRONOS_CHR_TX_UUID = BLE_UUID16_INIT(0xFEE2);

static uint16_t g_tx_val_handle = 0;
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;

ChronosService& ChronosService::GetInstance() {
    static ChronosService instance;
    return instance;
}

#if CONFIG_BT_NIMBLE_ENABLED

static int ChronosGattAccess(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 0) {
            uint8_t buf[256];
            uint16_t copy_len = len < sizeof(buf) ? len : sizeof(buf);
            os_mbuf_copydata(ctxt->om, 0, copy_len, buf);
            ChronosService::GetInstance().HandlePacket(buf, copy_len);
        }
        return 0;
    }
    return 0;
}

static const struct ble_gatt_svc_def g_chronos_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &CHRONOS_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // RX: Phone writes packets here
                .uuid = &CHRONOS_CHR_RX_UUID.u,
                .access_cb = ChronosGattAccess,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // TX: ESP32 notifies Phone here
                .uuid = &CHRONOS_CHR_TX_UUID.u,
                .access_cb = ChronosGattAccess,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &g_tx_val_handle,
            },
            { 0 }
        },
    },
    { 0 }
};

static void ChronosAdvertise(void);

static int ChronosGapEvent(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "Chronos BLE connected! Connection handle: %d", event->connect.conn_handle);
                g_conn_handle = event->connect.conn_handle;
            } else {
                ChronosAdvertise();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Chronos BLE disconnected! Reason: %d", event->disconnect.reason);
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ChronosAdvertise();
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ChronosAdvertise();
            break;

        default:
            break;
    }
    return 0;
}

static void ChronosAdvertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char* name = "Xiaozhi S3";
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    ble_uuid16_t uuids16[1] = { CHRONOS_SVC_UUID };
    fields.uuids16 = uuids16;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting adv fields: rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, ChronosGapEvent, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error starting adv: rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Chronos BLE advertising started as '%s'", name);
    }
}

static void NimbleHostTask(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

#endif // CONFIG_BT_NIMBLE_ENABLED

esp_err_t ChronosService::Start(const std::string& device_name) {
    device_name_ = device_name;
#if CONFIG_BT_NIMBLE_ENABLED
    ESP_LOGI(TAG, "Initializing NimBLE for Chronos...");
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init nimble port: %d", ret);
        return ret;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(g_chronos_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Gatts count config error: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(g_chronos_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Gatts add services error: %d", rc);
        return ESP_FAIL;
    }

    ble_svc_gap_device_name_set(device_name_.c_str());

    nimble_port_freertos_init(NimbleHostTask);
    ChronosAdvertise();
    return ESP_OK;
#else
    ESP_LOGW(TAG, "NimBLE not enabled in sdkconfig. Defaults to mock mode.");
    return ESP_OK;
#endif
}

void ChronosService::Stop() {
#if CONFIG_BT_NIMBLE_ENABLED
    nimble_port_stop();
    nimble_port_deinit();
#endif
}

void ChronosService::SendMusicControl(uint8_t action) {
#if CONFIG_BT_NIMBLE_ENABLED
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || g_tx_val_handle == 0) {
        return;
    }
    uint8_t payload[2] = { 0x06, action };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, sizeof(payload));
    if (om != NULL) {
        ble_gattc_notify_custom(g_conn_handle, g_tx_val_handle, om);
    }
#endif
}

void ChronosService::HandlePacket(const uint8_t* data, size_t length) {
    if (data == nullptr || length < 2) {
        return;
    }

    uint8_t cmd = data[0];
    switch (cmd) {
        case 0x01: // Time sync
            ParseTime(data + 1, length - 1);
            break;
        case 0x04: // Notification
            ParseNotification(data + 1, length - 1);
            break;
        case 0x05: // Battery level
            ParseBattery(data + 1, length - 1);
            break;
        case 0x06: // Music info
            ParseMusic(data + 1, length - 1);
            break;
        default:
            ESP_LOGD(TAG, "Received Chronos cmd: 0x%02X, len=%d", cmd, (int)length);
            break;
    }
}

void ChronosService::ParseTime(const uint8_t* data, size_t len) {
    if (len < 6) return;
    int year = (data[0] << 8) | data[1];
    int mon = data[2];
    int day = data[3];
    int hour = data[4];
    int min = data[5];
    int sec = (len >= 7) ? data[6] : 0;

    struct tm tm_time = {};
    tm_time.tm_year = year - 1900;
    tm_time.tm_mon = mon - 1;
    tm_time.tm_mday = day;
    tm_time.tm_hour = hour;
    tm_time.tm_min = min;
    tm_time.tm_sec = sec;

    time_t now = mktime(&tm_time);
    struct timeval tv = { .tv_sec = now, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "Synchronized time from Chronos: %04d-%02d-%02d %02d:%02d:%02d",
             year, mon, day, hour, min, sec);
}

void ChronosService::ParseBattery(const uint8_t* data, size_t len) {
    if (len < 1) return;
    phone_battery_ = data[0];
    ESP_LOGI(TAG, "Phone battery level: %d%%", phone_battery_);
    if (on_battery_) {
        on_battery_(phone_battery_);
    }
}

void ChronosService::ParseNotification(const uint8_t* data, size_t len) {
    // Format: [icon_id, title_len, title..., msg_len, msg...]
    if (len < 2) return;
    uint8_t icon_id = data[0];
    uint8_t title_len = data[1];
    if (len < (size_t)(2 + title_len)) return;

    std::string title(reinterpret_cast<const char*>(data + 2), title_len);
    size_t offset = 2 + title_len;
    std::string message;

    if (offset < len) {
        uint8_t msg_len = data[offset];
        offset++;
        if (offset + msg_len <= len) {
            message = std::string(reinterpret_cast<const char*>(data + offset), msg_len);
        } else if (offset < len) {
            message = std::string(reinterpret_cast<const char*>(data + offset), len - offset);
        }
    }

    std::string app_name = "Thông báo";
    switch (icon_id) {
        case 1: app_name = "Tin nhắn SMS"; break;
        case 5: app_name = "Facebook"; break;
        case 9: app_name = "Messenger"; break;
        case 11: app_name = "Telegram"; break;
        case 13: app_name = "Zalo"; break;
        default: app_name = "Cuộc gọi / Ứng dụng"; break;
    }

    ESP_LOGI(TAG, "[Chronos Alert] App: %s | From: %s | Msg: %s",
             app_name.c_str(), title.c_str(), message.c_str());

    if (on_notification_) {
        on_notification_({ app_name, title, message, icon_id });
    }
}

void ChronosService::ParseMusic(const uint8_t* data, size_t len) {
    if (len < 1) return;
    uint8_t state = data[0]; // 1 = play, 2 = pause, 0 = stop
    size_t offset = 1;
    std::string title;
    std::string artist;

    if (offset < len) {
        uint8_t title_len = data[offset++];
        if (offset + title_len <= len) {
            title = std::string(reinterpret_cast<const char*>(data + offset), title_len);
            offset += title_len;
        }
    }

    if (offset < len) {
        uint8_t artist_len = data[offset++];
        if (offset + artist_len <= len) {
            artist = std::string(reinterpret_cast<const char*>(data + offset), artist_len);
        }
    }

    if (on_music_) {
        on_music_({ title, artist, "", state, 0, 0 });
    }
}

