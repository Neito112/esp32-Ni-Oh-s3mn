#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_now.h"

namespace zclaw {

// Lệnh ESP-NOW 2 chiều
enum class EspNowCmd : uint8_t {
    ALERT   = 0x01,  // Cảnh báo khẩn -> đánh thức bot
    CONTROL = 0x02,  // Điều khiển (VOL:xx, MUTE:0/1, EMOTION:xxx)
    TEXT    = 0x03,  // Tin nhắn văn bản hiển thị popup
    STATUS  = 0x04,  // Trạng thái node
    ACK     = 0x05,  // Xác nhận nhận được gói tin
};

#pragma pack(push, 1)
struct EspNowPacket {
    uint8_t   magic;          // 0xEC (Esp-Claw)
    uint8_t   version;        // 0x01
    EspNowCmd cmd;
    uint16_t  seq_num;        // Số thứ tự gói tin (chống lặp gói)
    uint8_t   sender_mac[6];  // MAC thiết bị gửi
    uint16_t  payload_len;    // Độ dài payload thực tế
    char      payload[200];   // Nội dung UTF-8 null-terminated
    uint8_t   crc8;           // CRC-8/MAXIM của phần trước byte cuối
};
#pragma pack(pop)

static_assert(sizeof(EspNowPacket) <= 250, "EspNowPacket vượt quá giới hạn 250 bytes của ESP-NOW");

// Callback khi nhận gói tin hợp lệ (chạy trong context worker task, an toàn để block ngắn)
using EspNowRxCallback = std::function<void(const uint8_t sender_mac[6], const EspNowPacket& pkt)>;

class EspNowMeshManager {
public:
    static EspNowMeshManager& instance();

    // Khởi tạo ESP-NOW (gọi SAU khi esp_wifi_init/start, hỗ trợ chạy song song Wi-Fi STA)
    esp_err_t init();
    void deinit();

    // Đồng bộ lại kênh ESP-NOW theo kênh Wi-Fi hiện tại (gọi khi STA connect / đổi AP)
    esp_err_t sync_channel_with_wifi();
    uint8_t   get_channel() const { return wifi_channel_.load(); }

    // Gửi lệnh tới MAC đích hoặc Broadcast
    esp_err_t send_command(const uint8_t dst_mac[6], EspNowCmd cmd, const char* payload);
    esp_err_t broadcast(EspNowCmd cmd, const char* payload);

    // Quản lý peers
    esp_err_t add_peer(const uint8_t mac[6]);
    esp_err_t remove_peer(const uint8_t mac[6]);
    bool      is_peer_exists(const uint8_t mac[6]) const;

    // Đăng ký callback nhận gói tin (thread-safe)
    void set_rx_callback(EspNowRxCallback cb);

    // Tiện ích MAC
    void        get_own_mac(uint8_t mac_out[6]) const;
    static bool parse_mac_str(const char* str, uint8_t mac_out[6]);

    bool is_initialized() const { return initialized_.load(); }

private:
    EspNowMeshManager() = default;
    ~EspNowMeshManager() = default;
    EspNowMeshManager(const EspNowMeshManager&) = delete;
    EspNowMeshManager& operator=(const EspNowMeshManager&) = delete;

    struct QueuedRxItem {
        uint8_t      src_mac[6];
        EspNowPacket pkt;
    };

    // Static ESP-NOW & FreeRTOS callbacks
    static void on_data_recv(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len);
    static void on_data_sent(const uint8_t* mac_addr, esp_now_send_status_t status);
    static void rx_worker_task(void* arg);

    // Xử lý gói tin nhận được trong worker task
    void handle_received_packet(const uint8_t* sender_mac, const EspNowPacket& pkt);

    // CRC-8/MAXIM (polynomial 0x31)
    static uint8_t compute_crc8(const uint8_t* data, size_t len);

    // Khử trùng lặp gói tin (thread-safe dưới dedup_lock_)
    struct SeenEntry { uint8_t mac[6]; uint16_t seq; };
    static constexpr size_t DEDUP_SIZE = 32;
    SeenEntry    dedup_buf_[DEDUP_SIZE] = {};
    size_t       dedup_idx_  = 0;
    portMUX_TYPE dedup_lock_ = portMUX_INITIALIZER_UNLOCKED;

    bool check_and_mark_seen(const uint8_t mac[6], uint16_t seq);

    EspNowRxCallback     rx_callback_;
    SemaphoreHandle_t    cb_mutex_       = nullptr;
    QueueHandle_t        rx_queue_       = nullptr;
    TaskHandle_t         rx_task_handle_ = nullptr;

    std::atomic<bool>     initialized_{false};
    std::atomic<bool>     task_running_{false};
    std::atomic<uint16_t> seq_counter_{0};
    std::atomic<uint8_t>  wifi_channel_{1};

    uint8_t own_mac_[6] = {};
};

} // namespace zclaw
