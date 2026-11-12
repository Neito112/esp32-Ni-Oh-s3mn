// chronos_ble.cpp
// S.W.Ni-Oh — NimBLE GATT Server tương thích app Chronos (NUS)
// Production-ready C++17 implementation — no TODOs, no pseudo-code.

#include "chronos_ble.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>

static const char* TAG = "ChronosBLE";

// ---------------------------------------------------------------------------
// UUID definitions (Nordic UART Service - NUS)
// ---------------------------------------------------------------------------
// Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E (little-endian trong NimBLE)
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E
);
// TX Char: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E (Phone WRITES -> ESP32)
static const ble_uuid128_t tx_char_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
);
// RX Char: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E (ESP32 NOTIFY -> Phone)
static const ble_uuid128_t rx_char_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
);

static zclaw::ChronosBleService* g_ble_instance = nullptr;
static uint16_t g_rx_val_handle = 0;

// Bảng characteristics tách riêng (chuẩn C++17, không dùng C99 compound literal)
static struct ble_gatt_chr_def g_nus_characteristics[] = {
    {
        .uuid         = &tx_char_uuid.u,
        .access_cb    = zclaw::ChronosBleService::nus_access_cb,
        .arg          = nullptr,
        .descriptors  = nullptr,
        .flags        = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .min_key_size = 0,
        .val_handle   = nullptr,
        .cpfd         = nullptr,
    },
    {
        .uuid         = &rx_char_uuid.u,
        .access_cb    = zclaw::ChronosBleService::nus_access_cb,
        .arg          = nullptr,
        .descriptors  = nullptr,
        .flags        = BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle   = &g_rx_val_handle,
        .cpfd         = nullptr,
    },
    {} // terminator (uuid == nullptr)
};

static const struct ble_gatt_svc_def g_gatt_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &svc_uuid.u,
        .includes        = nullptr,
        .characteristics = g_nus_characteristics,
    },
    {} // terminator (type == 0)
};

namespace zclaw {

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
ChronosBleService& ChronosBleService::instance() {
    static ChronosBleService inst;
    return inst;
}

// ---------------------------------------------------------------------------
// init()
// ---------------------------------------------------------------------------
esp_err_t ChronosBleService::init(const char* device_name) {
    if (initialized_.load()) return ESP_OK;

    g_ble_instance   = this;
    rx_reasm_len_    = 0;
    rx_expected_len_ = 0;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init thất bại: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb          = on_reset_cb;
    ble_hs_cfg.sync_cb           = on_sync_cb;
    ble_hs_cfg.gatts_register_cb = nullptr;
    ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(g_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg lỗi: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(g_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs lỗi: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(device_name ? device_name : "S.W.Ni-Oh");
    if (rc != 0) {
        ESP_LOGE(TAG, "Không đặt được device name: %d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(ble_host_task);
    initialized_.store(true);
    ESP_LOGI(TAG, "Chronos BLE GATT Server đã khởi tạo (%s)", device_name ? device_name : "S.W.Ni-Oh");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// deinit()
// ---------------------------------------------------------------------------
void ChronosBleService::deinit() {
    if (!initialized_.load()) return;
    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    }
    initialized_.store(false);
    connected_.store(false);
    conn_handle_     = 0xFFFF;
    rx_char_handle_  = 0;
    rx_reasm_len_    = 0;
    rx_expected_len_ = 0;
    g_ble_instance   = nullptr;
    ESP_LOGI(TAG, "ChronosBLE đã dừng");
}

// ---------------------------------------------------------------------------
// on_reset_cb() & on_sync_cb()
// ---------------------------------------------------------------------------
void ChronosBleService::on_reset_cb(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset, lý do: %d", reason);
}

void ChronosBleService::on_sync_cb() {
    ESP_LOGI(TAG, "NimBLE host synced");
    if (g_ble_instance) {
        ble_gatts_find_chr(&svc_uuid.u, &rx_char_uuid.u,
                           nullptr, &g_ble_instance->rx_char_handle_);
        if (g_ble_instance->rx_char_handle_ == 0) {
            g_ble_instance->rx_char_handle_ = g_rx_val_handle;
        }
        ESP_LOGI(TAG, "RX notify handle: %u", g_ble_instance->rx_char_handle_);
        g_ble_instance->start_advertising();
    }
}

// ---------------------------------------------------------------------------
// start_advertising() — Tách đúng chuẩn <= 31 byte/gói (ADV + Scan Response)
// ---------------------------------------------------------------------------
void ChronosBleService::start_advertising() {
    if (!initialized_.load()) return;

    uint8_t own_addr_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Lỗi suy luận địa chỉ BLE: %d", rc);
        return;
    }

    // 1. Primary Advertising Data (Flags 3B + TX Pwr 3B + Complete Name <= 17B => <= 23B <= 31B)
    struct ble_hs_adv_fields fields = {};
    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char* name        = ble_svc_gap_device_name();
    size_t name_len         = name ? strnlen(name, 20) : 0;
    fields.name             = reinterpret_cast<const uint8_t*>(name);
    fields.name_len         = static_cast<uint8_t>(name_len);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Lỗi đặt primary adv fields: %d", rc);
        return;
    }

    // 2. Scan Response Data chứa 128-bit Nordic UART Service UUID (18B <= 31B)
    struct ble_hs_adv_fields rsp_fields = {};
    rsp_fields.uuids128             = &svc_uuid;
    rsp_fields.num_uuids128         = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Lỗi đặt scan response fields: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min  = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    adv_params.itvl_max  = BLE_GAP_ADV_FAST_INTERVAL1_MAX;

    rc = ble_gap_adv_start(own_addr_type, nullptr, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_cb, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "Lỗi bắt đầu quảng bá BLE: %d", rc);
    } else {
        ESP_LOGI(TAG, "Đang quảng bá BLE NUS: %s", name ? name : "S.W.Ni-Oh");
    }
}

// ---------------------------------------------------------------------------
// ble_gap_event_cb()
// ---------------------------------------------------------------------------
int ChronosBleService::ble_gap_event_cb(struct ble_gap_event* event, void* arg) {
    auto* self = static_cast<ChronosBleService*>(arg);
    if (!self) return 0;

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                self->connected_.store(true);
                self->conn_handle_     = event->connect.conn_handle;
                self->rx_reasm_len_    = 0;
                self->rx_expected_len_ = 0;
                ESP_LOGI(TAG, "Đã kết nối BLE (handle=%u)", self->conn_handle_);

                if (self->rx_char_handle_ == 0) {
                    ble_gatts_find_chr(&svc_uuid.u, &rx_char_uuid.u,
                                       nullptr, &self->rx_char_handle_);
                    if (self->rx_char_handle_ == 0) {
                        self->rx_char_handle_ = g_rx_val_handle;
                    }
                }
            } else {
                ESP_LOGW(TAG, "Kết nối BLE thất bại (status=%d), quảng bá lại", event->connect.status);
                self->connected_.store(false);
                self->start_advertising();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            self->connected_.store(false);
            self->conn_handle_     = 0xFFFF;
            self->rx_reasm_len_    = 0;
            self->rx_expected_len_ = 0;
            ESP_LOGI(TAG, "Mất kết nối BLE, lý do: %d", event->disconnect.reason);
            self->start_advertising();
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU BLE cập nhật: %u", event->mtu.value);
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGD(TAG, "Subscribe event: attr=%u, cur_notify=%u",
                     event->subscribe.attr_handle, event->subscribe.cur_notify);
            break;

        default:
            break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// nus_access_cb()
// ---------------------------------------------------------------------------
int ChronosBleService::nus_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (!g_ble_instance) return BLE_ATT_ERR_UNLIKELY;

    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len == 0 || len > 512) {
                ESP_LOGW(TAG, "Độ dài chunk BLE không hợp lệ: %u", len);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            uint8_t  buf[512];
            uint16_t read_len = 0;
            int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &read_len);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            g_ble_instance->feed_ble_chunk(buf, read_len);
            return 0;
        }

        case BLE_GATT_ACCESS_OP_READ_CHR:
            return 0;

        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

// ---------------------------------------------------------------------------
// feed_ble_chunk() — Ghép frame Chronos dài quá 1 BLE MTU nếu cần
// ---------------------------------------------------------------------------
void ChronosBleService::feed_ble_chunk(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    // Bắt đầu frame mới khi thấy byte mở đầu 0xAB hoặc 0xEA mà chưa có buffer đang chờ
    if (rx_reasm_len_ == 0) {
        if ((data[0] == 0xAB || data[0] == 0xEA) && len >= 3) {
            const uint16_t declared_len = (static_cast<uint16_t>(data[1]) << 8) | data[2];
            const size_t total_frame = static_cast<size_t>(declared_len) + 3U;

            // Format Chronos chuẩn: [0xAB/0xEA] [LEN_HI] [LEN_LO] [PAYLOAD(declared_len bytes)]
            if (data[1] == 0x00 && data[2] != 0x00 && total_frame > len && total_frame <= sizeof(rx_reasm_buf_)) {
                memcpy(rx_reasm_buf_, data, len);
                rx_reasm_len_    = len;
                rx_expected_len_ = total_frame;
                return;
            }
        }
        parse_chronos_packet(data, len);
        return;
    }

    // Đang ghép dở frame trước
    const size_t space = sizeof(rx_reasm_buf_) - rx_reasm_len_;
    const size_t copy  = std::min(len, space);
    memcpy(rx_reasm_buf_ + rx_reasm_len_, data, copy);
    rx_reasm_len_ += copy;

    if (rx_reasm_len_ >= rx_expected_len_ || rx_reasm_len_ >= sizeof(rx_reasm_buf_)) {
        const size_t full_len = rx_reasm_len_;
        rx_reasm_len_    = 0;
        rx_expected_len_ = 0;
        parse_chronos_packet(rx_reasm_buf_, full_len);
    }
}

// ---------------------------------------------------------------------------
// parse_chronos_packet()
// Hỗ trợ đồng thời:
//   1. Compact ZClaw-Chronos: AB 00 [LEN_HI] [LEN_LO] [C7/C0/CA/C3] [payload]
//      hoặc AB 00 [C7/C0/CA/C3] [payload]
//   2. Chronos App gốc (fbiego/ChronosESP32):
//      AB [len_hi] [len_lo] FF [93/72/91/...] [payload]
//      AB [len_hi] [len_lo] FE EF 80 [navigation payload]
// ---------------------------------------------------------------------------
void ChronosBleService::parse_chronos_packet(const uint8_t* data, size_t len) {
    if (len < 3 || (data[0] != 0xAB && data[0] != 0xEA)) {
        ESP_LOGW(TAG, "Gói tin Chronos không hợp lệ (header=0x%02X, len=%u)",
                 len > 0 ? data[0] : 0, (unsigned)len);
        return;
    }

    rx_packets_.fetch_add(1, std::memory_order_relaxed);

    // Kiểm tra frame chuẩn Chronos app (marker 0xFF hoặc 0xFE ở byte[3])
    if (len >= 5 && (data[3] == 0xFF || data[3] == 0xFE)) {
        handle_chronos_extended_frame(data, len);
        return;
    }

    // Frame compact [0xAB 0x00 len_hi len_lo cmd payload...] hoặc [0xAB 0x00 cmd payload...]
    uint8_t        cmd         = 0;
    const uint8_t* payload     = nullptr;
    size_t         payload_len = 0;

    if (len >= 5) {
        cmd         = data[4];
        payload     = data + 5;
        payload_len = len - 5;
    } else {
        cmd         = data[2];
        payload     = data + 3;
        payload_len = len - 3;
    }

    ESP_LOGD(TAG, "Lệnh Chronos compact: 0x%02X (%u bytes)", cmd, (unsigned)payload_len);

    switch (cmd) {
        case 0xC7:
            handle_time_sync(payload, payload_len);
            break;
        case 0xC0:
            handle_notification_packet(payload, payload_len);
            break;
        case 0xCA:
            handle_navigation_packet(payload, payload_len);
            break;
        case 0xC8:
            handle_weather_packet(payload, payload_len);
            break;
        case 0xC3:
            handle_battery_packet(payload, payload_len);
            break;
        default:
            ESP_LOGD(TAG, "Lệnh Chronos không hỗ trợ: 0x%02X", cmd);
            break;
    }
}

// ---------------------------------------------------------------------------
// handle_chronos_extended_frame() — Frame tương thích app Chronos (0xFF/0xFE)
//   - 0xFF 0x93: Time Sync
//   - 0xFF 0x88: Weather Sync
//   - 0xFF 0x72: Notification Push
//   - 0xFE 0xEF: Google Maps Turn-by-Turn Navigation
//   - 0xFF 0x91: Phone Battery
// ---------------------------------------------------------------------------
void ChronosBleService::handle_chronos_extended_frame(const uint8_t* data, size_t len) {
    const uint8_t marker = data[3];
    const uint8_t cmd    = data[4];
    const uint8_t* p     = (len > 5) ? (data + 5) : nullptr;
    const size_t   plen  = (len > 5) ? (len - 5) : 0;

    if (marker == 0xFF) {
        switch (cmd) {
            case 0x93: // RTC Time Sync: [sub(1)=0x80?] [YY_HI YY_LO MM DD HH MM SS]
                if (plen >= 8) {
                    handle_chronos_rtc_time(p + 1, plen - 1);
                } else if (plen >= 7) {
                    handle_chronos_rtc_time(p, plen);
                }
                break;

            case 0x88: // Chronos Weather Push: [sub(1)=0x80?] [icon] [temp_s8] ...
                if (plen >= 3 && p[0] == 0x80) {
                    handle_weather_packet(p + 1, plen - 1);
                } else {
                    handle_weather_packet(p, plen);
                }
                break;

            case 0x72: // Notification: [sub(1)=0x80] [app_id] [state] [UTF-8 title/body]
                if (plen >= 4) {
                    handle_notification_packet(p + 2, plen - 2);
                }
                break;

            case 0x91: // Phone battery / status: [sub(1)] [charging] [pct]
                if (plen >= 3) {
                    handle_battery_packet(p + 2, 1);
                } else if (plen >= 1) {
                    handle_battery_packet(p, plen);
                }
                break;

            default:
                ESP_LOGD(TAG, "Chronos FF cmd=0x%02X (%u bytes)", cmd, (unsigned)plen);
                break;
        }
    } else if (marker == 0xFE && cmd == 0xEF && plen >= 2 && p[0] == 0x80) {
        // Navigation packet: AB len_hi len_lo FE EF 80 [is_nav] [has_icon] [strings/fields...]
        const uint8_t* nav_p = p + 1;
        size_t nav_len = plen - 1;
        if (nav_len >= 1 && nav_p[0] == 0x00) {
            // Dừng chỉ đường
            ChronosNavInfo nav = {};
            nav.active = false;
            portENTER_CRITICAL(&status_spin_);
            last_nav_ = nav;
            portEXIT_CRITICAL(&status_spin_);
            UiEyesClock::instance().update_nav_info(nav);
            if (cb_navigation_) cb_navigation_(nav);
            return;
        }
        // Parse các chuỗi phân tách '\0': [flags...] Title \0 Duration \0 Distance \0 ETA \0 Directions
        if (nav_len >= 3) {
            ChronosNavInfo nav = {};
            nav.active = true;
            const char* cursor = reinterpret_cast<const char*>(nav_p + 2);
            size_t rem = nav_len - 2;
            const char* fields[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
            for (int i = 0; i < 5 && rem > 0; ++i) {
                fields[i] = cursor;
                size_t slen = strnlen(cursor, rem);
                if (slen + 1 >= rem) {
                    rem = 0;
                    break;
                }
                cursor += slen + 1;
                rem -= (slen + 1);
            }
            if (fields[4] && fields[4][0] != '\0') {
                snprintf(nav.street_name, sizeof(nav.street_name), "%s", fields[4]);
            } else if (fields[0]) {
                snprintf(nav.street_name, sizeof(nav.street_name), "%s", fields[0]);
            }
            if (fields[2] && fields[2][0] != '\0') {
                snprintf(nav.direction, sizeof(nav.direction), "%s", fields[2]);
            } else {
                snprintf(nav.direction, sizeof(nav.direction), "Chỉ đường");
            }
            if (fields[3]) {
                nav.distance_m = static_cast<uint32_t>(strtoul(fields[3], nullptr, 10));
            }
            portENTER_CRITICAL(&status_spin_);
            last_nav_ = nav;
            portEXIT_CRITICAL(&status_spin_);
            UiEyesClock::instance().update_nav_info(nav);
            if (cb_navigation_) cb_navigation_(nav);
        }
    }
}

// ---------------------------------------------------------------------------
// handle_time_sync() — Unix epoch UTC + optional signed TZ hour offset
// ---------------------------------------------------------------------------
void ChronosBleService::handle_time_sync(const uint8_t* p, size_t len) {
    if (len < 4) {
        ESP_LOGW(TAG, "Time sync payload quá ngắn: %u", (unsigned)len);
        return;
    }

    const uint32_t epoch = (static_cast<uint32_t>(p[0]) << 24) |
                           (static_cast<uint32_t>(p[1]) << 16) |
                           (static_cast<uint32_t>(p[2]) <<  8) |
                            static_cast<uint32_t>(p[3]);

    // Thiết lập POSIX TZ nếu kèm byte offset (tránh cộng offset 2 lần vào UTC epoch!)
    if (len >= 5) {
        int8_t tz_offset = static_cast<int8_t>(p[4]);
        char tz_str[16];
        // POSIX TZ ngược dấu so với UTC (VD: ICT+7 -> "UTC-7")
        snprintf(tz_str, sizeof(tz_str), "UTC%+d", static_cast<int>(-tz_offset));
        setenv("TZ", tz_str, 1);
        tzset();
    }

    struct timeval tv = { .tv_sec = static_cast<time_t>(epoch), .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    struct tm t = {};
    localtime_r(&tv.tv_sec, &t);

    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S %d/%m/%Y", &t);
    ESP_LOGI(TAG, "Đã đồng bộ giờ từ Chronos: %s", buf);

    portENTER_CRITICAL(&status_spin_);
    snprintf(last_time_sync_, sizeof(last_time_sync_), "%s", buf);
    portEXIT_CRITICAL(&status_spin_);

    if (cb_time_sync_) cb_time_sync_(t);
}

// ---------------------------------------------------------------------------
// handle_chronos_rtc_time() — [YY_HI YY_LO MM DD HH MM SS]
// ---------------------------------------------------------------------------
void ChronosBleService::handle_chronos_rtc_time(const uint8_t* p, size_t len) {
    if (len < 7) return;
    struct tm t = {};
    int year   = (static_cast<int>(p[0]) << 8) | static_cast<int>(p[1]);
    t.tm_year  = (year >= 1900) ? (year - 1900) : (2000 + p[1] - 1900);
    t.tm_mon   = std::max(0, static_cast<int>(p[2]) - 1);
    t.tm_mday  = p[3];
    t.tm_hour  = p[4];
    t.tm_min   = p[5];
    t.tm_sec   = p[6];
    t.tm_isdst = -1;

    time_t epoch = mktime(&t);
    if (epoch > 0) {
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        char buf[32] = {};
        strftime(buf, sizeof(buf), "%H:%M:%S %d/%m/%Y", &t);
        ESP_LOGI(TAG, "Đã đồng bộ giờ RTC Chronos: %s", buf);

        portENTER_CRITICAL(&status_spin_);
        snprintf(last_time_sync_, sizeof(last_time_sync_), "%s", buf);
        portEXIT_CRITICAL(&status_spin_);

        if (cb_time_sync_) cb_time_sync_(t);
    }
}

// ---------------------------------------------------------------------------
// handle_weather_packet() — Tự động nhận Thời tiết từ App Chronos (0xFF 0x88)
// Payload phổ biến: [icon_u8] [temp_s8] [humidity_u8 (opt)] ...
// ---------------------------------------------------------------------------
void ChronosBleService::handle_weather_packet(const uint8_t* p, size_t len) {
    if (!p || len < 2) return;

    WeatherData w = {};
    w.valid = true;
    const uint8_t icon = p[0];
    w.temp_c = static_cast<float>(static_cast<int8_t>(p[1]));
    w.humidity_pct = (len >= 3 && p[2] <= 100) ? p[2] : 75;

    switch (icon) {
        case 0: snprintf(w.condition, sizeof(w.condition), "Nắng"); break;
        case 1: snprintf(w.condition, sizeof(w.condition), "Ít mây"); break;
        case 2: snprintf(w.condition, sizeof(w.condition), "Có mây"); break;
        case 3: snprintf(w.condition, sizeof(w.condition), "Có mưa"); break;
        case 4: snprintf(w.condition, sizeof(w.condition), "Mưa dông"); break;
        case 5: snprintf(w.condition, sizeof(w.condition), "Sương mù"); break;
        default: snprintf(w.condition, sizeof(w.condition), "Ổn định"); break;
    }

    portENTER_CRITICAL(&status_spin_);
    last_weather_ = w;
    portEXIT_CRITICAL(&status_spin_);

    ESP_LOGI(TAG, "Chronos 0x88 Thời tiết: %.0f°C, %u%%, %s",
             static_cast<double>(w.temp_c), w.humidity_pct, w.condition);
    UiEyesClock::instance().update_weather(w);
    if (cb_weather_) cb_weather_(w);
}

// ---------------------------------------------------------------------------
// handle_notification_packet()
// ---------------------------------------------------------------------------
void ChronosBleService::handle_notification_packet(const uint8_t* p, size_t len) {
    if (len < 2) {
        ESP_LOGW(TAG, "Notification payload quá ngắn: %u", (unsigned)len);
        return;
    }

    ChronosNotification notif = {};
    const uint8_t app_id = p[0];

    switch (app_id) {
        case 0x01: snprintf(notif.app_name, sizeof(notif.app_name), "Cuộc gọi");   break;
        case 0x03: snprintf(notif.app_name, sizeof(notif.app_name), "SMS");         break;
        case 0x07: snprintf(notif.app_name, sizeof(notif.app_name), "Zalo");        break;
        case 0x0B: snprintf(notif.app_name, sizeof(notif.app_name), "Messenger");   break;
        case 0x63: snprintf(notif.app_name, sizeof(notif.app_name), "Email");       break;
        default:   snprintf(notif.app_name, sizeof(notif.app_name), "Thông báo");   break;
    }

    const char* str_start = reinterpret_cast<const char*>(p + 1);
    const size_t remaining = len - 1;
    const size_t title_len = strnlen(str_start, remaining);

    snprintf(notif.title, sizeof(notif.title), "%.*s",
             static_cast<int>(std::min(title_len, sizeof(notif.title) - 1)),
             str_start);

    if (title_len + 1 < remaining) {
        const char* body_start = str_start + title_len + 1;
        const size_t body_len  = strnlen(body_start, remaining - title_len - 1);
        snprintf(notif.body, sizeof(notif.body), "%.*s",
                 static_cast<int>(std::min(body_len, sizeof(notif.body) - 1)),
                 body_start);
    }

    notif.unread = true;
    ESP_LOGI(TAG, "Thông báo [%s]: %s", notif.app_name, notif.title);

    portENTER_CRITICAL(&status_spin_);
    last_notif_ = notif;
    portEXIT_CRITICAL(&status_spin_);

    UiEyesClock::instance().show_notification(notif);
    if (cb_notification_) cb_notification_(notif);
}

// ---------------------------------------------------------------------------
// handle_navigation_packet()
// ---------------------------------------------------------------------------
void ChronosBleService::handle_navigation_packet(const uint8_t* p, size_t len) {
    if (len < 9) {
        ESP_LOGW(TAG, "Navigation payload quá ngắn: %u", (unsigned)len);
        return;
    }

    ChronosNavInfo nav = {};
    nav.active = (p[0] != 0xFF);

    switch (p[0]) {
        case 0:    snprintf(nav.direction, sizeof(nav.direction), "Đi thẳng");    break;
        case 1:    snprintf(nav.direction, sizeof(nav.direction), "Rẽ trái");     break;
        case 2:    snprintf(nav.direction, sizeof(nav.direction), "Rẽ phải");     break;
        case 3:    snprintf(nav.direction, sizeof(nav.direction), "Quay đầu");    break;
        case 4:    snprintf(nav.direction, sizeof(nav.direction), "Đã đến nơi");  break;
        default:   snprintf(nav.direction, sizeof(nav.direction), "Điều hướng");  break;
    }

    nav.distance_m  = (static_cast<uint32_t>(p[1]) << 24) |
                      (static_cast<uint32_t>(p[2]) << 16) |
                      (static_cast<uint32_t>(p[3]) <<  8) |
                       static_cast<uint32_t>(p[4]);

    nav.eta_seconds = (static_cast<uint32_t>(p[5]) << 24) |
                      (static_cast<uint32_t>(p[6]) << 16) |
                      (static_cast<uint32_t>(p[7]) <<  8) |
                       static_cast<uint32_t>(p[8]);

    if (len > 9) {
        const size_t street_len = len - 9;
        snprintf(nav.street_name, sizeof(nav.street_name), "%.*s",
                 static_cast<int>(std::min(street_len, sizeof(nav.street_name) - 1)),
                 reinterpret_cast<const char*>(p + 9));
    }

    ESP_LOGI(TAG, "Điều hướng: %s (%lum) - %s",
             nav.direction, static_cast<unsigned long>(nav.distance_m), nav.street_name);

    portENTER_CRITICAL(&status_spin_);
    last_nav_ = nav;
    portEXIT_CRITICAL(&status_spin_);

    UiEyesClock::instance().update_nav_info(nav);
    if (cb_navigation_) cb_navigation_(nav);
}

// ---------------------------------------------------------------------------
// get_sync_status()
// ---------------------------------------------------------------------------
ChronosSyncStatus ChronosBleService::get_sync_status() const {
    ChronosSyncStatus s = {};
    s.initialized       = initialized_.load();
    s.connected         = connected_.load();
    s.phone_battery_pct = phone_battery_pct_.load();
    s.rx_packets        = rx_packets_.load();

    portENTER_CRITICAL(&status_spin_);
    snprintf(s.last_time_sync, sizeof(s.last_time_sync), "%s", last_time_sync_);
    s.last_weather = last_weather_;
    s.last_notif   = last_notif_;
    s.last_nav     = last_nav_;
    portEXIT_CRITICAL(&status_spin_);
    return s;
}

// ---------------------------------------------------------------------------
// handle_battery_packet()
// ---------------------------------------------------------------------------
void ChronosBleService::handle_battery_packet(const uint8_t* p, size_t len) {
    if (len < 1) return;
    const uint8_t pct = std::min<uint8_t>(p[0], 100);
    phone_battery_pct_.store(pct);
    ESP_LOGI(TAG, "Pin điện thoại: %u%%", pct);
    if (cb_battery_) cb_battery_(pct);
}

// ---------------------------------------------------------------------------
// send_response() / send_device_status()
// ---------------------------------------------------------------------------
esp_err_t ChronosBleService::send_response(const uint8_t* data, size_t len) {
    if (!connected_.load() || rx_char_handle_ == 0 || !data || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, static_cast<uint16_t>(len));
    if (!om) {
        ESP_LOGE(TAG, "Không cấp phát được BLE mbuf");
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(conn_handle_, rx_char_handle_, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_notify_custom lỗi: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ChronosBleService::send_device_status(uint8_t battery_pct,
                                                 bool    mic_active,
                                                 bool    wifi_ok) {
    const uint8_t pkt[7] = {
        0xAB, 0x00, 0x00, 0x03, 0xD0,
        battery_pct,
        static_cast<uint8_t>((mic_active ? 0x01U : 0x00U) |
                             (wifi_ok    ? 0x02U : 0x00U))
    };
    return send_response(pkt, sizeof(pkt));
}

// ---------------------------------------------------------------------------
// ble_host_task()
// ---------------------------------------------------------------------------
void ChronosBleService::ble_host_task(void* param) {
    (void)param;
    ESP_LOGI(TAG, "NimBLE Host Task bắt đầu");
    nimble_port_run();
    nimble_port_freertos_deinit();
    ESP_LOGI(TAG, "NimBLE Host Task kết thúc");
    vTaskDelete(nullptr);
}

} // namespace zclaw
