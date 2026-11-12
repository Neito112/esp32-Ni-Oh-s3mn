// espnow_comm.cpp
// Xiaozhi-VN ZClaw Edition — ESP-NOW 2 chiều song song Wi-Fi & BLE (ESP-IDF v5.x)
// Production-ready C++17 implementation — no TODOs, no pseudo-code.

#include "espnow_comm.h"
#include "audio_hal.h"
#include "ui_eyes_clock.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

static const char* TAG = "ESPNow";

namespace zclaw {

static constexpr uint8_t  kBroadcastMac[6]   = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static constexpr uint8_t  kMagicByte         = 0xEC;
static constexpr uint8_t  kProtoVersion      = 0x01;
static constexpr size_t   kRxQueueDepth      = 8;
static constexpr uint32_t kWorkerTaskStack   = 4096;
static constexpr UBaseType_t kWorkerTaskPrio = 5;

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------
EspNowMeshManager& EspNowMeshManager::instance() {
    static EspNowMeshManager inst;
    return inst;
}

// ---------------------------------------------------------------------------
// init()
// ---------------------------------------------------------------------------
esp_err_t EspNowMeshManager::init() {
    if (initialized_.load()) return ESP_OK;

    // Lấy MAC STA của thiết bị
    esp_err_t ret = esp_read_mac(own_mac_, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Không đọc được MAC STA: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MAC ESP-NOW (STA): %02X:%02X:%02X:%02X:%02X:%02X",
             own_mac_[0], own_mac_[1], own_mac_[2],
             own_mac_[3], own_mac_[4], own_mac_[5]);

    // Lấy kênh Wi-Fi hiện tại để đồng bộ ESP-NOW
    uint8_t primary = 1;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK && primary >= 1 && primary <= 14) {
        wifi_channel_.store(primary);
        ESP_LOGI(TAG, "Đồng bộ kênh Wi-Fi/ESP-NOW: kênh %u", primary);
    } else {
        wifi_channel_.store(1);
        ESP_LOGW(TAG, "Chưa có kênh Wi-Fi hoạt động, tạm đặt kênh %u", wifi_channel_.load());
    }

    // Reset dedup table
    portENTER_CRITICAL(&dedup_lock_);
    memset(dedup_buf_, 0, sizeof(dedup_buf_));
    dedup_idx_ = 0;
    portEXIT_CRITICAL(&dedup_lock_);

    cb_mutex_ = xSemaphoreCreateMutex();
    rx_queue_ = xQueueCreate(kRxQueueDepth, sizeof(QueuedRxItem));
    if (!cb_mutex_ || !rx_queue_) {
        ESP_LOGE(TAG, "Không đủ RAM tạo mutex/queue cho ESP-NOW");
        deinit();
        return ESP_ERR_NO_MEM;
    }

    task_running_.store(true);
    if (xTaskCreatePinnedToCore(rx_worker_task, "espnow_rx", kWorkerTaskStack,
                                this, kWorkerTaskPrio, &rx_task_handle_, 0) != pdPASS) {
        ESP_LOGE(TAG, "Không tạo được espnow_rx task");
        deinit();
        return ESP_FAIL;
    }

    ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init thất bại: %s", esp_err_to_name(ret));
        deinit();
        return ret;
    }

    ret = esp_now_register_recv_cb(on_data_recv);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Đăng ký recv_cb thất bại: %s", esp_err_to_name(ret));
        esp_now_deinit();
        deinit();
        return ret;
    }

    ret = esp_now_register_send_cb(on_data_sent);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Đăng ký send_cb thất bại: %s", esp_err_to_name(ret));
        esp_now_unregister_recv_cb();
        esp_now_deinit();
        deinit();
        return ret;
    }

    // Thêm Broadcast peer mặc định (FF:FF:FF:FF:FF:FF)
    ret = add_peer(kBroadcastMac);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Cảnh báo thêm Broadcast peer: %s", esp_err_to_name(ret));
    }

    initialized_.store(true);
    ESP_LOGI(TAG, "ESP-NOW 2 chiều sẵn sàng trên kênh %u", wifi_channel_.load());
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// deinit()
// ---------------------------------------------------------------------------
void EspNowMeshManager::deinit() {
    const bool was_init = initialized_.exchange(false);
    task_running_.store(false);

    if (was_init) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        esp_now_deinit();
    }

    // Đánh thức worker task nếu đang chờ queue để thoát sạch sẽ
    if (rx_queue_) {
        QueuedRxItem dummy = {};
        xQueueSend(rx_queue_, &dummy, 0);
    }
    for (int i = 0; i < 30 && rx_task_handle_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (rx_task_handle_) {
        vTaskDelete(rx_task_handle_);
        rx_task_handle_ = nullptr;
    }
    if (rx_queue_) {
        vQueueDelete(rx_queue_);
        rx_queue_ = nullptr;
    }
    if (cb_mutex_) {
        vSemaphoreDelete(cb_mutex_);
        cb_mutex_ = nullptr;
    }
    ESP_LOGI(TAG, "ESP-NOW đã dừng");
}

// ---------------------------------------------------------------------------
// sync_channel_with_wifi() — Cập nhật kênh toàn bộ peer khi Wi-Fi đổi kênh
// ---------------------------------------------------------------------------
esp_err_t EspNowMeshManager::sync_channel_with_wifi() {
    uint8_t primary = 1;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_err_t ret = esp_wifi_get_channel(&primary, &second);
    if (ret != ESP_OK || primary < 1 || primary > 14) {
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }

    const uint8_t prev = wifi_channel_.exchange(primary);
    if (!initialized_.load()) {
        return ESP_OK;
    }

    if (prev != primary) {
        ESP_LOGI(TAG, "Kênh Wi-Fi thay đổi (%u -> %u), đồng bộ lại ESP-NOW peers", prev, primary);
    }

    esp_now_peer_info_t peer = {};
    esp_err_t fetch_ret = esp_now_fetch_peer(true, &peer);
    while (fetch_ret == ESP_OK) {
        if (peer.channel != primary) {
            peer.channel = primary;
            esp_err_t mod_ret = esp_now_mod_peer(&peer);
            if (mod_ret != ESP_OK) {
                ESP_LOGW(TAG, "Lỗi cập nhật kênh peer: %s", esp_err_to_name(mod_ret));
            }
        }
        fetch_ret = esp_now_fetch_peer(false, &peer);
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// send_command() / broadcast()
// ---------------------------------------------------------------------------
esp_err_t EspNowMeshManager::send_command(const uint8_t dst_mac[6], EspNowCmd cmd, const char* payload) {
    if (!initialized_.load() || !dst_mac) return ESP_ERR_INVALID_STATE;

    EspNowPacket pkt = {};
    pkt.magic   = kMagicByte;
    pkt.version = kProtoVersion;
    pkt.cmd     = cmd;
    pkt.seq_num = seq_counter_.fetch_add(1);
    memcpy(pkt.sender_mac, own_mac_, 6);

    if (payload && payload[0] != '\0') {
        const size_t plen = strnlen(payload, sizeof(pkt.payload) - 1);
        memcpy(pkt.payload, payload, plen);
        pkt.payload[plen] = '\0';
        pkt.payload_len   = static_cast<uint16_t>(plen);
    } else {
        pkt.payload[0]  = '\0';
        pkt.payload_len = 0;
    }

    const size_t pkt_size_no_crc = offsetof(EspNowPacket, crc8);
    pkt.crc8 = compute_crc8(reinterpret_cast<const uint8_t*>(&pkt), pkt_size_no_crc);

    if (!esp_now_is_peer_exist(dst_mac)) {
        esp_err_t add_ret = add_peer(dst_mac);
        if (add_ret != ESP_OK) return add_ret;
    }

    esp_err_t ret = esp_now_send(dst_mac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Lỗi gửi ESP-NOW: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t EspNowMeshManager::broadcast(EspNowCmd cmd, const char* payload) {
    return send_command(kBroadcastMac, cmd, payload);
}

// ---------------------------------------------------------------------------
// Peer management
// ---------------------------------------------------------------------------
esp_err_t EspNowMeshManager::add_peer(const uint8_t mac[6]) {
    if (!mac) return ESP_ERR_INVALID_ARG;
    if (esp_now_is_peer_exist(mac)) return ESP_OK;

    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, mac, 6);
    peer_info.channel = wifi_channel_.load();
    peer_info.ifidx   = WIFI_IF_STA;
    peer_info.encrypt = false;

    esp_err_t ret = esp_now_add_peer(&peer_info);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Đã thêm ESP-NOW peer: %02X:%02X:%02X:%02X:%02X:%02X (kênh %u)",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], peer_info.channel);
    } else {
        ESP_LOGE(TAG, "Lỗi thêm peer: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t EspNowMeshManager::remove_peer(const uint8_t mac[6]) {
    if (!mac) return ESP_ERR_INVALID_ARG;
    if (!esp_now_is_peer_exist(mac)) return ESP_OK;

    esp_err_t ret = esp_now_del_peer(mac);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Đã xóa ESP-NOW peer: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return ret;
}

bool EspNowMeshManager::is_peer_exists(const uint8_t mac[6]) const {
    if (!mac) return false;
    return esp_now_is_peer_exist(mac);
}

// ---------------------------------------------------------------------------
// set_rx_callback()
// ---------------------------------------------------------------------------
void EspNowMeshManager::set_rx_callback(EspNowRxCallback cb) {
    if (!cb_mutex_) {
        rx_callback_ = cb;
        return;
    }
    if (xSemaphoreTake(cb_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        rx_callback_ = cb;
        xSemaphoreGive(cb_mutex_);
    }
}

// ---------------------------------------------------------------------------
// on_data_recv() — Chạy trong Wi-Fi task: chỉ verify nhanh & đẩy vào queue!
//                  Tuyệt đối KHÔNG gọi I2S/LVGL/blocking trực tiếp tại đây.
// ---------------------------------------------------------------------------
void EspNowMeshManager::on_data_recv(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len) {
    auto& self = EspNowMeshManager::instance();
    if (!self.initialized_.load() || !recv_info || !data || !self.rx_queue_) return;

    if (len != static_cast<int>(sizeof(EspNowPacket))) {
        ESP_LOGD(TAG, "Kích thước gói ESP-NOW không khớp: %d != %u", len, (unsigned)sizeof(EspNowPacket));
        return;
    }

    const EspNowPacket* pkt = reinterpret_cast<const EspNowPacket*>(data);
    if (pkt->magic != kMagicByte || pkt->version != kProtoVersion) {
        return;
    }

    const size_t pkt_size_no_crc = offsetof(EspNowPacket, crc8);
    const uint8_t expected_crc   = compute_crc8(data, pkt_size_no_crc);
    if (pkt->crc8 != expected_crc) {
        ESP_LOGW(TAG, "CRC8 sai: nhận 0x%02X, kỳ vọng 0x%02X", pkt->crc8, expected_crc);
        return;
    }

    // Bỏ qua gói tin loopback nếu chính mình broadcast
    if (memcmp(recv_info->src_addr, self.own_mac_, 6) == 0) {
        return;
    }

    if (self.check_and_mark_seen(recv_info->src_addr, pkt->seq_num)) {
        return;
    }

    QueuedRxItem item = {};
    memcpy(item.src_mac, recv_info->src_addr, 6);
    item.pkt = *pkt;
    // Bảo đảm payload luôn null-terminated an toàn
    const size_t safe_len = std::min<size_t>(item.pkt.payload_len, sizeof(item.pkt.payload) - 1);
    item.pkt.payload[safe_len] = '\0';
    item.pkt.payload_len       = static_cast<uint16_t>(safe_len);

    if (xQueueSend(self.rx_queue_, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Hàng đợi ESP-NOW RX đầy, bỏ qua gói tin seq=%u", pkt->seq_num);
    }
}

void EspNowMeshManager::on_data_sent(const uint8_t* mac_addr, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS && mac_addr) {
        ESP_LOGW(TAG, "Gửi ESP-NOW thất bại tới %02X:%02X:%02X:%02X:%02X:%02X",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
    }
}

// ---------------------------------------------------------------------------
// rx_worker_task() — Xử lý logic nặng (UI/Audio/ACK/Callback) ngoài Wi-Fi task
// ---------------------------------------------------------------------------
void EspNowMeshManager::rx_worker_task(void* arg) {
    auto* self = static_cast<EspNowMeshManager*>(arg);
    QueuedRxItem item = {};

    while (self->task_running_.load()) {
        if (xQueueReceive(self->rx_queue_, &item, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (!self->task_running_.load() || !self->initialized_.load()) {
                break;
            }
            self->handle_received_packet(item.src_mac, item.pkt);
        }
    }

    self->rx_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// handle_received_packet()
// ---------------------------------------------------------------------------
void EspNowMeshManager::handle_received_packet(const uint8_t* sender_mac, const EspNowPacket& pkt) {
    char sender_str[18];
    snprintf(sender_str, sizeof(sender_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             sender_mac[0], sender_mac[1], sender_mac[2],
             sender_mac[3], sender_mac[4], sender_mac[5]);

    ESP_LOGI(TAG, "Nhận ESP-NOW từ %s [cmd=0x%02X, seq=%u]: %s",
             sender_str, static_cast<uint8_t>(pkt.cmd), pkt.seq_num, pkt.payload);

    switch (pkt.cmd) {
        case EspNowCmd::ALERT: {
            // Đánh thức Bot + mắt Ngạc Nhiên + popup cảnh báo + âm báo
            UiEyesClock::instance().switch_to_chat_screen();
            UiEyesClock::instance().set_emotion(EyeEmotion::NGAC_NHIEN);

            ChronosNotification notif = {};
            snprintf(notif.app_name, sizeof(notif.app_name), "Cảnh báo ESP-NOW");
            snprintf(notif.title, sizeof(notif.title), "Node %s", sender_str);
            snprintf(notif.body, sizeof(notif.body), "%s",
                     pkt.payload[0] ? pkt.payload : "Phát hiện tín hiệu khẩn!");
            notif.unread = true;
            UiEyesClock::instance().show_notification(notif);

            AudioHal::instance().play_chime(AudioHal::ChimeType::ALERT);
            break;
        }

        case EspNowCmd::CONTROL: {
            if (strncmp(pkt.payload, "VOL:", 4) == 0) {
                int vol = atoi(pkt.payload + 4);
                vol = std::max(0, std::min(100, vol));
                AudioHal::instance().set_speaker_volume(static_cast<uint8_t>(vol));
                UiEyesClock::instance().show_volume_osd(static_cast<uint8_t>(vol));
            } else if (strncmp(pkt.payload, "MUTE:", 5) == 0) {
                bool mute = (atoi(pkt.payload + 5) != 0);
                AudioHal::instance().set_muted(mute);
                UiEyesClock::instance().show_volume_osd(
                    mute ? 0 : AudioHal::instance().get_speaker_volume());
            } else if (strncmp(pkt.payload, "EMOTION:", 8) == 0) {
                const char* em_str = pkt.payload + 8;
                EyeEmotion em = EyeEmotion::BINH_THUONG;
                if (strcmp(em_str, "NGHE") == 0 || strcmp(em_str, "DANG_NGHE") == 0) {
                    em = EyeEmotion::DANG_NGHE;
                } else if (strcmp(em_str, "SUY_NGHI") == 0) {
                    em = EyeEmotion::SUY_NGHI;
                } else if (strcmp(em_str, "VUI") == 0 || strcmp(em_str, "VUI_VE") == 0) {
                    em = EyeEmotion::VUI_VE;
                } else if (strcmp(em_str, "BUON") == 0) {
                    em = EyeEmotion::BUON;
                } else if (strcmp(em_str, "NGAC") == 0 || strcmp(em_str, "NGAC_NHIEN") == 0) {
                    em = EyeEmotion::NGAC_NHIEN;
                } else if (strcmp(em_str, "NGU") == 0 || strcmp(em_str, "NGU_GAT") == 0) {
                    em = EyeEmotion::NGU_GAT;
                }
                UiEyesClock::instance().switch_to_chat_screen();
                UiEyesClock::instance().set_emotion(em);
            }
            break;
        }

        case EspNowCmd::TEXT: {
            ChronosNotification notif = {};
            snprintf(notif.app_name, sizeof(notif.app_name), "ESP-NOW");
            snprintf(notif.title, sizeof(notif.title), "%s", sender_str);
            snprintf(notif.body, sizeof(notif.body), "%s", pkt.payload);
            notif.unread = true;
            UiEyesClock::instance().show_notification(notif);
            break;
        }

        case EspNowCmd::STATUS:
            ESP_LOGI(TAG, "Trạng thái từ %s: %s", sender_str, pkt.payload);
            break;

        case EspNowCmd::ACK:
            ESP_LOGD(TAG, "Nhận ACK từ %s (%s)", sender_str, pkt.payload);
            break;
    }

    // Sao chép callback dưới mutex rồi mới thực thi ngoài lock để tránh deadlock
    EspNowRxCallback cb_copy;
    if (cb_mutex_ && xSemaphoreTake(cb_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        cb_copy = rx_callback_;
        xSemaphoreGive(cb_mutex_);
    }
    if (cb_copy) {
        cb_copy(sender_mac, pkt);
    }

    // Gửi ACK xác nhận về node nguồn (nếu không phải gói ACK)
    const bool is_bcast = (memcmp(sender_mac, kBroadcastMac, 6) == 0);
    if (!is_bcast && pkt.cmd != EspNowCmd::ACK) {
        char ack_payload[24];
        snprintf(ack_payload, sizeof(ack_payload), "ACK:%u", pkt.seq_num);
        send_command(sender_mac, EspNowCmd::ACK, ack_payload);
    }
}

// ---------------------------------------------------------------------------
// CRC-8/MAXIM & Dedup
// ---------------------------------------------------------------------------
uint8_t EspNowMeshManager::compute_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = static_cast<uint8_t>((crc << 1) ^ 0x31);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

bool EspNowMeshManager::check_and_mark_seen(const uint8_t mac[6], uint16_t seq) {
    bool duplicate = false;
    portENTER_CRITICAL(&dedup_lock_);
    for (size_t i = 0; i < DEDUP_SIZE; i++) {
        if (memcmp(dedup_buf_[i].mac, mac, 6) == 0 && dedup_buf_[i].seq == seq) {
            duplicate = true;
            break;
        }
    }
    if (!duplicate) {
        memcpy(dedup_buf_[dedup_idx_].mac, mac, 6);
        dedup_buf_[dedup_idx_].seq = seq;
        dedup_idx_ = (dedup_idx_ + 1) % DEDUP_SIZE;
    }
    portEXIT_CRITICAL(&dedup_lock_);
    return duplicate;
}

// ---------------------------------------------------------------------------
// MAC Utilities
// ---------------------------------------------------------------------------
void EspNowMeshManager::get_own_mac(uint8_t mac_out[6]) const {
    if (mac_out) {
        memcpy(mac_out, own_mac_, 6);
    }
}

bool EspNowMeshManager::parse_mac_str(const char* str, uint8_t mac_out[6]) {
    if (!str || !mac_out) return false;

    if (strcasecmp(str, "broadcast") == 0 || strcmp(str, "*") == 0) {
        memcpy(mac_out, kBroadcastMac, 6);
        return true;
    }

    unsigned int b[6] = {};
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6 ||
        sscanf(str, "%2x-%2x-%2x-%2x-%2x-%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; ++i) {
            mac_out[i] = static_cast<uint8_t>(b[i] & 0xFFU);
        }
        return true;
    }
    return false;
}

} // namespace zclaw
