#pragma once
#include <cstdint>
#include <functional>
#include <atomic>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "ui_eyes_clock.h"  // ChronosNavInfo, ChronosNotification, WeatherData

struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace zclaw {

// Callback types
using TimeSyncCallback       = std::function<void(const struct tm& t)>;
using NotificationCallback   = std::function<void(const ChronosNotification& notif)>;
using NavigationCallback     = std::function<void(const ChronosNavInfo& nav)>;
using BatteryCallback        = std::function<void(uint8_t pct)>;
using WeatherRxCallback      = std::function<void(const WeatherData& weather)>;

struct ChronosSyncStatus {
    bool                 initialized;
    bool                 connected;
    uint8_t              phone_battery_pct;
    uint32_t             rx_packets;
    char                 last_time_sync[32];
    WeatherData          last_weather;
    ChronosNotification  last_notif;
    ChronosNavInfo       last_nav;
};

class ChronosBleService {
public:
    static ChronosBleService& instance();

    // Khởi tạo NimBLE host, đăng ký GATT services, bắt đầu quảng bá
    esp_err_t init(const char* device_name = "S.W.Ni-Oh");
    void deinit();

    // Đăng ký callbacks
    void on_time_sync(TimeSyncCallback cb)        { cb_time_sync_ = cb; }
    void on_notification(NotificationCallback cb) { cb_notification_ = cb; }
    void on_navigation(NavigationCallback cb)     { cb_navigation_ = cb; }
    void on_battery(BatteryCallback cb)           { cb_battery_ = cb; }
    void on_weather(WeatherRxCallback cb)         { cb_weather_ = cb; }

    // Gửi dữ liệu về phone qua RX characteristic (notify)
    esp_err_t send_response(const uint8_t* data, size_t len);

    // Gửi heartbeat/status về phone
    esp_err_t send_device_status(uint8_t battery_pct, bool mic_active, bool wifi_ok);

    bool              is_connected()      const { return connected_.load(); }
    uint16_t          get_conn_handle()   const { return conn_handle_; }
    uint8_t           get_phone_battery() const { return phone_battery_pct_.load(); }
    ChronosSyncStatus get_sync_status()   const;

    // NimBLE C callbacks (public so file-scope GATT table initializers can reference them)
    static int  ble_gap_event_cb(struct ::ble_gap_event* event, void* arg);
    static int  nus_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ::ble_gatt_access_ctxt* ctxt, void* arg);

private:
    ChronosBleService() = default;
    ~ChronosBleService() = default;
    ChronosBleService(const ChronosBleService&) = delete;
    ChronosBleService& operator=(const ChronosBleService&) = delete;

    // Callbacks đã đăng ký
    TimeSyncCallback     cb_time_sync_;
    NotificationCallback cb_notification_;
    NavigationCallback   cb_navigation_;
    BatteryCallback      cb_battery_;
    WeatherRxCallback    cb_weather_;

    // NimBLE state
    std::atomic<bool>    connected_{false};
    std::atomic<bool>    initialized_{false};
    std::atomic<uint8_t> phone_battery_pct_{100};
    std::atomic<uint32_t> rx_packets_{0};
    uint16_t             conn_handle_    = 0xFFFF;
    uint16_t             rx_char_handle_ = 0;  // Handle của RX char (để notify)

    mutable portMUX_TYPE status_spin_ = portMUX_INITIALIZER_UNLOCKED;
    char                 last_time_sync_[32] = {};
    WeatherData          last_weather_ = {};
    ChronosNotification  last_notif_ = {};
    ChronosNavInfo       last_nav_ = {};

    // Bộ đệm ghép gói Chronos nhiều chunk BLE
    uint8_t rx_reasm_buf_[512] = {};
    size_t  rx_reasm_len_      = 0;
    size_t  rx_expected_len_   = 0;

    // Parse incoming Chronos packets
    void feed_ble_chunk(const uint8_t* data, size_t len);
    void parse_chronos_packet(const uint8_t* data, size_t len);
    void handle_time_sync(const uint8_t* payload, size_t len);
    void handle_chronos_rtc_time(const uint8_t* payload, size_t len);
    void handle_chronos_extended_frame(const uint8_t* data, size_t len);
    void handle_notification_packet(const uint8_t* payload, size_t len);
    void handle_navigation_packet(const uint8_t* payload, size_t len);
    void handle_weather_packet(const uint8_t* payload, size_t len);
    void handle_battery_packet(const uint8_t* payload, size_t len);

    // NimBLE static helpers
    static void ble_host_task(void* param);
    static void on_sync_cb();
    static void on_reset_cb(int reason);
    void start_advertising();
};

} // namespace zclaw
